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

## Milestone M0: native mathematical equation editor, foundation (branch `feature/native-equation-editor-m0`)

Order M0 implements the approved architecture (native JUCE, structured trees, stable node ids, placeholders, document authoritative, MathIR separate, no MathLive/WebView2). Delivered: typeset **math cells** in the worksheet, entered in linear notation, saved and reopened as structure in `.djmath` version 2.

### What exists
- **Model** (`MathDoc`): immutable trees of Rows (every slot is a Row; infix stays flat) holding Number, Identifier, Operator, Relation, Symbol, Text, Fraction, Radical (optional index), Script (base + sub/sup), Delimited, BigOperator (limits) and Placeholder. Every node has a persistent id from a per-worksheet allocator (`nextNodeId` is saved, never reused). Strict validation (depth 64, 20,000 nodes per cell, unique non-zero ids, slot shapes).
- **Linear input** (`MathLinear`): `(x^2+1)/(x+3)`, `sqrt(...)`, `root(n, x)`, `x_i^2`, `sum_(i=1)^n`, `int_0^1`, Greek and function names, `<= >= != -> :=`, quoted text, `?` placeholder. `/` divides the run of factors just before it by the single factor after it. Missing operands become placeholders. `toLinear` is the inverse (round trip checked on 31 expressions). Re-parsing an edited cell keeps the ids of unchanged nodes (`adoptIds`). Linear text is stored as informational `origin`, never read back as the document.
- **Typesetting** (`MathFont`, `MathLayout`, `MathView`): bundled **STIX Two Math 2.12** (SIL OFL 1.1, unmodified, notice in THIRD_PARTY_NOTICES.md). `MathFont` reads cmap, hmtx, the OpenType MATH table (constants, italic corrections, size variants, glyph assemblies) and the CFF outlines itself. JUCE's memory-font path was tried first and rejected: on Windows it uses GDI and its glyph numbers do not match the font's, so shapes came out wrong. Layout follows TeX/OpenType MATH: atom classes and the TeX spacing table, four style levels, fractions, scripts, radicals with index, stretchy delimiters (variants or assembled), large operators with limits (above in display style for sum/product, beside for integrals), placeholders, and aligned systems on `&`. Every node gets a rectangle (hit testing is ready for M1).
- **Worksheet**: new `math` cell kind with a role (Expression, Definition, Equation, Constraint, Assumption, System of equations), a linear input, live typeset view, notes line, hover shows `node <id> <kind>`. Cells size to their content. `rebuild()` no longer destroys components: cells that still exist keep their component, focus and caret (checked by pointer identity). Selection follows focus. **+ Math** button and **File > Add Math Cell**. New cells scroll into view.
- **`.djmath` version 2**: `version`, `nextNodeId`, per-cell `math` (`schema`, `role`, `lines` of nodes) and `origin`. Version 1 files load unchanged; a `math` cell in a version 1 file, corrupt math, future versions and non-JSON are rejected without touching the open worksheet. Always saved as version 2 (older builds cannot open it). The recovery snapshot uses the same format.
- **Compiler boundary** (declared, not implemented): `MathSnapshot` (cell id, revision, role, immutable lines), `SourceRef`, `Diagnostic`, `AnalysisResult`, `MathFrontend`. `WorksheetDocument::snapshot`, agent tool `worksheet_math_snapshot`.
- **Agent/command service**: `worksheet_add_cell`/`worksheet_set_cell` take kind `math` and a `role`; cards and the `math.notation` reference card updated.
- **Branding**: the approved emblem is untouched in the project root. `tools/make_branding.py` writes `Resources/branding` (PNG 16..512 and a multi-size ICO, square, Lanczos). Used for the executable/window icon (embedded icon verified), the header logo and the Help page.

### Verified (Debug)
`djehuti_math.exe --math-check <json>`: 43 checks (font tables and outlines, 31 linear cases plus round trips, identity, malformed-tree rejection, v1/v2 persistence and rejections, snapshots, layout geometry for fractions/scripts/radicals/delimiters/limits/spacing, worksheet component preservation) plus `math-gallery.png` for visual inspection. `--dsp-check` (24) and `--foundation-check` (7) still pass. Real app: a math cell was added, typed into, role changed to a system, saved as version 2, reopened, and an existing FRust cell was run.

### Limits
- Linear input only: no mouse or structural editing yet (M1). Math cells are not evaluated; there is no MathIR frontend.
- No kerning (GPOS), math kerning of scripts, top accents, matrices, cases, accents/overbars or inline (text) style cells yet. Display style only.
- `lim_(h->0)` puts its subscript to the side, not below.
- Not verified in the GUI: the File menu item and the scroll-into-view behaviour (a Windows touch-keyboard host process kept taking the foreground and blocked clicks); both are verified by build only.
- No Release build or installer packaging; the app icon is checked in the executable, not in an installer.
- Operand-capture convention for `/` (documented in Help) and one-name-per-letter-run are policy decisions awaiting executive confirmation.

## S0 - MathIR and computational foundation

Branch `feature/s0-mathir-foundation`. Infrastructure and interfaces only; **no solver library is integrated** (no SymEngine, SymPy, Eigen).

### Modules (all new; GUI-independent)
| File | Role | Depends on |
|---|---|---|
| `MathIR` | Immutable typed expression DAG (`shared_ptr<const Expr>`): scalars (integer, rational, real, complex), symbols, constants, units, arithmetic, `Call`, `Sum/Product/Integral/Derivative`, `Index`, relations, `Define`, `Vector/Matrix/Tuple/System`, `Hole`. Types carry shape, number domain, physical `Dim` (7 SI exponents, may be unknown), sign. Every node carries a `Source` (cell, node id, item range). | std only |
| `MathUnits` | SI catalogue (no offset units), prefixes, unit-expression parser (`kg*m/s^2`, `km/h`, `kΩ`) | MathIR |
| `MathValue` | Value model: exact/approximate scalars, complex, vector, matrix, symbolic, boolean, Undefined, Failed; dimension, error bound, diagnostics, provenance | MathIR |
| `MathSolver` | Solver-independent interface: `Operation` (evaluate, simplify, solve, differentiate, integrate, numeric), capability discovery, `Request` (bindings, tolerance, cancel token, deadline), `Response` (status, value, diagnostics, provenance), `Registry`; one engine, `djehuti.reference`, a small exact/float evaluator of fully numeric expressions | MathValue |
| `MathDeps` | Name-based dependency graph: dependencies/dependents, topological order, cycles (Tarjan), conflicts, blocked cells, state stamps | std only |
| `MathFrontend` | `math_doc` trees -> MathIR; symbol resolution, type/dimension/shape checks, diagnostics with source references; implements `math_doc::MathFrontend::analyze` | MathDoc, MathIR, MathUnits |
| `MathWorkspace` | Snapshots -> analyses -> graph; cycle/duplicate/conflict diagnostics; revision-aware result tickets; `evaluate` runs dependencies first | all of the above |
| `MathResult` | Value/MathIR -> presentation trees with fresh node ids and `derivedFrom` source relations; display unit option | MathDoc |
| `MathCommands` | Command-service tools (below) | WorksheetPanel (document type only) |

### Notation meaning given by the frontend (also in card `math.ir`)
Definition cells (`a = 3`, `f(x) = ...`) define names; Assumption cells declare (`x in R`, `v in "m/s"`, `x > 0`); a quantity is a number plus a quoted unit (`9.81 "m/s^2"`); `sum_(i=1)^n`/`prod` body = following factors up to `+ - , ;` or a relation; `int_a^b body dx` runs to the differential; `dv/dt` and `(partial f)/(partial x)` are derivatives unless the names are defined; `[a,b]` vector, `[a,b;c,d]` matrix; pi, e, i are constants unless defined/declared/bound; an undefined symbol is a free real variable (information, not an error).

### Dependencies and results
Worksheet order does not matter. A cell depends on every cell that defines or declares a name it uses. Cycles (including `x = x + 1`) and cells depending on them are blocked. A result is stored only against a `Ticket` (cell revision + stamp of everything it depends on); an edit anywhere upstream makes the stored result stale and rejects late answers. Nothing from analysis or evaluation is written into the `.djmath` file (format unchanged, v1 and v2 load).

### Command service / agent tools (cards added to `math_cards.jsonl`)
`worksheet_math_analyze`, `worksheet_math_dependencies`, `worksheet_math_evaluate`, `math_solver_capabilities`. They share one dispatcher with the other 49 tools.

### Verified (Debug)
`djehuti_math.exe --mathir-check <json>`: 220 checks pass (rational overflow, dimensions, units, translation, types, dimension errors, symbol resolution, diagnostics and source ranges, dependency graph, workspace invalidation, cycles, revision-aware results, evaluation, solver registry/cancel/deadline, values, result display and IR round trips, commands, save/reopen). Also `--math-check` 43/43 and `--foundation-check` 7/7 after the change. Not verified: visual GUI (no GUI surface was added in S0).

### Limits
- Only the reference evaluator exists: no simplify, solve, differentiate or integrate engine; symbolic or unbound expressions are `Unsupported`.
- Decimal literals are approximate reals (9.81 is not 981/100). Integers beyond 18 digits become approximate with a warning. Exact arithmetic overflow falls back to floating point with a warning.
- Frontend does not yet support: prime notation, `±`, factorial, limits, higher-order derivatives, vector dot/cross products, `{}` sets, offset units (degC), `lim`, `gcd`, `mod`, `Re`, `Im`, `arg`, user-function evaluation by the reference engine.
- An Assumption cell holds one line (one declaration list, e.g. `x, y in R`), like every non-System role.
- Matrices display as bracketed lists with `;` (the document model has no matrix structure yet).
- Operand capture of `/` still applies: write `(10 "N") / (2 "m^2")` to divide a quantity.
- Assumed defaults to confirm: undeclared symbols are real with unknown dimension; `log` means base 10, `ln` natural.
- Results are not yet shown in worksheet cells; there is no GUI for them (S0 scope).
