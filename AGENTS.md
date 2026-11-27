# dot — project index

See [README.md](README.md) for product usage and supported platforms, and [Layout](#layout) for the repository layout.

## Code review

Complete the [code review](.github/agents/code-review.md) before reporting changes ready, opening a PR, or merging it.
Run it from the current session on the worktree containing the change; never change the session's directory or start
another session for it. For a PR, review its head against its target branch.
Stop and report if the review covered no changed files, covered the wrong diff, failed, was incomplete, or was
unavailable. The implementer fixes accepted findings within scope; ask the user before skipping one or expanding scope.
Rerun after any change to the diff, including review fixes. Proceed only when a complete review covers the final diff
and every accepted finding is fixed or explicitly waived by the user.

## Pull requests

Include `Copilot-Session: <session-id>` in the PR description, using the current Copilot session ID.

## Triage

Follow the [repository triage policy](.github/triage.md) when creating or revising issues.
Investigate the affected component through its scoped guide; use that component's validation, not another platform's.

## Compatibility

There are no external consumers pinned to the project's APIs, on-disk formats, or CLI.
Refactor, rename, or remove code when it makes the requested change cleaner; do not add compatibility shims or retain
deprecated paths for their own sake.

## Top priorities

Correctness wins over performance; both take priority over readability, terseness, and stylistic preferences.
Do not regress per-frame redraw cost. Desktop protocol and rendering constraints live in the
[C++ guide](desktop/src/AGENTS.md#behavior-and-performance).

## Layout

| Area | Start here |
| --- | --- |
| Android companion and console client | [android/AGENTS.md](android/AGENTS.md) |
| Common assets and release tooling | [common/AGENTS.md](common/AGENTS.md) |
| Design documentation | [docs/AGENTS.md](docs/AGENTS.md) |
| Desktop build and data flow | [desktop/AGENTS.md](desktop/AGENTS.md) |
| Desktop C++ and headers | [desktop/src/AGENTS.md](desktop/src/AGENTS.md) |
| Desktop QML | [desktop/qml/AGENTS.md](desktop/qml/AGENTS.md) |
| Desktop tests | [desktop/tests/AGENTS.md](desktop/tests/AGENTS.md) |

## Build

Use the component's build guide: [Android](android/AGENTS.md#build-and-validation) or
[desktop](desktop/AGENTS.md#build-and-deployment). Prerequisites are in [SETUP.md](SETUP.md).
The [workflow files](.github/workflows) define CI commands and path filters.

## Testing

Use the [desktop test guide](desktop/tests/AGENTS.md) for test registration, environment, and templates.
For Android and the console client, use [Android validation](android/AGENTS.md#build-and-validation).

### Verify, don't assume

Treat agent reports as claims, not proof. Run the applicable validation before claiming behavior works.
Desktop regression gates, hard assertions, and repetition requirements are in
[required evidence](desktop/tests/AGENTS.md#required-evidence).

## Conventions

- **Documentation:** `README.md` is frozen; do not edit it. Keep architecture details and operational rules in the
  scoped guides, not duplicated at the root.
- **Naming:** use lowercase `snake_case` for general-purpose files and directories. Preserve established C++
  `PascalCase` sources/headers and includes, QML component names, test/`.moc` names, Android framework/package paths,
  standard tool filenames, `.github` runtime paths, and vendored names. Never distinguish paths only by capitalization.

## Gotchas

See [desktop operational constraints](desktop/AGENTS.md#operational-constraints) and the affected scoped guide.
Use explicit checkout paths for shell operations; never assume a previous command's directory or environment persists.

## Worktree collaboration

Group delegated work by file ownership. Disjoint files may share a checkout; shared-file changes need separate
worktrees. Derive edit paths from the assigned worktree's `git rev-parse --show-toplevel`, never the primary checkout.
Desktop shared-file/build constraints are in the desktop guide.

When integrating delegated branches:

1. Agents commit to their assigned branches and stop; they never self-merge.
2. The parent cherry-picks completed commits sequentially rather than rebasing every intermediate commit.
3. After each integration, run the affected component's validation and confirm the product binary is current.
4. Keep worktrees and branches until the user verifies the integrated behavior, not merely until tests pass.
5. For an unpublished bad cherry-pick, prefer dropping it with `git rebase --onto`; use revert when others may have
   based work on the published commit.

## Visual validation

For desktop paint-path, font, cursor, or selection changes, inspect the rendered-window evidence required by the
[desktop visual-validation guide](desktop/AGENTS.md#visual-validation); passing tests alone are insufficient.
