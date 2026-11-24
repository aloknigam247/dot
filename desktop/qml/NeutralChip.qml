import QtQuick

NeutralControl {
    id: chip

    property string text

    accessibleName: text
    implicitWidth: label.implicitWidth + 2 * padding

    Text {
        id: label
        anchors.centerIn: parent
        color: chip.foreground
        font: chip.metrics.textFont
        text: chip.text
    }
}
