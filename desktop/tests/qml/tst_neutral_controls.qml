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

    readonly property var chrome: $connector.colorPalette

    DesktopMetrics {
        id: sharedMetrics
        grid: GridItem {}
    }

    Component {
        id: buttonComp
        NeutralButton {
            x: 50
            y: 50
            Accessible.name: "Action"
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
        id: focusComp
        Item {
            activeFocusOnTab: true
            height: 10
            width: 10
        }
    }

    Component {
        id: metricsComp
        TextMetrics { font: sharedMetrics.textFont }
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

    function checkStateColors(component, extra, data, restFill) {
        const props = Object.assign({enabled: data.enabled, selectable: data.selectable,
                                     selected: data.selected}, extra)
        mouseMove(testCase, 1, 1)
        const ui = make(component, props)
        const control = ui.control
        if (data.hover)
            mouseMove(control, control.width / 2, control.height / 2)
        if (data.press)
            mousePress(control)
        const expectedFill = data.fill === "resting" ? restFill : chrome[data.fill]
        const expectedForeground = chrome[data.foreground]
        const background = findChild(control, "background")
        verify(background !== null)
        verify(Qt.colorEqual(control.fill, expectedFill),
               `fill ${control.fill} != ${expectedFill} (${data.fill})`)
        verify(Qt.colorEqual(background.color, expectedFill),
               `background ${background.color} != ${expectedFill} (${data.fill})`)
        verify(Qt.colorEqual(control.foreground, expectedForeground),
               `foreground ${control.foreground} != ${expectedForeground} (${data.foreground})`)
        const label = findChild(control, "label")
        if (label !== null)
            verify(Qt.colorEqual(label.color, expectedForeground),
                   `label ${label.color} != ${expectedForeground} (${data.foreground})`)
        compare(control.hovered, data.enabled && (data.hover || data.press))
        compare(control.pressed, data.enabled && data.press)
        if (data.press) {
            mouseMove(control, -10, -10)
            mouseRelease(control, -10, -10)
        }
        compare(ui.spy.count, 0)
        control.visible = false
    }

    function test_palette_roles_are_distinct() {
        const roles = ["mutedText", "raised", "resting", "selectedText", "selection", "text"]
        for (let i = 0; i < roles.length; ++i)
            for (let j = i + 1; j < roles.length; ++j)
                verify(!Qt.colorEqual(chrome[roles[i]], chrome[roles[j]]), `${roles[i]} == ${roles[j]}`)
    }

    function test_button_state_colors_data() { return stateRows() }

    function test_button_state_colors(data) {
        checkStateColors(buttonComp, {iconOnly: false}, data, chrome.resting)
        checkStateColors(buttonComp, {iconOnly: true}, data, "transparent")
    }

    function test_chip_state_colors_data() { return stateRows() }

    function test_chip_state_colors(data) {
        checkStateColors(chipComp, {}, data, chrome.resting)
    }

    function test_centered_button_content() {
        const button = make(textButtonComp, {}).control
        const label = findChild(button, "label")
        verify(label !== null)
        compare(button.width, label.implicitWidth + 2 * button.padding)
        label.text += " with a longer label"
        compare(button.width, label.implicitWidth + 2 * button.padding)
    }

    function test_default_geometry_is_one_row() {
        const row = sharedMetrics.rowHeight
        const button = make(buttonComp, {}).control
        compare(button.height, row)
        compare(make(chipComp, {}).control.height, row)
        const icon = make(buttonComp, {iconOnly: true}).control
        compare(icon.width, row)
        compare(icon.height, row)
    }

    function test_chip_elides_overflowing_label() {
        const chip = make(chipComp, {text: "A chip label that is far too long to fit"}).control
        const label = findChild(chip, "label")
        verify(label !== null)
        const fullWidth = chip.implicitWidth
        compare(chip.width, fullWidth)
        verify(!label.truncated)

        chip.width = fullWidth / 2
        verify(label.truncated)
        compare(label.x, chip.padding)
        compare(label.width, chip.width - 2 * chip.padding)
        verify(label.contentWidth <= label.width)
        compare(chip.implicitWidth, fullWidth)
        compare(chip.Accessible.name, "A chip label that is far too long to fit")
    }

    function test_chip_label_is_literal_text() {
        const markup = "<b>main</b>"
        const chip = make(chipComp, {text: markup}).control
        const label = findChild(chip, "label")
        verify(label !== null)
        const literal = createTemporaryObject(metricsComp, testCase, {text: markup})
        verify(literal !== null)
        verify(Math.abs(label.contentWidth - literal.advanceWidth) < 1)
        compare(chip.Accessible.name, markup)
    }

    function test_descriptive_chip() {
        const ui = make(chipComp, {interactive: false, selectable: true, selected: true})
        const chip = ui.control
        compare(chip.Accessible.role, Accessible.StaticText)
        verify(!chip.Accessible.checkable)
        verify(!chip.Accessible.checked)
        verify(!chip.activeFocusOnTab)
        mouseMove(chip, chip.width / 2, chip.height / 2)
        verify(!chip.hovered)
        mousePress(chip)
        verify(!chip.pressed)
        verify(Qt.colorEqual(chip.fill, chrome.resting))
        mouseRelease(chip)
        mouseClick(chip)
        chip.forceActiveFocus()
        keyClick(Qt.Key_Space)
        keyClick(Qt.Key_Return)
        chip.activate()
        verify(Qt.colorEqual(chip.fill, chrome.resting))
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
        const ui = make(buttonComp, {})
        const button = ui.control
        button.forceActiveFocus()
        tryVerify(function() { return button.activeFocus })

        keyPress(Qt.Key_Space)
        verify(button.pressed)
        verify(!button.hovered)
        verify(Qt.colorEqual(button.fill, chrome.raised))
        compare(ui.spy.count, 0)
        keyRelease(Qt.Key_Space)
        verify(!button.pressed)
        verify(Qt.colorEqual(button.fill, chrome.resting))
        compare(ui.spy.count, 1)

        keyClick(Qt.Key_Return)
        compare(ui.spy.count, 2)
        keyClick(Qt.Key_Enter)
        compare(ui.spy.count, 3)
    }

    function test_key_repeat_activates_once_data() {
        return [{tag: "space", key: Qt.Key_Space, onPress: 0},
                {tag: "return", key: Qt.Key_Return, onPress: 1},
                {tag: "enter", key: Qt.Key_Enter, onPress: 1}]
    }

    function test_key_repeat_activates_once(data) {
        const ui = make(buttonComp, {})
        const button = ui.control
        button.forceActiveFocus()
        tryVerify(function() { return button.activeFocus })

        keyPress(data.key)
        compare(ui.spy.count, data.onPress)
        for (let i = 0; i < 3; ++i) {
            $keys.repeatKey(testCase.Window.window, data.key)
            compare(ui.spy.count, data.onPress)
        }
        compare(button.pressed, data.key === Qt.Key_Space)
        keyRelease(data.key)
        verify(!button.pressed)
        compare(ui.spy.count, 1)
    }

    function test_interrupted_space_press_does_not_activate_data() {
        return [{tag: "focus_loss"}, {tag: "interactive"}, {tag: "enabled"}]
    }

    function test_interrupted_space_press_does_not_activate(data) {
        const ui = make(buttonComp, {})
        const button = ui.control
        const other = createTemporaryObject(focusComp, testCase)
        verify(other !== null)
        button.forceActiveFocus()
        tryVerify(function() { return button.activeFocus })

        keyPress(Qt.Key_Space)
        verify(button.pressed)
        if (data.tag === "focus_loss") {
            other.forceActiveFocus()
            tryVerify(function() { return !button.activeFocus })
        } else if (data.tag === "interactive") {
            // Qt refuses to clear activeFocusOnTab while the item holds focus.
            ignoreWarning(/Cannot set activeFocusOnTab to false/)
            button.interactive = false
        } else {
            button.enabled = false
        }
        verify(!button.pressed)

        button.interactive = true
        button.enabled = true
        button.forceActiveFocus()
        tryVerify(function() { return button.activeFocus })
        keyRelease(Qt.Key_Space)
        verify(!button.pressed)
        compare(ui.spy.count, 0)

        keyClick(Qt.Key_Space)
        compare(ui.spy.count, 1)
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
}
