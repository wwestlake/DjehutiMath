# Djehuti Math — product direction and foundation

Status: initial specification, October 18, 2026. Bill Westlake directs priorities and architecture. Planned features below are not implementation claims.

## Product

A professional engineering and scientific mathematics application: a more capable mathematical workbench in its own design, with FRust as a first-class programming language. Mathematical worksheets, symbolic expressions, matrices, numerical solvers, plots, and agents should use shared mathematical services and values.

## Required product capabilities

- Symbolic algebra: exact expressions, assumptions, simplification, equation systems, differentiation, integration, series and transforms. Preserve domains and assumptions with results.
- Numerical computation: roots, nonlinear and linear systems, constrained optimization, quadrature, ODE/DAE and later PDE families, fitting and statistics. Expose method, convergence, residuals, tolerances and failure reasons.
- Linear algebra: real and complex dense/sparse matrices, decompositions, eigenproblems, least squares, matrix functions and appropriate large-system methods. Future tensor representation should share value semantics.
- Engineering quantities: units, dimensional validation, constants, uncertainty/precision metadata and coordinate systems. Do not silently conflate approximate values and exact symbolic values.
- Built-in engineering and science libraries: mechanics, circuits, controls, signal processing, thermodynamics, scientific data analysis and related disciplines.
- FRust: full editor, embedded compiler/JIT, projects/packages, diagnostics, execution cancellation and mathematical host APIs.
- Interactive visualization: plots, surfaces, vector fields, matrix exploration and exportable scientific figures.
- AI: BYOK provider integration, protected key storage, LiteSemRAG grounding in actual APIs, documentation and worked examples. Agent tools act on the real workspace; numerical results come from execution rather than invented text.
- Account and Help: existing LagDaemon login/profile services; versioned website documentation, contextual links, and a bundled guide until dedicated online docs are published.

## Application architecture

The standalone app reuses the proven JUCE lifecycle, Windows-click menus, CreationDock, account/profile, FRust and provider abstractions. It owns the DjehutiMath identity and data paths. CAD geometry and electrical simulation subsystems are excluded.

The mathematical domain must remain independent of UI widgets and LLM providers. One command service should connect worksheet actions, agent tools and FRust host interfaces. Future backend selection and the symbolic-expression/value model require architectural review before implementing an advanced solver stack.

The central worksheet contains explanatory notes, source and execution results. The full FRust editor is an equal dockable workspace. Project/navigation, variables, diagnostics and the agent surround a large central canvas. Avoid forms covering the work area.

## This first milestone

- Independent build and application identity.
- Dockable worksheet and editor, scalar runtime workspace, project outline and diagnostics.
- Real FRust worksheet-cell execution on the existing single worker. Results are transient; `.djmath` stores authored cells and stable identities.
- New/open/save, source import/export, ordinary-close worksheet recovery.
- JSON predefined layouts, user-named layouts and automatic live-layout persistence with Default fallback.
- Existing account flow, BYOK Settings, Math-specific DPAPI storage.
- Filesystem LiteSemRAG card retrieval and live tool definitions sourced from the same catalog.
- Bundled guide and website documentation preparation.

The scalar workspace is an initial host bridge, not the final units/matrix/expression model. No alternate expression parser or toy symbolic solver is introduced. Shared compiler, installed LLVM and other dependencies are reused in place.

## Subsequent architecture discussion

Inventory FRust numerical/linear-algebra pods and established native solver libraries, then choose a symbolic backend and mathematical value/units model together. Set backend licensing, error contracts, cancellation and reproducibility requirements before adding solver families. CAD/DEWB data exchange is a future integration through explicit contracts; neither application's document or simulation state becomes owned by Math.
