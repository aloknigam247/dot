import QtQuick
import Dot 1.0

QtObject {
    required property GridItem grid

    readonly property real cellBaseline: grid.cellBaseline
    readonly property real cellWidth: grid.cellWidth
    readonly property real cornerRadius: 4
    readonly property real cursorCornerRadius: 2
    readonly property real frameGap: 4
    readonly property real iconSize: textFont.pixelSize * 6 / 7
    readonly property real nestedGap: 2
    readonly property real padding: 4
    readonly property real paneCornerRadius: 6
    readonly property real rowHeight: grid.cellHeight
    readonly property font textFont: grid.textFont
    readonly property real textInset: nestedGap + padding
    readonly property real topBandHeight: bandHeight()

    function bandHeight(): real {
        return grid.cellHeight + 2 * padding
    }

    function gridOrigin(): point {
        const dpr = grid.metricsDpr
        const inset = frameGap + textInset
        return Qt.point(Math.ceil(inset * dpr) / dpr, Math.ceil((inset + bandHeight()) * dpr) / dpr)
    }

    function chatAllocation(windowWidth: real, chatVisible: bool): int {
        return chatVisible ? Math.min(360, Math.floor(windowWidth / 2)) : 0
    }

    function windowWidthForPaneExtent(extent: int, chatVisible: bool): int {
        return chatVisible ? extent + Math.min(360, extent) : extent
    }

    function editorPaneRect(windowWidth: real, windowHeight: real, chatVisible: bool): rect {
        const top = frameGap + bandHeight()
        return Qt.rect(frameGap, top, Math.max(0, windowWidth - 2 * frameGap - chatAllocation(windowWidth, chatVisible)),
                       Math.max(0, windowHeight - top - frameGap))
    }

    function chatPaneRect(windowWidth: real, windowHeight: real, chatVisible: bool): rect {
        const allocation = chatAllocation(windowWidth, chatVisible)
        const top = frameGap + bandHeight()
        return Qt.rect(windowWidth - allocation, top, Math.max(0, allocation - frameGap),
                       Math.max(0, windowHeight - top - frameGap))
    }

    function gridRectForWindow(windowWidth: real, windowHeight: real, chatVisible: bool): rect {
        const inset = frameGap + textInset
        const origin = gridOrigin()
        return Qt.rect(origin.x, origin.y,
                       Math.max(0, windowWidth - chatAllocation(windowWidth, chatVisible) - inset - origin.x),
                       Math.max(0, windowHeight - inset - origin.y))
    }

    function cellsForWindow(windowWidth: real, windowHeight: real, chatVisible: bool): size {
        const bounds = gridRectForWindow(windowWidth, windowHeight, chatVisible)
        return grid.cellsForSize(Qt.size(bounds.width, bounds.height))
    }

    function windowSizeForGrid(cols: int, rows: int, chatVisible: bool): size {
        const inset = frameGap + textInset
        const origin = gridOrigin()
        const extent = Math.ceil(cols * grid.cellWidth + origin.x + inset)
        return Qt.size(windowWidthForPaneExtent(extent, chatVisible),
                       Math.ceil(rows * grid.cellHeight + origin.y + inset))
    }

    Component.onCompleted: {
        if (!grid)
            throw new Error("DesktopMetrics requires a live GridItem")
    }
}
