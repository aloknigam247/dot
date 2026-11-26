// Regression guard for the post-T6 visible resize jump.
//
// T6 (perf(boot): fire nvim_ui_attach early) issues nvim_ui_attach(80, 24)
// in main.cpp BEFORE the QQmlApplicationEngine is constructed, so the nvim
// handshake overlaps with QML cold compile. The user-visible side effect was
// that the window appeared at the 80x24 placeholder size and then visibly
// jumped to the real window geometry once Main.qml's Component.onCompleted
// issued an nvim_ui_try_resize.
//
// The fix keeps the perf win but hides the window (visible: false in Main.qml)
// until the first redraw flush arrives at the post-resize grid size. This test
// verifies that ordering:
//   1. Immediately after loadFromModule, window is NOT visible.
//   2. After the grid reaches the requested size and a flush arrives, window
//      becomes visible.
//   3. The grid 1 size at that moment is the real geometry, NOT 80x24.

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QtTest>

#include "GridModel.h"
#include "IntegrationHelpers.h"
#include "NvimConnector.h"
#include "support/PaneGeometryOracle.h"

using namespace dot;
using namespace dot::test;

namespace {

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

} // namespace

class TestBootNoResizeJump : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        qRegisterMetaType<dot::Notification>("dot::Notification");
        qRegisterMetaType<dot::ObjectHandlePtr>("dot::ObjectHandlePtr");
    }

    void windowHiddenUntilResizeFlush() {
        NvimConnector conn;
        QVERIFY(startTestNvim(conn));

        // Mirror main.cpp's early nvim_ui_attach(80, 24): the whole point of
        // this fix is to keep the window hidden during the 80x24 phase.
        QVERIFY(conn.attachUi(80, 24));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("$connector"), &conn);
        engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
        QVERIFY(!engine.rootObjects().isEmpty());
        QQuickWindow *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);

        // Step 1: window must NOT be visible immediately after load. This is
        // the load-bearing assertion — if Main.qml ever flips back to
        // visible: true at construction time, the resize jump returns.
        QVERIFY2(!window->isVisible(), "Window was visible immediately after loadFromModule — "
                                       "Main.qml must start with visible: false to avoid the "
                                       "80x24 -> real-size jump.");

        // Drive the event loop until the connector has fully attached and the
        // post-attach tryResize has propagated to grid 1.
        QVERIFY(waitUntil([&]() { return conn.attached(); }, 5000));

        // Step 2: wait for window to become visible (gated on the
        // matching-size flush in Main.qml).
        QVERIFY2(waitUntil([&]() { return window->isVisible(); }, 5000),
                 "Window never became visible — flush listener or safety "
                 "timer in Main.qml didn't fire.");

        // Step 3: at the moment of becoming visible, grid 1 must be at the
        // window's real geometry, not the 80x24 placeholder. We can't
        // intercept the exact frame of the visibility flip in a portable
        // way, but we can assert grid 1 has grown past 80x24 by the time
        // visible is true. Any reasonable 1200x780 window with a typical
        // monospace cell exceeds 80 columns.
        GridModel *grid = conn.grid();
        QVERIFY(grid);
        const int cols = grid->gridCols(1);
        const int rows = grid->gridRows(1);
        QVERIFY2(
            cols > 80 || rows > 24,
            qPrintable(QStringLiteral("Grid 1 was still at 80x24 when window became visible "
                                      "(cols=%1, rows=%2) — resize didn't complete before show.")
                           .arg(cols)
                           .arg(rows)));
    }

    void safetyTimeoutSizesActualGridBeforeShow() { exerciseHiddenStartup(Scenario::Late); }
    void largeGuifontBeforeShowShowsWithoutSafetyTimeout() {
        exerciseHiddenStartup(Scenario::Font);
    }
    void linespaceBeforeShowRetargetsRows() { exerciseHiddenStartup(Scenario::Linespace); }
    void neovimPickedSizeBeforeShowIsKept() { exerciseHiddenStartup(Scenario::Splits); }

private:
    enum class Scenario {
        Font,
        Late,
        Linespace,
        Splits
    };

    void exerciseHiddenStartup(Scenario scenario) {
        NvimConnector conn;
        QQmlApplicationEngine engine;
        const QSize initial(80, scenario == Scenario::Splits ? 60 : 24);
        QVERIFY(startTestNvim(conn));
        QVERIFY(conn.attachUi(initial.width(), initial.height()));
        QTRY_VERIFY(conn.attached() && conn.colorPalette()->chromeValid() &&
                    conn.colorPalette()->paneValid() && liveCells(conn) == initial);
        if(scenario == Scenario::Splits) {
            conn.command(QStringLiteral("lua for _ = 1, 24 do vim.cmd.split() end"));
            const auto count = evalSync(conn, QStringLiteral("winnr('$')"));
            QVERIFY(count);
            QCOMPARE(count->toInt(), 25);
        } else if(scenario == Scenario::Font) {
            conn.command(QStringLiteral("set guifont=Cascadia\\ Code:h48"));
        } else if(scenario == Scenario::Linespace) {
            conn.command(QStringLiteral("set linespace=8"));
        }

        QSignalSpy direct(&conn, &NvimConnector::nvimRequestedResize);
        QSignalSpy fits(conn.resizeCoalescer(), &ResizeCoalescer::resizeRequested);
        engine.rootContext()->setContextProperty(QStringLiteral("$connector"), &conn);
        engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && !window->isVisible());
        auto *grid = window->findChild<GridItem *>();
        QVERIFY(grid);
        const qreal dpr = window->devicePixelRatio();
        const auto oldMetrics = fontMetricsOracle(*grid, 0, dpr);
        const QSize startup(1200, 780);
        const auto oldPane = paneOracle(startup, false, oldMetrics, dpr);
        const auto requested = [&] {
            return QSize(window->property("_requestedCols").toInt(),
                         window->property("_requestedRows").toInt());
        };
        QVERIFY(requested() != initial);
        QSignalSpy rowsChanged(window, SIGNAL(_requestedRowsChanged()));
        QVERIFY(rowsChanged.isValid());
        QSize firstCells;
        QSize firstSize;
        connect(window, &QWindow::visibleChanged, window, [&](bool visible) {
            if(visible && firstSize.isEmpty()) {
                firstCells = liveCells(conn);
                firstSize = window->size();
            }
        });
        if(scenario == Scenario::Late || scenario == Scenario::Splits) {
            if(scenario == Scenario::Splits) {
                QTRY_COMPARE(direct.count(), 1);
                QVERIFY(!window->isVisible());
                QCOMPARE(direct.first()[0].toInt(), requested().width());
                QVERIFY(direct.first()[1].toInt() != requested().height());
            }
            QVERIFY(window->setProperty("_showSafetyExpired", true));
            QVERIFY(QMetaObject::invokeMethod(window, "_showIfReady"));
        }
        QTRY_VERIFY(window->isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto cm = fontMetricsOracle(*grid, conn.linespace(), dpr);
        const auto target = paneOracle(startup, false, cm, dpr);
        if(scenario == Scenario::Font || scenario == Scenario::Linespace) {
            QVERIFY(!window->property("_showSafetyExpired").toBool());
            QCOMPARE(requested(), target.cells);
            QCOMPARE(firstCells, target.cells);
            if(scenario == Scenario::Font) {
                QCOMPARE(grid->fontSize(), 48.0);
                QVERIFY(qFloor(1200 / cm.cellWidth) < 80);
                QVERIFY(target.cells != QSize(std::max(80, qFloor(1200 / cm.cellWidth)),
                                              std::max(24, qFloor(780 / cm.cellHeight))));
            } else {
                QCOMPARE(conn.linespace(), 8);
                QCOMPARE(cm.cellWidth, oldMetrics.cellWidth);
                QVERIFY(cm.cellHeight != oldMetrics.cellHeight);
                QVERIFY(target.cells.height() != oldPane.cells.height());
                QVERIFY(target.cells.height() !=
                        std::max(3, qFloor(oldPane.grid.height() / cm.cellHeight)));
                QCOMPARE(rowsChanged.count(), 1);
                QCOMPARE(fits.count(), 0);
                QCOMPARE(direct.count(), 0);
            }
        } else if(scenario == Scenario::Late) {
            QCOMPARE(firstCells, initial);
            QTRY_COMPARE(fits.count(), 1);
            QCOMPARE(fits.first(), QVariantList({ initial.width(), initial.height() }));
            QCOMPARE(direct.count(), 0);
        } else {
            QCOMPARE(firstCells, QSize(direct.first()[0].toInt(), direct.first()[1].toInt()));
            QCOMPARE(fits.count(), 0);
        }
        QCOMPARE(firstSize,
                 windowSizeOracle(firstCells.width(), firstCells.height(), false, cm, dpr));
        QTRY_COMPARE(liveCells(conn), firstCells);
        comparePaneScene(window, paneOracle(firstSize, false, cm, dpr), false);
        checkNoResizeEcho(conn);
        if(scenario == Scenario::Splits) QCOMPARE(fits.count(), 0);
    }
};

QTEST_MAIN(TestBootNoResizeJump)
#include "test_boot_no_resize_jump.moc"
