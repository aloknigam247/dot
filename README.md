# dot

[CI workflow](.github/workflows/ci.yml)

## Overview

dot is a Neovim GUI client for Windows, written in C++23 / Qt 6.10 / QML. It drives an embedded
`nvim --embed` process and talks to it over msgpack-rpc. The user-facing shell is QML, the text grid
uses independent Qt Quick background/text scene-graph layers, and keyboard input is translated from Qt key events into nvim
keycodes. It is currently Windows-only (built with the Visual Studio 2026 generator and MSVC).
The editor and its AI harness share the dot name.

For building and running, see [`SETUP.md`](SETUP.md). For contributor conventions, the correctness
and performance invariants, and the test tiers, see [`AGENTS.md`](AGENTS.md).
Application artwork and export instructions live in [`common\icons`](common/icons/README.md).

The approved UI target is defined in [`docs/design/DESIGN.md`](docs/design/DESIGN.md), with dot composition,
implementation mappings, and reference screenshots in [`docs/design/implementation.md`](docs/design/implementation.md).
The design is not a claim of native implementation parity.

## Repository layout

| Directory | Contents |
| --- | --- |
| `.github\` | GitHub workflows and agent integrations |
| `android\` | Android application, shared JVM chat client, and Gradle build |
| `common\` | Shared artwork, release tooling, and patch-coverage checker |
| `desktop\` | C++/QML client, build configuration, tests, and desktop tooling |
| `desktop\copilot_helper\` | Rust [Copilot CLI helper](#copilot-cli-helper) |
| `docs\` | Repository-wide design documentation |

## Versioning

The [version workflow](.github/workflows/semantic-release.yml) runs on its configured schedule or by manual dispatch on
`main`. It versions Android and desktop dot independently, running them sequentially to avoid competing pushes.

| Component | Tag | Version files | Changelog |
| --- | --- | --- | --- |
| Android | `android-vX.Y.Z` | `android/app/build.gradle.kts` | `android/CHANGELOG.md` |
| Desktop dot | `dot-vX.Y.Z` | `desktop/CMakeLists.txt`, `desktop/vcpkg.json` | `desktop/CHANGELOG.md` |

[Release path filters](common/release/release.config.cjs) count `android/` for Android, `desktop/` for dot, and
`common/` for both. Files outside those directories, including this README, do not affect either version or changelog.

semantic-release uses conventional commits: `feat:` bumps minor, `fix:` and `perf:` bump patch, and breaking changes
(`!` or a `BREAKING CHANGE:` footer) bump major. Without an existing component tag, its first version is `1.0.0`.
Non-breaking documentation, chore, and refactor commits alone do not trigger a version.

Android's `versionName` follows its semantic version; `versionCode` increases once per new Android version. Desktop
versions stay synchronized, and C++/Windows executable metadata is generated from CMake's project version.
The version workflow pushes version commits and tags only; it does not publish binaries.
See [Android release signing](android/README.md#release-signing) for optimized APK builds and protected-tag
verification.

Set the repository Actions secret `RELEASE_TOKEN` to a repository-scoped PAT with **Contents: read/write**. Its identity
must be allowed to push directly to `main` and create tags. Checkout uses a read-only workflow token without persisting
credentials; a missing or invalid release token fails the workflow. Do not delete or move version tags.

## Desktop releases

Install [Neovim nightly on PATH](SETUP.md#neovim-nightly-on-path), then download the Windows x64 ZIP from
[GitHub Releases](https://github.com/aloknigam247/dot/releases). Extract the entire archive and run:

```ps1
.\bin\dot.exe
```

If Neovim cannot start, dot keeps its main window open and shows the attempted executable and native launch error.

Keep the accompanying runtime directories and `third_party` notices with the executable.

The [release workflow](.github/workflows/release.yml) publishes verified ZIPs when desktop release tags are pushed.
For a failed publishing run, inspect the Release first. Remove an incomplete draft before retrying the publishing job;
do not remove a published Release or its version tag.

## Configuration

Neovim exposes `g:dot` when running in this GUI. Set `g:dot_rounded_highlights` to select rounded highlight groups:

```lua
vim.g.dot_rounded_highlights = { "FloatBorder", "NormalFloat" }
vim.api.nvim_set_hl(0, "DotAccent", { link = "Function" })
```

Desktop colors come from Neovim. The default [`Dot*` highlights](docs/design/implementation.md#dot-owned-chrome-roles)
link to standard highlight groups after startup; muted text and selection prefer the matching MiniStatusline groups when
available. Override or relink any public `Dot*` group in your Neovim configuration or colorscheme.
Globals are read atomically on attach and restart; missing values return to defaults without overriding CLI values.

Dot-specific settings use the `g:dot_*` and `--dot-*` application prefixes. The session cache uses dot's Qt application-data
location; caches from other application identities are not imported.

Neovim's `guicursor` controls cursor shape, bar/underline percentage, and blink timing, including live changes.
The cursor uses the shared [design corners](docs/design/DESIGN.md#4-metrics), clamped to its actual shape.
Its foreground and background come from the active Neovim mode highlight; valid attribute ID `0` reverses Neovim's
default foreground/background pair.

## Images

Pass PNG bytes to [`vim.ui.img`](https://neovim.io/doc/user/lua.html#vim.ui.img):

```lua
local id = vim.ui.img.set(vim.fn.readblob([[C:\images\example.png]]), {
  col = 5,
  row = 5,
  width = 16,
})
```

`col` and `row` are one-based screen cells, not buffer positions. `width` and `height` are cell dimensions;
the image preserves its aspect ratio. Omit both dimensions for natural size in logical pixels.
Signed `zindex` controls layering; negative values place images below text, and the cursor stays above images.

Use `set(id, opts)` to update a placement, `get(id)` to inspect its requested options, and `del(id)` to remove it.
Images follow actual grid scrolling, not buffer/extmark positions. Plugins must re-place them after other viewport
changes; a successful `set` restores scroll-clipped content. Invalid input and exhausted
[resource limits](desktop/include/ImageModel.h) raise a Lua error without silently evicting existing images.

## Copilot CLI helper

`dot_copilot_helper` is a standalone process for dot chat; dot does not launch it yet. It connects to the installed
GitHub Copilot CLI through the official `copilot-sdk` crate and never downloads or bundles a CLI. It uses the first
`copilot` executable on `PATH` (with `PATHEXT` extensions) that completes a protocol 4 SDK handshake. Each handshake
and session creation must finish within `DOT_COPILOT_HELPER_STARTUP_TIMEOUT_MS` milliseconds (default 30000); a status
or auth query that exceeds it reports a `null` `version` or `unknown` `auth`.
The helper runs one session in its working directory, denies every permission request, disables client telemetry, and
leaves credentials with the CLI. Build and test it as described in [SETUP.md](SETUP.md#10-copilot-cli-helper).

The helper exchanges one JSON object per line on stdin and stdout. Helper protocol version 1 records:

| Direction | `type` | Fields |
| --- | --- | --- |
| Input | `cancel` | `request_id`, `turn_id` |
| Input | `send` | `prompt`, `request_id` |
| Input | `shutdown` | `request_id` |
| Output | `cancel_acknowledged` | `request_id`, `turn_id` |
| Output | `cancel_rejected` | `kind` (`abort_failed`, `no_active_turn`, `turn_mismatch`, `turn_starting`), `message`, `request_id` |
| Output | `error` | `kind` (`cli_not_found`, `cli_unusable`, `event_stream_closed`, `event_stream_lagged`, `invalid_request`, `session_shutdown`, `session_start_failed`), `message`; startup errors add `protocol` and, for discovery, `candidates`; invalid requests add `request_id` when present |
| Output | `item` | `item` (the SDK event's exact JSON serialization, as a string), `source` (`lifecycle`, `session`) |
| Output | `ready` | `cli` (`auth`: `signed_in`, `signed_out`, or `unknown` with `auth_error`; `path`; `protocol`; `version` or `null`), `protocol` |
| Output | `send_accepted` | `request_id`, `turn_id` |
| Output | `send_rejected` | `kind` (`cancel_pending`, `empty_turn_id`, `reuse_rejected`, `send_failed`, `session_failed`, `turn_active`), `message`, `request_id` |
| Output | `turn_ended` | `outcome` (`aborted`, `completed`, `failed`), `request_id`, `turn_id` |

Each discovery candidate reports `message`, `outcome` (`protocol_mismatch`, `protocol_unverified`, `startup_failed`, or
`timed_out`), and `path`; a protocol mismatch adds `cli_protocol` and `required_protocol`, plus `cli_version` when the
CLI's version is available.

- `ready` precedes every `item`. A startup `error` is the only record before the helper exits with code 1.
- At most one turn is active. `send_accepted` precedes exactly one `turn_ended` for that turn; a rejected send has none.
  A cancel acknowledgement never ends a turn. Turn correlation follows the SDK's `send_and_wait`: start and end events
  that omit their correlation IDs are ignored, so such a turn stays active until `shutdown`. A root `session.error`
  whose `originatingMessageId` is absent or names the turn makes that turn end `failed`, unless aborted, at its next
  end event.
- Sends are rejected with `cancel_pending` until every cancel has its `cancel_acknowledged` or `cancel_rejected`.
- After an `empty_turn_id` or `send_failed` rejection, an `event_stream_closed`, `event_stream_lagged`, or
  `session_shutdown` error, or a turn end that cannot prove a safe boundary, later sends are rejected until the helper
  restarts.
- Each record is written to stdout before the next event is handled, so `event_stream_lagged` means stdout went unread
  while more events arrived than the SDK buffers (512 session events, 256 lifecycle events).
- A `shutdown` request or closed stdin ends any active turn as `failed`, stops the CLI, and exits with code 0. A stdin
  read error does the same but exits with code 1. An I/O error that stops the helper is written to stderr.

## Architecture

dot is a thin, reactive layer over Neovim: Neovim owns all editing state and emits a redraw event
stream, and dot's job is to turn that stream into pixels and to turn user input back into nvim
keycodes.

The component paths below are relative to `desktop\`.

The Neovim-side dot bootstrap lives in [`lua/dot_backend.lua`](desktop/lua/dot_backend.lua).

### Redraw pipeline (nvim → screen)

```
nvim --embed
    │  msgpack-rpc (redraw events)
    ▼
MsgpackRpc          transport: frames msgpack-rpc over the nvim process pipes
    ▼
NvimConnector       decodes and dispatches redraw events (grid_line, grid_scroll,
    │               mode_change, hl_attr_define, hl_group_set, default_colors_set, flush, …)
    ├──────────────► GridModel        row-major cell grid (text + hl_id per cell)
    ├──────────────► HighlightTable   hl_attr cache (colours + attrs per hl_id)
    └──────────────► ModeInfo         cursor shape / current mode index
                         │
                         ▼
                     GridItem   (QQuickItem)   renders grid layers
                     CursorItem (QQuickItem)   renders the cursor overlay
                         │
                         ▼
                     QML shell (qml/: Main, Shell, ChatPanel)
```

`NvimConnector` exposes redraw notifications as `Q_SIGNAL`s that the QML shell connects to
directly — there is no imperative pull from QML back into RPC state.

[`ColorPalette`](desktop/include/ColorPalette.h) resolves `HighlightTable` and `ModeInfo` into complete content-color
pairs before each redraw `flush`, alongside a complete Neovim-owned chrome snapshot. QML and `CursorItem` read these
cached properties.
Missing required values invalidate the affected pair and appear in `resolutionErrors`; restart invalidates content
immediately. See the [color-binding contract](docs/design/implementation.md#color-bindings) and its documented contrast
limitations.

The cursor snaps to Neovim's position with no movement-animation option. Blink timing follows `guicursor`.

`ImageModel` owns PNG resources and projected placements received through image RPC requests. `ImageItem` manages
independent per-image texture items between the grid's background/text layers. Image-only changes do not repaint
the text grid.

### Input path (keyboard → nvim)

```
Qt key event on GridItem
    ▼
InputHandler::keyToNvim   QKeyEvent → well-bracketed nvim keycode string
    ▼
NvimConnector::input()    forwards the keycode to nvim
    ▼
MsgpackRpc                writes it over msgpack-rpc
    ▼
nvim --embed
```

`GridItem` owns the Qt input event and calls `InputHandler` — QML never invokes `InputHandler`
directly.

### Load-bearing invariants

- The redraw event stream from `nvim --embed` is the source of truth for the text grid and cursor; image operations
  supply the separate image state.
- Grid rendering reads only its models and presentation metrics, never RPC state or image resources.

## Launch performance (Windows cold start)

The first launch of dot *after a build or update* can take 10-20 seconds, while every subsequent
launch is ~1 second. This is not dot code — it is **Microsoft Defender's real-time scanning of the
build tree**. On a rebuild, two scans stack: a "first-sight" scan of `dot.exe` and the Qt DLLs it
loads (Defender caches its verdict by file **content hash**, so new bytes mean a new scan), and a
scan of the several GB of freshly-written compile intermediates (`.obj`/`.pdb`/`.lib`) — which
saturates disk I/O that the first launch then competes with.

Two things keep cold start down:

- **The deploy is trimmed to only the Qt modules dot actually loads.** The post-build step copies an
  explicit list of ~20 runtime DLLs plus the `QtQuick`, `QtQml`, and `QtTest` QML modules that are
  imported, instead of the whole Qt Quick tree (Controls/Dialogs/Particles/Effects, ~150 MB). Fewer
  and smaller files means less for Defender to scan on first sight.

- **A Defender exclusion for the build tree collapses the cold launch outright** (post-build first
  launch ~10-13s → ~2s in measurement). Run this once, from an **elevated** PowerShell:

  ```ps1
  pwsh -NoProfile -File desktop\scripts\add_defender_exclusion.ps1
  ```

  By default it excludes the whole `desktop\build\` tree — deliberately, because the dominant cost is
  scanning the compile intermediates, not just the deployed `dot.exe`. Pass `-Path <dir>` for a
  custom location (e.g. a worktree's own `desktop\build\` dir), or `-Remove` to undo. An excluded directory
  is no longer scanned by real-time protection, so only exclude build output you produce and trust —
  never a broad location like your whole user profile or a downloads folder.
