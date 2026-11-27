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

    Rectangle {
        anchors.fill: parent
        color: "white"
    }

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
    function checkCaret(p, position) {
        const caret = findChild(p, "commandCaret")
        const edit = findChild(p, "commandLayout")
        const rect = edit.positionToRectangle(position)
        compare(caret.mapToItem(p, 0, 0), edit.mapToItem(p, rect.x, rect.y))
    }
    function blockRows(p) {
        return findChild(p, "blockContent").children.filter(row => row.objectName === "blockLine")
    }
    function show(text, pos, indent = 0) {
        $cmdlineFixture.showCmdline(1, ":", "", [chunk(text)], pos, indent)
    }
    function init() {
        failOnWarning(/TypeError|ReferenceError|Binding loop|Unable to assign/)
    }
    function cleanup() {
        $cmdlineFixture.resetCmdline()
        $cmdlineFixture.setCursorShape("block", 100)
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

    function test_caret_shapes() {
        const p = make()
        show("abc", 1)
        const caret = findChild(p, "commandCaret")
        const edit = findChild(p, "commandLayout")
        const glyph = findChild(caret, "caretGlyph")
        const cell = edit.positionToRectangle(3).x - edit.positionToRectangle(2).x
        verify(cell > 0)
        const height = p.caretRect.height
        const top = edit.y + p.caretRect.y
        compare(glyph.getText(2, 3), "b")
        verify(glyph.visible)
        $cmdlineFixture.setCursorShape("horizontal", 25)
        compare(p.caretShape, "horizontal")
        compare(caret.width, cell)
        compare(caret.height, Math.max(1, height * 0.25))
        compare(caret.y, top + height - caret.height)
        verify(!glyph.visible)
        $cmdlineFixture.setCursorShape("vertical", 50)
        compare(p.caretShape, "vertical")
        compare(caret.width, Math.max(1, cell * 0.5))
        compare(caret.height, height)
        compare(caret.y, top)
        verify(!glyph.visible)
        $cmdlineFixture.setCursorShape("horizontal", 150)
        compare(caret.height, height)
        $cmdlineFixture.setCursorShape("vertical", 0)
        compare(caret.width, Math.max(1, cell / 100))
        $cmdlineFixture.setCursorShape("vertical", 25, false)
        compare(p.caretShape, "block")
        compare(caret.width, cell)
        verify(glyph.visible)
    }

    function test_exact_fit_end_caret() {
        const p = make()
        show("abc", 3)
        const caret = findChild(p, "commandCaret")
        const edit = findChild(p, "commandLayout")
        const viewport = edit.parent
        const exact = Math.ceil(edit.positionToRectangle(4).x)
        p.paneBounds = Qt.rect(8, 24, p.paneBounds.width - viewport.width + exact, 320)
        tryCompare(viewport, "width", exact)
        checkCaret(p, 4)
        const last = edit.positionToRectangle(3)
        verify(caret.y > last.y || caret.x > last.x)
        verify(caret.x + caret.width <= viewport.width)
    }

    function test_styled_grapheme_under_block() {
        const p = make()
        $cmdlineFixture.defineHighlight(13, {bold: true, italic: true, strikethrough: true, underline: true})
        $cmdlineFixture.showCmdline(1, ":", "", [chunk("a"), chunk("e\u0301", 13), chunk("z")], 1, 0)
        const caret = findChild(p, "commandCaret")
        const edit = findChild(p, "commandLayout")
        const glyph = findChild(caret, "caretGlyph")
        verify(glyph.visible)
        checkCaret(p, 2)
        const start = edit.positionToRectangle(2)
        const end = edit.positionToRectangle(4)
        compare(caret.width, end.x - start.x)
        compare(glyph.mapToItem(edit, 0, 0), Qt.point(0, 0))
        compare(glyph.getText(2, 4), "e\u0301")
        for (const pos of [2, 3]) {
            const format = $cmdlineFixture.renderedAt(glyph, pos)
            compare(format.foreground, p.colors.cursorForeground)
            verify(format.bold && format.italic && format.strikeout && format.underline)
        }
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
        waitForRendering(p)
        const x = p.x + p.width / 2
        const bottom = p.y + p.height
        const [far, near] = $cmdlineFixture.grabPixels(test, [Qt.point(x, bottom + 8), Qt.point(x, bottom + 1)])
        compare(far, "#ffffff")
        verify(near.r > 0 && near.r < 1 && near.r === near.g && near.g === near.b)
        compare(p.caretColor, $connector.colorPalette.cursorBackground)
        compare(p.caretShape, "block")
        for (const role of ["chrome", "cursor", "overlay"]) {
            $cmdlineFixture.setRoleResolved(role, false)
            verify(!p.available && !p.visible)
            $cmdlineFixture.setRoleResolved(role, true)
            verify(p.available && p.visible)
        }
        p.paneVisible = false
        verify(!p.visible)
    }

    function test_special_character_at_end() {
        const p = make()
        const edit = findChild(p, "commandLayout")
        for (const text of ["ab", ""]) {
            show(text, text.length)
            $cmdlineFixture.setSpecialCharacter(1, "\"", false)
            compare(p.displayedText, text + "\"")
            compare(p.chunkStyles[p.chunkStyles.length - 1].text, "\"")
            compare(p.caretRect, edit.positionToRectangle(1 + text.length))
        }
    }

    function test_special_character_at_styled_boundary() {
        const p = make()
        const edit = findChild(p, "commandLayout")
        $cmdlineFixture.defineHighlight(13, {foreground: 0xabcdef})
        $cmdlineFixture.showCmdline(1, ":", "", [chunk("ab"), chunk("\u{1f600}q", 13)], 2, 0)
        for (const shift of [true, false]) {
            $cmdlineFixture.setSpecialCharacter(1, "^", shift)
            compare(edit.getText(0, edit.length), shift ? ":ab^\u{1f600}q" : ":ab^q")
            compare($cmdlineFixture.renderedAt(edit, 3).foreground, "#abcdef")
            checkCaret(p, 3)
        }
    }

    function test_style_updates_and_blocks() {
        const p = make()
        $cmdlineFixture.setOverlayColors("#123456", "#987654")
        $cmdlineFixture.defineHighlight(11, {reverse: true, undercurl: true})
        $cmdlineFixture.defineHighlight(12, {foreground: 0xff0123, reverse: true})
        $cmdlineFixture.showBlock([[chunk("<&", 11), chunk("  "), chunk("Q", 12)]])
        tryCompare(p, "visible", true)
        compare(p.activeRow, null)
        compare(blockRows(p).length, 1)
        const row = blockRows(p)[0]
        compare(row.textFormat, Text.RichText)
        const first = $cmdlineFixture.renderedAt(row, 0)
        compare(first.text, "<&  Q")
        compare(first.foreground, "#987654")
        compare(first.background, "#123456")
        verify(first.underline)
        const space = $cmdlineFixture.renderedAt(row, 2)
        compare(space.foreground, "#123456")
        compare(space.background, "#987654")
        compare($cmdlineFixture.renderedAt(row, 4).background, "#ff0123")
        $cmdlineFixture.showCmdline(1, "/", "", [chunk("a", 12), chunk("<b>  &")], 1, 0)
        const edit = findChild(p, "commandLayout")
        compare(edit.getText(0, edit.length), "/a<b>  &")
        compare(p.chunkStyles[1].background, "#987654")
        const rev = p.styleRevision
        $cmdlineFixture.defineHighlight(12, {foreground: 0xabcdef})
        $cmdlineFixture.defineHighlight(12, {foreground: 0xabcdef, bold: true, italic: true,
                                             strikethrough: true, underline: true})
        compare(p.styleRevision, rev)
        $cmdlineFixture.emitFlush()
        compare(p.styleRevision, rev + 1)
        $cmdlineFixture.emitFlush()
        compare(p.styleRevision, rev + 1)
        compare(blockRows(p)[0], row)
        compare($cmdlineFixture.renderedAt(row, 4).foreground, "#abcdef")
        const styled = $cmdlineFixture.renderedAt(edit, 1)
        compare(styled.foreground, "#abcdef")
        verify(styled.bold && styled.italic && styled.strikeout && styled.underline)
        const plain = $cmdlineFixture.renderedAt(edit, 2)
        verify(!plain.bold && !plain.italic && !plain.strikeout && !plain.underline)
        p.paneVisible = false
        $cmdlineFixture.defineHighlight(12, {background: 0x455667})
        $cmdlineFixture.emitFlush()
        compare(p.styleRevision, rev + 1)
        p.paneVisible = true
        compare(p.chunkStyles[0].background, "#455667")
        $cmdlineFixture.hideBlock()
        compare(blockRows(p).length, 0)
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
        checkCaret(p, 33)
        compare(edit.positionToRectangle(13).y, 0)
        verify(edit.lineCount > 1)
        const oldY = p.caretRect.y
        p.paneBounds = Qt.rect(8, 24, 15 * p.metrics.cellWidth, 240)
        tryVerify(() => p.caretRect.y > oldY)
        compare(p.caretRect, edit.positionToRectangle(33))
        checkCaret(p, 33)
        const narrowerY = p.caretRect.y
        p.metrics.grid.fontSize *= 1.5
        tryVerify(() => p.caretRect.y > narrowerY)
        compare(p.caretRect, edit.positionToRectangle(33))
        checkCaret(p, 33)

        const lines = []
        for (let i = 0; i < 200; ++i) lines.push([chunk("row " + i)])
        show("x", 1)
        $cmdlineFixture.showBlock(lines)
        tryCompare(p, "height", p.paneBounds.y + p.paneBounds.height - p.metrics.textInset - p.y)
        const area = findChild(p, "blockArea")
        const rendered = blockRows(p)
        compare(rendered.length, lines.length)
        tryVerify(() => rendered[0].mapToItem(area, 0, rendered[0].height).y < 0)
        verify(rendered[199].mapToItem(area, 0, 0).y >= 0)
        verify(rendered[199].mapToItem(area, 0, rendered[199].height).y <= area.height)
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
