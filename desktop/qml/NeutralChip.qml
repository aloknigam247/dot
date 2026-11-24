import QtQuick

NeutralControl {
    id: chip

    property string text

    Accessible.name: text
    implicitWidth: label.implicitWidth + 2 * padding

    Text {
        id: label
        objectName: "label"
        anchors.fill: parent
        anchors.leftMargin: chip.padding
        anchors.rightMargin: chip.padding
        color: chip.foreground
        elide: Text.ElideRight
        font: chip.metrics.textFont
        horizontalAlignment: Text.AlignHCenter
        text: chip.text
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignVCenter
    }
}
