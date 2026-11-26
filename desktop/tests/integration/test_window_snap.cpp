// The shown window fits the actual grid plus its pane and frame. Resizing in
// either direction must not echo the acknowledged cell count back to Neovim.

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItemGrabResult>
#include <QScopeGuard>
#include <QScreen>
#include <QtTest>

#include "IntegrationHelpers.h"
#include "support/PaneGeometryOracle.h"
#include "support/PlatformGeometry.h"

using namespace dot;
using namespace dot::test;

namespace {

struct Scene {
    NvimConnector conn;
    QQmlApplicationEngine engine;
    GridItem *grid = nullptr;
    QQuickItem *toggle = nullptr;
    QQuickWindow *window = nullptr;

    void load() {
        engine.rootContext()->setContextProperty(QStringLiteral("$connector"), &conn);
        engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
        QVERIFY(!engine.rootObjects().isEmpty());
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        grid = window->findChild<GridItem *>();
        toggle = window->findChild<QQuickItem *>(QStringLiteral("chatToggle"));
        QVERIFY(grid && toggle);
    }

    CellMetrics metrics() const {
        return fontMetricsOracle(*grid, conn.linespace(), window->devicePixelRatio());
    }
    PaneOracle oracle(bool chat) const {
        return paneOracle(window->size(), chat, metrics(), window->devicePixelRatio());
    }
    QSize inverse(QSize cells, bool chat) const {
        return windowSizeOracle(cells.width(), cells.height(), chat, metrics(),
                                window->devicePixelRatio());
    }
    void toggleChat() {
        QTest::mouseClick(window, Qt::LeftButton, {}, sceneRect(toggle).center().toPoint());
    }
    void requestFromNvim(QSize target, bool chat, int settleMs = 0) {
        const QSize expected = inverse(target, chat);
        QVERIFY(liveCells(conn) != target);
        QVERIFY(window->size() != expected);
        QSignalSpy fits(conn.resizeCoalescer(), &ResizeCoalescer::resizeRequested);
        conn.command(
            QStringLiteral("set columns=%1 lines=%2").arg(target.width()).arg(target.height()));
        QTRY_COMPARE(liveCells(conn), target);
        QCOMPARE(window->size(), expected);
        deliverPlatformGeometry(window, window->geometry());
        comparePaneScene(window, oracle(chat), chat);
        if(settleMs) QTest::qWait(settleMs);
        checkNoResizeEcho(conn);
        QCOMPARE(window->size(), expected);
        QCOMPARE(liveCells(conn), target);
        QCOMPARE(fits.count(), 0);
    }
};

void checkShortChat(Scene &s, bool zero) {
    const auto band = s.oracle(true).bandHeight;
    const int paneHeight = zero ? 2 : qRound(band);
    auto rect = s.window->geometry();
    rect.setHeight(qRound(band) + 8 + paneHeight);
    deliverPlatformGeometry(s.window, rect);
    const auto expected = s.oracle(true);
    QCOMPARE(expected.chatPane.height(), qreal(paneHeight));
    if(!zero) QVERIFY(paneHeight > 4 && paneHeight < band + 4);
    comparePaneScene(s.window, expected, true);
    if(zero) {
        auto *panel = s.window->findChild<QQuickItem *>(QStringLiteral("chatPanel"));
        QVERIFY(panel);
        const auto items = panel->findChildren<QQuickItem *>();
        auto it = std::find_if(items.cbegin(), items.cend(),
                               [](auto *item) { return item->inherits("QQuickListView"); });
        QVERIFY(it != items.cend());
        QCOMPARE((*it)->height(), 0.0);
    }
    QCOMPARE(s.window->color(), s.conn.colorPalette()->frame());
    const auto grab = s.window->contentItem()->grabToImage();
    QVERIFY(grab);
    QTRY_VERIFY(!grab->image().isNull());
    const QImage image = grab->image();
    const QRect pane = expected.chatPane.toRect();
    QVERIFY(image.size() == rect.size());
    for(int x = pane.left(); x <= pane.right(); ++x) {
        for(int y = pane.bottom() + 1; y < image.height(); ++y)
            QCOMPARE(image.pixelColor(x, y).alpha(), 0);
        if(!zero) {
            const qreal left = expected.chatPane.left() + 6;
            const qreal right = expected.chatPane.right() - 6;
            if(x + 0.5 >= left && x + 0.5 <= right) continue;
            const QPointF center(x + 0.5 < left ? left : right, expected.chatPane.bottom() - 6);
            for(int y = pane.bottom() - 5; y <= pane.bottom(); ++y) {
                if(QLineF(center, QPointF(x + 0.5, y + 0.5)).length() > 7)
                    QCOMPARE(image.pixelColor(x, y).alpha(), 0);
            }
        }
    }
}

void checkMinimum(Scene &s) {
    auto r = s.window->geometry();
    r.setSize({ 200, 100 });
    QSignalSpy direct(&s.conn, &NvimConnector::nvimRequestedResize);
    QSignalSpy fits(s.conn.resizeCoalescer(), &ResizeCoalescer::resizeRequested);
    deliverPlatformGeometry(s.window, r);
    const auto expected = s.oracle(true);
    QVERIFY(!expected.grid.isEmpty());
    QVERIFY(expected.grid.width() < 12 * s.metrics().cellWidth);
    QVERIFY(expected.grid.height() < 3 * s.metrics().cellHeight);
    QCOMPARE(expected.cells, QSize(12, 3));
    QTRY_COMPARE(liveCells(s.conn), expected.cells);
    QCOMPARE(fits.count(), 1);
    QCOMPARE(fits.first(), QVariantList({ 12, 3 }));
    QCOMPARE(direct.count(), 0);
    comparePaneScene(s.window, expected, true);
    FlushLatch flush(&s.conn);
    s.conn.command(QStringLiteral("set nowrap | call setline(1, repeat('x', 200)) | "
                                  "syntax match DotRed /x/ | hi DotRed guibg=#ff0000 | redraw!"));
    QVERIFY(flush.wait());
    const auto grab = s.window->contentItem()->grabToImage();
    QVERIFY(grab);
    QTRY_VERIFY(!grab->image().isNull());
    const QImage image = grab->image();
    bool inside = false;
    for(int y = 0; y < image.height(); ++y) {
        for(int x = 0; x < image.width(); ++x) {
            if(image.pixelColor(x, y) != QColor(Qt::red)) continue;
            QVERIFY(expected.grid.contains(QPointF(x + 0.5, y + 0.5)));
            inside = true;
        }
    }
    QVERIFY(inside);
}

} // namespace

class TestWindowSnap : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        qRegisterMetaType<dot::Notification>("dot::Notification");
        qRegisterMetaType<dot::ObjectHandlePtr>("dot::ObjectHandlePtr");
    }

    void shownWindowFitsActualGrid() {
        Scene s;
        QVERIFY(startTestNvim(s.conn));
        s.load();
        QVERIFY(s.window && QTest::qWaitForWindowExposed(s.window));
        const QSize cells = liveCells(s.conn);
        QVERIFY(!cells.isEmpty());
        QCOMPARE(s.window->size(), s.inverse(cells, false));
        comparePaneScene(s.window, s.oracle(false), false);
        checkNoResizeEcho(s.conn);
    }

    void nvimSetColumnsAndLinesResizesWindow() {
        Scene s;
        QVERIFY(startTestNvim(s.conn));
        s.load();
        QVERIFY(s.window && QTest::qWaitForWindowExposed(s.window));
        const QSize boot = liveCells(s.conn);
        QVERIFY(boot.width() > 20);
        QVERIFY(boot.height() > 10);
        s.requestFromNvim(boot - QSize(7, 4), false, 200);
    }

    void paneInsetsParticipateInBothResizeDirections_data() {
        QTest::addColumn<QString>("action");
        QTest::addColumn<bool>("chat");
        for(const auto *name:
            { "chat-short", "chat-zero", "dock", "empty", "fullscreen", "guifont", "linespace",
              "minimum", "nvim-request", "nvim-request-small", "stale-ack", "startup", "undock" }) {
            const QString action = QString::fromLatin1(name);
            const bool chat =
                action.startsWith("chat-") || action == "minimum" || action == "undock";
            QTest::newRow(name) << action << chat;
            if(action == "fullscreen" || action == "guifont" || action == "linespace" ||
               action == "nvim-request")
                QTest::newRow(qPrintable(action + "-chat")) << action << true;
        }
    }

    void paneInsetsParticipateInBothResizeDirections() {
        QFETCH(QString, action);
        QFETCH(bool, chat);
        Scene s;
        QVERIFY(startTestNvim(s.conn));
        QVERIFY(s.conn.attachUi(80, 24));
        QVERIFY(waitForAttach(&s.conn));
        QSignalSpy fits(s.conn.resizeCoalescer(), &ResizeCoalescer::resizeRequested);
        QList<QSize> flushes;
        connect(&s.conn, &NvimConnector::flush, this, [&] { flushes.append(liveCells(s.conn)); });
        s.load();
        QVERIFY(s.grid && s.window && s.toggle && !s.window->isVisible());
        qsizetype showFlush = -1;
        QSize firstGrid;
        // Main's QML flush handler runs before C++ connections, and the palette only becomes
        // valid during flush handling, so the show lands inside the flush recorded at this index.
        connect(s.window, &QWindow::visibleChanged, s.window, [&](bool visible) {
            if(visible && firstGrid.isEmpty()) {
                showFlush = flushes.size();
                firstGrid = liveCells(s.conn);
            }
        });
        QVERIFY(QTest::qWaitForWindowExposed(s.window));
        const qreal dpr = s.window->devicePixelRatio();
        const auto cm = s.metrics();
        if(action == "startup") {
            const auto target = paneOracle({ 1200, 780 }, false, cm, dpr).cells;
            QVERIFY(target != QSize(std::max(80, qFloor(1200 / cm.cellWidth)),
                                    std::max(24, qFloor(780 / cm.cellHeight))));
            QVERIFY(showFlush >= 0);
            QTRY_VERIFY(flushes.size() > showFlush);
            QCOMPARE(flushes.at(showFlush), target);
            QCOMPARE(firstGrid, target);
            QCOMPARE(s.window->property("_requestedCols").toInt(), target.width());
            QCOMPARE(s.window->property("_requestedRows").toInt(), target.height());
            QVERIFY(!s.window->property("_showSafetyExpired").toBool());
            QCOMPARE(s.window->size(), s.inverse(target, false));
            QTest::qWait(100);
            QCOMPARE(fits.count(), 0);
        }
        if(action == "minimum") {
            s.conn.command(QStringLiteral("set guifont=Cascadia\\ Code:h24"));
            QTRY_COMPARE(s.grid->fontSize(), 24.0);
            QTRY_COMPARE(liveCells(s.conn), s.oracle(false).cells);
        }
        if(chat) {
            s.toggleChat();
            QTRY_COMPARE(liveCells(s.conn), s.oracle(true).cells);
        }
        const QRect saved = s.window->geometry();
        const QSize before = liveCells(s.conn);
        fits.clear();
        if(action == "dock" || action == "undock") {
            s.toggleChat();
            chat = !chat;
            QTRY_COMPARE(liveCells(s.conn), s.oracle(chat).cells);
            QCOMPARE(fits.count(), 1);
            QCOMPARE(s.window->geometry(), saved);
        } else if(action.startsWith("nvim-request")) {
            s.requestFromNvim(action == "nvim-request" ? QSize(100, 30) : QSize(12, 3), chat);
        } else if(action == "stale-ack") {
            QRect larger = saved;
            larger.setSize(saved.size() +
                           QSize(qRound(3 * cm.cellWidth), qRound(2 * cm.cellHeight)));
            const QSize a = before - QSize(2, 1);
            const QSize b = paneOracle(larger.size(), false, cm, dpr).cells;
            QVERIFY(before != a && before != b && a != b);
            QList<QSize> seen;
            connect(s.conn.grid(), &GridModel::sizeChanged, this,
                    [&] { seen.append(liveCells(s.conn)); });
            QSignalSpy direct(&s.conn, &NvimConnector::nvimRequestedResize);
            deliverPlatformGeometry(s.window, larger);
            QSignalSpy widths(s.window, &QWindow::widthChanged);
            QSignalSpy heights(s.window, &QWindow::heightChanged);
            s.conn.tryResize(a.width(), a.height());
            s.conn.tryResize(b.width(), b.height());
            QTRY_COMPARE(liveCells(s.conn), b);
            QVERIFY(seen.indexOf(a) >= 0 && seen.indexOf(b, seen.indexOf(a) + 1) >= 0);
            checkNoResizeEcho(s.conn);
            QCOMPARE(s.window->geometry(), larger);
            QCOMPARE(direct.count(), 0);
            QCOMPARE(widths.count(), 0);
            QCOMPARE(heights.count(), 0);
            QCOMPARE(fits.count(), 0);
        } else if(action == "fullscreen") {
            auto *screen = s.window->screen();
            const QRect original = screen->geometry();
            const QRect available = screen->availableGeometry();
            const QRect full(original.topLeft(), QSize(1920, 1080));
            const auto restore = qScopeGuard([&] {
                QWindowSystemInterface::handleScreenGeometryChange(screen, original, available);
                QWindowSystemInterface::flushWindowSystemEvents();
            });
            QWindowSystemInterface::handleScreenGeometryChange(screen, full, full);
            QWindowSystemInterface::flushWindowSystemEvents();
            QCOMPARE(screen->geometry(), full);
            const auto target = paneOracle(full.size(), chat, cm, dpr).cells;
            QVERIFY(target.width() > 12);
            QVERIFY(target.width() != paneOracle(full.size(), !chat, cm, dpr).cells.width());
            const auto dock = s.oracle(chat).chatPane.width();
            QVERIFY(target != QSize(std::max(1, qFloor((full.width() - dock) / cm.cellWidth)),
                                    std::max(1, qFloor(full.height() / cm.cellHeight))));
            s.window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(s.window));
            QTest::keyClick(s.window, Qt::Key_F11);
            QCOMPARE(s.window->visibility(), QWindow::FullScreen);
            QTRY_COMPARE(liveCells(s.conn), target);
            deliverPlatformGeometry(s.window, full);
            comparePaneScene(s.window, s.oracle(chat), chat);
            checkNoResizeEcho(s.conn);
            QCOMPARE(fits.count(), 0);
            QTest::keyClick(s.window, Qt::Key_F11);
            QCOMPARE(s.window->visibility(), QWindow::Windowed);
            deliverPlatformGeometry(s.window, saved);
            QTRY_COMPARE(liveCells(s.conn), before);
        } else if(action == "guifont" || action == "linespace") {
            if(action == "guifont") {
                s.conn.command(QStringLiteral("set guifont=Cascadia\\ Code:h24"));
                QTRY_COMPARE(s.grid->fontSize(), 24.0);
                QVERIFY(s.metrics().cellWidth != cm.cellWidth);
            } else {
                s.conn.command(QStringLiteral("set linespace=4"));
                QTRY_COMPARE(s.conn.linespace(), 4);
            }
            QVERIFY(s.metrics().cellHeight != cm.cellHeight);
            const QSize target = s.oracle(chat).cells;
            QVERIFY(target != before);
            QTRY_COMPARE(liveCells(s.conn), target);
            QCOMPARE(fits.count(), 1);
            QCOMPARE(s.window->geometry(), saved);
        } else if(action == "minimum") {
            checkMinimum(s);
        } else if(action.startsWith("chat-")) {
            checkShortChat(s, action == "chat-zero");
        } else if(action == "empty") {
            QVERIFY(before.width() > 12 && before.height() > 3);
            deliverPlatformGeometry(s.window, QRect(saved.topLeft(), QSize(6, 6)));
            QVERIFY(s.oracle(false).grid.isEmpty());
            comparePaneScene(s.window, s.oracle(false), false);
            s.conn.resizeCoalescer()->flushNow();
            QCOMPARE(fits.count(), 0);
            QCOMPARE(liveCells(s.conn), before);
            deliverPlatformGeometry(s.window, saved);
            checkNoResizeEcho(s.conn);
            QCOMPARE(fits.count(), 0);
            QCOMPARE(liveCells(s.conn), before);
        }
        if(action == "dock" || action == "undock" || action == "guifont" || action == "linespace") {
            const auto target = s.oracle(chat).cells;
            QCOMPARE(fits.first(), QVariantList({ target.width(), target.height() }));
        }
        comparePaneScene(s.window, s.oracle(chat), chat);
        checkNoResizeEcho(s.conn);
    }
};

QTEST_MAIN(TestWindowSnap)
#include "test_window_snap.moc"
