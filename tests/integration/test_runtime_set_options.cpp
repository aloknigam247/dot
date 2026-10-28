// Tier-2 integration test: drive a real nvim --embed, run :set commands, and
// verify the option_set redraw events propagate live into NvimConnector's
// Q_PROPERTYs. Also verifies the resize policy on metric changes (guifont):
// rows/cols stay constant, window pixel size scales to match the new metrics.

#include <QFontMetricsF>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QtTest>

#include "GridItem.h"
#include "GridModel.h"
#include "IntegrationHelpers.h"
#include "NvimConnector.h"
#include "ResizeCoalescer.h"

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

QQuickWindow *loadMainQml(QQmlApplicationEngine &engine, NvimConnector *conn) {
    engine.rootContext()->setContextProperty(QStringLiteral("$connector"), conn);
    engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
    if(engine.rootObjects().isEmpty()) return nullptr;
    return qobject_cast<QQuickWindow *>(engine.rootObjects().first());
}

QRect paintedBounds(const QImage &image) {
    QRect bounds;
    for(int y = 0; y < image.height(); ++y) {
        for(int x = 0; x < image.width(); ++x) {
            if(image.pixelColor(x, y).alpha() != 0) bounds |= QRect(x, y, 1, 1);
        }
    }
    return bounds;
}

} // namespace

class TestRuntimeSetOptions : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        qRegisterMetaType<dot::Notification>("dot::Notification");
        qRegisterMetaType<dot::ObjectHandlePtr>("dot::ObjectHandlePtr");
    }

    // `:set guifont=...` round-trips through nvim and arrives as an
    // option_set redraw event. Spy on guifontChanged and assert the
    // Q_PROPERTY contains the new value.
    void setGuifontFiresSignal() {
        NvimConnector conn;
        QVERIFY(startTestNvim(conn));
        QSignalSpy attachSpy(&conn, &NvimConnector::attachedChanged);
        QVERIFY(conn.attachUi(80, 24));
        QVERIFY(attachSpy.wait(5000));
        QVERIFY(waitForFlush(&conn));

        QSignalSpy spy(&conn, &NvimConnector::guifontChanged);
        conn.command(QStringLiteral("set guifont=Cascadia\\ Mono:h14"));
        QVERIFY2(
            waitUntil([&] { return conn.guifont() == QStringLiteral("Cascadia Mono:h14"); }, 5000),
            qPrintable(QStringLiteral("guifont was '%1'").arg(conn.guifont())));
        QVERIFY(spy.count() >= 1);
    }

    void setLinespaceFiresSignal() {
        NvimConnector conn;
        QVERIFY(startTestNvim(conn));
        QSignalSpy attachSpy(&conn, &NvimConnector::attachedChanged);
        QVERIFY(conn.attachUi(80, 24));
        QVERIFY(attachSpy.wait(5000));
        QVERIFY(waitForFlush(&conn));

        QSignalSpy spy(&conn, &NvimConnector::linespaceChanged);
        conn.command(QStringLiteral("set linespace=4"));
        QVERIFY2(waitUntil([&] { return conn.linespace() == 4; }, 5000),
                 qPrintable(QStringLiteral("linespace was %1").arg(conn.linespace())));
        QVERIFY(spy.count() >= 1);
    }

    void guifontPropagatesThroughQmlScene() {
        const QStringList backgrounds{ QStringLiteral("dark"), QStringLiteral("light") };
        const qreal fontSizes[]{ 6.0, 12.0, 24.0 };
        const QString inkText = QStringLiteral("Agjpqy|");
        const int lineSpaces[]{ 0, 6 };
        NvimConnector conn;
        QVERIFY(startTestNvim(conn));
        QSignalSpy flushSpy(&conn, &NvimConnector::flush);

        QQmlApplicationEngine engine;
        QQuickWindow *window = loadMainQml(engine, &conn);
        QVERIFY2(window, "Main.qml failed to load");
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(waitForAttach(&conn));
        QVERIFY(waitUntil([&] { return !flushSpy.isEmpty(); }, 5000));

        auto *grid = window->findChild<GridItem *>();
        auto *icon = window->findChild<QQuickItem *>(QStringLiteral("chatIcon"));
        auto *input = window->findChild<QQuickItem *>(QStringLiteral("chatInput"));
        auto *panel = window->findChild<QQuickItem *>(QStringLiteral("chatPanel"));
        auto *shell = window->findChild<QQuickItem *>(QStringLiteral("shell"));
        auto *toggle = window->findChild<QQuickItem *>(QStringLiteral("chatToggle"));
        QVERIFY(grid && icon && input && panel && shell && toggle);
        auto *metrics = shell->property("metrics").value<QObject *>();
        QVERIFY(metrics);
        QCOMPARE(metrics->property("grid").value<QObject *>(), grid);
        QCOMPARE(panel->property("metrics").value<QObject *>(), metrics);
        QCOMPARE(toggle->property("metrics").value<QObject *>(), metrics);

        const QPoint toggleCenter =
            toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, {}, toggleCenter);
        QVERIFY(waitUntil([&] { return input->hasActiveFocus(); }, 3000));

        QSignalSpy fontSpy(grid, &GridItem::fontChanged);
        QSignalSpy guifontSpy(&conn, &NvimConnector::guifontChanged);
        QSignalSpy paintedSpy(icon, SIGNAL(painted()));
        QVERIFY(paintedSpy.isValid());
        for(const QString &background: backgrounds) {
            for(qreal size: fontSizes) {
                for(int linespace: lineSpaces) {
                    const QString expectedFont = QStringLiteral("Cascadia Code:h%1").arg(size);
                    const bool fontChanges = conn.guifont() != expectedFont;
                    fontSpy.clear();
                    guifontSpy.clear();
                    paintedSpy.clear();
                    conn.command(
                        QStringLiteral("set background=%1 guifont=Cascadia\\ Code:h%2 linespace=%3")
                            .arg(background)
                            .arg(size)
                            .arg(linespace));
                    QVERIFY(waitUntil([&] {
                        return conn.guifont() == expectedFont && conn.linespace() == linespace &&
                               grid->fontSize() == size && !fontSpy.isEmpty();
                    }, 5000));
                    if(fontChanges) QVERIFY(!guifontSpy.isEmpty());
                    QVERIFY(waitUntil([&] { return !paintedSpy.isEmpty(); }, 3000));
                    QVERIFY(input->hasActiveFocus());
                    QCOMPARE(conn.guifontFamily(), QStringLiteral("Cascadia Code"));
                    QCOMPARE(conn.guifontSize(), size);
                    const auto actualBackground = evalSync(conn, QStringLiteral("&background"));
                    QVERIFY(actualBackground);
                    QCOMPARE(actualBackground->toString(), background);

                    const QFont inputFont = input->property("font").value<QFont>();
                    const QFontMetricsF fm(inputFont);
                    QCOMPARE(inputFont, grid->textFont());
                    QCOMPARE(metrics->property("textFont").value<QFont>(), grid->textFont());
                    QCOMPARE(metrics->property("cellBaseline").toReal(), grid->baseline());
                    QCOMPARE(metrics->property("cellWidth").toReal(), grid->cellWidth());
                    QCOMPARE(metrics->property("rowHeight").toReal(), grid->cellHeight());
                    QCOMPARE(fm.horizontalAdvance(QLatin1Char('M')), grid->cellWidth());
                    QCOMPARE(input->property("padding").toReal(), 4.0);
                    QCOMPARE(input->parentItem()->property("radius").toReal(), 4.0);
                    QCOMPARE(toggle->property("cornerRadius").toReal(), 4.0);
                    QCOMPARE(toggle->property("padding").toReal(), 4.0);
                    QCOMPARE(icon->property("cornerRadius").toReal(), 4.0);
                    QCOMPARE(icon->property("padding").toReal(), 4.0);
                    QCOMPARE(icon->position(), QPointF(0, 0));
                    QCOMPARE(icon->size(), toggle->size());
                    QCOMPARE(input->height() - 2 * input->property("padding").toReal(),
                             grid->cellHeight());
                    QCOMPARE(toggle->height() - 2 * toggle->property("padding").toReal(),
                             grid->cellHeight());
                    QCOMPARE(toggle->width(), toggle->height());
                    QVERIFY(grid->cellWidth() > 0 && grid->cellHeight() > 0);

                    QVERIFY(input->setProperty("text", inkText));
                    QTest::keyClick(window, Qt::Key_End);
                    const QRectF ink =
                        fm.tightBoundingRect(inkText).translated(0, input->baselineOffset());
                    const QRectF cursor = input->property("cursorRectangle").toRectF();
                    QVERIFY2(ink.top() >= 0 && ink.bottom() <= input->height(),
                             qPrintable(QStringLiteral("Ink %1..%2 exceeds input height %3")
                                            .arg(ink.top())
                                            .arg(ink.bottom())
                                            .arg(input->height())));
                    QVERIFY(cursor.top() >= 0 && cursor.bottom() <= input->height());

                    QVERIFY(input->setProperty("text", QStringLiteral("M")));
                    QTest::keyClick(window, Qt::Key_Home);
                    const qreal startX = input->property("cursorRectangle").toRectF().x();
                    QTest::keyClick(window, Qt::Key_End);
                    QCOMPARE(input->property("cursorRectangle").toRectF().x() - startX,
                             grid->cellWidth());

                    QVERIFY(input->setProperty("text", inkText.repeated(20)));
                    QVERIFY(input->property("contentWidth").toReal() > input->width());
                    for(Qt::Key key: { Qt::Key_Home, Qt::Key_End }) {
                        QTest::keyClick(window, key);
                        const QRectF visibleCursor = input->property("cursorRectangle").toRectF();
                        QVERIFY(visibleCursor.left() >= 0 &&
                                visibleCursor.right() <= input->width());
                        QVERIFY(visibleCursor.top() >= 0 &&
                                visibleCursor.bottom() <= input->height());
                    }

                    const auto grab = icon->grabToImage();
                    QVERIFY(grab);
                    QVERIFY(waitUntil([&] { return !grab->image().isNull(); }, 3000));
                    const QImage image = grab->image();
                    const QRect bounds = paintedBounds(image);
                    QVERIFY2(!bounds.isEmpty(), "The chat icon has no painted ink");
                    QVERIFY2(bounds.left() > 0 && bounds.top() > 0 &&
                                 bounds.right() < image.width() - 1 &&
                                 bounds.bottom() < image.height() - 1,
                             qPrintable(
                                 QStringLiteral("Icon ink %1,%2..%3,%4 reaches texture edge %5x%6")
                                     .arg(bounds.left())
                                     .arg(bounds.top())
                                     .arg(bounds.right())
                                     .arg(bounds.bottom())
                                     .arg(image.width())
                                     .arg(image.height())));
                }
            }
        }
    }

    void gridDetachmentDoesNotRequestResize() {
        NvimConnector conn;
        QQuickWindow window;
        GridItem grid;
        grid.setConnector(&conn);
        grid.setSize(QSizeF(800, 600));
        grid.setParentItem(window.contentItem());
        conn.resizeCoalescer()->flushNow();

        const qreal cellHeight = grid.cellHeight();
        const qreal cellWidth = grid.cellWidth();
        QSignalSpy fontSpy(&grid, &GridItem::fontChanged);
        QSignalSpy resizeSpy(conn.resizeCoalescer(), &ResizeCoalescer::resizeRequested);
        grid.setParentItem(nullptr);
        conn.resizeCoalescer()->flushNow();
        QCoreApplication::processEvents();
        QCOMPARE(fontSpy.count(), 0);
        QCOMPARE(resizeSpy.count(), 0);
        QCOMPARE(grid.cellHeight(), cellHeight);
        QCOMPARE(grid.cellWidth(), cellWidth);

        grid.setParentItem(window.contentItem());
        QVERIFY(!fontSpy.isEmpty());
        QCOMPARE(grid.cellHeight(), cellHeight);
        QCOMPARE(grid.cellWidth(), cellWidth);
        grid.setParentItem(nullptr);
    }
};

QTEST_MAIN(TestRuntimeSetOptions)
#include "test_runtime_set_options.moc"
