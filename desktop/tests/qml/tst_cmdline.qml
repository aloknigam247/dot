import QtQuick
import QtTest
import Dot 1.0

TestCase {
    id: test
    name: "CommandPanel"
    width: 440
    height: 360
    visible: true
    when: windowShown

    Component {
        id: component
        CommandPanel {
            connector: $connector
            metrics: DesktopMetrics { grid: GridItem {} }
            paneBounds: Qt.rect(8, 24, 420, 320)
            paneVisible: true
        }
    }

    function make() {
        const panel = createTemporaryObject(component, test)
        verify(panel !== null)
        return panel
    }
    function chunk(text, id = 0) { return {attrId: id, text: text} }
    function show(text, pos, indent = 0) {
        $cmdlineFixture.showCmdline(1, ":", "", [chunk(text)], pos, indent)
    }
    function init() {
        failOnWarning(/TypeError|ReferenceError|Binding loop|Unable to assign/)
    }
    function cleanup() {
        $cmdlineFixture.resetCmdline()
        for (const role of ["chrome", "cursor", "overlay"]) $cmdlineFixture.setRoleResolved(role, true)
    }

    function test_active_row_and_special_text() {
        const p = make()
        verify(!p.visible)
        show("ab", 1)
        tryCompare(p, "visible", true)
        const edit = findChild(p, "commandLayout")
        compare(p.caretRect, edit.positionToRectangle(2))
        const x = p.caretRect.x
        const rev = p.styleRevision
        show("xyz", 1, 2)
        compare(p.displayedText, "  xyz")
        compare(p.chunkStyles[0].text, "xyz")
        compare(p.caretRect, edit.positionToRectangle(4))
        verify(p.caretRect.x > x)
        $cmdlineFixture.setPosition(1, 2)
        compare(p.caretRect, edit.positionToRectangle(5))
        $cmdlineFixture.setSpecialCharacter(1, "\"", true)
        compare(p.chunkStyles[0].text, "xy\"z")
        $cmdlineFixture.setSpecialCharacter(1, "^", false)
        compare(p.chunkStyles[0].text, "xy^")
        compare(p.styleRevision, rev)
        compare(p.kindMark, ":")
        compare(p.Accessible.name, ":  xy^")
        compare(p.Accessible.role, Accessible.StaticText)
        $cmdlineFixture.showCmdline(2, "", "Name: ", [chunk("child")], 5, 0)
        compare(p.kindMark, "Name: ")
        compare(p.displayedText, "child")
        $cmdlineFixture.hideCmdline(2)
        compare(p.displayedText, "  xy^")
        $cmdlineFixture.hideCmdline(1)
        verify(!p.visible)
        compare(p.activeRow, null)
    }

    function test_geometry_and_palette() {
        const p = make()
        show("abc", 3)
        const field = findChild(p, "commandField")
        const bg = findChild(p, "panelBackground")
        compare(p.x, p.paneBounds.x + p.metrics.textInset)
        compare(p.y, p.paneBounds.y + 0.20 * p.paneBounds.height)
        compare(p.width, p.paneBounds.width - 2 * p.metrics.textInset)
        compare(bg.radius, p.metrics.paneCornerRadius)
        compare(bg.border.width, 0)
        compare(field.radius, p.metrics.cornerRadius)
        compare(field.x, p.metrics.nestedGap)
        compare(field.border.color, $connector.colorPalette.accent)
        compare(field.border.width, 1)
        const shadows = p.children.filter(i => i.objectName === "shadowLayer")
        compare(shadows.length, 3)
        for (const [i, layer] of shadows.entries()) {
            verify(p.children.indexOf(layer) < p.children.indexOf(bg))
            compare(layer.width, p.width + 2 * (3 - i))
        }
        compare(p.caretColor, $connector.colorPalette.cursorBackground)
        compare(p.caretShape, ["block", "horizontal", "vertical"][$connector.modeInfo.cursorShape])
        for (const role of ["chrome", "cursor", "overlay"]) {
            $cmdlineFixture.setRoleResolved(role, false)
            verify(!p.available && !p.visible)
            $cmdlineFixture.setRoleResolved(role, true)
            verify(p.available && p.visible)
        }
        p.paneVisible = false
        verify(!p.visible)
    }

    function test_style_updates_and_blocks() {
        const p = make()
        $cmdlineFixture.setOverlayColors("#123456", "#987654")
        $cmdlineFixture.defineHighlight(11, {reverse: true, undercurl: true})
        $cmdlineFixture.defineHighlight(12, {foreground: 0xff0123, reverse: true})
        $cmdlineFixture.showBlock([[chunk("<&", 11), chunk("  "), chunk("Q", 12)]])
        tryCompare(p, "visible", true)
        compare(p.activeRow, null)
        compare(p.blockLines, ["<&  Q"])
        compare(p.blockStyles[0][0].foreground, "#987654")
        compare(p.blockStyles[0][0].background, "#123456")
        verify(p.blockStyles[0][0].underline)
        compare(p.blockStyles[0][1].foreground, "#123456")
        compare(p.blockStyles[0][1].background, "#987654")
        compare(p.blockStyles[0][2].background, "#ff0123")
        $cmdlineFixture.showCmdline(1, "/", "", [chunk("a", 12), chunk("<b>  &")], 1, 0)
        const edit = findChild(p, "commandLayout")
        compare(edit.getText(0, edit.length), "/a<b>  &")
        compare(p.chunkStyles[1].background, "#987654")
        const rev = p.styleRevision
        $cmdlineFixture.defineHighlight(12, {foreground: 0xabcdef})
        $cmdlineFixture.defineHighlight(12, {foreground: 0xabcdef, bold: true, italic: true})
        compare(p.styleRevision, rev)
        $cmdlineFixture.emitFlush()
        compare(p.styleRevision, rev + 1)
        $cmdlineFixture.emitFlush()
        compare(p.styleRevision, rev + 1)
        compare(p.chunkStyles[0].foreground, "#abcdef")
        verify(p.chunkStyles[0].bold && p.chunkStyles[0].italic)
        p.paneVisible = false
        $cmdlineFixture.defineHighlight(12, {background: 0x455667})
        $cmdlineFixture.emitFlush()
        compare(p.styleRevision, rev + 1)
        p.paneVisible = true
        compare(p.chunkStyles[0].background, "#455667")
        $cmdlineFixture.hideBlock()
        compare(p.blockLines.length, 0)
    }

    function test_wrapped_caret_and_overflow() {
        const p = make()
        const a = "x".repeat(12)
        const b = "y".repeat(20)
        const c = "z".repeat(12)
        p.paneBounds = Qt.rect(8, 24, 28 * p.metrics.cellWidth, 240)
        $cmdlineFixture.showCmdline(1, ":", "", [chunk(a), chunk(b), chunk(c)], 32, 0)
        const edit = findChild(p, "commandLayout")
        const field = findChild(p, "commandField")
        tryVerify(() => p.caretRect.y > 0)
        compare(p.caretRect, edit.positionToRectangle(33))
        compare(edit.positionToRectangle(13).y, 0)
        verify(edit.lineCount > 1)
        const oldY = p.caretRect.y
        p.paneBounds = Qt.rect(8, 24, 15 * p.metrics.cellWidth, 240)
        tryVerify(() => p.caretRect.y > oldY)
        compare(p.caretRect, edit.positionToRectangle(33))
        const narrowerY = p.caretRect.y
        p.metrics.grid.fontSize *= 1.5
        tryVerify(() => p.caretRect.y > narrowerY)
        compare(p.caretRect, edit.positionToRectangle(33))

        const lines = []
        for (let i = 0; i < 200; ++i) lines.push([chunk("row " + i)])
        show("x", 1)
        $cmdlineFixture.showBlock(lines)
        tryCompare(p, "height", p.paneBounds.y + p.paneBounds.height - p.metrics.textInset - p.y)
        const area = findChild(p, "blockArea")
        const rendered = area.children[0].children.filter(i => i.objectName === "blockLine")
        compare(rendered.length, lines.length)
        tryVerify(() => rendered[0].mapToItem(area, 0, rendered[0].height).y < 0)
        verify(rendered[199].mapToItem(area, 0, 0).y >= 0)
        verify(area.clip)
        verify(field.y >= 0 && field.y + field.height <= p.height)
        $cmdlineFixture.hideBlock()
        show("a".repeat(5000), 5000)
        tryVerify(() => edit.positionToRectangle(5001).y > p.height)
        compare(p.caretRect, edit.positionToRectangle(5001))
        const top = edit.mapToItem(p, p.caretRect.x, p.caretRect.y)
        verify(top.y >= 0 && top.y + p.caretRect.height <= p.height)
        verify(p.y + p.height <= p.paneBounds.y + p.paneBounds.height - p.metrics.textInset)
    }
}
