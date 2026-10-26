#include "DjehutiMath.h"
#include "MathKnowledge.h"
#include "MathSession.h"
#include <cmath>

namespace
{
enum { newFile = 1, openFile, saveFile, saveAsFile, openSource, saveSource, quitApp, agentSettings = 20, login, logout, localHelp, onlineHelp, aboutApp, saveLayout = 40, presetBase = 100, userLayoutBase = 200, viewBase = 500 };
std::unique_ptr<juce::TextEditor> readout()
{
    auto editor = std::make_unique<juce::TextEditor>(); editor->setMultiLine(true); editor->setReadOnly(true); editor->setScrollbarsShown(true);
    editor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff141c26)); editor->setColour(juce::TextEditor::textColourId, juce::Colour(0xffcbd5e1)); editor->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack); editor->setFont(juce::Font("Consolas", 13.0f, juce::Font::plain)); return editor;
}
juce::String result(bool ok, const juce::String& message)
{
    auto* object = new juce::DynamicObject(); object->setProperty("ok", ok); object->setProperty(ok ? "message" : "error", message); return juce::JSON::toString(juce::var(object));
}
juce::File resources(const juce::String& name)
{
    const auto installed = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile(name);
    if (installed.exists()) return installed;
    const auto source = juce::File(DJEHUTIMATH_SOURCE_ROOT);
    return name == "default_layouts.json" ? source.getChildFile("Resources").getChildFile(name) : source.getChildFile(name);
}
}
juce::File DjehutiMath::stateDirectory() const { return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("DjehutiMath"); }
DjehutiMath::DjehutiMath()
{
    juce::PropertiesFile::Options options; options.applicationName = "DjehutiMath"; options.folderName = "DjehutiMath"; options.filenameSuffix = ".settings"; properties.setStorageParameters(options);
    auth.loadFromDisk(); addAndMakeVisible(menu); addAndMakeVisible(avatar);
    dock = std::make_unique<CreationDock::DockManager>(*this); addAndMakeVisible(*dock);
    document->reset(); auto recovery = stateDirectory().getChildFile("session.djmath"); juce::String error;
    if (recovery.existsAsFile() && document->load(recovery, error))
    {
        const auto path = properties.getUserSettings()->getValue("worksheetPath");
        if (juce::File::isAbsolutePath(path)) worksheetFile = juce::File(path);
        document->modified = properties.getUserSettings()->getBoolValue("worksheetModified", false);
    }
    auto sheet = std::make_unique<WorksheetPanel>(document); worksheet = sheet.get(); worksheet->onChanged = [this] { refreshProject(); };
    dock->registerPanel("worksheet", "Worksheet", std::move(sheet), CreationDock::DockTargetZone::CenterTab);
    auto language = std::make_unique<FrustPanel>(); frust = language.get(); dock->registerPanel("frust", "FRust", std::move(language), CreationDock::DockTargetZone::CenterTab);
    // Node programming (the DEWB/FrustIDE node editor), the debugger that steps its programs, and the DSP workbench.
    auto nodeEditor = std::make_unique<NodeDesignerPanel>(); nodes = nodeEditor.get();
    nodes->defaultFolder = [this] { auto folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("DjehutiMath").getChildFile("Node Programs"); folder.createDirectory(); return folder; };
    nodes->onRunRequested = [this](const juce::String& script, const juce::String& label) { std::string error; if (!frust->start(script, label, error)) appendLog("Node program " + label + " did not start: " + juce::String(error)); else dock->activatePanel("frust"); };
    nodes->onDebugRequested = [this] { const auto why = startNodeDebugging(); if (why.isNotEmpty()) appendLog("Start Debugging: " + why); else dock->activatePanel("debugger"); };
    nodes->onDebugMarkersChanged = [this] { syncDebugMarkers(); };
    dock->registerPanel("nodes", "Node Designer", std::move(nodeEditor), CreationDock::DockTargetZone::CenterTab);
    FrustDebuggerPanel::Actions debugActions;
    debugActions.startDebugging = [this] { return startNodeDebugging(); };
    debugActions.breakpoints = [this] { std::vector<frust_exec::Breakpoint> list; juce::String ignored; const auto lines = nodes->nodeLines(ignored); for (const auto& m : nodes->breakpointMarkers()) list.push_back({ m.nodeId.toStdString(), lines.count(m.nodeId) != 0 ? lines.at(m.nodeId) : 0, m.enabled, 0 }); return list; };
    debugActions.setBreakpointEnabled = [this](const juce::String& nodeId, bool enabled) { juce::String ignored; nodes->setBreakpoint(nodeId, true, enabled, ignored); };
    debugActions.removeBreakpoint = [this](const juce::String& nodeId) { juce::String ignored; nodes->setBreakpoint(nodeId, false, true, ignored); };
    auto debuggerView = std::make_unique<FrustDebuggerPanel>(std::move(debugActions)); debugger = debuggerView.get();
    dock->registerPanel("debugger", "Debugger", std::move(debuggerView), CreationDock::DockTargetZone::Right);
    auto dspView = std::make_unique<DspPanel>(audio); dsp = dspView.get(); dsp->onLog = [this](const juce::String& line) { appendLog(line); };
    dock->registerPanel("dsp", "DSP Effects", std::move(dspView), CreationDock::DockTargetZone::CenterTab);
    executionListener = frust_exec::Executor::instance().addListener([this](const frust_exec::Snapshot&) { executionChanged(); });
    localApi = std::make_unique<LocalAgentApi>();
    localApi->onToolCatalog = [] {
        juce::Array<juce::var> list;
        for (const auto& tool : math_knowledge::tools())
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("name", juce::String(tool.name)); o->setProperty("description", juce::String(tool.description)); o->setProperty("parameters", juce::JSON::parse(juce::String(tool.parametersJson)));
            list.add(juce::var(o));
        }
        return juce::var(list);
    };
    localApi->onToolCall = [this](const juce::String& name, const juce::var& arguments, LocalAgentApi::Completion completion) {
        executeCommand(name.toStdString(), juce::JSON::toString(arguments, true).toStdString(), nullptr, [completion](juce::String text) {
            const auto parsed = juce::JSON::parse(text);
            completion(!(parsed.isObject() && parsed.hasProperty("ok") && !(bool) parsed.getProperty("ok", true)), text);
        });
    };
    if (!localApi->start()) appendLog("The local agent API could not start; external agents cannot reach the tools.");
    auto projectView = readout(); project = projectView.get(); dock->registerPanel("project", "Project", std::move(projectView), CreationDock::DockTargetZone::Left);
    auto variableView = readout(); variables = variableView.get(); dock->registerPanel("variables", "Variables", std::move(variableView), CreationDock::DockTargetZone::Right);
    auto log = readout(); diagnostics = log.get(); diagnostics->setText("Djehuti Math foundation\nFRust worker and scalar workspace ready.\nSymbolic algebra, matrix backends and plotting are planned.\n", false); dock->registerPanel("diagnostics", "Diagnostics", std::move(log), CreationDock::DockTargetZone::Bottom);
    auto agent = std::make_unique<DjehutiAgentPanel>(properties, [safe = juce::Component::SafePointer<DjehutiMath>(this)](const std::string& name, const std::string& arguments, std::shared_ptr<DjehutiAgentRequestState> state) -> std::string {
        // All UI and agent mutations go through the message-thread dispatcher.
        if (safe == nullptr || state == nullptr || state->cancelled || !state->alive) return "{\"ok\":false,\"error\":\"Request cancelled\"}";
        if (juce::MessageManager::getInstance()->isThisTheMessageThread()) return safe->dispatch(name, arguments, state).toStdString();
        struct Reply { juce::WaitableEvent ready; juce::String value; std::atomic<bool> expired {false}; };
        auto reply = std::make_shared<Reply>();
        juce::MessageManager::callAsync([safe, name, arguments, state, reply] {
            if (reply->expired || safe == nullptr || state->cancelled || !state->alive) { reply->value = "{\"ok\":false,\"error\":\"Request cancelled\"}"; reply->ready.signal(); return; }
            // A program run or compile answers when it settles; every other command answers at once.
            safe->executeCommand(name, arguments, state, [reply](juce::String text) { reply->value = std::move(text); reply->ready.signal(); });
        });
        if (!reply->ready.wait(130000)) { reply->expired = true; return "{\"ok\":false,\"error\":\"UI command timed out; inspect the workspace before retrying\"}"; }
        return reply->value.toStdString();
    });
    dock->registerPanel("agent", "BYOK Agent", std::move(agent), CreationDock::DockTargetZone::Right);
    const auto saved = juce::JSON::parse(stateDirectory().getChildFile("layout.json").loadFileAsString());
    if (!applyLayout(saved)) applyLayout(layoutCatalog()["presets"][0]["layout"]);
    lastLayout = persistedLayout = juce::JSON::toString(dock->captureLayout(), true); refreshProject(); startTimer(1000);
}
DjehutiMath::~DjehutiMath()
{
    stopTimer(); if (localApi != nullptr) localApi->stop(); frust_exec::Executor::instance().removeListener(executionListener); auth.cancelLogin(); stateDirectory().createDirectory(); dock->saveLayoutToFile(stateDirectory().getChildFile("layout.json"));
    juce::String error; const bool dirty = document->modified; document->save(stateDirectory().getChildFile("session.djmath"), error); document->modified = dirty;
    properties.getUserSettings()->setValue("worksheetPath", worksheetFile.getFullPathName());
    properties.getUserSettings()->setValue("worksheetModified", dirty);
    properties.saveIfNeeded(); dock.reset();
}
void DjehutiMath::executeCommand(const std::string& name, const std::string& argumentsJson, std::shared_ptr<DjehutiAgentRequestState> state, std::function<void(juce::String)> done)
{
    if (state == nullptr) state = std::make_shared<DjehutiAgentRequestState>();
    // A program run or compile answers when it settles; every other command answers at once.
    if (!dispatchAsync(name, argumentsJson, done)) done(dispatch(name, argumentsJson, state));
}
void DjehutiMath::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff101720)); }
void DjehutiMath::resized() { auto area = getLocalBounds(); auto header = area.removeFromTop(42); menu.setBounds(header.removeFromLeft(650)); avatar.setBounds(header.removeFromRight(avatar.getPreferredWidth()).reduced(4, 5)); dock->setBounds(area); }
void DjehutiMath::refreshProject()
{
    juce::String text = document->title + (document->modified ? " *" : "") + "\n\n" + (worksheetFile == juce::File() ? "Unsaved worksheet" : worksheetFile.getFileName()) + "\n\nCELLS\n";
    for (size_t i = 0; i < document->cells.size(); ++i) text += juce::String((int)i + 1) + "  " + (document->cells[i].kind == "frust" ? "FRust" : "Notes") + "\n";
    text += "\nFRust programs share the scalar workspace.\nRuntime results are regenerated, not saved as authoritative calculations."; project->setText(text, false);
}
void DjehutiMath::timerCallback()
{
    const auto values = juce::JSON::toString(MathSession::instance().inspect(), false);
    if (values != lastVariables) { lastVariables = values; variables->setText(values == "{}" ? "No variables yet.\n\nRun the example worksheet cell.\n\nFRust API:\nmath_set_scalar(name, value)\nmath_get_scalar(name)" : values, false); }
    auto layout = juce::JSON::toString(dock->captureLayout(), true); const auto now = juce::Time::currentTimeMillis();
    if (layout != lastLayout) { lastLayout = layout; changedAt = now; }
    else if (layout != persistedLayout && now - changedAt >= 1500) { dock->saveLayoutToFile(stateDirectory().getChildFile("layout.json")); persistedLayout = layout; }
}
juce::var DjehutiMath::layoutCatalog() const { return juce::JSON::parse(resources("default_layouts.json").loadFileAsString()); }
bool DjehutiMath::applyLayout(juce::var layout)
{
    if (!layout.isObject() || !layout["zones"].isObject()) return false;
    // DockManager moves listed panels; it does not hide omitted ones.
    // Hide first so startup and presets never open every registered panel.
    bool hasPanels = false;
    for (const auto& zone : {"left", "center", "right", "bottom"}) if (auto* ids = layout["zones"][zone]["panels"].getArray()) for (const auto& id : *ids) if (dock->findPanelById(id.toString()) != nullptr) hasPanels = true;
    if (auto* floats = layout["floating"].getArray()) for (const auto& entry : *floats) if (dock->findPanelById(entry["id"].toString()) != nullptr) hasPanels = true;
    if (!hasPanels) return false;
    if (auto* floats = layout["floating"].getArray()) for (auto& entry : *floats) if (auto* object = entry.getDynamicObject())
    {
        auto bounds = juce::Rectangle<int>((int)entry["x"], (int)entry["y"], juce::jlimit(250, 3000, (int)entry["w"]), juce::jlimit(180, 2000, (int)entry["h"]));
        if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(bounds)) { bounds = bounds.constrainedWithin(display->userArea); object->setProperty("x", bounds.getX()); object->setProperty("y", bounds.getY()); object->setProperty("w", bounds.getWidth()); object->setProperty("h", bounds.getHeight()); }
    }
    for (const auto& id : dock->getRegisteredPanelIds()) dock->hidePanel(id);
    dock->applyLayout(layout); return true;
}
juce::StringArray DjehutiMath::getMenuBarNames() { return {"File", "View", "Layout", "Settings", "Account", "Help"}; }
juce::PopupMenu DjehutiMath::getMenuForIndex(int index, const juce::String&)
{
    juce::PopupMenu out;
    if (index == 0) { out.addItem(newFile, "New Worksheet"); out.addItem(openFile, juce::String::fromUTF8("Open Worksheet…")); out.addItem(saveFile, "Save Worksheet"); out.addItem(saveAsFile, juce::String::fromUTF8("Save Worksheet As…")); out.addSeparator(); out.addItem(openSource, juce::String::fromUTF8("Open FRust Source…")); out.addItem(saveSource, juce::String::fromUTF8("Save FRust Source As…")); out.addSeparator(); out.addItem(quitApp, "Exit"); }
    if (index == 1) { int i = viewBase; for (const auto& id : dock->getRegisteredPanelIds()) out.addItem(i++, dock->getPanelTitle(id), true, dock->isPanelOpen(id)); }
    if (index == 2)
    {
        const auto catalog = layoutCatalog();
        if (auto* presets = catalog["presets"].getArray()) { int i = presetBase; for (const auto& preset : *presets) out.addItem(i++, preset["name"].toString()); }
        out.addSeparator(); out.addItem(saveLayout, juce::String::fromUTF8("Save Layout…")); juce::PopupMenu user;
        const auto saved = juce::JSON::parse(stateDirectory().getChildFile("user_layouts.json").loadFileAsString()); if (auto* layouts = saved.getArray()) { int i = userLayoutBase; for (const auto& value : *layouts) user.addItem(i++, value["name"].toString()); }
        if (user.getNumItems() == 0) user.addItem(999, "No saved user layouts", false); out.addSubMenu("User Layouts", user);
    }
    if (index == 3) out.addItem(agentSettings, juce::String::fromUTF8("Agent Settings…"));
    if (index == 4) { out.addItem(login, juce::String::fromUTF8("Sign In…"), !auth.hasValidSession()); out.addItem(logout, "Sign Out", auth.hasValidSession()); }
    if (index == 5) { out.addItem(localHelp, "Getting Started"); out.addItem(onlineHelp, "LagDaemon Website"); out.addItem(aboutApp, "About Djehuti Math"); }
    return out;
}
void DjehutiMath::menuItemSelected(int id, int)
{
    if (id >= viewBase && id < viewBase + (int)dock->getRegisteredPanelIds().size()) { dock->togglePanel(dock->getRegisteredPanelIds()[(size_t)(id - viewBase)]); return; }
    if (id >= presetBase && id < userLayoutBase) { applyLayout(layoutCatalog()["presets"][id - presetBase]["layout"]); return; }
    if (id >= userLayoutBase && id < viewBase) { const auto saved = juce::JSON::parse(stateDirectory().getChildFile("user_layouts.json").loadFileAsString()); applyLayout(saved[id - userLayoutBase]["layout"]); return; }
    switch (id)
    {
        case newFile: ifDiscardAllowed([this] { newWorksheet(); }); break;
        case openFile: ifDiscardAllowed([this] { chooseFile(false); }); break;
        case saveFile: case saveAsFile: if (id == saveAsFile || worksheetFile == juce::File()) chooseFile(true); else { juce::String error; if (!document->save(worksheetFile, error)) diagnostics->setText(error, false); refreshProject(); } break;
        case openSource: case saveSource:
        {
            const bool save = id == saveSource; chooser = std::make_unique<juce::FileChooser>(save ? "Save FRust Source" : "Open FRust Source", juce::File(), "*.frust;*.fr");
            chooser->launchAsync((save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting : juce::FileBrowserComponent::openMode) | juce::FileBrowserComponent::canSelectFiles, [safe = juce::Component::SafePointer<DjehutiMath>(this), save](const auto& fc) { if (safe == nullptr || fc.getResult() == juce::File()) return; if (save) { if (!fc.getResult().replaceWithText(safe->frust->getSource())) safe->diagnostics->setText("Could not save FRust source.", false); } else safe->frust->setSource(fc.getResult().loadFileAsString()); safe->dock->activatePanel("frust"); }); break;
        }
        case quitApp: confirmCloseThen([] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }); break;
        case agentSettings: showAgentSettings(); break;
        case login: auth.beginLogin(); break;
        case logout: auth.clearSession(); break;
        case localHelp: juce::URL(resources("docs").getChildFile("getting-started.html")).launchInDefaultBrowser(); break;
        case onlineHelp: juce::URL("https://lagdaemon.com").launchInDefaultBrowser(); break;
        case aboutApp: juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Djehuti Math 0.1.0", "LagDaemon Software\nEngineering mathematics workbench\n\nFoundation: worksheets, FRust, BYOK agent and LiteSemRAG. Advanced mathematical backends are the next phase."); break;
        case saveLayout: saveUserLayout(); break;
        default: break;
    }
}
void DjehutiMath::newWorksheet() { document->reset(); document->modified = true; worksheetFile = {}; worksheet->rebuild(); refreshProject(); }
void DjehutiMath::ifDiscardAllowed(std::function<void()> action)
{
    if (!document->modified) { action(); return; }
    juce::AlertWindow::showOkCancelBox(juce::AlertWindow::WarningIcon, "Unsaved worksheet", "Continue without saving changes to the worksheet file? Use File > Save Worksheet first to keep changes in your named file.", "Continue", "Cancel", this,
        juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<DjehutiMath>(this), action = std::move(action)](int accepted) { if (safe != nullptr && accepted != 0) action(); }));
}
void DjehutiMath::confirmCloseThen(std::function<void()> action) { ifDiscardAllowed(std::move(action)); }
void DjehutiMath::chooseFile(bool saveAs)
{
    chooser = std::make_unique<juce::FileChooser>(saveAs ? "Save Worksheet" : "Open Worksheet", worksheetFile, "*.djmath");
    chooser->launchAsync((saveAs ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting : juce::FileBrowserComponent::openMode) | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<DjehutiMath>(this), saveAs](const auto& fc) { if (safe == nullptr || fc.getResult() == juce::File()) return; auto file = saveAs ? fc.getResult().withFileExtension("djmath") : fc.getResult(); juce::String error; if (saveAs ? safe->document->save(file, error) : safe->document->load(file, error)) { safe->worksheetFile = file; safe->worksheet->rebuild(); safe->refreshProject(); } else safe->diagnostics->setText(error, false); });
}
void DjehutiMath::showAgentSettings()
{
    juce::DialogWindow::LaunchOptions options; options.dialogTitle = juce::String::fromUTF8("Djehuti Math — Agent Settings"); options.dialogBackgroundColour = juce::Colour(0xff171b20); options.content.setOwned(new DjehutiAgentSettings(properties, {})); options.content->setSize(520, 530); options.escapeKeyTriggersCloseButton = true; options.useNativeTitleBar = true; options.resizable = false; options.launchAsync();
}
void DjehutiMath::saveUserLayout()
{
    auto* alert = new juce::AlertWindow("Save Layout", "Name this workspace layout", juce::AlertWindow::NoIcon); alert->addTextEditor("name", "My workspace"); alert->addButton("Save", 1); alert->addButton("Cancel", 0);
    alert->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<DjehutiMath>(this), alert](int accepted) { if (accepted == 0 || safe == nullptr) return; const auto name = alert->getTextEditorContents("name").trim(); if (name.isEmpty()) return; auto file = safe->stateDirectory().getChildFile("user_layouts.json"); auto saved = juce::JSON::parse(file.loadFileAsString()); juce::Array<juce::var> entries; if (auto* array = saved.getArray()) entries = *array; auto* item = new juce::DynamicObject(); item->setProperty("name", name); item->setProperty("layout", safe->dock->captureLayout()); bool replaced = false; for (auto& value : entries) if (value["name"].toString() == name) { value = juce::var(item); replaced = true; break; } if (!replaced) entries.add(juce::var(item)); safe->stateDirectory().createDirectory(); file.replaceWithText(juce::JSON::toString(entries, false)); }), true);
}
juce::String DjehutiMath::dispatch(const std::string& name, const std::string& arguments, std::shared_ptr<DjehutiAgentRequestState> state)
{
    if (state->cancelled || !state->alive) return result(false, "Request cancelled");
    const auto args = juce::JSON::parse(arguments.empty() ? "{}" : juce::String(arguments));
    if (!args.isObject()) return result(false, "Arguments must be a JSON object.");
    bool handled = true; const auto programming = dispatchProgramming(name, args, handled); if (handled) return programming;
    if (name == "worksheet_inspect") return juce::JSON::toString(document->toJson());
    if (name == "worksheet_save" || name == "worksheet_open")
    {
        const auto path = args["path"].toString();
        if (!juce::File::isAbsolutePath(path) || !path.endsWithIgnoreCase(".djmath")) return result(false, "Use an absolute .djmath file path.");
        if (name == "worksheet_open" && document->modified) return result(false, "Save the modified worksheet before opening another.");
        juce::String error; const juce::File file(path);
        const bool ok = name == "worksheet_save" ? document->save(file, error) : document->load(file, error);
        if (ok) { worksheetFile = file; worksheet->rebuild(); refreshProject(); } return result(ok, ok ? file.getFullPathName() : error);
    }
    if (name == "worksheet_add_cell") { const auto id = worksheet->addCell(args["kind"].toString(), args["source"].toString()); return result(id.isNotEmpty(), id.isNotEmpty() ? id : "Cell kind must be text or frust; at most 200 cells of 100000 characters."); }
    if (name == "worksheet_set_cell") return result(worksheet->setCell(args["id"].toString(), args["source"].toString()), "Cell updated if the id exists and source fits the limit.");
    if (name == "worksheet_run_cell") { juce::String error; const bool ok = worksheet->runCell(args["id"].toString(), error); return result(ok, ok ? "Calculation started; use execution_status for its result." : error); }
    if (name == "workspace_inspect") return juce::JSON::toString(MathSession::instance().inspect());
    if (name == "frust_source") { if (args.hasProperty("source")) frust->setSource(args["source"].toString()); dock->activatePanel("frust"); return result(true, frust->getSource()); }
    if (name == "frust_run") { const auto source = args.hasProperty("source") ? args["source"].toString() : frust->getSource(); std::string error; bool ok = frust->start(source, "Agent FRust", error); return result(ok, ok ? "FRust started; use execution_status for the actual result." : juce::String(error)); }
    if (name == "execution_stop") { std::string error; bool ok = frust_exec::Executor::instance().stop(error); return result(ok, ok ? "Stop requested." : juce::String(error)); }
    if (name == "execution_status")
    {
        const auto snap = frust_exec::Executor::instance().snapshot();
        auto* value = new juce::DynamicObject();
        value->setProperty("state", frust_exec::stateName(snap.state));
        value->setProperty("output", juce::String(snap.result.output));
        value->setProperty("diagnostics", juce::String(snap.result.report()));
        value->setProperty("succeeded", snap.state == frust_exec::State::Completed && snap.result.ok);
        return juce::JSON::toString(juce::var(value));
    }
    if (name == "knowledge_search") return result(true, math_knowledge::contextForQuery(args["query"].toString()));
    if (name == "layout_inspect") return juce::JSON::toString(dock->captureLayout());
    if (name == "layout_apply") { const auto catalog = layoutCatalog(); if (auto* presets = catalog["presets"].getArray()) for (const auto& preset : *presets) if (preset["name"].toString().equalsIgnoreCase(args["name"].toString())) return result(applyLayout(preset["layout"]), "Layout applied."); const auto saved = juce::JSON::parse(stateDirectory().getChildFile("user_layouts.json").loadFileAsString()); if (auto* users = saved.getArray()) for (const auto& entry : *users) if (entry["name"].toString() == args["name"].toString()) return result(applyLayout(entry["layout"]), "User layout applied."); return result(false, "Unknown layout."); }
    if (name == "layout_show_panel") { auto id = args["id"].toString(); if (dock->findPanelById(id) == nullptr) return result(false, "Unknown panel id."); dock->activatePanel(id); return result(true, "Panel opened."); }
    if (name == "open_help") { menuItemSelected(localHelp, 0); return result(true, "Opened Getting Started."); }
    return result(false, "Unknown tool.");
}
