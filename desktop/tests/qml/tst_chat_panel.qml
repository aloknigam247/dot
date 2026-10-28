import QtQuick
import QtTest
import Dot 1.0

// Tier 3 — ChatPanel / ChatModel in isolation. Each test gets a fresh panel via
// createTemporaryObject so model state never leaks between cases.
TestCase {
    name: "ChatPanel"
    width: 400
    height: 500
    visible: true
    when: windowShown

    Component {
        id: panelComp
        ChatPanel {
            anchors.fill: parent
            metrics: DesktopMetrics { grid: GridItem {} }
        }
    }

    Component {
        id: mainComp
        Main { visible: true }
    }

    Component {
        id: fontSpyComp
        SignalSpy { signalName: "fontChanged" }
    }

    SignalSpy {
        id: closedSpy
        signalName: "closed"
    }

    function make() {
        var p = createTemporaryObject(panelComp, this)
        verify(p !== null)
        return p
    }

    function makeMain() {
        const main = createTemporaryObject(mainComp, null)
        verify(main !== null)
        const panel = findChild(main, "chatPanel")
        const shell = findChild(main, "shell")
        const toggle = findChild(main, "chatToggle")
        verify(panel !== null && shell !== null && toggle !== null)
        verify(shell.metrics !== null && shell.metrics.grid !== null)
        return {
            grid: shell.metrics.grid,
            icon: findChild(main, "chatIcon"),
            input: panel.inputField,
            metrics: shell.metrics,
            panel: panel,
            shell: shell,
            toggle: toggle,
            window: main
        }
    }

    function init() {
        failOnWarning(/TypeError|ReferenceError|Binding loop|Unable to assign/)
        $connector.onOptionSet("linespace", 0)
    }

    function test_hidden_by_default() {
        var p = make()
        verify(!p.visible)
        compare(p.model.count, 0)
    }

    function test_open_focuses_input_close_hides() {
        var p = make()
        closedSpy.clear()
        closedSpy.target = p

        p.open()
        verify(p.visible)
        tryVerify(function() { return p.inputField.activeFocus })

        p.requestClose()
        verify(!p.visible)
        compare(closedSpy.count, 1)
    }

    function test_append_block_adds_external_messages() {
        var p = make()
        p.model.appendBlock("user", "hello")
        p.model.appendBlock("assistant", "reply")

        compare(p.model.count, 2)
        compare(p.model.authorAt(0), "user")
        compare(p.model.textAt(0), "hello")
        compare(p.model.authorAt(1), "assistant")
        compare(p.model.textAt(1), "reply")
    }

    function test_empty_append_block_is_ignored() {
        var p = make()
        p.model.appendBlock("assistant", "")
        compare(p.model.count, 0)
    }

    function test_geometry_does_not_track_font() {
        const ui = makeMain()
        verify(ui.icon !== null)
        for (const size of [6, 12, 24]) {
            ui.grid.fontSize = size
            compare(ui.metrics.cornerRadius, 4)
            compare(ui.metrics.padding, 4)
            compare(ui.input.padding, 4)
            compare(ui.input.parent.radius, 4)
            compare(ui.toggle.cornerRadius, 4)
            compare(ui.toggle.padding, 4)
            compare(ui.icon.cornerRadius, 4)
            compare(ui.icon.padding, 4)
            compare(ui.icon.x, 0)
            compare(ui.icon.y, 0)
            compare(ui.icon.width, ui.toggle.width)
            compare(ui.icon.height, ui.toggle.height)
            compare(ui.icon.strokeWidth, 1.5)
            verify(ui.icon.symbolSize > 0)
            verify(ui.icon.symbolSize <= ui.icon.width - 2 * ui.icon.padding)
            verify(ui.icon.symbolSize <= ui.icon.height - 2 * ui.icon.padding)
        }
        compare(ui.window.screen.devicePixelRatio, $expectedScaleFactor)
    }

    function test_metrics_survive_detach_and_reattach() {
        const ui = makeMain()
        const fontSpy = createTemporaryObject(fontSpyComp, this, {target: ui.grid})
        verify(fontSpy !== null)
        const rowHeight = ui.metrics.rowHeight
        const cellWidth = ui.metrics.cellWidth
        const parent = ui.grid.parent

        ui.grid.parent = null
        compare(fontSpy.count, 0)
        compare(ui.metrics.rowHeight, rowHeight)
        compare(ui.metrics.cellWidth, cellWidth)
        compare(ui.panel.metrics, ui.metrics)
        compare(ui.toggle.metrics, ui.metrics)

        ui.grid.parent = parent
        verify(fontSpy.count > 0)
        compare(ui.metrics.rowHeight, rowHeight)
        compare(ui.metrics.cellWidth, cellWidth)
        compare(ui.metrics.grid, ui.grid)
    }

    function test_shared_row_tracks_font() {
        const ui = makeMain()
        compare(ui.panel.metrics, ui.metrics)
        compare(ui.toggle.metrics, ui.metrics)
        ui.grid.fontSize = 12
        const initialRow = ui.metrics.rowHeight

        for (const size of [12, 24]) {
            ui.grid.fontSize = size
            compare(ui.input.font.family, ui.grid.fontName)
            compare(ui.input.font.pointSize, size)
            compare(ui.input.font.letterSpacing, ui.metrics.textFont.letterSpacing)
            compare(ui.input.height - 2 * ui.input.padding, ui.grid.cellHeight)
            compare(ui.toggle.height - 2 * ui.toggle.padding, ui.grid.cellHeight)
            compare(ui.toggle.width, ui.toggle.height)
            compare(ui.metrics.cellWidth, ui.grid.cellWidth)
            compare(ui.metrics.cellBaseline, ui.grid.cellBaseline)
        }
        verify(ui.metrics.rowHeight > initialRow)
        const fontRow = ui.metrics.rowHeight
        $connector.onOptionSet("linespace", 6)
        verify(ui.metrics.rowHeight > fontRow)
        compare(ui.input.height - 2 * ui.input.padding, ui.grid.cellHeight)
        compare(ui.toggle.height - 2 * ui.toggle.padding, ui.grid.cellHeight)
        compare(ui.input.font.pointSize, 24)
        $connector.onOptionSet("linespace", 0)
        compare(ui.metrics.rowHeight, fontRow)
    }

    function test_startup_metrics_match_window_dpr() {
        const ui = makeMain()
        const cellWidth = ui.metrics.cellWidth
        const fontSize = ui.grid.fontSize
        const rowHeight = ui.metrics.rowHeight
        compare(ui.window.screen.devicePixelRatio, $expectedScaleFactor)
        verify(cellWidth > 0 && rowHeight > 0)
        verify(ui.input.height > 0 && ui.toggle.width > 0 && ui.toggle.height > 0)

        ui.grid.fontSize = fontSize + 1
        ui.grid.fontSize = fontSize
        compare(ui.metrics.cellWidth, cellWidth)
        compare(ui.metrics.rowHeight, rowHeight)
        compare(ui.input.height - 2 * ui.input.padding, rowHeight)
        compare(ui.toggle.height - 2 * ui.toggle.padding, rowHeight)
    }

    function test_teardown_keeps_bindings_valid() {
        const ui = makeMain()
        ui.window.close()
        ui.window.destroy()
        wait(0)
    }
}
