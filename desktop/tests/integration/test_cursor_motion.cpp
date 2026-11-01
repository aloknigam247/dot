#include "CursorItem.h"
#include "IntegrationHelpers.h"
#include "support/QuickRasterizer.h"

#include <QGuiApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSGRectangleNode>
#include <QSignalSpy>
#include <QtTest>

using namespace dot;
using namespace dot::test;

namespace {

struct CursorFrame {
    QRectF bounds;
    const QSGRectangleNode *node = nullptr;
    int updates = 0;
};

class CursorProbe : public CursorItem {
public:
    using CursorItem::CursorItem;

    CursorFrame frame() const {
        const QMutexLocker lock(&m_mutex);
        return m_frame;
    }

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override {
        QSGNode *root = CursorItem::updatePaintNode(oldNode, data);
        const QSGNode *blocks = root ? root->firstChild() : nullptr;
        const auto *block =
            blocks ? dynamic_cast<const QSGRectangleNode *>(blocks->firstChild()) : nullptr;
        const QMutexLocker lock(&m_mutex);
        m_frame.bounds = block ? block->rect() : QRectF{};
        m_frame.node = block;
        ++m_frame.updates;
        return root;
    }

private:
    CursorFrame m_frame;
    mutable QMutex m_mutex;
};

QRectF cellRect(const CursorItem &cursor, QPoint cell) {
    return QRectF(cell.x() * cursor.cellWidth(), cell.y() * cursor.cellHeight(), cursor.cellWidth(),
                  cursor.cellHeight());
}

void setBlink(NvimConnector &conn, int wait, int on, int off) {
    msgpack::sbuffer bytes;
    msgpack::packer<msgpack::sbuffer> pk(&bytes);
    pk.pack_array(1);
    pk.pack_map(6);
    pk.pack("attr_id");
    pk.pack(0);
    pk.pack("blinkoff");
    pk.pack(off);
    pk.pack("blinkon");
    pk.pack(on);
    pk.pack("blinkwait");
    pk.pack(wait);
    pk.pack("cell_percentage");
    pk.pack(100);
    pk.pack("cursor_shape");
    pk.pack("block");
    const auto modes = msgpack::unpack(bytes.data(), bytes.size());
    conn.modeInfo()->setModes(modes.get(), true);
    conn.modeInfo()->setCurrentMode(QStringLiteral("normal"), 0);
}

void prepareCursor(NvimConnector &conn, CursorItem &cursor, QPoint initialCell = {}) {
    conn.grid()->resize(12, 8);
    conn.grid()->setCursor(initialCell.y(), initialCell.x());
    conn.highlights()->setDefaultColors(0xFF00FF, 0x000000, 0x000000);
    cursor.setCellHeight(20.0);
    cursor.setCellWidth(10.0);
    cursor.setSize(QSizeF(200, 200));
    cursor.setConnector(&conn);
    setBlink(conn, 0, 0, 0);
}

struct CursorScene {
    NvimConnector conn;
    QuickRasterizer raster;
    CursorProbe cursor{ raster.window()->contentItem() };

    explicit CursorScene(QPoint initialCell = {}) {
        raster.window()->setColor(Qt::black);
        prepareCursor(conn, cursor, initialCell);
    }

    QImage render() { return raster.render(&cursor); }
};

} // namespace

class TestCursorMotion : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QuickRasterizer::useSoftwareBackend();
        qRegisterMetaType<dot::Notification>("dot::Notification");
        qRegisterMetaType<dot::ObjectHandlePtr>("dot::ObjectHandlePtr");
        qInfo() << "sizeof(CursorItem):" << sizeof(CursorItem) << "bytes";
    }

    void activeGridChangesSnap() {
        CursorScene scene;
        scene.conn.grid()->resize(2, 12, 8);
        scene.conn.grid()->setCursor(1, 2, 2);
        QVERIFY(!scene.render().isNull());
        QVERIFY(scene.conn.grid()->takeDirty(1));
        QVERIFY(scene.conn.grid()->takeDirty(2));
        const auto *node = scene.cursor.frame().node;
        QVERIFY(node);

        scene.conn.grid()->setCursor(2, 2, 3);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, QPoint(3, 2)));
        QCOMPARE(scene.cursor.frame().node, node);
        QVERIFY(!scene.conn.grid()->isDirty(1));
        QVERIFY(!scene.conn.grid()->isDirty(2));

        scene.conn.grid()->setCursor(1, 2, 2);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, QPoint(2, 2)));
    }

    void adjacentMovesSnap_data() {
        QTest::addColumn<QPoint>("destination");
        QTest::newRow("diagonal") << QPoint(3, 3);
        QTest::newRow("down") << QPoint(2, 3);
        QTest::newRow("left") << QPoint(1, 2);
        QTest::newRow("right") << QPoint(3, 2);
        QTest::newRow("up") << QPoint(2, 1);
    }

    void adjacentMovesSnap() {
        QFETCH(QPoint, destination);
        CursorScene scene;
        QVERIFY(scene.raster.isUnscaled());
        scene.conn.grid()->setCursor(2, 2);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, QPoint(2, 2)));
        QVERIFY(scene.conn.grid()->takeDirty(1));
        const auto *node = scene.cursor.frame().node;
        QVERIFY(node);

        // No event-loop advancement between the move and its first rendered frame.
        scene.conn.grid()->setCursor(destination.y(), destination.x());
        const QImage image = scene.render();
        QVERIFY(!image.isNull());
        QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, destination));
        QCOMPARE(scene.cursor.frame().node, node);
        QCOMPARE(image.pixelColor(cellRect(scene.cursor, destination).center().toPoint()),
                 QColor(Qt::magenta));
        QCOMPARE(image.pixelColor(cellRect(scene.cursor, QPoint(2, 2)).center().toPoint()),
                 QColor(Qt::black));
        QVERIFY(!scene.conn.grid()->isDirty(1));
    }

    void blinkTimersAndActivityRemainIndependent() {
        NvimConnector conn;
        QQuickWindow window;
        CursorProbe cursor(window.contentItem());
        prepareCursor(conn, cursor);
        setBlink(conn, 300, 300, 300);
        conn.grid()->setCursor(2, 2);
        QVERIFY(conn.grid()->takeDirty(1));
        window.resize(200, 200);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QTRY_VERIFY_WITH_TIMEOUT(cursor.frame().node != nullptr, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(cursor.frame().node == nullptr, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(cursor.frame().node != nullptr, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(cursor.frame().node == nullptr, 5000);
        const int updates = cursor.frame().updates;

        conn.grid()->setCursor(2, 3);
        QTRY_VERIFY_WITH_TIMEOUT(cursor.frame().updates > updates, 5000);
        QVERIFY(cursor.frame().node);
        QCOMPARE(cursor.frame().bounds, cellRect(cursor, QPoint(3, 2)));
        QVERIFY(!conn.grid()->isDirty(1));
        QVERIFY(!cursor.hasActiveFocus());
    }

    void cursorMovementReachesRenderedMain() {
        NvimConnector conn;
        QQmlApplicationEngine engine;
        QVERIFY(startTestNvim(conn));
        engine.rootContext()->setContextProperty(QStringLiteral("$connector"), &conn);
        engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(waitForAttach(&conn));
        auto *cursor = window->findChild<CursorItem *>();
        QQuickItem *focus = window->activeFocusItem();
        QVERIFY(cursor);
        QVERIFY(focus);
        QVERIFY(!cursor->hasActiveFocus());

        conn.command(
            QStringLiteral("highlight Cursor guifg=#000000 guibg=#FF00FF | "
                           "set guicursor=a:block-Cursor-blinkwait0-blinkon0-blinkoff0 "
                           "nonumber norelativenumber signcolumn=no | "
                           "call setline(1, ['        cursor', '        motion']) | normal! gg0"));
        QTRY_VERIFY_WITH_TIMEOUT(
            conn.grid()->dumpAscii().startsWith(QStringLiteral("        cursor")), 10000);
        QTest::keyClick(window, Qt::Key_I);
        QTRY_COMPARE(conn.modeInfo()->currentName(), QStringLiteral("insert"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(conn.modeInfo()->currentName(), QStringLiteral("normal"));
        QTRY_COMPARE(conn.highlights()->resolved(conn.modeInfo()->attrId()).bg,
                     QColor(Qt::magenta));
        QTRY_COMPARE(conn.grid()->cursorRow(), 0);
        QTRY_COMPARE(conn.grid()->cursorCol(), 0);
        QTRY_COMPARE(conn.modeInfo()->blinkOn(), 0);

        QSignalSpy moved(conn.grid(), &GridModel::cursorChanged);
        QTest::keyClick(window, Qt::Key_L);
        QTRY_VERIFY(!moved.isEmpty());
        QTRY_COMPARE(conn.grid()->cursorCol(), 1);
        QCOMPARE(conn.grid()->cursorRow(), 0);
        QCOMPARE(window->activeFocusItem(), focus);
        QVERIFY(!cursor->hasActiveFocus());

        const auto capture = cursor->grabToImage();
        QVERIFY(capture);
        QSignalSpy captured(capture.data(), &QQuickItemGrabResult::ready);
        QVERIFY(captured.wait(5000));
        const QImage image = capture->image();
        QVERIFY(!image.isNull());
        QCOMPARE(image.width(), qRound(cursor->width()));
        QCOMPARE(image.pixelColor(cellRect(*cursor, QPoint(1, 0)).center().toPoint()),
                 QColor(Qt::magenta));
        QVERIFY(image.pixelColor(cellRect(*cursor, QPoint(0, 0)).center().toPoint()) !=
                QColor(Qt::magenta));
    }

    void dirtyGridAndLargeJumpsSnap() {
        CursorScene scene;
        scene.conn.grid()->setCursor(2, 2);
        QVERIFY(!scene.render().isNull());
        QVERIFY(scene.conn.grid()->takeDirty(1));

        scene.conn.grid()->scroll(0, 8, 0, 12, 1);
        QVERIFY(scene.conn.grid()->isDirty(1));
        scene.conn.grid()->setCursor(3, 2);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, QPoint(2, 3)));
        QVERIFY(scene.conn.grid()->isDirty(1));
        QVERIFY(scene.conn.grid()->takeDirty(1));

        scene.conn.grid()->setCursor(7, 11);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, QPoint(11, 7)));
        QVERIFY(!scene.conn.grid()->isDirty(1));
    }

    void initialPlacementAndMetricChangesUseCurrentCell() {
        CursorScene scene(QPoint(3, 2));
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, QPoint(3, 2)));

        scene.conn.grid()->setCursor(2, 4);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, QPoint(4, 2)));
        QVERIFY(scene.conn.grid()->takeDirty(1));
        scene.cursor.setCellHeight(17.5);
        scene.cursor.setCellWidth(12.5);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, QPoint(4, 2)));
        QVERIFY(!scene.conn.grid()->isDirty(1));
    }

    void rapidMovesSnap() {
        CursorScene scene;
        scene.conn.grid()->setCursor(2, 2);
        QVERIFY(!scene.render().isNull());
        QVERIFY(scene.conn.grid()->takeDirty(1));
        const auto *node = scene.cursor.frame().node;
        QVERIFY(node);

        for(const QPoint cell: { QPoint(3, 2), QPoint(4, 2), QPoint(3, 2), QPoint(2, 2),
                                 QPoint(2, 3), QPoint(2, 2) }) {
            scene.conn.grid()->setCursor(cell.y(), cell.x());
            QVERIFY(!scene.render().isNull());
            QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, cell));
            QCOMPARE(scene.cursor.frame().node, node);
            QVERIFY(!scene.conn.grid()->isDirty(1));
        }
    }

    void resetAndInvalidCursorClearNodes_data() {
        QTest::addColumn<QPoint>("invalidCell");
        QTest::newRow("column-after-end") << QPoint(12, 2);
        QTest::newRow("column-before-start") << QPoint(-1, 2);
        QTest::newRow("row-after-end") << QPoint(2, 8);
        QTest::newRow("row-before-start") << QPoint(2, -1);
    }

    void resetAndInvalidCursorClearNodes() {
        QFETCH(QPoint, invalidCell);
        CursorScene scene;
        scene.conn.grid()->setCursor(2, 2);
        QVERIFY(!scene.render().isNull());
        QVERIFY(scene.cursor.frame().node);

        scene.conn.grid()->setCursor(invalidCell.y(), invalidCell.x());
        QVERIFY(!scene.render().isNull());
        QVERIFY(!scene.cursor.frame().node);
        QVERIFY(scene.cursor.frame().bounds.isEmpty());

        scene.conn.grid()->setCursor(2, 2);
        QVERIFY(!scene.render().isNull());
        QVERIFY(scene.cursor.frame().node);
        scene.conn.grid()->reset();
        QVERIFY(!scene.render().isNull());
        QVERIFY(!scene.cursor.frame().node);

        scene.conn.grid()->resize(12, 8);
        scene.conn.grid()->setCursor(1, 1);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.cursor.frame().bounds, cellRect(scene.cursor, QPoint(1, 1)));
        scene.cursor.setConnector(nullptr);
        QVERIFY(!scene.render().isNull());
        QVERIFY(!scene.cursor.frame().node);
    }
};

QTEST_MAIN(TestCursorMotion)
#include "test_cursor_motion.moc"
