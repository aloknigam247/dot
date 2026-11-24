import QtQuick
import QtTest
import Dot 1.0

// Tier 3 — NeutralButton / NeutralChip state colors, input, and accessibility.
TestCase {
    id: testCase
    name: "NeutralControls"
    width: 400
    height: 300
    visible: true
    when: windowShown

    QtObject {
        id: darkPalette
        readonly property color mutedText: "#808080"
        readonly property color raised: "#303030"
        readonly property color resting: "#202020"
        readonly property color selectedText: "#ffffff"
        readonly property color selection: "#264f78"
        readonly property color text: "#d4d4d4"
    }

    QtObject {
        id: lightPalette
        readonly property color mutedText: "#6e6e6e"
        readonly property color raised: "#e5e5e5"
        readonly property color resting: "#f3f3f3"
        readonly property color selectedText: "#000000"
        readonly property color selection: "#cce4f7"
        readonly property color text: "#1e1e1e"
    }

    DesktopMetrics {
        id: sharedMetrics
        grid: GridItem {}
    }

    Component {
        id: buttonComp
        NeutralButton {
            x: 50
            y: 50
            accessibleName: "Action"
            metrics: sharedMetrics
        }
    }

    Component {
        id: chipComp
        NeutralChip {
            x: 50
            y: 50
            metrics: sharedMetrics
            text: "Chip"
        }
    }

    Component {
        id: spyComp
        SignalSpy { signalName: "activated" }
    }

    Component {
        id: textButtonComp
        NeutralButton {
            metrics: sharedMetrics
            Text {
                objectName: "label"
                anchors.centerIn: parent
                font: sharedMetrics.textFont
                text: "Centered button"
            }
        }
    }

    function init() {
        failOnWarning(/TypeError|ReferenceError|Binding loop|Unable to assign/)
        mouseMove(testCase, 1, 1)
    }

    function make(component, properties) {
        const control = createTemporaryObject(component, testCase, properties)
        verify(control !== null)
        const spy = createTemporaryObject(spyComp, testCase, {target: control})
        verify(spy !== null)
        return {control: control, spy: spy}
    }

    function stateRows() {
        return [
            {tag: "rest", enabled: true, fill: "resting", foreground: "text", hover: false, press: false,
             selectable: true, selected: false},
            {tag: "hovered", enabled: true, fill: "raised", foreground: "text", hover: true, press: false,
             selectable: true, selected: false},
            {tag: "pressed", enabled: true, fill: "raised", foreground: "text", hover: false, press: true,
             selectable: true, selected: false},
            {tag: "selected", enabled: true, fill: "selection", foreground: "selectedText", hover: false,
             press: false, selectable: true, selected: true},
            {tag: "selected_hovered", enabled: true, fill: "selection", foreground: "selectedText", hover: true,
             press: false, selectable: true, selected: true},
            {tag: "disabled", enabled: false, fill: "resting", foreground: "mutedText", hover: true, press: true,
             selectable: true, selected: false},
            {tag: "disabled_selected", enabled: false, fill: "resting", foreground: "mutedText", hover: true,
             press: true, selectable: true, selected: true},
            {tag: "unselectable_selected", enabled: true, fill: "resting", foreground: "text", hover: false,
             press: false, selectable: false, selected: true}
        ]
    }

    function checkStateColors(component, extra, data, palette, restFill) {
        const props = Object.assign({colors: palette, enabled: data.enabled, selectable: data.selectable,
                                     selected: data.selected}, extra)
        mouseMove(testCase, 1, 1)
        const ui = make(component, props)
        const control = ui.control
        if (data.hover)
            mouseMove(control, control.width / 2, control.height / 2)
        if (data.press)
            mousePress(control)
        const expectedFill = data.fill === "resting" ? restFill : palette[data.fill]
        verify(Qt.colorEqual(control.fill, expectedFill),
               `fill ${control.fill} != ${expectedFill} (${data.fill})`)
        verify(Qt.colorEqual(control.foreground, palette[data.foreground]),
               `foreground ${control.foreground} != ${palette[data.foreground]} (${data.foreground})`)
        compare(control.hovered, data.enabled && (data.hover || data.press))
        compare(control.pressed, data.enabled && data.press)
        if (data.press) {
            mouseMove(control, -10, -10)
            mouseRelease(control, -10, -10)
        }
        compare(ui.spy.count, 0)
        control.visible = false
    }

    function test_button_state_colors_data() { return stateRows() }

    function test_button_state_colors(data) {
        for (const palette of [darkPalette, lightPalette]) {
            checkStateColors(buttonComp, {iconOnly: false}, data, palette, palette.resting)
            checkStateColors(buttonComp, {iconOnly: true}, data, palette, "transparent")
        }
    }

    function test_chip_state_colors_data() { return stateRows() }

    function test_chip_state_colors(data) {
        for (const palette of [darkPalette, lightPalette])
            checkStateColors(chipComp, {}, data, palette, palette.resting)
    }

    function test_centered_button_content() {
        const button = make(textButtonComp, {}).control
        const label = findChild(button, "label")
        verify(label !== null)
        compare(button.width, label.implicitWidth + 2 * button.padding)
        label.text += " with a longer label"
        compare(button.width, label.implicitWidth + 2 * button.padding)
    }

    function test_descriptive_chip() {
        const ui = make(chipComp, {colors: darkPalette, interactive: false, selectable: true, selected: true})
        const chip = ui.control
        compare(chip.Accessible.role, Accessible.StaticText)
        verify(!chip.Accessible.checkable)
        verify(!chip.Accessible.checked)
        verify(!chip.activeFocusOnTab)
        mouseMove(chip, chip.width / 2, chip.height / 2)
        verify(!chip.hovered)
        mousePress(chip)
        verify(!chip.pressed)
        verify(Qt.colorEqual(chip.fill, darkPalette.resting))
        mouseRelease(chip)
        mouseClick(chip)
        chip.forceActiveFocus()
        keyClick(Qt.Key_Space)
        keyClick(Qt.Key_Return)
        chip.activate()
        verify(Qt.colorEqual(chip.fill, darkPalette.resting))
        compare(ui.spy.count, 0)
    }

    function test_pointer_activation() {
        const ui = make(buttonComp, {})
        const button = ui.control
        mouseClick(button)
        compare(ui.spy.count, 1)

        mousePress(button)
        verify(button.pressed)
        mouseMove(button, -10, -10)
        mouseRelease(button, -10, -10)
        verify(!button.pressed)
        compare(ui.spy.count, 1)
        verify(!button.activeFocus)
    }

    function test_keyboard_activation() {
        const ui = make(buttonComp, {colors: darkPalette})
        const button = ui.control
        button.forceActiveFocus()
        tryVerify(function() { return button.activeFocus })

        keyPress(Qt.Key_Space)
        verify(button.pressed)
        verify(!button.hovered)
        verify(Qt.colorEqual(button.fill, darkPalette.raised))
        compare(ui.spy.count, 0)
        keyRelease(Qt.Key_Space)
        verify(!button.pressed)
        verify(Qt.colorEqual(button.fill, darkPalette.resting))
        compare(ui.spy.count, 1)

        keyClick(Qt.Key_Return)
        compare(ui.spy.count, 2)
        keyClick(Qt.Key_Enter)
        compare(ui.spy.count, 3)
    }

    function test_disabled_blocks_activation() {
        const ui = make(buttonComp, {})
        const button = ui.control
        button.forceActiveFocus()
        tryVerify(function() { return button.activeFocus })
        button.enabled = false

        mousePress(button)
        verify(!button.pressed)
        mouseRelease(button)
        mouseClick(button)
        keyPress(Qt.Key_Space)
        verify(!button.pressed)
        keyRelease(Qt.Key_Space)
        keyClick(Qt.Key_Return)
        keyClick(Qt.Key_Enter)
        button.activate()
        verify(!button.pressed)
        compare(ui.spy.count, 0)
    }

    function test_accessibility_properties() {
        const plain = make(buttonComp, {}).control
        compare(plain.Accessible.name, "Action")
        compare(plain.Accessible.role, Accessible.Button)
        verify(!plain.Accessible.checkable)
        verify(!plain.Accessible.checked)

        const unselectable = make(buttonComp, {selected: true}).control
        verify(!unselectable.Accessible.checkable)
        verify(!unselectable.Accessible.checked)

        const toggle = make(buttonComp, {selectable: true}).control
        verify(toggle.Accessible.checkable)
        verify(!toggle.Accessible.checked)
        toggle.selected = true
        verify(toggle.Accessible.checked)

        const chip = make(chipComp, {}).control
        compare(chip.Accessible.name, "Chip")
        compare(chip.Accessible.role, Accessible.Button)
    }

    function test_icon_size_tracks_font() {
        const grid = sharedMetrics.grid
        grid.fontSize = 12
        compare(sharedMetrics.iconSize, grid.textFont.pixelSize * 6 / 7)
        const small = sharedMetrics.iconSize
        grid.fontSize = 24
        compare(sharedMetrics.iconSize, grid.textFont.pixelSize * 6 / 7)
        verify(sharedMetrics.iconSize > small)
    }
}
