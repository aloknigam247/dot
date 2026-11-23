# Desktop QML

## Entry points

- [ChatPanel.qml](ChatPanel.qml): dot-native transcript UI; creates `ChatModel` and exposes its model/input surface.
- [DesktopMetrics.qml](DesktopMetrics.qml): geometry derived from grid metrics.
- [Main.qml](Main.qml): window readiness, startup-error presentation, title/chrome wiring, and chat visibility.
- [Shell.qml](Shell.qml): long-lived `baseGrid`, `ImageItem`, `CursorItem`, and `focusGrid()`.

QML registration lives in [CMakeLists.txt](../CMakeLists.txt).
Use the [design index](../../docs/AGENTS.md) for geometry and color bindings instead of copying tokens into this guide.
Build through the [desktop guide](../AGENTS.md#build-and-deployment); select validation from the
[test guide](../tests/AGENTS.md#required-evidence).

## Binding and focus contract

- **Context:** [main.cpp](../src/main.cpp) supplies the application-owned context objects. Do not shadow a registered
  `QML_ELEMENT` with `setContextProperty`; expose bindable C++ state through `Q_PROPERTY` with NOTIFY.
- **Cursor:** keep the cursor overlay unfocused and above the grid. Bind its font and cell metrics to `baseGrid`;
  do not duplicate metric calculations.
- **Focus:** editor focus belongs to the long-lived `baseGrid`, never a Repeater delegate. Return focus through
  `Shell.focusGrid()` when chat closes. `Component.onCompleted` cannot restore focus after an item is destroyed.
- **Notifications:** use `Connections` and property bindings for updates, not imperative polling of the connector.
- **Reactive delegates:** when a per-ID model needs field updates, expose a stable QObject proxy with NOTIFY properties.
  Never refresh a Repeater with `model = null; model = list`; preserve identity, focus, caches, and blink phase.
- **Readiness:** preserve `Main.qml`'s palette-validity and startup-error gates when changing visibility or layout.
