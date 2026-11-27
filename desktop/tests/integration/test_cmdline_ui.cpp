#include "CursorItem.h"
#include "GridItem.h"
#include "IntegrationHelpers.h"

#include <QInputMethodEvent>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItemGrabResult>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSet>
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
    QQuickItem *layout = nullptr;
    QQuickItem *panel = nullptr;
    QQuickWindow *window = nullptr;

    bool acceptsInputMethod() const { return grid->flags().testFlag(QQuickItem::ItemAcceptsInputMethod); }

    void type(const QByteArray &text) const {
        for(char key: text) QTest::keyClick(window, key);
    }

    QString text() const { return panel->property("displayedText").toString(); }

    QVariantList list(const char *name) const {
        const auto value = panel->property(name);
        if(value.metaType() == QMetaType::fromType<QJSValue>()) return value.value<QJSValue>().toVariant().toList();
        return value.toList();
    }

    void verifyCaret(int position) const {
        QRectF expected;
        QVERIFY(QMetaObject::invokeMethod(layout, "positionToRectangle", Q_RETURN_ARG(QRectF, expected),
                                          Q_ARG(int, position)));
        QVERIFY(expected.height() > 0);
        QCOMPARE(panel->property("caretRect").toRectF(), expected);
    }
};

} // namespace

class TestCmdlineUi : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<CommandPanelHarness> h;
    QByteArray previousGate;

private slots:
    void initTestCase() {
        previousGate = qgetenv("DOT_EXT_CMDLINE");
        qputenv("DOT_EXT_CMDLINE", "1");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    void cleanupTestCase() {
        if(previousGate.isNull()) qunsetenv("DOT_EXT_CMDLINE");
        else qputenv("DOT_EXT_CMDLINE", previousGate);
    }

    void init() {
        h = std::make_unique<CommandPanelHarness>();
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
        QVERIFY(h->layout);
        QTRY_VERIFY(h->panel->property("available").toBool());
        QVERIFY(!h->panel->isVisible());
    }

    void cleanup() { h.reset(); }

    void blockStaysWithNextLevel() {
        h->type(":function F()");
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_COMPARE(h->list("blockLines"), QVariantList{QStringLiteral("function F()")});
        QTRY_COMPARE(h->text(), QStringLiteral("  "));
        QVERIFY(h->panel->isVisible());
        h->verifyCaret(h->panel->property("kindMark").toString().size() + 2);
        h->type("endfunction");
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_VERIFY(!h->panel->isVisible());
        QVERIFY(h->list("blockLines").isEmpty());
        QCOMPARE(evalSync(h->conn, QStringLiteral("exists('*F')")), std::optional<QVariant>(1));
    }

    void cancelDoesNotExecute() {
        h->type(":let g:dot_cancel = 1");
        QTRY_COMPARE(h->text(), QStringLiteral("let g:dot_cancel = 1"));
        QTest::keyClick(h->window, Qt::Key_Escape);
        QTRY_VERIFY(!h->panel->isVisible());
        QCOMPARE(evalSync(h->conn, QStringLiteral("exists('g:dot_cancel')")), std::optional<QVariant>(0));
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
        const auto enabled = [&] { return h->grid->inputMethodQuery(Qt::ImEnabled).toBool(); };
        const auto commit = [&](const QString &text) {
            QInputMethodEvent event;
            event.setCommitString(text);
            QCoreApplication::sendEvent(h->grid, &event);
            return event.isAccepted();
        };
        h->type("iabc");
        QTRY_VERIFY(h->conn.grid()->dumpAscii().contains(QStringLiteral("abc")));
        QVERIFY(!h->acceptsInputMethod());
        QVERIFY(!enabled());
        QVERIFY(!commit(QStringLiteral("x")));
        QTest::keyClick(h->window, Qt::Key_Escape);
        QCOMPARE(evalSync(h->conn, QStringLiteral("getline(1)")), std::optional<QVariant>(QStringLiteral("abc")));
        h->type(":let g:dot_commit = '");
        QTRY_VERIFY(enabled());
        QVERIFY(h->acceptsInputMethod());
        QInputMethodEvent preedit(QStringLiteral("not committed"), {});
        QCoreApplication::sendEvent(h->grid, &preedit);
        QVERIFY(commit(literal));
        h->type("'");
        QTRY_COMPARE(h->text(), QStringLiteral("let g:dot_commit = '%1'").arg(literal));
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_VERIFY(!enabled());
        QVERIFY(!h->acceptsInputMethod());
        QCOMPARE(getVarSync(h->conn, QStringLiteral("dot_commit")), std::optional<QVariant>(literal));
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
        h->conn.command(QStringLiteral("restart!"));
        QTRY_COMPARE_WITH_TIMEOUT(attached.count(), 1, 20000);
        QTRY_VERIFY(h->panel->property("available").toBool());
        QCOMPARE(removed.count(), 0);
        QCOMPARE(reset.count(), 1);
        QVERIFY(!h->panel->isVisible() && !h->panel->property("activeRow").value<QObject *>());
        QVERIFY(h->text().isEmpty() && h->list("blockLines").isEmpty());
        QVERIFY(!h->acceptsInputMethod());
        QVERIFY(!h->grid->inputMethodQuery(Qt::ImEnabled).toBool());
        QCOMPARE(evalSync(h->conn, QStringLiteral("nvim_list_uis()[0].ext_cmdline")), std::optional<QVariant>(true));
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
        h->conn.command(QStringLiteral("highlight DotPanelTest guifg=#91abcd guibg=#273849"));
        h->type(":call input({'prompt':'Name: ', 'highlight':{s -> empty(s) ? [] : [[0,1,'DotPanelTest']]}})");
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_COMPARE(h->panel->property("kindMark").toString(), QStringLiteral("Name: "));
        h->type("ab");
        QTRY_COMPARE(h->text(), QStringLiteral("ab"));
        const auto spans = h->list("chunkStyles");
        QCOMPARE(spans.size(), 2);
        const auto colored = spans.first().toMap();
        const auto plain = spans.last().toMap();
        QCOMPARE(colored.value("text").toString(), QStringLiteral("a"));
        QCOMPARE(colored.value("foreground").value<QColor>(), QColor("#91abcd"));
        QCOMPARE(colored.value("background").value<QColor>(), QColor("#273849"));
        QCOMPARE(plain.value("text").toString(), QStringLiteral("b"));
        QCOMPARE(plain.value("foreground").value<QColor>(), h->conn.colorPalette()->overlayForeground());
        QCOMPARE(plain.value("background").value<QColor>(), h->conn.colorPalette()->overlayBackground());

        auto *quickDocument = h->layout->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quickDocument && quickDocument->textDocument());
        QTextCursor cursor(quickDocument->textDocument());
        cursor.setPosition(h->panel->property("kindMark").toString().size() + 1);
        QCOMPARE(cursor.charFormat().foreground().color(), QColor("#91abcd"));
        QCOMPARE(cursor.charFormat().background().color(), QColor("#273849"));

        const auto grab = h->panel->grabToImage();
        QVERIFY(grab);
        QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
        if(grab->image().isNull()) QVERIFY(ready.wait(5000));
        const QImage image = grab->image();
        QVERIFY(!image.isNull());
        QSet<QRgb> pixels;
        for(int y = 0; y < image.height(); ++y)
            for(int x = 0; x < image.width(); ++x) pixels.insert(image.pixel(x, y));
        QVERIFY(pixels.size() > 2);
        QVERIFY(pixels.contains(h->conn.colorPalette()->overlayBackground().rgb()));
        QVERIFY(pixels.contains(h->conn.colorPalette()->accent().rgb()));
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
        QTRY_COMPARE(h->text(), QStringLiteral("let g:dot_panel_count = get(g:, 'dot_panel_count', 0) + 1"));
        QVERIFY(h->panel->isVisible() && !h->cursor->isVisible());
        QCOMPARE(h->window->activeFocusItem(), h->grid);
        QCOMPARE(h->panel->property("kindMark").toString(), QStringLiteral(":"));
        QCOMPARE(h->panel->property("caretShape").toString(), QStringLiteral("block"));
        QCOMPARE(h->panel->property("caretColor").value<QColor>(), h->conn.colorPalette()->cursorBackground());
        QVERIFY(!redraw.isEmpty());
        QTest::keyClick(h->window, Qt::Key_Return);
        QTRY_VERIFY(!h->panel->isVisible());
        QCOMPARE(getVarSync(h->conn, QStringLiteral("dot_panel_count")), std::optional<QVariant>(1));
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
