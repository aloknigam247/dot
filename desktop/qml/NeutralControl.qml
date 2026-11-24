import QtQuick

Item {
    id: control

    required property DesktopMetrics metrics

    property string accessibleName
    property QtObject colors: $connector.colorPalette
    default property alias content: contentItem.data
    property bool interactive: true
    property bool selectable: false
    property bool selected: false
    property bool transparentRest: false

    readonly property real cornerRadius: metrics.cornerRadius
    readonly property color fill: !enabled ? (transparentRest ? "transparent" : colors.resting)
                                : _isSelected ? colors.selection
                                : (hovered || pressed) ? colors.raised
                                : (transparentRest ? "transparent" : colors.resting)
    readonly property color foreground: !enabled ? colors.mutedText
                                      : _isSelected ? colors.selectedText
                                      : colors.text
    readonly property bool hovered: _live && pointerArea.containsMouse
    readonly property real padding: metrics.padding
    readonly property bool pressed: _live && (pointerArea.pressed || _keyPressed)

    readonly property alias _content: contentItem
    readonly property bool _isSelected: interactive && selectable && selected
    property bool _keyPressed: false
    readonly property bool _live: enabled && interactive

    signal activated()

    function activate() {
        if (!_live) return
        activated()
    }

    // The Button role already advertises Press. Use Qt Quick's action hook without adding a second Press action.
    function accessiblePressAction() { activate() }

    activeFocusOnTab: interactive
    implicitHeight: metrics.rowHeight + 2 * padding

    Accessible.checkable: interactive && selectable
    Accessible.checked: _isSelected
    Accessible.focusable: activeFocusOnTab
    Accessible.name: accessibleName
    Accessible.role: interactive ? Accessible.Button : Accessible.StaticText

    Keys.enabled: _live
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Space) {
            event.accepted = true
            if (!event.isAutoRepeat) _keyPressed = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            event.accepted = true
            if (!event.isAutoRepeat) activate()
        }
    }
    Keys.onReleased: function(event) {
        if (event.key !== Qt.Key_Space) return
        event.accepted = true
        if (event.isAutoRepeat || !_keyPressed) return
        _keyPressed = false
        activate()
    }

    onActiveFocusChanged: if (!activeFocus) _keyPressed = false
    on_LiveChanged: if (!_live) _keyPressed = false

    Rectangle {
        anchors.fill: parent
        color: control.fill
        radius: control.cornerRadius
    }

    Item {
        id: contentItem
        anchors.fill: parent
    }

    MouseArea {
        id: pointerArea
        anchors.fill: parent
        enabled: control.interactive
        hoverEnabled: true
        onClicked: control.activate()
    }
}
