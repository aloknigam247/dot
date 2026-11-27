import QtQuick
import Dot 1.0

Item {
    id: panel
    required property NvimConnector connector
    required property DesktopMetrics metrics
    required property rect paneBounds
    required property bool paneVisible

    readonly property Item activeRow: levels.count ? levels.itemAt(levels.count - 1) : null
    readonly property bool available: paneVisible && colors.overlayValid && colors.chromeValid && colors.cursorValid
    readonly property var blockStyles: connector.cmdline.blockLines.map(line => styles(line, styleRevision))
    readonly property var blockLines: blockStyles.map(line => plain(line))
    readonly property color caretColor: colors.cursorBackground
    readonly property rect caretRect: activeRow ? input.cursorRectangle : Qt.rect(0, 0, 0, 0)
    readonly property string caretShape: ["block", "horizontal", "vertical"][connector.modeInfo.cursorShape]
    readonly property var chunkStyles: {
        if (!activeRow) return []
        const spans = styles(activeRow.chunks, styleRevision)
        const mark = activeRow.specialCharacter
        if (!mark) return spans
        let offset = activeRow.cursorTextOffset
        for (const span of spans) {
            if (offset < span.text.length) {
                const skip = activeRow.specialShift ? 0 : characterAt(span.text, offset).length
                span.text = span.text.slice(0, offset) + mark + span.text.slice(offset + skip)
                return spans
            }
            offset -= span.text.length
        }
        spans.push(style(0, mark))
        return spans
    }
    readonly property string displayedText: indentText + plain(chunkStyles)
    readonly property string kindMark: activeRow ? activeRow.firstCharacter || activeRow.prompt : ""
    readonly property int styleRevision: refresh.revision

    readonly property ColorPalette colors: connector.colorPalette
    readonly property int cursorPosition: kindMark.length + indentText.length
                                          + (activeRow ? activeRow.cursorTextOffset : 0)
    readonly property string indentText: activeRow ? " ".repeat(activeRow.indent) : ""
    readonly property real maximumHeight: Math.max(0, paneBounds.y + paneBounds.height - metrics.textInset - y)

    x: paneBounds.x + metrics.textInset
    y: paneBounds.y + 0.20 * paneBounds.height
    width: Math.max(0, paneBounds.width - 2 * metrics.textInset)
    height: Math.min(maximumHeight, field.height + (field.visible ? 2 * metrics.nestedGap : 0)
                     + (connector.cmdline.blockActive ? blockContent.height + 2 * metrics.padding : 0))
    visible: available && (connector.cmdline.activeLevel > 0 || connector.cmdline.blockActive)
    Accessible.name: kindMark + displayedText
    Accessible.role: Accessible.StaticText

    function characterAt(text, offset) {
        return offset < text.length ? String.fromCodePoint(text.codePointAt(offset)) : ""
    }
    function escapeHtml(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }
    function style(id, text) {
        const attr = connector.highlights.attributeMap(id)
        const bg = attr.background ?? colors.overlayBackground
        const fg = attr.foreground ?? colors.overlayForeground
        return {text: text, background: attr.reverse ? fg : bg, foreground: attr.reverse ? bg : fg,
                bold: attr.bold, italic: attr.italic, strikethrough: attr.strikethrough,
                underline: attr.underline || attr.undercurl}
    }
    function styles(chunks, revision) {
        return chunks.map(chunk => style(chunk.attrId, chunk.text))
    }
    function plain(spans) {
        return spans.map(span => span.text).join("")
    }
    function rich(prefix, spans) {
        let html = "<span style='white-space:pre-wrap'>" + escapeHtml(prefix)
        for (const s of spans) {
            const decoration = (s.underline ? " underline" : "") + (s.strikethrough ? " line-through" : "")
            html += "<span style='color:" + s.foreground + ";background-color:" + s.background
                  + (s.bold ? ";font-weight:bold" : "") + (s.italic ? ";font-style:italic" : "")
                  + (decoration ? ";text-decoration:" + decoration : "") + "'>" + escapeHtml(s.text) + "</span>"
        }
        return html + "</span>"
    }

    QtObject {
        id: refresh
        property int revision: 0
        property bool stale: false
        function ifStale() {
            if (stale && panel.visible) {
                stale = false
                ++revision
            }
        }
    }
    onVisibleChanged: refresh.ifStale()
    Connections {
        target: panel.connector.highlights
        function onChanged() { refresh.stale = true }
    }
    Connections {
        enabled: panel.visible
        target: panel.connector
        function onFlush() { refresh.ifStale() }
    }
    Repeater {
        id: levels
        model: panel.connector.cmdline
        delegate: Item {
            required property var chunks
            required property int cursorTextOffset
            required property string firstCharacter
            required property int indent
            required property int index
            required property string prompt
            required property string specialCharacter
            required property bool specialShift
            visible: index === levels.count - 1
        }
    }
    Repeater {
        model: 3
        delegate: Rectangle {
            required property int index
            readonly property int spread: 3 - index
            objectName: "shadowLayer"
            anchors.fill: parent
            anchors.margins: -spread
            anchors.topMargin: 1 - spread
            anchors.bottomMargin: -1 - spread
            color: Qt.rgba(0, 0, 0, 0.04 * (index + 1))
            radius: panel.metrics.paneCornerRadius + spread
        }
    }
    Rectangle {
        objectName: "panelBackground"
        anchors.fill: parent
        border.width: 0
        color: panel.colors.overlayBackground
        radius: panel.metrics.paneCornerRadius
    }
    Item {
        id: blockArea
        objectName: "blockArea"
        x: panel.metrics.padding
        y: panel.metrics.padding
        width: Math.max(0, panel.width - 2 * x)
        height: Math.max(0, (field.visible ? field.y - panel.metrics.nestedGap : panel.height) - 2 * y)
        clip: true
        Column {
            id: blockContent
            anchors.bottom: parent.bottom
            width: parent.width
            Repeater {
                model: panel.blockStyles
                delegate: Text {
                    required property var modelData
                    objectName: "blockLine"
                    width: blockContent.width
                    color: panel.colors.overlayForeground
                    font: panel.metrics.textFont
                    text: panel.rich("", modelData)
                    textFormat: Text.RichText
                    wrapMode: Text.WrapAnywhere
                }
            }
        }
    }
    Rectangle {
        id: field
        objectName: "commandField"
        anchors.bottom: parent.bottom
        anchors.bottomMargin: panel.metrics.nestedGap
        x: panel.metrics.nestedGap
        width: Math.max(0, panel.width - 2 * x)
        height: visible ? Math.max(0, Math.min(panel.maximumHeight - 2 * x,
                                               input.contentHeight + 2 * panel.metrics.padding)) : 0
        color: panel.colors.overlayBackground
        border.color: panel.colors.accent
        radius: panel.metrics.cornerRadius
        visible: panel.activeRow !== null

        Item {
            id: viewport
            anchors.fill: parent
            anchors.margins: panel.metrics.padding
            clip: true
            TextEdit {
                id: input
                objectName: "commandLayout"
                y: -Math.max(0, panel.caretRect.y + panel.caretRect.height - viewport.height)
                width: Math.max(0, viewport.width)
                color: panel.colors.overlayForeground
                font: panel.metrics.textFont
                text: panel.rich(panel.kindMark + panel.indentText, panel.chunkStyles)
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.WrapAnywhere
                readOnly: true
                enabled: false
                activeFocusOnPress: false
                cursorPosition: Math.min(length, panel.cursorPosition)
                cursorVisible: false
            }
            Rectangle {
                readonly property real fraction: panel.connector.modeInfo.cellPercentage / 100
                readonly property string glyph: panel.characterAt(panel.kindMark + panel.displayedText,
                                                                  panel.cursorPosition)
                readonly property real cell: {
                    const r = panel.caretRect
                    const next = input.positionToRectangle(Math.min(input.length, panel.cursorPosition + glyph.length))
                    return next.y === r.y && next.x > r.x ? next.x - r.x : panel.metrics.cellWidth
                }
                x: Math.min(panel.caretRect.x, Math.max(0, viewport.width - width))
                y: input.y + panel.caretRect.y + (panel.caretShape === "horizontal" ? panel.caretRect.height - height : 0)
                width: panel.caretShape === "vertical" ? Math.max(1, cell * fraction) : cell
                height: panel.caretShape === "horizontal" ? Math.max(1, panel.caretRect.height * fraction)
                                                          : panel.caretRect.height
                color: panel.caretColor
                radius: panel.metrics.cursorCornerRadius
                Text {
                    anchors.fill: parent
                    color: panel.colors.cursorForeground
                    font: input.font
                    text: parent.glyph
                    textFormat: Text.PlainText
                    visible: panel.caretShape === "block"
                }
            }
        }
    }
}
