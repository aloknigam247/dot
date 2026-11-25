# Desktop

## Index

- [CMakeLists.txt](CMakeLists.txt): source/header pairs in `DOT_LIB_SOURCES`, the `Dot` QML module, and deployment.
- [copilot_bridge](copilot_bridge): Rust Copilot CLI bridge; see [SETUP.md](../SETUP.md#10-copilot-cli-bridge).
- [include](include): public headers; implementations and change entrypoints are in the [C++ guide](src/AGENTS.md).
- [lua/dot_backend.lua](lua/dot_backend.lua): Neovim-side backend; bundled by [resources/dot.qrc](resources/dot.qrc).
- [QML guide](qml/AGENTS.md): scene composition, bindings, and focus ownership.
- [Test guide](tests/AGENTS.md): regression gates, QPA selection, registration, and smoke-test templates.

## Data flow

1. Neovim's `nvim --embed` stream enters [MsgpackRpc](src/MsgpackRpc.cpp), which owns the process/socket transport and
   emits decoded notifications. Arena-lifetime rules are in the [C++ guide](src/AGENTS.md#behavior-and-performance).
2. [NvimConnector](src/NvimConnector.cpp) routes `onNotification` through `handleRedraw`. It owns the transport,
   `GridModel`, `HighlightTable`, `ModeInfo`, `ImageModel`, `ColorPalette`, and `ResizeCoalescer`.
3. Redraw events update [GridModel](include/GridModel.h) and the companion models; the connector publishes `flush`.
4. [GridItem](src/GridItem.cpp) consumes dirty cell state on flush and renders through `updatePaintNode`, using
   `GridRuns` and scene-graph node pools. `CursorItem` renders cursor activity independently of cell content.
5. [Main.qml](qml/Main.qml) hosts [Shell.qml](qml/Shell.qml), which instantiates the grid and its overlays.
   QML composes these items and binds to model properties/signals; it is not a downstream pixel consumer.

Application construction and QML context wiring start in [src/main.cpp](src/main.cpp).
Attach capabilities are defined by `packAttachOptions` in `NvimConnector.cpp`, not by the model's grid-ID API.

## Build and deployment

Run desktop build, test, and script commands from this directory; run Copilot bridge Cargo commands from
[copilot_bridge](copilot_bridge). Follow [SETUP.md](../SETUP.md) for prerequisites.
[CMakePresets.json](CMakePresets.json) owns generator, toolchain, triplet, configuration, and output-directory settings.
`VCPKG_ROOT` supplies the toolchain; configure restores dependencies from [vcpkg.json](vcpkg.json) against
[vcpkg-configuration.json](vcpkg-configuration.json). Do not update the baseline as part of an unrelated change.

Prefer `release` for implementation and validation; use `dev` only for active debugging or Debug-specific tooling.
Do not combine the `dev` preset with `--config Release`.

```ps1
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
```

For Debug, substitute `dev` in all three commands. Output locations are documented in [SETUP.md](../SETUP.md#6-build).
Use `--parallel` without a fixed core count.

The Windows `POST_BUILD` block in `CMakeLists.txt` stages the selected Qt runtime, plugins, QML modules, and `qt.conf`
beside `dot.exe` for both preset configurations. Its `CMAKE_BUILD_TYPE` branch selects the vcpkg prefix and DLL suffix.
Non-Debug installation also uses `qt_generate_deploy_app_script`; a build-tree launch does not require installation.

For deploy omissions, fix `DOT_QT_RUNTIME_DLLS` or the QML copy commands, not the
[deploy-manifest guard](tests/check_deploy.ps1). For portable packages, use
[package_release.ps1](scripts/package_release.ps1) and [verify_release.ps1](scripts/verify_release.ps1);
their arguments and the [release workflow](../.github/workflows/release.yml) define the packaging procedure.

## Operational constraints

- **Build isolation:** serialize builds sharing a build directory; vcpkg locks and compiler PDBs are shared.
  Treat `CMakeLists.txt`, `src/main.cpp`, and the grid/connector sources as shared-file hotspots.
  Recheck alphabetical test registration when combining changes to `tests/CMakeLists.txt`.
- **Executable freshness:** after library-only changes, build the `dot` target before user validation and verify the
  executable timestamp against the change being tested. If stale, remove that worktree's executable and rebuild
  `dot` with the selected preset; passing statically linked test binaries does not prove the product was relinked.
- **Line endings:** preserve CRLF in desktop text files. Generated Neovim init fixtures must use explicit LF
  rather than platform-default line endings.
- **Profiling:** `BootProfile` in `src/main.cpp` gates timing on `DOT_BOOT_PROFILE=1`. For windowed launches, set
  `DOT_BOOT_PROFILE_FILE` to a checkout-local log path; timings are appended there as well as sent to `qDebug`.
- **QPA:** use the per-test environment in [tests/CMakeLists.txt](tests/CMakeLists.txt); do not globally force
  `offscreen` or override font-sensitive tests with a headless platform.

## Visual validation

Use the [visual-validate-dot skill](../.github/skills/visual-validate-dot/SKILL.md) and
[screenshot_dot.ps1](scripts/screenshot_dot.ps1), then inspect the captured image.
Pass the intended worktree executable with `-Exe` and an explicit checkout-local `-OutPath`.
Use `-InitFile` for Neovim configuration containing font names with spaces rather than retokenized `-c` arguments.
If another window appears in the capture, foreground dot or adjust `-SettleMs` and its placement.
The script owns the `PrintWindow`/DirectComposition capture details.
