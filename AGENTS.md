# Djehuti Math working rules

Bill directs architecture and scope. This is a professional engineering mathematics application.

- Inspect and reuse existing capabilities. Keep assignments focused. Preserve unrelated work, user data, credentials, licenses, and artwork.
- Use this single shared checkout and its build/output directories across agents. No per-agent clones, worktrees, toolchains, or executable drops. Coordinate one writable owner per repo and one build at a time.
- Work on a feature branch. Build and minimally check the changed function, commit specific files, push the feature branch, merge into the actual default branch, and push the default branch in the same assignment. Respect protections; no force push, blind staging, reset, or cleanup.
- Windows development uses Debug; distribution and installers use Release. Build one affected target, single core (`--parallel 1` or `/m:1`). Reuse existing FRust and LLVM installations.
- Redirect build output to a log and await completion. Do not poll compiler/linker processes, timers, or unchanged logs. Inspect relevant diagnostics after failure. Stop only processes owned by this assignment.
- Minimal functional verification of changed work. No broad regression suites, duplicate configurations, or large new JUCE test runners after each order. Bill directs broader regression after a substantial section. Documentation changes need no app build.
- User actions and agent tools use the same command service, with accurate schemas and LiteSemRAG cards. Keep descriptions and Help aligned with implemented behavior.
- Distinguish source inspection, compilation, functional checks, and actual visual evidence. Update the handoff with real results and remaining limits. Keep narration and repeated reads brief. Stop at the assignment boundary.
- Briefings are context only unless explicitly accompanied by an assignment. Do not message other chats without Bill's authorization.
- App identity, settings, auth sessions, keys, worksheet files, and layout state belong to DjehutiMath, independently of CAD, DEWB, and Suite.
