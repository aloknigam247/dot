# dot: repository triage policy

## Categories

Use one to three applicable categories: `bug`, `chore`, `docs`, `feature`, `performance`, `refactor`, `spike`,
`tech-debt`, `test`. A findings-only spike uses `spike` as its sole category.

## Investigation

Use root `AGENTS.md` sections Compatibility, Top priorities, Layout, and Gotchas. Identify the affected desktop or
Android surface; read its scoped guides rather than repeating their architecture and invariants in each issue.

For desktop work, locate the actual transport, redraw/model, input, paint, or QML boundary. Base proposals on the
correctness and performance rules in those guides, not stale component descriptions.

## Validation

Use `AGENTS.md` Testing and Verify, don't assume, plus `desktop\tests\AGENTS.md`, to select the applicable test tier,
hard assertions, and repetition requirements. Reference Build for the working directory, preset, and quality gates.

Include changed-behavior cases and affected existing tests for implementation leaves. For native visual changes,
require the rendered-window evidence specified by `AGENTS.md` Visual validation and the `visual-validate-dot` skill.
Android work must use Android validation; desktop ctest/QML results are not evidence for Android behavior.
