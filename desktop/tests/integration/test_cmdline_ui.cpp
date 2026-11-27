#include "CursorItem.h"
#include "GridItem.h"
#include "IntegrationHelpers.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItemGrabResult>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTextCursor>
#include <QTextDocument>

#include <memory>

using namespace dot;
using namespace dot::test;

namespace {

struct CommandPanelHarness {
    NvimConnector conn;
    QQmlApplicationEngine engine;
    CursorItem *cursor = nullptr;
    GridItem *grid = nullptr;
    QQuickItem *blockContent = nullptr;
    QQuickItem *layout = nullptr;
    QQuickItem *panel = nullptr;
    QQuickWindow *window = nullptr;

    bool acceptsInputMethod() const {
        return grid->flags().testFlag(QQuickItem::ItemAcceptsInputMethod);
    }

    bool commit(const QString &text, int start = 0, int length = 0) const {
        QInputMethodEvent event;
        event.setCommitString(text, start, length);
        QCoreApplication::sendEvent(QGuiApplication::focusObject(), &event);
        return event.isAccepted();
    }

    void type(const QByteArray &text) const {
        for(char key: text) QTest::keyClick(window, key);
    }

    QString text() const { return panel->property("displayedText").toString(); }

    QStringList renderedBlock() const {
        QStringList result;
        for(const auto *row: blockContent->childItems()) {
            if(row->objectName() != QStringLiteral("blockLine")) continue;
            QTextDocument doc;
            doc.setHtml(row->property("text").toString());
            result.append(doc.toPlainText());
        }
        return result;
    }

    void verifyCaret(int position) const {
        QRectF expected;
        QVERIFY(QMetaObject::invokeMethod(layout, "positionToRectangle",
                                          Q_RETURN_ARG(QRectF, expected), Q_ARG(int, position)));
        QVERIFY(expected.height() > 0);
        QCOMPARE(panel->property("caretRect").toRectF(), expected);
        const auto *caret = panel->findChild<QQuickItem *>(QStringLiteral("commandCaret"));
        QVERIFY(caret && caret->isVisible());
        const auto top = panel->property("caretShape").toString() == QStringLiteral("horizontal")
                             ? expected.bottom() - caret->height()
                             : expected.y();
        QCOMPARE(caret->mapToItem(panel, QPointF()),
                 layout->mapToItem(panel, QPointF(expected.x(), top)));
    }
};

} // namespace

class TestCmdlineUi : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<CommandPanelHarness> h;

private slots:
    void initTestCase() { QQuickWindow::setGraphicsApi(QSGRendererInterface::Software); }

    void init() {
        h = std::make_unique<CommandPanelHarness>();
        h->conn.setExtCmdline(true);
        FlushLatch initialFlush(&h->conn);
        QVERIFY(startTestNvim(h->conn));
        h->engine.rootContext()->setContextProperty(QStringLiteral("$connector"), &h->conn);
        h->engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
        QVERIFY(!h->engine.rootObjects().isEmpty());
        h->window = qobject_cast<QQuickWindow *>(h->engine.rootObjects().first());
        QVERIFY(h->window);
        QVERIFY(waitForAttach(&h->conn) && initialFlush.wait());
        QVERIFY(QTest::qWaitForWindowExposed(h->window));
        h->cursor = h->window->findChild<CursorItem *>();
        h->grid = qobject_cast<GridItem *>(h->window->activeFocusItem());
        h->panel = h->window->findChild<QQuickItem *>(QStringLiteral("commandPanel"));
        QVERIFY(h->cursor && h->grid && h->panel);
        h->layout = h->panel->findChild<QQuickItem *>(QStringLiteral("commandLayout"));
        h->blockContent = h->panel->findChild<QQuickItem *>(QStringLiteral("blockContent"));
        QVERIFY(h->layout && h->blockContent);
        QTRY_VERIFY(h->panel->property("available").toBool());
        QVERIFY(!h->panel->isVisible());
    }

    void cleanup() { h.reset(); }

    void blockStaysWithNextLevel() {
        h->type(":function F()");
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_COMPARE(h->renderedBlock(), QStringList{ QStringLiteral("function F()") });
        QTRY_COMPARE(h->text(), QStringLiteral("  "));
        QVERIFY(h->panel->isVisible());
        h->verifyCaret(h->panel->property("kindMark").toString().size() + 2);
        h->type("endfunction");
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_VERIFY(!h->panel->isVisible());
        QVERIFY(h->renderedBlock().isEmpty());
        QCOMPARE(evalSync(h->conn, QStringLiteral("exists('*F')")), std::optional<QVariant>(1));
    }

    void cancelDoesNotExecute() {
        h->type(":let g:dot_cancel = 1");
        QTRY_COMPARE(h->text(), QStringLiteral("let g:dot_cancel = 1"));
        QTest::keyClick(h->window, Qt::Key_Escape);
        QTRY_VERIFY(!h->panel->isVisible());
        QCOMPARE(evalSync(h->conn, QStringLiteral("exists('g:dot_cancel')")),
                 std::optional<QVariant>(0));
        QCOMPARE(h->window->activeFocusItem(), h->grid);
        QVERIFY(h->cursor->isVisible());
    }

    void failedCommandLeavesErrorVisible() {
        h->type(":call NoSuchFunction()");
        QTRY_COMPARE(h->text(), QStringLiteral("call NoSuchFunction()"));
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_VERIFY(!h->panel->isVisible());
        QTRY_VERIFY(h->conn.grid()->dumpAscii().contains(QStringLiteral("E117:")));
    }

    void imeCommitReachesCommandLineOnce() {
        const QString literal(u"\u6F22<x>");
        const auto enabled = [] {
            return QInputMethod::queryFocusObject(Qt::ImEnabled, {}).toBool();
        };
        QTRY_COMPARE(QGuiApplication::focusObject(), h->grid);
        h->type("iabc");
        QTRY_VERIFY(h->conn.grid()->dumpAscii().contains(QStringLiteral("abc")));
        QVERIFY(!h->acceptsInputMethod());
        QVERIFY(!enabled());
        QVERIFY(!h->commit(QStringLiteral("x")));
        QTest::keyClick(h->window, Qt::Key_Escape);
        QCOMPARE(evalSync(h->conn, QStringLiteral("getline(1)")),
                 std::optional<QVariant>(QStringLiteral("abc")));
        h->type(":let g:dot_commit = '");
        QTRY_VERIFY(enabled());
        QCOMPARE(QGuiApplication::focusObject(), h->grid);
        QVERIFY(h->acceptsInputMethod());
        QInputMethodEvent preedit(QStringLiteral("not committed"), {});
        QCoreApplication::sendEvent(h->grid, &preedit);
        QVERIFY(!preedit.isAccepted());
        QVERIFY(!h->commit({}));
        QVERIFY(!h->commit(QStringLiteral("x"), -1, 1));
        QVERIFY(!h->commit({}, -1, 1));
        QVERIFY(!h->commit(QStringLiteral("x"), 1, 0));
        QVERIFY(!h->commit(QStringLiteral("x"), 0, 1));
        QVERIFY(h->commit(literal));
        h->type("'");
        QTRY_COMPARE(h->text(), QStringLiteral("let g:dot_commit = '%1'").arg(literal));
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_VERIFY(!enabled());
        QVERIFY(!h->acceptsInputMethod());
        QCOMPARE(getVarSync(h->conn, QStringLiteral("dot_commit")),
                 std::optional<QVariant>(literal));
        h->type(":");
        QTRY_VERIFY(enabled());
        QVERIFY(h->acceptsInputMethod());
        QTest::keyClick(h->window, Qt::Key_Escape);
        QTRY_VERIFY(!enabled());
        QVERIFY(!h->acceptsInputMethod());
    }

    void nestedPanelRestoresOuter() {
        h->type(":echo ");
        QTRY_COMPARE(h->text(), QStringLiteral("echo "));
        QTest::keyClick(h->window, Qt::Key_R, Qt::ControlModifier);
        h->type("=1+1");
        QTRY_COMPARE(h->text(), QStringLiteral("1+1"));
        QCOMPARE(h->panel->property("kindMark").toString(), QStringLiteral("="));
        QCOMPARE(h->conn.cmdline()->activeLevel(), 2);
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_COMPARE(h->text(), QStringLiteral("echo 2"));
        QCOMPARE(h->conn.cmdline()->activeLevel(), 1);
        QCOMPARE(h->panel->property("kindMark").toString(), QStringLiteral(":"));
        QVERIFY(h->panel->isVisible());
    }

    void resetClosesPanel() {
        const auto apiLevel = evalSync(h->conn, QStringLiteral("api_info().version.api_level"));
        QVERIFY(apiLevel);
        if(apiLevel->toInt() < 14) QSKIP("nvim API level < 14: :restart UI event unavailable");
        h->type(":abc");
        QTRY_COMPARE(h->text(), QStringLiteral("abc"));
        QVERIFY(h->panel->isVisible());
        QSignalSpy attached(&h->conn, &NvimConnector::attachComplete);
        QSignalSpy removed(h->conn.cmdline(), &CmdlineModel::rowsRemoved);
        QSignalSpy reset(h->conn.cmdline(), &CmdlineModel::modelReset);
        bool handoff = false;
        QObject handoffContext;
        connect(&h->conn, &NvimConnector::attachedChanged, &handoffContext, [&] {
            if(h->conn.attached()) return;
            handoff = true;
            QCOMPARE(h->conn.cmdline()->activeLevel(), 1);
            QVERIFY(!h->acceptsInputMethod());
            QVERIFY(!h->grid->inputMethodQuery(Qt::ImEnabled).toBool());
            QVERIFY(!h->commit(QStringLiteral("stale")));
        });
        h->conn.command(QStringLiteral("restart!"));
        QTRY_COMPARE_WITH_TIMEOUT(attached.count(), 1, 20000);
        QTRY_VERIFY(h->panel->property("available").toBool());
        QVERIFY(handoff);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(reset.count(), 1);
        QVERIFY(!h->panel->isVisible() && !h->panel->property("activeRow").value<QObject *>());
        QVERIFY(h->text().isEmpty() && h->renderedBlock().isEmpty());
        QVERIFY(!h->acceptsInputMethod());
        QVERIFY(!h->grid->inputMethodQuery(Qt::ImEnabled).toBool());
        QCOMPARE(evalSync(h->conn, QStringLiteral("nvim_list_uis()[0].ext_cmdline")),
                 std::optional<QVariant>(true));
        h->type(":");
        QTRY_VERIFY(h->panel->isVisible());
        QTest::keyClick(h->window, Qt::Key_Escape);
        h->type("ifresh");
        QTRY_VERIFY(h->conn.grid()->dumpAscii().contains(QStringLiteral("fresh")));
    }

    void searchAndPromptRender() {
        h->type("/needle");
        QTRY_COMPARE(h->text(), QStringLiteral("needle"));
        QCOMPARE(h->panel->property("kindMark").toString(), QStringLiteral("/"));
        QTest::keyClick(h->window, Qt::Key_Escape);
        QTRY_VERIFY(!h->panel->isVisible());
        h->conn.command(QStringLiteral(
            "highlight DotPanelTest guifg=#91abcd guibg=#273849 gui=bold,italic,underline,strikethrough"));
        h->type(
            ":call input({'prompt':'Name: ', 'highlight':{s -> empty(s) ? [] : [[0,1,'DotPanelTest']]}})");
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_COMPARE(h->panel->property("kindMark").toString(), QStringLiteral("Name: "));
        h->type("ab");
        QTRY_COMPARE(h->text(), QStringLiteral("ab"));
        auto *quickDocument = h->layout->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quickDocument && quickDocument->textDocument());
        QTextCursor cursor(quickDocument->textDocument());
        cursor.setPosition(h->panel->property("kindMark").toString().size() + 1);
        QCOMPARE(cursor.charFormat().foreground().color(), QColor("#91abcd"));
        QCOMPARE(cursor.charFormat().background().color(), QColor("#273849"));
        QVERIFY(cursor.charFormat().fontWeight() >= QFont::Bold);
        QVERIFY(cursor.charFormat().fontItalic() && cursor.charFormat().fontUnderline() &&
                cursor.charFormat().fontStrikeOut());
        QVERIFY(cursor.movePosition(QTextCursor::NextCharacter));
        QCOMPARE(cursor.charFormat().foreground().color(),
                 h->conn.colorPalette()->overlayForeground());
        QCOMPARE(cursor.charFormat().background().color(),
                 h->conn.colorPalette()->overlayBackground());
        QVERIFY(cursor.charFormat().fontWeight() < QFont::Bold);
        QVERIFY(!cursor.charFormat().fontItalic() && !cursor.charFormat().fontUnderline() &&
                !cursor.charFormat().fontStrikeOut());

        const auto *root = h->window->contentItem();
        QRectF start, end;
        const int offset = h->panel->property("kindMark").toString().size();
        QVERIFY(QMetaObject::invokeMethod(h->layout, "positionToRectangle",
                                          Q_RETURN_ARG(QRectF, start), Q_ARG(int, offset)));
        QVERIFY(QMetaObject::invokeMethod(h->layout, "positionToRectangle",
                                          Q_RETURN_ARG(QRectF, end), Q_ARG(int, offset + 1)));
        start.setRight(end.x());
        const QRectF cell = h->layout->mapRectToItem(root, start);
        QVERIFY(cell.width() > 0 && cell.height() > 0);
        const auto grab = h->window->contentItem()->grabToImage();
        QVERIFY(grab);
        QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
        if(grab->image().isNull()) QVERIFY(ready.wait(5000));
        const QImage image = grab->image();
        QVERIFY(!image.isNull());
        const QRect pixels =
            QTransform::fromScale(image.width() / root->width(), image.height() / root->height())
                .mapRect(cell)
                .toRect();
        QVERIFY(image.rect().contains(pixels) && !pixels.isEmpty());
        bool background = false;
        bool ink = false;
        for(int y = pixels.top(); y <= pixels.bottom(); ++y) {
            for(int x = pixels.left(); x <= pixels.right(); ++x) {
                const QColor color = image.pixelColor(x, y);
                background |= color == QColor("#273849");
                ink |= color == QColor("#91abcd");
            }
        }
        QVERIFY(background && ink);
    }

    void specialCharacterShowsAtCaret() {
        h->type(":ab");
        QTest::keyClick(h->window, Qt::Key_Left);
        QTest::keyClick(h->window, Qt::Key_R, Qt::ControlModifier);
        QTRY_COMPARE(h->text(), QStringLiteral("a\"b"));
        h->verifyCaret(2);
        const auto *model = h->conn.cmdline();
        const auto chunks = model->index(0).data(CmdlineModel::ChunksRole).toList();
        QCOMPARE(chunks.size(), 1);
        QCOMPARE(chunks.first().toMap().value("text").toString(), QStringLiteral("ab"));
    }

    void typedColonCommandExecutes() {
        QVERIFY(h->cursor->isVisible());
        QSignalSpy redraw(&h->conn, &NvimConnector::flush);
        h->type(":let g:dot_panel_count = get(g:, 'dot_panel_count', 0) + 1");
        QTRY_COMPARE(h->text(),
                     QStringLiteral("let g:dot_panel_count = get(g:, 'dot_panel_count', 0) + 1"));
        QVERIFY(h->panel->isVisible() && !h->cursor->isVisible());
        QCOMPARE(h->window->activeFocusItem(), h->grid);
        QCOMPARE(h->panel->property("kindMark").toString(), QStringLiteral(":"));
        QCOMPARE(h->panel->property("caretShape").toString(), QStringLiteral("block"));
        QCOMPARE(h->panel->property("caretColor").value<QColor>(),
                 h->conn.colorPalette()->cursorBackground());
        QVERIFY(!redraw.isEmpty());
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_VERIFY(!h->panel->isVisible());
        QCOMPARE(getVarSync(h->conn, QStringLiteral("dot_panel_count")),
                 std::optional<QVariant>(1));
        QCOMPARE(h->window->activeFocusItem(), h->grid);
        QVERIFY(h->cursor->isVisible());
    }

    void unicodeCaretTracksContent() {
        const QString unicode(u"a\u00e9\U0001f600");
        h->type(":");
        QTest::sendKeyEvent(QTest::Click, h->window, Qt::Key_unknown, unicode, Qt::NoModifier);
        QTRY_COMPARE(h->text(), unicode);
        h->verifyCaret(5);
        QTest::keyClick(h->window, Qt::Key_Left);
        QTRY_COMPARE(h->panel->property("cursorPosition").toInt(), 3);
        h->verifyCaret(3);
        QCOMPARE(h->text(), unicode);
    }
};

QTEST_MAIN(TestCmdlineUi)
#include "test_cmdline_ui.moc"
