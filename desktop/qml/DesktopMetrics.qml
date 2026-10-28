import QtQuick
import Dot 1.0

QtObject {
    required property GridItem grid

    readonly property real cellBaseline: grid.cellBaseline
    readonly property real cellWidth: grid.cellWidth
    readonly property real cornerRadius: 4
    readonly property real padding: 4
    readonly property real rowHeight: grid.cellHeight
    readonly property font textFont: grid.textFont

    Component.onCompleted: {
        if (!grid)
            throw new Error("DesktopMetrics requires a live GridItem")
    }
}
