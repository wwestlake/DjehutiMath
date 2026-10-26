#include "NodeCompiler.h"
#include "FrustEngine.h"

#include <juce_core/juce_core.h>

#include <cstring>
#include <cctype>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <vector>

#include <CompilerApi.h>

using namespace frust;

namespace node_compiler {

namespace {

struct InputRef {
    bool isParam = false;
    std::string name; // param name or node id
};

struct NodeDef {
    std::string id;
    std::string type;
    juce::var raw;
    std::vector<InputRef> inputs;
};

struct ParamDef {
    std::string name;
    std::string type;
};

bool ParseInputRef(const juce::var& v, InputRef& out, std::string& err) {
    if (!v.isObject()) { err = "input ref must be an object"; return false; }
    auto* obj = v.getDynamicObject();
    if (obj->hasProperty("ref")) {
        out.isParam = false;
        out.name = obj->getProperty("ref").toString().toStdString();
        return true;
    }
    if (obj->hasProperty("param")) {
        out.isParam = true;
        out.name = obj->getProperty("param").toString().toStdString();
        return true;
    }
    err = "input ref must have 'ref' or 'param'";
    return false;
}

// Kahn's algorithm - real cycle detection, not silently ignored.
bool TopoSort(const std::vector<NodeDef>& nodes, std::vector<std::string>& order, std::string& err) {
    std::set<std::string> ids;
    for (auto& n : nodes) ids.insert(n.id);

    std::map<std::string, std::vector<std::string>> dependents;
    std::map<std::string, int> inDegree;
    for (auto& n : nodes) inDegree[n.id] = 0;

    for (auto& n : nodes) {
        for (auto& in : n.inputs) {
            if (in.isParam) continue;
            if (!ids.count(in.name)) {
                err = "node '" + n.id + "' references unknown node '" + in.name + "'";
                return false;
            }
            dependents[in.name].push_back(n.id);
            inDegree[n.id]++;
        }
    }

    std::vector<std::string> ready;
    for (auto& n : nodes) if (inDegree[n.id] == 0) ready.push_back(n.id);

    while (!ready.empty()) {
        std::string id = ready.back();
        ready.pop_back();
        order.push_back(id);
        for (auto& dep : dependents[id]) {
            if (--inDegree[dep] == 0) ready.push_back(dep);
        }
    }

    if (order.size() != nodes.size()) {
        err = "graph has a cycle in its node references";
        return false;
    }
    return true;
}

bool IsNumericType(const std::string& t) { return t == "i64" || t == "f64"; }

std::string FormatFnFor(const std::string& t) {
    if (t == "i64") return "frust_format_i64";
    if (t == "f64") return "frust_format_f64";
    if (t == "bool") return "frust_format_bool";
    return "";
}

struct CodegenState {
    // A programmable component's program: its pins, parameters and state
    // variables by name -> index (the host functions take the index).
    bool component = false;
    std::map<std::string, int> pinIndex, paramIndex, stateIndex;
    std::map<std::string, NodeDef*> nodesById;
    std::map<std::string, std::string> paramType;  // param name -> type
    std::map<std::string, std::string> resultType; // node id -> its result Frust type
    std::set<std::string> externDeclared;
    std::ostringstream externs;
    std::ostringstream body;
    std::string* err;
};

std::string RefName(const InputRef& in) { return in.name; }

std::string RefType(CodegenState& st, const InputRef& in) {
    if (in.isParam) return st.paramType.count(in.name) ? st.paramType[in.name] : "";
    return st.resultType.count(in.name) ? st.resultType[in.name] : "";
}

void DeclareExtern(CodegenState& st, const std::string& decl, const std::string& key) {
    if (st.externDeclared.count(key)) return;
    st.externDeclared.insert(key);
    st.externs << decl << "\n";
}

// Emits `let <id>: <type> = <expr>;` and records the node's result type.
// Returns false (setting *st.err) on a real graph error.
bool EmitNode(CodegenState& st, const NodeDef& n) {
    const std::string& type = n.type;

    auto binaryArith = [&](const char* op) -> bool {
        if (n.inputs.size() != 2) { *st.err = "node '" + n.id + "' (" + type + ") needs exactly 2 inputs"; return false; }
        std::string lt = RefType(st, n.inputs[0]);
        std::string rt = RefType(st, n.inputs[1]);
        if (lt.empty() || rt.empty()) { *st.err = "node '" + n.id + "': input type unknown (bad ref?)"; return false; }
        if (!IsNumericType(lt) || !IsNumericType(rt)) { *st.err = "node '" + n.id + "': " + type + " needs numeric inputs"; return false; }
        std::string resultType = (lt == "f64" || rt == "f64") ? "f64" : "i64";
        st.body << "    let " << n.id << ": " << resultType << " = " << RefName(n.inputs[0]) << " " << op << " " << RefName(n.inputs[1]) << ";\n";
        st.resultType[n.id] = resultType;
        return true;
    };

    auto comparison = [&](const char* op) -> bool {
        if (n.inputs.size() != 2) { *st.err = "node '" + n.id + "' (" + type + ") needs exactly 2 inputs"; return false; }
        std::string lt = RefType(st, n.inputs[0]);
        std::string rt = RefType(st, n.inputs[1]);
        if (lt.empty() || rt.empty()) { *st.err = "node '" + n.id + "': input type unknown (bad ref?)"; return false; }
        st.body << "    let " << n.id << ": bool = " << RefName(n.inputs[0]) << " " << op << " " << RefName(n.inputs[1]) << ";\n";
        st.resultType[n.id] = "bool";
        return true;
    };

    if (type == "literal_i64" || type == "literal_f64" || type == "literal_bool" || type == "literal_string") {
        if (!n.raw.getDynamicObject()->hasProperty("value")) { *st.err = "literal node '" + n.id + "' needs a 'value'"; return false; }
        juce::var val = n.raw.getDynamicObject()->getProperty("value");
        std::string frustType, literalText;
        if (type == "literal_i64") { frustType = "i64"; literalText = std::to_string((int64_t)val); }
        else if (type == "literal_f64") {
            frustType = "f64";
            std::ostringstream f;
            f << std::setprecision(17) << (double)val;
            literalText = f.str();
            if (literalText.find_first_of(".eEn") == std::string::npos) literalText += ".0"; // 3 -> 3.0 (an f64 literal)
        }
        else if (type == "literal_bool") { frustType = "bool"; literalText = (bool)val ? "true" : "false"; }
        else {
            frustType = "String";
            juce::String s = val.toString();
            // Escape embedded quotes/backslashes so the generated
            // literal round-trips correctly (confirmed \" already works
            // - see Codegen.h's test coverage).
            juce::String escaped = s.replace("\\", "\\\\").replace("\"", "\\\"");
            literalText = "\"" + escaped.toStdString() + "\"";
        }
        st.body << "    let " << n.id << ": " << frustType << " = " << literalText << ";\n";
        st.resultType[n.id] = frustType;
        return true;
    }
    if (type == "add") return binaryArith("+");
    if (type == "sub") return binaryArith("-");
    if (type == "mul") return binaryArith("*");
    if (type == "div") return binaryArith("/");
    if (type == "mod") return binaryArith("%");
    if (type == "eq")  return comparison("==");
    if (type == "neq") return comparison("!=");
    if (type == "lt")  return comparison("<");
    if (type == "gt")  return comparison(">");
    if (type == "le")  return comparison("<=");
    if (type == "ge")  return comparison(">=");

    if (type == "if") {
        if (n.inputs.size() != 3) { *st.err = "node '" + n.id + "' (if) needs exactly 3 inputs: [cond, then, else]"; return false; }
        std::string condType = RefType(st, n.inputs[0]);
        if (condType != "bool") { *st.err = "node '" + n.id + "': if's condition input must be bool"; return false; }
        std::string thenType = RefType(st, n.inputs[1]);
        std::string elseType = RefType(st, n.inputs[2]);
        if (thenType.empty() || elseType.empty()) { *st.err = "node '" + n.id + "': then/else input type unknown"; return false; }
        if (thenType != elseType) { *st.err = "node '" + n.id + "': if's then/else branches have different types (" + thenType + " vs " + elseType + ")"; return false; }
        st.body << "    let " << n.id << ": " << thenType << " = if (" << RefName(n.inputs[0]) << ") { "
                << RefName(n.inputs[1]) << " } else { " << RefName(n.inputs[2]) << " };\n";
        st.resultType[n.id] = thenType;
        return true;
    }

    if (type == "call") {
        auto* obj = n.raw.getDynamicObject();
        if (!obj->hasProperty("function") || !obj->hasProperty("returnType")) {
            *st.err = "node '" + n.id + "' (call) needs 'function' and 'returnType'";
            return false;
        }
        std::string fnName = obj->getProperty("function").toString().toStdString();
        std::string retType = obj->getProperty("returnType").toString().toStdString();
        std::vector<std::string> paramTypes;
        if (obj->hasProperty("paramTypes") && obj->getProperty("paramTypes").isArray()) {
            for (auto& pt : *obj->getProperty("paramTypes").getArray()) paramTypes.push_back(pt.toString().toStdString());
        }
        if (paramTypes.size() != n.inputs.size()) {
            *st.err = "node '" + n.id + "': " + std::to_string(n.inputs.size()) + " input(s) but " + std::to_string(paramTypes.size()) + " paramTypes";
            return false;
        }
        std::ostringstream sig;
        sig << "extern fn " << fnName << "(";
        for (size_t i = 0; i < paramTypes.size(); ++i) { if (i) sig << ", "; sig << "a" << i << ": " << paramTypes[i]; }
        sig << ") -> " << retType << ";";
        DeclareExtern(st, sig.str(), "fn:" + fnName);

        st.body << "    let " << n.id << ": " << retType << " = " << fnName << "(";
        for (size_t i = 0; i < n.inputs.size(); ++i) { if (i) st.body << ", "; st.body << RefName(n.inputs[i]); }
        st.body << ");\n";
        st.resultType[n.id] = retType;
        return true;
    }

    // ---- programmable component nodes (Workbench) ----
    // Each reads or drives the component's electrical interface through the
    // Workbench's host functions (FrustComponent.cpp), by index: the name a
    // node is given ("text") is looked up in the component's pins,
    // parameters or state. Values are f64 volts, amps, seconds.
    if (type.rfind("pc_", 0) == 0) {
        auto* obj = n.raw.getDynamicObject();
        const auto name = obj->getProperty("text").toString().trim().toStdString();
        auto lookup = [&](const std::map<std::string, int>& table, const char* what, int& index) -> bool {
            if (!st.component) { *st.err = "node '" + n.id + "' (" + type + ") belongs in a programmable component's program"; return false; }
            const auto found = table.find(name);
            if (found == table.end()) {
                std::string known;
                for (const auto& [k, v] : table) known += (known.empty() ? "" : ", ") + k;
                *st.err = "node '" + n.id + "': the component has no " + what + " '" + name + "'" + (known.empty() ? std::string() : " (it has: " + known + ")");
                return false;
            }
            index = found->second;
            return true;
        };
        auto host = [&](const std::string& decl, const std::string& key) { DeclareExtern(st, decl, key); };
        // An input value as f64 (an integer is promoted; a bool is 1.0 / 0.0).
        auto valueInput = [&](std::string& expr) -> bool {
            if (n.inputs.size() != 1) { *st.err = "node '" + n.id + "' (" + type + ") needs its value input wired"; return false; }
            const auto t = RefType(st, n.inputs[0]);
            if (t == "f64") expr = RefName(n.inputs[0]);
            else if (t == "i64") expr = "(" + RefName(n.inputs[0]) + " + 0.0)";
            else if (t == "bool") expr = "if (" + RefName(n.inputs[0]) + ") { 1.0 } else { 0.0 }";
            else { *st.err = "node '" + n.id + "': its value must be a number or a bool, not " + (t.empty() ? std::string("unknown") : t); return false; }
            return true;
        };
        int index = 0;
        std::string call, value;
        if (type == "pc_pin_voltage") {
            if (!lookup(st.pinIndex, "pin", index)) return false;
            host("extern fn djehuti_pc_v(pin: i64) -> f64;", "djehuti_pc_v");
            call = "djehuti_pc_v(" + std::to_string(index) + ")";
        } else if (type == "pc_pin_current") {
            if (!lookup(st.pinIndex, "pin", index)) return false;
            host("extern fn djehuti_pc_i(pin: i64) -> f64;", "djehuti_pc_i");
            call = "djehuti_pc_i(" + std::to_string(index) + ")";
        } else if (type == "pc_time") {
            if (!st.component) { *st.err = "node '" + n.id + "' (pc_time) belongs in a programmable component's program"; return false; }
            host("extern fn djehuti_pc_time() -> f64;", "djehuti_pc_time");
            call = "djehuti_pc_time()";
        } else if (type == "pc_timestep") {
            if (!st.component) { *st.err = "node '" + n.id + "' (pc_timestep) belongs in a programmable component's program"; return false; }
            host("extern fn djehuti_pc_dt() -> f64;", "djehuti_pc_dt");
            call = "djehuti_pc_dt()";
        } else if (type == "pc_parameter") {
            if (!lookup(st.paramIndex, "parameter", index)) return false;
            host("extern fn djehuti_pc_param(index: i64) -> f64;", "djehuti_pc_param");
            call = "djehuti_pc_param(" + std::to_string(index) + ")";
        } else if (type == "pc_state_get") {
            if (!lookup(st.stateIndex, "state variable", index)) return false;
            host("extern fn djehuti_pc_state(index: i64) -> f64;", "djehuti_pc_state");
            call = "djehuti_pc_state(" + std::to_string(index) + ")";
        } else if (type == "pc_state_set") {
            if (!lookup(st.stateIndex, "state variable", index) || !valueInput(value)) return false;
            host("extern fn djehuti_pc_set_state(index: i64, value: f64) -> f64;", "djehuti_pc_set_state");
            call = "djehuti_pc_set_state(" + std::to_string(index) + ", " + value + ")";
        } else if (type == "pc_drive") {
            if (!lookup(st.pinIndex, "pin", index) || !valueInput(value)) return false;
            host("extern fn djehuti_pc_drive(pin: i64, value: f64) -> f64;", "djehuti_pc_drive");
            call = "djehuti_pc_drive(" + std::to_string(index) + ", " + value + ")";
        } else if (type == "pc_drive_current") {
            if (!lookup(st.pinIndex, "pin", index) || !valueInput(value)) return false;
            host("extern fn djehuti_pc_drive_current(pin: i64, amps: f64) -> f64;", "djehuti_pc_drive_current");
            call = "djehuti_pc_drive_current(" + std::to_string(index) + ", " + value + ")";
        } else if (type == "pc_set_resistance") {
            if (!lookup(st.pinIndex, "pin", index) || !valueInput(value)) return false;
            host("extern fn djehuti_pc_set_resistance(pin: i64, ohms: f64) -> f64;", "djehuti_pc_set_resistance");
            call = "djehuti_pc_set_resistance(" + std::to_string(index) + ", " + value + ")";
        } else {
            *st.err = "unknown node type '" + type + "' (node '" + n.id + "')";
            return false;
        }
        st.body << "    let " << n.id << ": f64 = " << call << ";\n";
        st.resultType[n.id] = "f64";
        return true;
    }

    if (type == "print") {
        auto* obj = n.raw.getDynamicObject();
        if (!obj->hasProperty("valueType")) { *st.err = "node '" + n.id + "' (print) needs 'valueType'"; return false; }
        if (n.inputs.size() != 1) { *st.err = "node '" + n.id + "' (print) needs exactly 1 input"; return false; }
        std::string valueType = obj->getProperty("valueType").toString().toStdString();
        DeclareExtern(st, "extern fn frust_print_str(val: String);", "frust_print_str");
        if (valueType == "string") {
            st.body << "    frust_print_str(" << RefName(n.inputs[0]) << ");\n";
        } else {
            std::string fmtFn = FormatFnFor(valueType);
            if (fmtFn.empty()) { *st.err = "node '" + n.id + "': unknown print valueType '" + valueType + "'"; return false; }
            std::string retT = (valueType == "bool") ? "bool" : valueType;
            DeclareExtern(st, "extern fn " + fmtFn + "(val: " + retT + ") -> String;", fmtFn);
            st.body << "    frust_print_str(" << fmtFn << "(" << RefName(n.inputs[0]) << "));\n";
        }
        // print has no value - not recorded in resultType, so it can
        // never be referenced by a downstream node or chosen as the
        // graph's "output" (checked explicitly below).
        return true;
    }

    *st.err = "unknown node type '" + type + "' (node '" + n.id + "')";
    return false;
}

// Workbench copy: the generated source is checked with the Workbench's
// embedded FRust compiler (frust::Compile through frust_engine, the same
// compiler that runs it). The reference parsed it through frust_lang's
// internal Lexer/Parser/Codegen.
bool ValidateCompiles(const std::string& source, std::string& err, bool debug = false) {
    const auto checked = frust_engine::check("node_program.fr", debug ? DebugPrelude() + source : source);
    if (checked.ok)
        return true;
    std::ostringstream msg;
    for (const auto& d : checked.diagnostics)
        if (d.error)
            msg << d.text() << "\n";
    if (!checked.error.empty())
        msg << checked.error << "\n";
    err = msg.str().empty() ? std::string("the FRust compiler rejected the generated source") : msg.str();
    return false;
}

// Workbench copy: executable and state-machine programs print through the
// FRust runtime's exported functions directly - the lowering the v1 `print`
// node already uses, and exactly what core's println_* do. The reference
// imported the core pod and called println_* unqualified, which the current
// FRust compiler rejects (pod functions are called through the pod's
// namespace) and which needs core's prebuilt object at run time, which an
// in-process JIT load does not provide.
std::string PrintStatement(const std::string& valueType, const std::string& expression) {
    if (valueType == "string") return "frust_print_str(" + expression + ");";
    return "frust_print_str(frust_format_" + valueType + "(" + expression + "));";
}

std::string PrintExterns(const std::set<std::string>& valueTypes) {
    std::string out = "extern fn frust_print_str(val: String);\n";
    for (const auto& t : valueTypes)
        if (t != "string")
            out += "extern fn frust_format_" + t + "(val: " + t + ") -> String;\n";
    return out;
}

// Workbench debugger instrumentation (see DebugInfo). Each call is one
// statement put on the line it belongs to.
std::string DebugEnter(int fn) { return "djehuti_dbg_enter(" + std::to_string(fn) + ");"; }
std::string DebugLeave(int fn) { return "djehuti_dbg_leave(" + std::to_string(fn) + ");"; }

// The type's record call, or "" for a type the debugger cannot show.
std::string DebugRecord(int slot, const std::string& type, const std::string& expression) {
    const auto n = std::to_string(slot);
    if (type == "i64") return "djehuti_dbg_i64(" + n + ", " + expression + ");";
    if (type == "f64") return "djehuti_dbg_f64(" + n + ", " + expression + ");";
    if (type == "bool") return "djehuti_dbg_bool(" + n + ", if (" + expression + ") { 1 } else { 0 });";
    if (type == "String" || type == "string") return "djehuti_dbg_str(" + n + ", " + expression + ");";
    return {};
}

int AddFunction(DebugInfo& info, const std::string& name, const std::string& nodeId, int firstLine) {
    info.functions.push_back({ name, nodeId, firstLine, 0 });
    return (int)info.functions.size() - 1;
}

// Each function runs to the line before the next one starts (blank lines
// between them have no code); the last to the end of the program.
void FinishFunctionRanges(DebugInfo& info, int totalLines) {
    for (auto& fn : info.functions) {
        fn.lastLine = totalLines;
        for (const auto& other : info.functions)
            if (other.firstLine > fn.firstLine && other.firstLine - 1 < fn.lastLine)
                fn.lastLine = other.firstLine - 1;
    }
}

int AddSlot(DebugInfo& info, const std::string& name, const std::string& type,
            std::vector<std::string> nodeIds, std::vector<std::string> valueNames = {}) {
    info.slots.push_back({ name, type == "string" ? "String" : type, std::move(nodeIds), std::move(valueNames) });
    return (int)info.slots.size() - 1;
}

int CountLines(const std::string& text) {
    return (int)std::count(text.begin(), text.end(), '\n');
}

} // namespace

CompileResult CompileGraphToSource(const std::string& graphJson) {
    std::vector<SourceMapEntry> sourceMap;
    DebugInfo debug;
    return CompileGraphToSource(graphJson, {}, sourceMap, debug);
}

CompileResult CompileGraphToSource(const std::string& graphJson, const CompileOptions& options,
                                   std::vector<SourceMapEntry>& sourceMap, DebugInfo& debug) {
    CompileResult result;
    const bool instrument = options.debugInstrumentation;

    juce::var parsed;
    auto parseResult = juce::JSON::parse(juce::String(graphJson), parsed);
    if (parseResult.failed() || !parsed.isObject()) {
        result.errorMessage = "invalid graph JSON: " + parseResult.getErrorMessage().toStdString();
        return result;
    }
    auto* root = parsed.getDynamicObject();

    if (!root->hasProperty("functionName") || !root->hasProperty("output") || !root->hasProperty("nodes")) {
        result.errorMessage = "graph JSON needs 'functionName', 'output', and 'nodes'";
        return result;
    }
    std::string functionName = root->getProperty("functionName").toString().toStdString();
    std::string outputId = root->getProperty("output").toString().toStdString();

    std::vector<ParamDef> params;
    if (root->hasProperty("params") && root->getProperty("params").isArray()) {
        for (auto& p : *root->getProperty("params").getArray()) {
            if (!p.isObject()) { result.errorMessage = "each param must be an object"; return result; }
            auto* pObj = p.getDynamicObject();
            ParamDef pd;
            pd.name = pObj->getProperty("name").toString().toStdString();
            pd.type = pObj->getProperty("type").toString().toStdString();
            params.push_back(pd);
        }
    }

    if (root->getProperty("component").isObject() && !root->hasProperty("output"))
        root->setProperty("output", "");
    if (!parsed.getDynamicObject()->getProperty("nodes").isArray()) {
        result.errorMessage = "'nodes' must be an array";
        return result;
    }

    std::vector<NodeDef> nodes;
    std::set<std::string> seenIds;
    for (auto& nv : *root->getProperty("nodes").getArray()) {
        if (!nv.isObject()) { result.errorMessage = "each node must be an object"; return result; }
        auto* nObj = nv.getDynamicObject();
        NodeDef n;
        n.id = nObj->getProperty("id").toString().toStdString();
        n.type = nObj->getProperty("type").toString().toStdString();
        n.raw = nv;
        if (n.id.empty()) { result.errorMessage = "node missing 'id'"; return result; }
        if (!seenIds.insert(n.id).second) { result.errorMessage = "duplicate node id '" + n.id + "'"; return result; }
        if (nObj->hasProperty("inputs") && nObj->getProperty("inputs").isArray()) {
            for (auto& iv : *nObj->getProperty("inputs").getArray()) {
                InputRef ref;
                std::string err;
                if (!ParseInputRef(iv, ref, err)) { result.errorMessage = "node '" + n.id + "': " + err; return result; }
                n.inputs.push_back(ref);
            }
        }
        nodes.push_back(std::move(n));
    }

    std::vector<std::string> order;
    std::string topoErr;
    if (!TopoSort(nodes, order, topoErr)) {
        result.errorMessage = topoErr;
        return result;
    }

    CodegenState st;
    std::string genErr;
    st.err = &genErr;
    if (const auto* component = root->getProperty("component").getDynamicObject()) {
        st.component = true;
        auto names = [component](const char* key, std::map<std::string, int>& into) {
            if (const auto* list = component->getProperty(key).getArray())
                for (int i = 0; i < list->size(); ++i)
                    into[list->getReference(i).toString().toStdString()] = i;
        };
        names("pins", st.pinIndex);
        names("parameters", st.paramIndex);
        names("state", st.stateIndex);
    }
    for (auto& p : params) st.paramType[p.name] = p.type;
    std::map<std::string, NodeDef*> byId;
    for (auto& n : nodes) byId[n.id] = &n;

    // Each node's line in the body (and, debugging, its value recorded on it).
    std::vector<std::pair<std::string, int>> nodeBodyLines;
    for (auto& id : order) {
        const int bodyLine = CountLines(st.body.str());
        if (!EmitNode(st, *byId[id])) {
            result.errorMessage = genErr;
            return result;
        }
        nodeBodyLines.push_back({ id, bodyLine });
        if (instrument && st.resultType.count(id)) {
            const auto record = DebugRecord((int)debug.slots.size(), st.resultType[id], id);
            if (!record.empty()) {
                AddSlot(debug, id, st.resultType[id], { id });
                auto text = st.body.str();
                text.pop_back(); // the let's newline
                st.body.str(text + " " + record + "\n");
                st.body.seekp(0, std::ios_base::end);
            }
        }
    }

    // A component's program acts through its pins and state; compute()
    // returns 0.0 and the "output" is not used.
    if (st.component)
        outputId.clear();
    if (!st.component && !st.resultType.count(outputId)) {
        result.errorMessage = "output node '" + outputId + "' does not exist or produces no value (e.g. a 'print' node can't be the output)";
        return result;
    }
    std::string returnType = st.component ? std::string("f64") : st.resultType[outputId];
    const std::string returnValue = st.component ? std::string("0.0") : outputId;

    std::ostringstream src;
    src << st.externs.str();
    if (!st.externs.str().empty()) src << "\n";
    const int bodyStart = CountLines(src.str()) + 2; // the signature is the line before
    src << "pub fn " << functionName << "(";
    for (size_t i = 0; i < params.size(); ++i) { if (i) src << ", "; src << params[i].name << ": " << params[i].type; }
    src << ") -> " << returnType << " = {";
    int fn = -1;
    if (instrument) {
        fn = AddFunction(debug, functionName, "", bodyStart - 1);
        src << " " << DebugEnter(fn);
        for (const auto& p : params) {
            const auto record = DebugRecord((int)debug.slots.size(), p.type, p.name);
            if (record.empty()) continue;
            AddSlot(debug, p.name, p.type, {});
            src << " " << record;
        }
    }
    src << "\n";
    src << st.body.str();
    src << "    " << (instrument ? DebugLeave(fn) + " " : std::string()) << returnValue << "\n";
    src << "}\n";
    for (const auto& [id, bodyLine] : nodeBodyLines)
        sourceMap.push_back({ id, "", bodyStart + bodyLine, 5 });
    if (instrument)
        FinishFunctionRanges(debug, CountLines(src.str()));

    std::string validateErr;
    if (!ValidateCompiles(src.str(), validateErr, instrument)) {
        result.errorMessage = "generated source does not compile: " + validateErr + "\n--- generated source ---\n" + src.str();
        return result;
    }

    result.ok = true;
    result.source = src.str();
    return result;
}

namespace {

std::string JsonString(const juce::var& obj, const char* name, const std::string& fallback = {}) {
    if (!obj.isObject()) return fallback;
    auto* dyn = obj.getDynamicObject();
    if (dyn == nullptr || !dyn->hasProperty(name)) return fallback;
    return dyn->getProperty(name).toString().toStdString();
}

std::string JsonString(const juce::DynamicObject* obj, const char* name, const std::string& fallback = {}) {
    if (obj == nullptr || !obj->hasProperty(name)) return fallback;
    return obj->getProperty(name).toString().toStdString();
}

std::string SanitizeIdentifier(std::string text, const std::string& fallback) {
    std::string out;
    for (char c : text) {
        if (std::isalnum((unsigned char)c) || c == '_') out.push_back(c);
        else if (c == '-' || c == ' ' || c == '.') out.push_back('_');
    }
    if (out.empty()) out = fallback;
    if (!std::isalpha((unsigned char)out.front()) && out.front() != '_')
        out = "_" + out;
    return out;
}

std::string EscapeFrustString(const juce::String& s) {
    return ("\"" + s.replace("\\", "\\\\").replace("\"", "\\\"")).toStdString() + "\"";
}

std::string NormalizePrintValueType(const std::string& type) {
    if (type == "String") return "string";
    if (type == "bool" || type == "i64" || type == "f64" || type == "string") return type;
    return "string";
}

struct ExecValue {
    std::string expression;
    std::string type;
};

struct StateMachineState {
    std::string id;
    std::string name;
    bool initial = false;
    bool terminal = false;
};

struct StateMachineEvent {
    std::string id;
    std::string name;
};

struct StateMachineTransition {
    std::string id;
    std::string from;
    std::string to;
    std::string event;
    std::string guard;
    std::string action;
};

bool ResolveExecutableValue(const std::map<std::string, juce::var>& nodesById,
                            const juce::var& ref,
                            ExecValue& value,
                            std::string& err) {
    if (!ref.isObject()) {
        err = "print value input must be an object reference";
        return false;
    }

    auto* refObj = ref.getDynamicObject();
    const auto sourceId = JsonString(refObj, "ref");
    if (sourceId.empty()) {
        err = "print value input must reference a node";
        return false;
    }

    auto found = nodesById.find(sourceId);
    if (found == nodesById.end()) {
        err = "print value references unknown node '" + sourceId + "'";
        return false;
    }

    auto* node = found->second.getDynamicObject();
    const auto type = JsonString(node, "type");
    if (type == "literal_string") {
        value.expression = EscapeFrustString(node->getProperty("text").toString());
        value.type = "string";
        return true;
    }
    if (type == "literal_i64") {
        value.expression = std::to_string((int64_t)node->getProperty("value"));
        value.type = "i64";
        return true;
    }
    if (type == "literal_f64") {
        value.expression = std::to_string((double)node->getProperty("value"));
        value.type = "f64";
        return true;
    }
    if (type == "const_bool" || type == "literal_bool") {
        value.expression = (bool)node->getProperty("value") ? "true" : "false";
        value.type = "bool";
        return true;
    }

    err = "executable print currently supports literal string/i64/f64/bool values; node '" + sourceId
        + "' is type '" + type + "'";
    return false;
}

bool CompileExecutablePod(const juce::var& parsed, SchematicCompileResult& result, bool instrument) {
    auto* root = parsed.getDynamicObject();
    const auto diagramName = JsonString(root, "name", JsonString(root, "functionName", "node_program"));
    result.packageName = SanitizeIdentifier(diagramName, "node_program");
    result.namespaceName = JsonString(root, "namespace", result.packageName);
    result.artifactKind = ArtifactKind::FrustExecutablePod;
    result.entryFile = "src/main.fr";

    if (!root->hasProperty("nodes") || !root->getProperty("nodes").isArray()) {
        result.errorMessage = "executable schematic needs a nodes array";
        return false;
    }

    std::map<std::string, juce::var> nodesById;
    for (auto& nodeVar : *root->getProperty("nodes").getArray()) {
        if (!nodeVar.isObject()) {
            result.errorMessage = "each node must be an object";
            return false;
        }
        auto* node = nodeVar.getDynamicObject();
        const auto id = JsonString(node, "id");
        if (id.empty()) {
            result.errorMessage = "node missing id";
            return false;
        }
        nodesById[id] = nodeVar;
    }

    std::ostringstream body;
    std::set<std::string> printTypes;
    std::vector<std::string> printedNodes;
    std::vector<std::string> valueNodes; // the node each Print prints
    bool emittedAction = false;
    for (auto& nodeVar : *root->getProperty("nodes").getArray()) {
        auto* node = nodeVar.getDynamicObject();
        const auto type = JsonString(node, "type");
        if (type != "print") continue;

        auto inputs = node->getProperty("inputs");
        if (!inputs.isArray() || inputs.getArray()->size() < 1) {
            result.errorMessage = "print node '" + JsonString(node, "id") + "' needs a value input";
            return false;
        }

        // Rich execution schematics use input 0 for exec and input 1 for
        // data. Older pure graph print nodes use input 0 for the value.
        const int valueIndex = inputs.getArray()->size() > 1 ? 1 : 0;
        ExecValue value;
        std::string err;
        if (!ResolveExecutableValue(nodesById, inputs.getArray()->getReference(valueIndex), value, err)) {
            result.errorMessage = "print node '" + JsonString(node, "id") + "': " + err;
            return false;
        }

        const auto valueType = NormalizePrintValueType(value.type);
        if (valueType != "string" && valueType != "i64" && valueType != "f64" && valueType != "bool") {
            result.errorMessage = "print node '" + JsonString(node, "id") + "' has unsupported value type '" + valueType + "'";
            return false;
        }
        body << "    ";
        if (instrument) {
            // The value the Print node prints, recorded for its node and the
            // node it comes from.
            const auto printId = JsonString(node, "id");
            const auto record = DebugRecord((int)result.debug.slots.size(), valueType, value.expression);
            if (!record.empty()) {
                std::vector<std::string> ids { printId };
                const auto source = JsonString(inputs.getArray()->getReference(valueIndex), "ref");
                if (!source.empty() && source != printId) ids.push_back(source);
                AddSlot(result.debug, source.empty() ? printId : source, valueType, ids);
                body << record << " ";
            }
        }
        body << PrintStatement(valueType, value.expression) << "\n";
        printTypes.insert(valueType);
        printedNodes.push_back(JsonString(node, "id"));
        emittedAction = true;
    }

    if (!emittedAction) {
        result.errorMessage = "executable schematic has no runnable action nodes yet";
        return false;
    }

    std::ostringstream src;
    const auto externs = PrintExterns(printTypes);
    src << externs << "\n";
    const int mainFn = instrument ? AddFunction(result.debug, "main", "", CountLines(src.str()) + 1) : -1;
    src << "fn main() -> i64 = {" << (instrument ? " " + DebugEnter(mainFn) : std::string()) << "\n";
    src << body.str();
    src << "    " << (instrument ? DebugLeave(mainFn) + " " : std::string()) << "0\n";
    src << "}\n";
    // Source map: each Print node's line in main().
    int line = (int)std::count(externs.begin(), externs.end(), '\n') + 3;
    for (const auto& id : printedNodes)
        result.sourceMap.push_back({ id, result.entryFile, line++, 5 });
    if (instrument)
        FinishFunctionRanges(result.debug, CountLines(src.str()));

    result.source = src.str();
    result.files.push_back({ result.entryFile, result.source });

    std::ostringstream frate;
    frate << "{\n"
          << "  \"name\": \"" << result.packageName << "\",\n"
          << "  \"version\": \"0.1.0\",\n"
          << "  \"type\": \"bin\",\n"
          << "  \"description\": \"Generated from a node schematic. The schematic JSON is authoritative.\",\n"
          << "  \"exports\": [],\n"
          << "  \"dependencies\": []\n"
          << "}\n";
    result.frateJson = frate.str();
    result.files.push_back({ "frate.json", result.frateJson });

    std::string validateErr;
    if (!ValidateCompiles(result.source, validateErr, instrument)) {
        result.errorMessage = "generated executable source does not compile: " + validateErr
            + "\n--- generated source ---\n" + result.source;
        return false;
    }

    result.ok = true;
    return true;
}

std::string StateFnName(const std::string& id) {
    return "state_" + SanitizeIdentifier(id, "unnamed");
}

std::string EventFnName(const std::string& id) {
    return "event_" + SanitizeIdentifier(id, "unnamed");
}

bool ParseStateMachine(const juce::var& parsed,
                       std::vector<StateMachineState>& states,
                       std::vector<StateMachineEvent>& events,
                       std::vector<StateMachineTransition>& transitions,
                       std::string& initialState,
                       std::string& err) {
    auto* root = parsed.getDynamicObject();
    if (root == nullptr) {
        err = "state-machine schematic root must be an object";
        return false;
    }

    auto statesVar = root->getProperty("states");
    if (!statesVar.isArray()) {
        err = "state-machine schematic needs a states array";
        return false;
    }

    std::set<std::string> stateIds;
    for (auto& stateVar : *statesVar.getArray()) {
        if (!stateVar.isObject()) {
            err = "each state must be an object";
            return false;
        }
        auto* stateObj = stateVar.getDynamicObject();
        StateMachineState state;
        state.id = JsonString(stateObj, "id");
        state.name = JsonString(stateObj, "name", state.id);
        state.initial = (bool)stateObj->getProperty("initial");
        state.terminal = (bool)stateObj->getProperty("terminal");
        if (state.id.empty()) {
            err = "state missing id";
            return false;
        }
        if (!stateIds.insert(state.id).second) {
            err = "duplicate state id '" + state.id + "'";
            return false;
        }
        if (state.initial) {
            if (!initialState.empty()) {
                err = "state machine can only have one initial state";
                return false;
            }
            initialState = state.id;
        }
        states.push_back(std::move(state));
    }

    if (states.empty()) {
        err = "state machine needs at least one state";
        return false;
    }
    if (initialState.empty()) {
        err = "state machine needs one initial state";
        return false;
    }

    auto eventsVar = root->getProperty("events");
    std::set<std::string> eventIds;
    if (eventsVar.isArray()) {
        for (auto& eventVar : *eventsVar.getArray()) {
            if (!eventVar.isObject()) {
                err = "each event must be an object";
                return false;
            }
            auto* eventObj = eventVar.getDynamicObject();
            StateMachineEvent event;
            event.id = JsonString(eventObj, "id");
            event.name = JsonString(eventObj, "name", event.id);
            if (event.id.empty()) {
                err = "event missing id";
                return false;
            }
            if (eventIds.insert(event.id).second)
                events.push_back(std::move(event));
        }
    }

    auto transitionsVar = root->getProperty("transitions");
    if (transitionsVar.isArray()) {
        std::set<std::string> transitionIds;
        for (auto& transitionVar : *transitionsVar.getArray()) {
            if (!transitionVar.isObject()) {
                err = "each transition must be an object";
                return false;
            }
            auto* transitionObj = transitionVar.getDynamicObject();
            StateMachineTransition transition;
            transition.id = JsonString(transitionObj, "id");
            transition.from = JsonString(transitionObj, "from");
            transition.to = JsonString(transitionObj, "to");
            transition.event = JsonString(transitionObj, "event");
            transition.guard = JsonString(transitionObj, "guard");
            transition.action = JsonString(transitionObj, "action");
            if (transition.id.empty()) {
                err = "transition missing id";
                return false;
            }
            if (!transitionIds.insert(transition.id).second) {
                err = "duplicate transition id '" + transition.id + "'";
                return false;
            }
            if (!stateIds.count(transition.from)) {
                err = "transition '" + transition.id + "' references unknown source state '" + transition.from + "'";
                return false;
            }
            if (!stateIds.count(transition.to)) {
                err = "transition '" + transition.id + "' references unknown destination state '" + transition.to + "'";
                return false;
            }
            if (transition.event.empty()) {
                err = "transition '" + transition.id + "' needs an event";
                return false;
            }
            if (eventIds.insert(transition.event).second)
                events.push_back({ transition.event, transition.event });
            transitions.push_back(std::move(transition));
        }
    }

    return true;
}

bool CompileStateMachinePod(const juce::var& parsed, SchematicCompileResult& result, bool instrument) {
    auto* root = parsed.getDynamicObject();
    const auto diagramName = JsonString(root, "name", "state_machine");
    auto targetOptions = root->getProperty("targetOptions");
    auto frustOptions = targetOptions.getProperty("frust", {});
    const auto projectType = JsonString(frustOptions, "projectType", "lib");
    result.packageName = SanitizeIdentifier(diagramName, "state_machine");
    result.namespaceName = JsonString(root, "namespace", result.packageName);
    result.artifactKind = projectType == "bin" ? ArtifactKind::FrustExecutablePod : ArtifactKind::FrustLibraryPod;
    result.entryFile = projectType == "bin" ? "src/main.fr" : "src/lib.fr";

    std::vector<StateMachineState> states;
    std::vector<StateMachineEvent> events;
    std::vector<StateMachineTransition> transitions;
    std::string initialState;
    std::string parseErr;
    if (!ParseStateMachine(parsed, states, events, transitions, initialState, parseErr)) {
        result.errorMessage = parseErr;
        return false;
    }

    // The source line by line, so every state, event and transition node has
    // its line (the source map) and, debugging, its calls on that line.
    std::vector<std::string> out;
    auto emit = [&out](const std::string& text) { out.push_back(text); return (int)out.size(); };
    auto mapNode = [&result](const std::string& id, int line) { result.sourceMap.push_back({ id, result.entryFile, line, 1 }); };
    std::vector<std::string> stateNames, eventNames;
    for (const auto& state : states) stateNames.push_back(state.name);
    for (const auto& event : events) eventNames.push_back(event.name);
    // A value recorded for the debugger on the line it is computed on.
    auto record = [&](const std::string& name, const std::vector<std::string>& valueNames) {
        return instrument ? " " + DebugRecord(AddSlot(result.debug, name, "i64", {}, valueNames), "i64", name) : std::string();
    };
    auto enter = [&](const std::string& fnName, const std::string& nodeId, int& fn) {
        if (!instrument) return std::string();
        fn = AddFunction(result.debug, fnName, nodeId, (int)out.size() + 1); // the line about to be emitted
        return " " + DebugEnter(fn);
    };
    auto leave = [&](int fn) { return instrument ? DebugLeave(fn) + " " : std::string(); };

    if (projectType == "bin") {
        std::istringstream externs(PrintExterns({ "string" }));
        for (std::string line; std::getline(externs, line);) emit(line);
        emit("");
    }
    emit("// Generated from state-machine schematic '" + diagramName + "'.");
    emit("// The schematic JSON remains authoritative; this is a lowerable Frust artifact.");
    emit("");

    auto constantFn = [&](const std::string& name, size_t value, const std::string& nodeId) {
        int fn = -1;
        const auto entered = enter(name, nodeId, fn);
        return emit("pub fn " + name + "() -> i64 = {" + entered + " " + leave(fn) + std::to_string(value) + " }");
    };
    for (size_t i = 0; i < states.size(); ++i)
        mapNode(states[i].id, constantFn(StateFnName(states[i].id), i, states[i].id));
    emit("");
    for (size_t i = 0; i < events.size(); ++i)
        mapNode(events[i].id, constantFn(EventFnName(events[i].id), i, events[i].id));
    emit("");

    int fn = -1;
    emit("pub fn initial_state() -> i64 = {" + enter("initial_state", "", fn));
    if (instrument)
        emit("    let initial: i64 = " + StateFnName(initialState) + "();" + record("initial", stateNames) + " " + leave(fn) + "initial");
    else
        emit("    " + StateFnName(initialState) + "()");
    emit("}");
    emit("");

    emit("pub fn state_name(state: i64) -> String = {" + enter("state_name", "", fn) + record("state", stateNames));
    for (size_t i = 0; i < states.size(); ++i)
        emit(std::string("    ") + (i == 0 && instrument ? "let name: String = " : "") + "if (state == " + StateFnName(states[i].id) + "()) { "
             + EscapeFrustString(juce::String(states[i].name)) + " } else {");
    emit("    \"Unknown\"");
    for (size_t i = 0; i < states.size(); ++i)
        emit(i + 1 < states.size() || !instrument ? "    }" : "    }; " + leave(fn) + "name");
    emit("}");
    emit("");

    emit("pub fn step(current_state: i64, event: i64) -> i64 = {" + enter("step", "", fn)
         + record("current_state", stateNames) + record("event", eventNames));
    const auto nextRecord = record("next_state", stateNames);
    emit("    let mut next_state: i64 = current_state;" + nextRecord);
    for (const auto& transition : transitions) {
        emit("    if (current_state == " + StateFnName(transition.from) + "()) {");
        emit("        if (event == " + EventFnName(transition.event) + "()) {");
        if (!transition.guard.empty())
            emit("            // guard: " + transition.guard);
        if (!transition.action.empty())
            emit("            // action: " + transition.action);
        mapNode(transition.id, emit("            next_state = " + StateFnName(transition.to) + "();" + nextRecord));
        emit("        } else { next_state = next_state; };");
        emit("    } else { next_state = next_state; };");
    }
    emit("    " + leave(fn) + "next_state");
    emit("}");

    if (projectType == "bin") {
        emit("");
        emit("fn main() -> i64 = {" + enter("main", "", fn));
        emit("    let state: i64 = initial_state();" + record("state", stateNames));
        emit("    " + PrintStatement("string", "\"Initial state:\""));
        emit("    " + PrintStatement("string", "state_name(state)"));
        if (!transitions.empty()) {
            emit("    let after: i64 = step(state, " + EventFnName(transitions.front().event) + "());" + record("after", stateNames));
            emit("    " + PrintStatement("string", "\"After first transition event:\""));
            emit("    " + PrintStatement("string", "state_name(after)"));
        }
        emit("    " + leave(fn) + "0");
        emit("}");
    }

    std::ostringstream src;
    for (const auto& line : out)
        src << line << "\n";
    if (instrument)
        FinishFunctionRanges(result.debug, (int)out.size());
    result.source = src.str();
    result.files.push_back({ result.entryFile, result.source });

    std::ostringstream frate;
    frate << "{\n"
          << "  \"name\": \"" << result.packageName << "\",\n"
          << "  \"version\": \"0.1.0\",\n"
          << "  \"type\": \"" << (projectType == "bin" ? "bin" : "lib") << "\",\n"
          << "  \"description\": \"Generated from a state-machine node schematic. The schematic JSON is authoritative.\",\n"
          << "  \"exports\": [],\n"
          << "  \"dependencies\": []\n"
          << "}\n";
    result.frateJson = frate.str();
    result.files.push_back({ "frate.json", result.frateJson });

    result.diagnostics.push_back("state-machine lowering emitted integer-backed states/events; enum-backed lowering is the next compatibility step");

    std::string validateErr;
    if (!ValidateCompiles(result.source, validateErr, instrument)) {
        result.errorMessage = "generated state-machine source does not compile: " + validateErr
            + "\n--- generated source ---\n" + result.source;
        return false;
    }

    result.ok = true;
    return true;
}

} // namespace

std::string DebugPrelude() {
    return "extern fn djehuti_dbg_enter(function: i64) -> i64;\n"
           "extern fn djehuti_dbg_leave(function: i64) -> i64;\n"
           "extern fn djehuti_dbg_i64(slot: i64, value: i64) -> i64;\n"
           "extern fn djehuti_dbg_f64(slot: i64, value: f64) -> i64;\n"
           "extern fn djehuti_dbg_bool(slot: i64, value: i64) -> i64;\n"
           "extern fn djehuti_dbg_str(slot: i64, value: String) -> i64;\n";
}

SchematicCompileResult CompileSchematic(const std::string& schematicJson) {
    return CompileSchematic(schematicJson, {});
}

SchematicCompileResult CompileSchematic(const std::string& schematicJson, const CompileOptions& options) {
    SchematicCompileResult result;
    const bool instrument = options.debugInstrumentation;

    juce::var parsed;
    auto parseResult = juce::JSON::parse(juce::String(schematicJson), parsed);
    if (parseResult.failed() || !parsed.isObject()) {
        result.errorMessage = "invalid schematic JSON: " + parseResult.getErrorMessage().toStdString();
        return result;
    }

    auto* root = parsed.getDynamicObject();
    const auto diagramType = JsonString(root, "diagramType", "node_graph");
    auto targetOptions = root->getProperty("targetOptions");
    auto frustOptions = targetOptions.getProperty("frust", {});
    const auto projectType = JsonString(frustOptions, "projectType", "function");

    if (diagramType == "state_machine") {
        CompileStateMachinePod(parsed, result, instrument);
        return result;
    }

    if (projectType == "bin") {
        CompileExecutablePod(parsed, result, instrument);
        return result;
    }

    const auto functionResult = CompileGraphToSource(schematicJson, options, result.sourceMap, result.debug);
    if (!functionResult.ok) {
        result.errorMessage = functionResult.errorMessage;
        return result;
    }

    result.ok = true;
    result.artifactKind = projectType == "lib" ? ArtifactKind::FrustLibraryPod : ArtifactKind::FunctionSource;
    result.packageName = SanitizeIdentifier(JsonString(root, "name", "node_graph"), "node_graph");
    result.namespaceName = JsonString(root, "namespace", result.packageName);
    result.entryFile = projectType == "lib" ? "src/lib.fr" : "";
    result.source = functionResult.source;
    if (result.artifactKind == ArtifactKind::FrustLibraryPod) {
        result.files.push_back({ "src/lib.fr", result.source });
        result.frateJson = "{\n"
            "  \"name\": \"" + result.packageName + "\",\n"
            "  \"version\": \"0.1.0\",\n"
            "  \"type\": \"lib\",\n"
            "  \"description\": \"Generated from a node schematic. The schematic JSON is authoritative.\",\n"
            "  \"exports\": [],\n"
            "  \"dependencies\": []\n"
            "}\n";
        result.files.push_back({ "frate.json", result.frateJson });
    }
    return result;
}

} // namespace node_compiler
