# Desktop C++

## Change entrypoints

Public headers live in `..\include`; CMake generates `Version.h` in the build tree.
[CMakeLists.txt](../CMakeLists.txt) pairs headers with implementations in `DOT_LIB_SOURCES`.
Add source/header pairs there, not to a separate handwritten inventory.

- **CLI and startup:** [ArgvParser.cpp](ArgvParser.cpp), [main.cpp](main.cpp), and
  [ArgvParser.h](../include/ArgvParser.h).
- **Configuration:** [Config.cpp](Config.cpp), [ConfigCliReader.cpp](ConfigCliReader.cpp), and
  [ConfigGGlobalReader.cpp](ConfigGGlobalReader.cpp).
- **Cursor:** [CursorBlinkState.cpp](CursorBlinkState.cpp), [CursorItem.cpp](CursorItem.cpp), and
  [ModeInfo.cpp](ModeInfo.cpp).
- **Grid and transport:** follow the [desktop data flow](../AGENTS.md#data-flow).
- **Images:** [ImageItem.cpp](ImageItem.cpp), [ImageModel.cpp](ImageModel.cpp), and
  [dot_backend.lua](../lua/dot_backend.lua).
- **Rendering:** [CellMetrics.cpp](CellMetrics.cpp) for cell geometry; [GridRuns.cpp](GridRuns.cpp) for draw-ready runs;
  [RectNodePool.cpp](RectNodePool.cpp) and [TextNodePool.cpp](TextNodePool.cpp) for scene-graph reuse.
- **Theme:** [ColorPalette.cpp](ColorPalette.cpp) and [HighlightTable.cpp](HighlightTable.cpp); follow the
  [color-binding contract](../../docs/design/implementation.md#color-bindings).
- **Window integration:** [AppIcon.cpp](AppIcon.cpp), [SessionCache.cpp](SessionCache.cpp),
  [VersionString.cpp](VersionString.cpp), and [WindowChrome.cpp](WindowChrome.cpp).

Build through the [desktop guide](../AGENTS.md#build-and-deployment); apply the
[test evidence gates](../tests/AGENTS.md#required-evidence) to changed behavior.

## Behavior and performance

- **Allocation:** batch text by highlight/font state. Reuse hot buffers with `QVarLengthArray`, `QString::reserve`,
  or explicit sizing; avoid per-cell allocation and append loops. Justify increased redraw complexity with profiling
  at a representative large grid rather than weakening the frame budget.
- **Grid storage:** `GridSurface::cellRows` is `QVector<QVector<Cell>>`. `GridModel::scroll` rotates row handles only
  for full-width scrolls; partial-width scrolls copy the affected column band. Preserve the full-width fast path.
- **Invalidation:** content-changing model operations set per-surface dirty state; cursor movement does not.
  `GridItem::onFlush` consumes `takeDirty(gridId)`. Preserve cursor-only flushes without a cell-render update;
  font, size, and highlight changes still invalidate rendering separately. Do not manually throttle Qt's coalesced
  `update()` without profiling evidence.
- **Msgpack lifetime:** pass `msgpack::object` views by const reference; copying a view does not extend its lifetime.
  Retain the owning `ObjectHandlePtr` for deferred work. `Notification` and `RpcRequest` in
  [MsgpackRpc.h](../include/MsgpackRpc.h) carry that ownership across signal/slot boundaries.
- **Protocol:** the Neovim redraw stream is authoritative. Validate supported event shapes; silent guards may tolerate
  new variants but must not conceal bugs in supported events. With `ext_hlstate`, consume all of
  `[id, rgb_attr, cterm_attr, info]` for `hl_attr_define`;
  `HighlightTable` uses `ui_name`/`hi_name` metadata for rounding.
- **Rendering boundary:** scene-graph updates may read cached grid/highlight/mode state and update renderer caches,
  but must not mutate those models or issue RPC.
- **Text:** preserve ordinary-text font fallback and the separate per-cell PUA placement in
  `GridItem::updatePaintNode`; `GridRuns` skips PUA collection for rows without PUA cells.
- **Thread ownership:** scene-graph nodes and render-path font-engine resources belong to the render thread.
  Construct and release any `QRawFont` cache there, before scene-graph teardown; use a direct invalidation connection
  if needed. Qt owns attached node roots: on a null `oldNode`, forget cached pool pointers rather than deleting them.

## C++ conventions

- **Input:** `InputHandler::keyToNvim` must emit valid, bracket-balanced Neovim key notation without raw control bytes.
  Preserve literal-less-than escaping and modifier handling.
- **Namespace:** use `namespace dot`.
- **Properties:** follow the [QML binding contract](../qml/AGENTS.md#binding-and-focus-contract).
  Avoid Qt FINAL member names on `QQuickItem` subclasses; use domain names such as `cellBaseline`.
- **Warnings:** follow the compiler flags in `CMakeLists.txt` and [.clang-tidy](../.clang-tidy); treat warnings as bugs.
  For Windows redirected stdin, use `_setmode(_fileno(stdin), _O_BINARY)`, not `std::freopen(nullptr, ...)`.

## Adding a config option

1. Register it in `main.cpp` before `ConfigCliReader::extract`; see `Config::resolve` for precedence.
2. Apply its resolved value and connect `Config::changed` to the consumer before the `attachComplete` handler reads
   globals through `ConfigGGlobalReader::read`.
3. Use the existing registered-option CLI extraction rather than adding a parallel `--dot-*` parser.

## Consuming a Neovim CLI argument locally

1. Add the necessary state to `DotArgs` in `..\include\ArgvParser.h`.
2. Consume it in `parseArgv` before the `editorArgs` fallback; do not forward an argument owned by dot.
3. Perform any pre-Qt stdin work after help/version handling and before `QGuiApplication` construction.
4. Implement the equivalent RPC in `NvimConnector`, following its request-packing methods.
5. Wire it on `attachComplete` in `main.cpp`; capture deferred payloads by value.
