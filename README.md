# Djehuti Math

Professional engineering mathematics workbench from LagDaemon Software. The first milestone is an independent Windows application foundation, reusing the MIT-licensed DjehutiCAD shell and canonical FRust libraries.

## Foundation

- Dockable worksheet, FRust editor, project, variables, diagnostics, and BYOK agent panels.
- Notes and executable FRust worksheet cells, versioned `.djmath` persistence, ordinary-close session recovery.
- Native FRust compilation/JIT on a worker, Run/Stop, dark editor, source import/export, shared finite-real scalar workspace.
- LagDaemon account integration, Math-specific settings/session storage, DPAPI-protected BYOK keys.
- Filesystem LiteSemRAG reference-card retrieval on each agent request. Live tools are generated from the same catalog as their knowledge cards.
- JSON layout presets, named user layouts and debounced current-layout persistence. First run uses Default.
- Bundled Help and shared website link. Dedicated website docs and live Math login registration are pending server verification.

Symbolic math, numerical solver families, matrices/tensors, dimensional units, and plotting are planned, not implemented in the shell.

## Build

The shared installed toolchains are used in place. CMake imports the existing `D:/FrustLang/lib/Debug` or `Release` libraries and LLVM at `D:/000 Creation Suite/apps/CreationEngine/vcpkg_installed/x64-windows`; it does not build or copy the compiler toolchain. These libraries must be built for the matching configuration and toolset in their canonical checkout. Override the root paths with CMake cache variables when the drive is reorganized.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target DjehutiMath --parallel 1
```

Executable: `D:/DjehutiMath/bin/Debug/djehuti_math.exe`.

Optional focused foundation check uses the same executable: `djehuti_math.exe --foundation-check D:/DjehutiMath/build/foundation-check.json`. No separate JUCE test runner.

See `docs/PROJECT_SPECIFICATION.md`, `docs/getting-started.html`, and `HANDOFF.md` for scope and actual validation.
