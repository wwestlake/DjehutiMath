# Djehuti Math foundation handoff

October 18, 2026. Bill approved the independent application foundation and a **public MIT repository**, consistent with CAD and the other independent projects.

## Location and ownership

- Shared checkout: `D:/DjehutiMath`.
- Repository: https://github.com/wwestlake/DjehutiMath; default branch `main`.
- Build: `build`; executable: `bin/Debug/djehuti_math.exe`.
- Drive: https://drive.google.com/drive/folders/1RliSnC0voGdUZwyNTc69JFvkADbgbnam.
- Follow AGENTS.md. One writable owner per repository; one single-core build at a time. Never create per-agent toolchains/build drops. CAD and DEWB have other active owners and were used only as read-only references.

## Implemented foundation

The standalone JUCE application has its own identity, 42 px Windows-click menu header, LagDaemon account/profile flow, docking, Worksheet, FRust, Project, Variables, Diagnostics and BYOK Agent panels. It contains no CAD/OCCT geometry, mechanical solver, electrical simulation or rendering-engine subsystem.

Worksheets contain editable notes and executable FRust cells. Run-cell goes through the actual shared execution worker and embedded compiler/JIT, with outputs and diagnostics in the cell. The full FRust panel preserves the existing dark syntax palette, compile diagnostics, Run/Stop and editor persistence. File commands support `.djmath` new/open/save and standalone FRust source import/export. `examples/ohms-law.djmath` is an explicit engineering example.

The initial mathematical host bridge exposes `math_set_scalar(name, f64)` and `math_get_scalar(name)` to worksheets and editor programs. The scalar workspace is mutex-protected and shared within this application process. Missing values return NaN; stored values must be finite. This is a foundation, not the final units/matrix/symbolic value model.

`.djmath` version 1 saves authored cell identities, kinds and sources, never treats runtime output as an authoritative saved result, rejects malformed/unsupported documents before replacing the current one, and saves through a temporary file. Ordinary close writes a separate recovery snapshot plus saved-file association/modified state.

Layout → Default / Focused Worksheet / FRust Development comes from `Resources/default_layouts.json`. Named user layouts appear in the same menu. Current layout is saved with debounce and on ordinary close. Restore hides all registered panels before applying the listed panels, avoiding the previous startup-all-panels problem. Floating-only states are also accepted. First-run Default hides Diagnostics.

The BYOK agent currently exposes **OpenAI** through the existing provider library, configurable model/profile, cancellation and per-conversation context. Keys live under user-scoped DPAPI at `HKCU/Software/DjehutiMath/Credentials`; no keys are copied from another app or stored in settings. An in-flight provider request may finish after cancellation, but cancellation blocks further tool actions and results.

Filesystem LiteSemRAG retrieves relevant reference cards for each outbound request. `knowledge/cards/math_cards.jsonl` contains 20 reference cards including 16 live tools. Tool definitions come from those same cards, preventing separate tool-schema/catalog copies from diverging. This is retrieval grounding, not LLM weight training or a claim that ISD research features are implemented.

## Dependencies and reuse

Selected MIT CAD shell components were read from CAD's committed `main`, then adapted in this repository. Active CAD edits were preserved. No GPL Electronics Workbench implementation was copied. The Math card retriever is new application code using the shared filesystem-card approach.

CMake imports canonical prebuilt FRust Debug/Release libraries at `D:/FrustLang/lib`, including CreationDock, ai_provider, Frate, compiler, runtime and plugin host. LLVM remains at `D:/000 Creation Suite/apps/CreationEngine/vcpkg_installed/x64-windows`. **That old-looking directory is an active dependency, not disposable build output.** No shared language/LLVM toolchain was rebuilt or copied. JUCE source is `D:/JUCE`. Configuration paths are cache variables. Match imported library configuration and MSVC toolset when rebuilding their canonical source later.

See THIRD_PARTY_NOTICES.md for attribution and the separate JUCE/dependency distribution terms.

## Validation record

The final Debug build succeeded. Seven focused checks passed in approximately five seconds: worksheet save/reopen, malformed-document rejection preserving current state, the **actual WorksheetPanel Run-cell command** compiling and executing FRust on the real shared worker, publication of the cell output and scalar answer 42, LiteSemRAG/tool discovery, scalar validation and a disposable DPAPI credential round trip. The check uses the existing application executable, queues worker notifications back to the test's main thread, and has a bounded deadline. Results: `build/foundation-check.json`; build log: `build/build-foundation.log`. `git diff --check` also passed.

The real application launched and exposed the worksheet, cell sources, Run button, account and menus in its accessibility tree. Native screenshot capture failed with `FrameArrived timed out` and `window capture timed out`; indexed input failed with `coordinate input geometry is unavailable`. A normal close subsequently persisted the expected Default layout (Project left, Worksheet/FRust center, Variables/Agent right, no bottom panels) and worksheet recovery file. This is launch/accessibility/persistence evidence, **not a visual interaction pass**.

Accessibility revealed garbled punctuation from narrow C++ string literals. The source now explicitly converts UTF-8 through JUCE and compiles the relevant application files as UTF-8. The corrected subtitle was verified in the final running executable's accessibility tree. Only the exact unchanged sample note created by the first test launch was corrected in its recovery snapshot; user-authored documents were not rewritten. The final application was left open for Bill to inspect.

No broad regression suite, standalone JUCE test runner, Release build or real paid provider call was run. A dummy credential with a unique name was removed after checking its round trip. Real LagDaemon login and the Math product's server registration remain unverified.

## Next architecture work

The application specification is `docs/PROJECT_SPECIFICATION.md` (also saved in the Drive folder). Symbolic algebra, matrix/tensor values, complex/sparse backends, units, advanced numerical solver families, plotting and engineering/science libraries are planned. No backend selection has been finalized. Inspect existing FRust numerical pods and native library options, then agree on expression/value/units semantics before implementing solver families.

`docs/getting-started.html` is bundled Help; the app links to the actual LagDaemon website home. Dedicated Math website docs are **not published**. `docs/WEBSITE_HELP_HANDOFF.md` prepares a separate website assignment, including product registration, a verified documentation route and contextual Help links. Do not invent a live route or deploy website changes under the desktop assignment.

## DEWB capability parity work (branch `feature/dewb-capability-parity`, uncommitted)

Work order: bring FRust, node programming, the agent, VST generation, audio pipeline access and sound-effects DSP up to DEWB's level, adapting rather than copying circuit-specific code. Math has no circuit solver or schematics; the DSP/VST chain is Math-owned and ready for the future Math-to-FRust compiler (`dsp_effect::EffectFrontend`, `registerFrontend`; no frontend is registered yet).

### What exists
- **FRust:** `compileToObject`, `loadObject`, `manifestJson` added to `FrustEngine`; DEWB's Debugger panel; debug programs also get the math host functions. DSP-only host math (`djehuti_dsp_sin/cos/tan/tanh/atan/exp/log/sqrt/pow`) is registered; ordinary worksheet scripts still have no sqrt/sin.
- **Nodes:** DEWB's `NodeCompiler.cpp` and `NodeDesignerPanel` copied unchanged (self-contained). Panels: Node Designer, Debugger. Layout presets: Node Programming, Audio DSP. The node compiler's executable `print` accepts only literal values (a DEWB limitation, not changed).
- **Audio:** `AudioPipeline` rewritten (not copied): device opens on demand and is released by Stop Live, chunk-safe callback, file or hardware input, WAV in/out, meters, resampled playback; silent (not pass-through) with no effect attached.
- **DSP/VST:** `DspEffect` (directives, native COFF compile, `.frust-il` artifact v1 with magic `DJEHUTI_MATH_EFFECT_IL`, integrity hashes, real-time `Runtime`, offline render, harmonic measurement), `DspPanel`, `VstExporter`, and the plugin `DjehutiMathEffectVst` (`Source/GeneratedVst`). The app build also builds the plugin and copies it to `templates/vst3`.
- **Agent/command service:** 48 tools from the cards (about 30 new), all through `DjehutiMath::executeCommand`; async tools reply when a run/compile/debug step settles. Agent loop: outcome tool, no-progress guard, provider item preservation, model listing in Agent Settings. `LocalAgentApi`: localhost-only, bearer-token HTTP (`GET /v1/status`, `GET /v1/tools`, `POST /v1/tools/call`); port and token in `%APPDATA%\DjehutiMathgent-api.json` while the app runs. Anything running as this user that can read that file can call the tools, including file-writing ones.

### Verified (Debug)
- `djehuti_math.exe --dsp-check <json>`: 24 checks passed (effects match C++ references, diagnostics map to author lines, artifact tamper rejection, WAV round trip, chunked callback, file loop, node compile/run/save/reopen, VST3 export and a real JUCE VST3 host load with parameter, bypass, state restore, damaged-artifact inert load). `--foundation-check` still passes. Reports: `build/dsp-check.json`, `build/foundation-check-2.json`; build logs `build/build-dewb-parity*.log`.
- Real app via the local API and GUI: DSP compile (~130 ms, off the UI thread), render, parameter change, VST3 export, node graph built by tools, compile, run, breakpoint, step over, variables, stop; live audio on the real device with file input, Stop released the device; the DSP Effects, Node Designer and Debugger panels rendered.

### Not done / limits
- **VST3 identity:** every export shares the runtime's compiled class identity (as DEWB's installed template does). Host loading was checked only with the in-app JUCE host, not a DAW. No Release build, no installer packaging.
- **Not ported from DEWB:** markdown/KaTeX report viewer and web search (WebView/curl are off in Math), electronics cookbook, acceptance and gap registry, FRust programmable components (the Node Designer's component panel stays hidden).
- **Not exercised:** the agent loop against a real provider (compiled only), Agent Settings model list and the changed dialog layout, hardware microphone input, stereo effects (live processing is mono copied to all outputs), the debugger panel's buttons by mouse (driven through tools instead), the FRust panel stale-status and Variables empty-state defects from the initial assessment (unchanged).
- Nothing is committed or pushed.
