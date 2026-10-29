// Drive the real Main.qml scene through Qt's focus chain so losing editor or
// chat focus fails the test instead of being bypassed by direct RPC input.

#include <QAbstractSocket>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSharedPointer>
#include <QTcpServer>
#include <QtTest>

#include "ChatModel.h"
#include "CursorItem.h"
#include "GridItem.h"
#include "IntegrationHelpers.h"
#include "NvimConnector.h"

using namespace dot;
using namespace dot::test;

namespace {

QQuickWindow *loadMainQml(QQmlApplicationEngine &engine, NvimConnector *conn) {
    engine.rootContext()->setContextProperty(QStringLiteral("$connector"), conn);
    engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
    if(engine.rootObjects().isEmpty()) return nullptr;
    return qobject_cast<QQuickWindow *>(engine.rootObjects().first());
}

template <typename F>
bool waitUntil(F &&predicate, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while(!predicate()) {
        if(t.elapsed() >= timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return true;
}

QImage grabItem(QQuickItem *item, int timeoutMs = 2000) {
    QSharedPointer<QQuickItemGrabResult> result = item->grabToImage();
    if(!result) return {};
    QElapsedTimer t;
    t.start();
    while(result->image().isNull() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return result->image();
}

int distinctColours(const QImage &img) {
    QHash<QRgb, int> seen;
    for(int y = 0; y < img.height(); ++y) {
        for(int x = 0; x < img.width(); ++x) {
            seen.insert(img.pixel(x, y), 1);
            if(seen.size() > 1) return seen.size();
        }
    }
    return seen.size();
}

void clickItem(QQuickWindow *window, QQuickItem *item) {
    const QPointF c = item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0));
    QTest::mouseClick(window, Qt::LeftButton, {}, c.toPoint());
}

QQuickItem *findTextItem(QQuickItem *root, const QString &text) {
    if(root->inherits("QQuickText") && root->property("text").toString() == text) { return root; }
    const auto children = root->childItems();
    for(QQuickItem *child: children) {
        if(auto *item = findTextItem(child, text)) return item;
    }
    return nullptr;
}

} // namespace

class TestChatPanel : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        qRegisterMetaType<dot::Notification>("dot::Notification");
        qRegisterMetaType<dot::ObjectHandlePtr>("dot::ObjectHandlePtr");
    }

    void chatPanelEndToEnd_data() {
        QTest::addColumn<bool>("restart");
        QTest::newRow("default") << false;
        QTest::newRow("restart") << true;
    }

    void chatPanelEndToEnd() {
        const QString replyText = QStringLiteral("Transcript message");
        const QString smallFontCommand =
            QStringLiteral("set guifont=Cascadia\\ Code:h12 linespace=0");
        QFETCH(bool, restart);
        NvimConnector conn;
        QVERIFY(startTestNvim(conn));

        QQmlApplicationEngine engine;
        QQuickWindow *window = loadMainQml(engine, &conn);
        QVERIFY2(window, "Main.qml failed to load");
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(waitForAttach(&conn));
        QVERIFY(waitForFlush(&conn));

        auto *toggle = window->findChild<QQuickItem *>(QStringLiteral("chatToggle"));
        auto *panel = window->findChild<QQuickItem *>(QStringLiteral("chatPanel"));
        auto *model = window->findChild<ChatModel *>();
        QVERIFY2(toggle, "chatToggle button not found in scene");
        QVERIFY2(panel, "chatPanel not found in scene");
        QVERIFY2(model, "ChatModel not found in scene");

        auto *cursor = window->findChild<CursorItem *>();
        auto *grid = window->findChild<GridItem *>();
        auto *input = window->findChild<QQuickItem *>(QStringLiteral("chatInput"));
        auto *shell = window->findChild<QQuickItem *>(QStringLiteral("shell"));
        QVERIFY(cursor && grid && input && shell);
        auto *metrics = shell->property("metrics").value<QObject *>();
        QVERIFY(metrics);
        if(restart) {
            const auto apiLevel = evalSync(conn, QStringLiteral("api_info().version.api_level"));
            QVERIFY(apiLevel);
            if(apiLevel->toInt() < 14) QSKIP("nvim API level < 14: :restart UI event unavailable");
        }
        conn.command(smallFontCommand);
        QVERIFY(waitUntil([&] {
            return grid->fontSize() == 12.0 && conn.linespace() == 0 &&
                   conn.grid()->gridCols(1) == static_cast<int>(shell->width() / grid->cellWidth());
        }, 5000));

        // Focus starts on the grid; record it so we can prove it is restored.
        QQuickItem *gridFocus = window->activeFocusItem();
        QVERIFY2(gridFocus != nullptr, "No active focus item after attach");
        QVERIFY(!panel->isVisible());
        const int baseCols = conn.grid()->gridCols(1);
        QVERIFY(baseCols > 0);

        clickItem(window, toggle);
        QVERIFY2(waitUntil([&] { return panel->isVisible(); }, 3000),
                 "Panel did not become visible after clicking the toggle");
        QVERIFY2(waitUntil(
                     [&] {
            QQuickItem *f = window->activeFocusItem();
            return f && f->objectName() == QStringLiteral("chatInput");
        }, 3000),
                 "Chat input did not take focus on open");

        QVERIFY2(waitUntil([&] { return conn.grid()->gridCols(1) < baseCols; }, 3000),
                 "Grid columns did not shrink when the panel opened");

        qInfo() << "Panel TCP servers:" << panel->findChildren<QTcpServer *>().size()
                << "sockets:" << panel->findChildren<QAbstractSocket *>().size();
        QVERIFY(panel->findChildren<QTcpServer *>().isEmpty());
        QVERIFY(panel->findChildren<QAbstractSocket *>().isEmpty());

        QTest::keyClick(window, Qt::Key_H);
        QTest::keyClick(window, Qt::Key_I);
        QCOMPARE(input->property("text").toString(), QStringLiteral("hi"));

        conn.command(QStringLiteral("set guifont=Cascadia\\ Code:h24 linespace=6"));
        QVERIFY(waitUntil([&] { return grid->fontSize() == 24.0 && conn.linespace() == 6; }, 5000));
        QCOMPARE(window->activeFocusItem(), input);
        QCOMPARE(input->property("text").toString(), QStringLiteral("hi"));
        QCOMPARE(input->property("font").value<QFont>(), grid->textFont());
        QCOMPARE(input->height() - 2 * input->property("padding").toReal(), grid->cellHeight());
        QCOMPARE(cursor->connector(), &conn);
        QCOMPARE(grid->connector(), &conn);
        QCOMPARE(cursor->cellHeight(), grid->cellHeight());
        QCOMPARE(cursor->cellWidth(), grid->cellWidth());
        QCOMPARE(cursor->baseline(), grid->baseline());
        QCOMPARE(cursor->fontName(), grid->fontName());
        QCOMPARE(cursor->fontSize(), grid->fontSize());

        if(restart) {
            int newPid = 0;
            const auto oldPid = evalSync(conn, QStringLiteral("getpid()"));
            QVERIFY(oldPid && oldPid->toInt() > 0);
            QSignalSpy disconnectedSpy(&conn, &NvimConnector::disconnected);
            conn.command(QStringLiteral("restart!"));
            QVERIFY2(waitUntil(
                         [&] {
                const auto pid = evalSync(conn, QStringLiteral("getpid()"), 1000);
                if(pid && pid->toInt() > 0 && pid->toInt() != oldPid->toInt())
                    newPid = pid->toInt();
                return newPid != 0 && conn.attached();
            }, 20000),
                     "Chat-open restart did not reconnect to a new Neovim process");
            QCOMPARE(disconnectedSpy.count(), 0);
            QCOMPARE(window->findChild<QQuickItem *>(QStringLiteral("chatInput")), input);
            QCOMPARE(window->findChild<QQuickItem *>(QStringLiteral("chatToggle")), toggle);
            QCOMPARE(window->findChild<CursorItem *>(), cursor);
            QCOMPARE(shell->property("metrics").value<QObject *>(), metrics);
            QCOMPARE(metrics->property("grid").value<QObject *>(), grid);
            QCOMPARE(window->activeFocusItem(), input);
            QCOMPARE(input->property("text").toString(), QStringLiteral("hi"));
            QCOMPARE(input->property("font").value<QFont>(), grid->textFont());
            QVERIFY(panel->isVisible());
            QVERIFY(panel->findChildren<QTcpServer *>().isEmpty());
            QVERIFY(panel->findChildren<QAbstractSocket *>().isEmpty());
        }

        conn.command(smallFontCommand);
        QVERIFY(waitUntil([&] { return grid->fontSize() == 12.0 && conn.linespace() == 0; }, 5000));
        QCOMPARE(window->activeFocusItem(), input);
        QCOMPARE(input->property("text").toString(), QStringLiteral("hi"));
        QCOMPARE(cursor->connector(), &conn);
        QCOMPARE(grid->connector(), &conn);
        QCOMPARE(cursor->cellHeight(), grid->cellHeight());
        QCOMPARE(cursor->cellWidth(), grid->cellWidth());
        QCOMPARE(cursor->baseline(), grid->baseline());
        QCOMPARE(cursor->fontName(), grid->fontName());
        QCOMPARE(cursor->fontSize(), grid->fontSize());
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(input->property("text").toString(), QString());

        // Allow delayed work to run before ruling out a fabricated reply.
        QTest::qWait(250);
        QCOMPARE(model->count(), 0);

        model->appendBlock(QStringLiteral("assistant"), replyText);
        QCOMPARE(model->count(), 1);
        QCOMPARE(model->authorAt(0), QStringLiteral("assistant"));
        QCOMPARE(model->textAt(0), replyText);
        QVERIFY2(waitUntil(
                     [&] {
            auto *message = findTextItem(panel, replyText);
            return message && message->isVisible() && message->width() > 0 && message->height() > 0;
        }, 3000),
                 "Transcript text did not appear in a visible delegate");

        const QImage shot = grabItem(panel);
        QVERIFY2(!shot.isNull(), "grabToImage returned a null image for the panel");
        QVERIFY2(distinctColours(shot) > 1,
                 "Panel rendered a single flat colour (black-on-black / opacity-0 / z-order?)");

        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY2(waitUntil([&] { return !panel->isVisible(); }, 3000),
                 "Panel did not close on Escape");
        QVERIFY2(waitUntil([&] { return conn.grid()->gridCols(1) == baseCols; }, 3000),
                 "Grid columns did not restore after closing the panel");
        QVERIFY2(waitUntil([&] { return window->activeFocusItem() == gridFocus; }, 3000),
                 "Focus was not handed back to the grid after closing the panel");

        // Keystrokes reach nvim again: enter insert mode, type 'x', escape.
        QTest::keyClick(window, Qt::Key_I);
        QTest::keyClick(window, Qt::Key_X);
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY2(
            waitUntil([&] { return conn.grid()->dumpAscii().contains(QLatin1Char('x')); }, 3000),
            "nvim did not receive keystrokes after the panel closed");

        clickItem(window, toggle);
        QVERIFY(waitUntil([&] { return panel->isVisible() && window->activeFocusItem() == input; },
                          3000));
        QCOMPARE(window->findChild<ChatModel *>(), model);
        QCOMPARE(model->count(), 1);
        QCOMPARE(model->textAt(0), replyText);
        QVERIFY(panel->findChildren<QTcpServer *>().isEmpty());
        QVERIFY(panel->findChildren<QAbstractSocket *>().isEmpty());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(waitUntil(
            [&] { return !panel->isVisible() && window->activeFocusItem() == gridFocus; }, 3000));
    }
};

QTEST_MAIN(TestChatPanel)
#include "test_chat_panel.moc"
