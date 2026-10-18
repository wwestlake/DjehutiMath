// node_compiler - graph JSON -> real .frust source text.
//
// Nodes are DATA, never executed directly - a graph description compiles
// down to genuine .frust source (parse -> AST -> LLVM IR through the
// normal frust_lang pipeline, loadable via frust_plugin_host exactly
// like a hand-written plugin). There is no separate graph-interpreter
// runtime. The visual editor that produces this JSON is a separate
// concern (Creation Suite, not this repo) - this library's job starts
// at "graph JSON in" and ends at "compilable .frust source out."
//
// -----------------------------------------------------------------------
// Graph JSON schema (v1)
// -----------------------------------------------------------------------
//
//   {
//     "functionName": "compute",
//     "params": [ {"name": "x", "type": "i64"} ],
//     "output": "result",
//     "nodes": [
//       {"id": "ten", "type": "literal_i64", "value": 10},
//       {"id": "sum", "type": "add", "inputs": [{"param": "x"}, {"ref": "ten"}]},
//       {"id": "result", "type": "mul", "inputs": [{"ref": "sum"}, {"ref": "ten"}]}
//     ]
//   }
//
// Every node has a unique "id" and a "type". Every input reference is
// either `{"ref": "<node id>"}` (another node's single output) or
// `{"param": "<name>"}` (one of the generated function's own
// parameters) - there is no third, inline-literal form: a constant is
// always its own literal_* node, matching how a real node-graph editor
// represents one (a "Number: 10" node you drag in), not a hidden magic
// value.
//
// Node types (v1 - see the session plan for what's deliberately NOT in
// v1: loops, node-defined structs, sub-graph composition):
//   literal_i64/literal_f64/literal_bool/literal_string - "value", no inputs
//   add/sub/mul/div/mod       - 2 inputs, numeric (matches Frust's own
//                               operators, including its automatic
//                               int/float promotion - see Codegen.h's
//                               compileBinary)
//   eq/neq/lt/gt/le/ge        - 2 inputs -> bool
//   if                        - 3 inputs: [cond, then, else]
//   call                      - N inputs (args) + "function" (name),
//                               "paramTypes" (array of type names),
//                               "returnType". Calls an EXTERN C-ABI
//                               function (host-registered via
//                               frust_plugin_register_host_function, or
//                               process-exported) - the compiler emits
//                               the needed `extern fn` declaration
//                               itself, once per unique function name.
//                               Not for calling other Frust-source
//                               functions (frust_plugin_host is
//                               single-file only - see its README).
//   print                     - 1 input + "valueType" - sugar for
//                               calling frust_print_i64/f64/bool/str
//                               (the same runtime exports console_io.fr
//                               wraps), auto-emitting their externs.
//
// Each node becomes one `let <id>: <type> = <expr>;` binding, in
// topologically-sorted order (a real DAG, not just a tree - a node with
// multiple consumers is bound ONCE and referenced by name from each
// consumer, not re-evaluated per use - required for correctness once a
// node can have a side effect, e.g. `print`, and desirable even for
// pure nodes to avoid duplicated generated code). A cycle in the input
// references is a real compile error, not silently ignored.

#pragma once

// Workbench copy of FrustLang projects/10_node_compiler (1b95d9f), adapted:
// generated source is validated with the app's embedded compiler, core is
// the locally cached version, and the standalone library's C ABI is omitted.

#include <stdint.h>

#ifdef __cplusplus

#include <string>
#include <vector>

namespace node_compiler {

struct CompileResult {
    bool ok = false;
    std::string source;       // valid only if ok
    std::string errorMessage; // valid only if !ok
};

enum class ArtifactKind {
    FunctionSource,
    FrustExecutablePod,
    FrustLibraryPod
};

struct GeneratedFile {
    std::string path;    // package-relative path, e.g. src/main.fr
    std::string content;
};

struct RequiredPod {
    std::string name;
    std::string version;
};

struct SourceMapEntry {
    std::string nodeId;
    std::string generatedFile;
    int line = 0;
    int column = 1;
};

// Workbench debugger support. A debug compile emits the same program with
// calls to the Workbench's debugger host functions (DebugPrelude) added on
// the lines they belong to, so line numbers do not move: entering and
// leaving each generated function, and each value a node produces. What the
// debugger shows is what those calls report while the program runs.
struct DebugFunction {
    std::string name;    // generated FRust function
    std::string nodeId;  // the node it belongs to ("" for the program's own)
    int firstLine = 0;   // its lines in the generated program; a tick outside
    int lastLine = 0;    // them is not this call's (e.g. a callee's prologue)
};

struct DebugSlot {
    std::string name;                  // the generated variable
    std::string type;                  // i64, f64, bool or String
    std::vector<std::string> nodeIds;  // nodes whose value this is
    std::vector<std::string> valueNames; // i64 values that name something (state, event): index -> name
};

struct DebugInfo {
    std::vector<DebugFunction> functions; // index = djehuti_dbg_enter argument
    std::vector<DebugSlot> slots;         // index = djehuti_dbg_<type> slot argument
};

struct CompileOptions {
    bool debugInstrumentation = false;
};

// The extern declarations a debug compile's source needs; the run puts them
// ahead of the program (the program's own line numbers do not change).
std::string DebugPrelude();

struct SchematicCompileResult {
    bool ok = false;
    DebugInfo debug;          // with CompileOptions::debugInstrumentation
    ArtifactKind artifactKind = ArtifactKind::FunctionSource;
    std::string packageName;
    std::string namespaceName;
    std::string entryFile;
    std::string source;       // primary source, kept for older callers/UI previews
    std::string frateJson;    // valid for pod artifacts
    std::vector<GeneratedFile> files;
    std::vector<RequiredPod> requiredPods;
    std::vector<SourceMapEntry> sourceMap;
    std::vector<std::string> diagnostics;
    std::string errorMessage; // valid only if !ok
};

// Parses graphJson, topologically sorts the nodes, emits .frust source,
// then VALIDATES that source actually parses and compiles (Workbench
// copy: with the app's embedded FRust compiler, frust_engine::check, pods
// from the local Frate cache) before returning success. A
// syntactically-valid-looking graph that produces source with a real
// type error comes back as ok=false with that error, not silently
// handed to the caller as if it were fine.
CompileResult CompileGraphToSource(const std::string& graphJson);
// The same with a source map (each node's line) and, when asked for, the
// debugger instrumentation.
CompileResult CompileGraphToSource(const std::string& graphJson, const CompileOptions& options,
                                   std::vector<SourceMapEntry>& sourceMap, DebugInfo& debug);

// Compiles the richer schematic/package JSON. This is the real boundary
// for IDE node schematics: it accepts v1 graphs and v2 schematics,
// preserves architectural metadata such as namespace/events/messages/
// execution contexts for diagnostics/source maps, and returns the target
// artifact the build/run layer should materialize.
//
// The older CompileGraphToSource API remains for tests and callers that
// only need a pure function source string.
SchematicCompileResult CompileSchematic(const std::string& schematicJson);
SchematicCompileResult CompileSchematic(const std::string& schematicJson, const CompileOptions& options);

} // namespace node_compiler

#endif
