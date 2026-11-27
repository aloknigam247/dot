# Desktop tests

## Index

[CMakeLists.txt](CMakeLists.txt) owns target names, tier labels, timeouts, resources, and per-test environments.
Do not infer application type or QPA requirements from a directory name.

- [integration](integration): connector/product tests; start with
  [test_attach_and_render.cpp](integration/test_attach_and_render.cpp) or
  [test_user_smoke.cpp](integration/test_user_smoke.cpp).
- [qml](qml): Qt Quick Test; start with [tst_chat_panel.qml](qml/tst_chat_panel.qml).
- [support](support): shared RPC, image, color, and rasterization helpers.
- [unit](unit): model and rendering tests; start with [test_grid_model.cpp](unit/test_grid_model.cpp) or
  [test_input_handler.cpp](unit/test_input_handler.cpp).

The `unit`, `integration`, and `qml` CTest labels define the tiers. Not every unit test is guiless, and not every
integration-labelled test starts Neovim. Integration helpers locate the nightly Neovim described in
[SETUP.md](../../SETUP.md#neovim-nightly-on-path).

## Environment and registration

The deployed headless QPA is `minimal`, not `offscreen`; QML smoke tests select software rendering before creating
windows. Font-sensitive pixel tests use their own environment: `test_pixel_snapshot` is deliberately outside the
minimal-QPA list, and image-render tests explicitly select `windows`. Preserve each target's settings.

Register C++ tests with `dot_add_test(name path)` in the appropriate tier section, maintaining alphabetical order.
Give every registered test exactly one tier label, including raw `add_test` entries.
Add tests needing the integration environment to its `foreach(t ...)` list. QML consumers must also link
`dot_libplugin` and `Qt6::Quick`. Keep performance gates serial as specified in `CMakeLists.txt`.

## Required evidence

Run CTest from `desktop\` after the [selected build](../AGENTS.md#build-and-deployment).
Use these selectors with `ctest --preset release`:

- Input encoding: `-R "test_input"`.
- Paint path, fonts, or redraw dispatch: `-R "attach_and_render|resize|insert_and_quit"`.
- QML: `-R "^test_qml"`.

For a focused model run:

```ps1
ctest --preset release --output-on-failure -R "^test_grid_model$"
```

- **Event regressions:** changes to `default_colors_set`, `flush`, `grid_line`, `grid_scroll`, `hl_attr_define`, or
  `mode_change` require a regression test in `unit` or `integration` before merging.
- **Hard assertions:** inspect the source for a failing `QVERIFY`/`QCOMPARE` on the actual feature signal.
  Warnings, soft thresholds, and unrelated pixel heuristics are not proof.
- **Input encoding:** `test_input_handler` must pass; prefer a fuzz case for changed key encoding.
- **Repeatability:** rerun every new tier-2 test 5–10 times before trusting it.
- **Visual behavior:** follow [rendered-window validation](../AGENTS.md#visual-validation). Ligature assertions must
  prove glyph substitution, not kerning or subpixel-density changes.

## Integration and smoke harness

Use `startTestNvim` from [IntegrationHelpers.h](integration/IntegrationHelpers.h); it supplies `--clean`.
Pass fixture-specific options explicitly rather than inheriting the developer's Neovim configuration.
When testing an intentionally disabled extension, assert the behavior defined by `packAttachOptions` in
`NvimConnector.cpp` and name the gate; do not silently enable it for the test.
Use `QSignalSpy` for redraw signals and `GridModel::dumpAscii()` for cell snapshots.

For a user-POV test, follow `test_user_smoke.cpp`:

1. Set `QQuickWindow::setGraphicsApi(QSGRendererInterface::Software)` before creating any window.
2. Supply the required application context objects, then load `Dot` / `Main` through `QQmlApplicationEngine`.
3. Await asynchronous Neovim state with event-processing helpers rather than fixed sleeps.
4. Send input through `QTest::keyClick(window, key)`, never directly through `conn.input()`, to exercise focus routing.
5. Capture the subtree with `QQuickItem::grabToImage()`, not `QQuickWindow::grabWindow()` under `minimal`.
   Assert a non-null image with multiple pixel colors for visibility; add feature-specific assertions separately.

For generated init fixtures, follow the [desktop operational guide](../AGENTS.md#operational-constraints).

## Deployment checks

`test_bridge_staged` checks that the Windows x64 bridge is staged beside `dot.exe`.

[check_deploy.ps1](check_deploy.ps1), registered as `test_deploy_manifest`, inspects the built executable/plugins and
QML imports without launching the app. Other tests' vcpkg search paths can conceal a missing deployed dependency.
Follow the [deployment repair rule](../AGENTS.md#build-and-deployment), not a test-expectation change.
