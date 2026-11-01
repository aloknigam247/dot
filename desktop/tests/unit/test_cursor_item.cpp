#include <limits>
#include <QtTest>

#include "CursorItem.h"
#include "ModeInfo.h"

using namespace dot;

class TestCursorItem : public QObject {
    Q_OBJECT
private slots:
    void blockShapeFillsCell() {
        const QRectF r = CursorItem::cursorRectFor(3, 5, 8.0, 16.0, CursorShape::Block, 20);
        QCOMPARE(r.x(), 5.0 * 8.0);
        QCOMPARE(r.y(), 3.0 * 16.0);
        QCOMPARE(r.width(), 8.0);
        QCOMPARE(r.height(), 16.0);
    }

    void verticalShapeIsLeftSliver() {
        const QRectF r = CursorItem::cursorRectFor(3, 5, 8.0, 16.0, CursorShape::Vertical, 40);
        QCOMPARE(r.x(), 5.0 * 8.0);
        QCOMPARE(r.y(), 3.0 * 16.0);
        QCOMPARE(r.width(), 8.0 * 0.40);
        QCOMPARE(r.height(), 16.0);
    }

    void horizontalShapeIsBottomSliver() {
        const QRectF r = CursorItem::cursorRectFor(3, 5, 8.0, 16.0, CursorShape::Horizontal, 30);
        const qreal expectedH = 16.0 * 0.30;
        QCOMPARE(r.x(), 5.0 * 8.0);
        QCOMPARE(r.width(), 8.0);
        QCOMPARE(r.height(), expectedH);
        QCOMPARE(r.y(), 3.0 * 16.0 + 16.0 - expectedH);
        QCOMPARE(r.bottom(), 4.0 * 16.0);
    }

    void slimShapesClampToOnePixel() {
        const QRectF vert = CursorItem::cursorRectFor(0, 0, 4.0, 4.0, CursorShape::Vertical, 1);
        const QRectF horiz = CursorItem::cursorRectFor(0, 0, 4.0, 4.0, CursorShape::Horizontal, 1);
        QCOMPARE(vert.width(), 1.0);
        QCOMPARE(horiz.height(), 1.0);
    }

    void rectAtPixelMatchesCellMath() {
        for(const CursorShape shape:
            { CursorShape::Block, CursorShape::Horizontal, CursorShape::Vertical }) {
            const QRectF cellBased = CursorItem::cursorRectFor(3, 5, 8.0, 16.0, shape, 40);
            const QRectF pixelBased =
                CursorItem::cursorRectAtPixel(QPointF(5 * 8.0, 3 * 16.0), 8.0, 16.0, shape, 40);
            QCOMPARE(cellBased, pixelBased);
        }
    }

    // Fractional cell metrics can place the cursor between whole pixels.
    void rectAtPixelHandlesFractionalPosition() {
        const QRectF r =
            CursorItem::cursorRectAtPixel(QPointF(42.5, 19.3), 8.0, 16.0, CursorShape::Block, 100);
        QCOMPARE(r.x(), 42.5);
        QCOMPARE(r.y(), 19.3);
        QCOMPARE(r.width(), 8.0);
        QCOMPARE(r.height(), 16.0);
    }

    void unionedRectCoversBothCells() {
        const QRectF oldRect = CursorItem::cursorRectFor(3, 5, 8.0, 16.0, CursorShape::Block, 100);
        const QRectF newRect = CursorItem::cursorRectFor(3, 6, 8.0, 16.0, CursorShape::Block, 100);
        const QRect dirty = oldRect.united(newRect).toAlignedRect();
        QVERIFY(dirty.contains(oldRect.toAlignedRect()));
        QVERIFY(dirty.contains(newRect.toAlignedRect()));
        QCOMPARE(dirty.width(), 16);
        QCOMPARE(dirty.height(), 16);
    }

    void cornersClampToThinShape_data() {
        QTest::addColumn<QSizeF>("size");
        QTest::addColumn<qreal>("radius");
        QTest::addColumn<qreal>("expected");
        QTest::newRow("block") << QSizeF(10, 20) << qreal(2) << qreal(2);
        QTest::newRow("empty") << QSizeF(0, 20) << qreal(2) << qreal(0);
        QTest::newRow("fractional") << QSizeF(0.5, 0.25) << qreal(2) << qreal(0.25 / 2);
        QTest::newRow("horizontal") << QSizeF(10, 1) << qreal(2) << qreal(0.5);
        QTest::newRow("infinite-height")
            << QSizeF(10, std::numeric_limits<qreal>::infinity()) << qreal(2) << qreal(0);
        QTest::newRow("infinite-radius")
            << QSizeF(10, 20) << std::numeric_limits<qreal>::infinity() << qreal(0);
        QTest::newRow("infinite-width")
            << QSizeF(std::numeric_limits<qreal>::infinity(), 20) << qreal(2) << qreal(0);
        QTest::newRow("nan-radius")
            << QSizeF(10, 20) << std::numeric_limits<qreal>::quiet_NaN() << qreal(0);
        QTest::newRow("negative-radius") << QSizeF(10, 20) << qreal(-1) << qreal(0);
        QTest::newRow("vertical") << QSizeF(1, 20) << qreal(2) << qreal(0.5);
        QTest::newRow("zero-radius") << QSizeF(10, 20) << qreal(0) << qreal(0);
    }

    void cornersClampToThinShape() {
        QFETCH(QSizeF, size);
        QFETCH(qreal, radius);
        QFETCH(qreal, expected);
        QCOMPARE(CursorItem::cornerRadiusFor(QRectF(QPointF(7.5, 9.25), size), radius), expected);
    }

    void invalidGeometryIsEmpty_data() {
        QTest::addColumn<QPointF>("origin");
        QTest::addColumn<QSizeF>("size");
        const qreal infinity = std::numeric_limits<qreal>::infinity();
        const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
        QTest::newRow("infinite-height") << QPointF(0, 0) << QSizeF(10, infinity);
        QTest::newRow("infinite-width") << QPointF(0, 0) << QSizeF(infinity, 20);
        QTest::newRow("infinite-x") << QPointF(infinity, 0) << QSizeF(10, 20);
        QTest::newRow("infinite-y") << QPointF(0, infinity) << QSizeF(10, 20);
        QTest::newRow("nan-height") << QPointF(0, 0) << QSizeF(10, nan);
        QTest::newRow("nan-width") << QPointF(0, 0) << QSizeF(nan, 20);
        QTest::newRow("nan-x") << QPointF(nan, 0) << QSizeF(10, 20);
        QTest::newRow("nan-y") << QPointF(0, nan) << QSizeF(10, 20);
        QTest::newRow("negative-height") << QPointF(0, 0) << QSizeF(10, -1);
        QTest::newRow("negative-width") << QPointF(0, 0) << QSizeF(-1, 20);
        QTest::newRow("overflow-x") << QPointF(std::numeric_limits<qreal>::max(), 0)
                                    << QSizeF(std::numeric_limits<qreal>::max(), 20);
        QTest::newRow("overflow-y") << QPointF(0, std::numeric_limits<qreal>::max())
                                    << QSizeF(10, std::numeric_limits<qreal>::max());
        QTest::newRow("zero-height") << QPointF(0, 0) << QSizeF(10, 0);
        QTest::newRow("zero-width") << QPointF(0, 0) << QSizeF(0, 20);
    }

    void invalidGeometryIsEmpty() {
        QFETCH(QPointF, origin);
        QFETCH(QSizeF, size);
        for(const CursorShape shape:
            { CursorShape::Block, CursorShape::Horizontal, CursorShape::Vertical }) {
            QCOMPARE(CursorItem::cursorRectAtPixel(origin, size.width(), size.height(), shape, 40),
                     QRectF());
        }
    }

    void percentageControlsBarExtent_data() {
        QTest::addColumn<int>("percentage");
        QTest::addColumn<qreal>("verticalWidth");
        QTest::addColumn<qreal>("horizontalHeight");
        QTest::newRow("above-maximum") << std::numeric_limits<int>::max() << qreal(10) << qreal(20);
        QTest::newRow("eighty") << 80 << qreal(10 * 0.80) << qreal(20 * 0.80);
        QTest::newRow("forty") << 40 << qreal(10 * 0.40) << qreal(20 * 0.40);
        QTest::newRow("hundred") << 100 << qreal(10) << qreal(20);
        QTest::newRow("negative") << std::numeric_limits<int>::min() << qreal(1) << qreal(1);
        QTest::newRow("one") << 1 << qreal(1) << qreal(1);
        QTest::newRow("twenty") << 20 << qreal(10 * 0.20) << qreal(20 * 0.20);
        QTest::newRow("zero") << 0 << qreal(1) << qreal(1);
    }

    void percentageControlsBarExtent() {
        QFETCH(int, percentage);
        QFETCH(qreal, verticalWidth);
        QFETCH(qreal, horizontalHeight);
        const QPointF origin(7.5, 9.25);
        const QRectF horizontal =
            CursorItem::cursorRectAtPixel(origin, 10, 20, CursorShape::Horizontal, percentage);
        const QRectF vertical =
            CursorItem::cursorRectAtPixel(origin, 10, 20, CursorShape::Vertical, percentage);
        QCOMPARE(horizontal.width(), 10.0);
        QCOMPARE(horizontal.height(), horizontalHeight);
        QCOMPARE(horizontal.bottom(), origin.y() + 20);
        QCOMPARE(vertical.topLeft(), origin);
        QCOMPARE(vertical.width(), verticalWidth);
        QCOMPARE(vertical.height(), 20.0);
    }

    void subpixelCellsContainEveryShape() {
        const QRectF cell(QPointF(7.5, 9.25), QSizeF(0.5, 0.25));
        for(const CursorShape shape:
            { CursorShape::Block, CursorShape::Horizontal, CursorShape::Vertical }) {
            const QRectF rect = CursorItem::cursorRectAtPixel(cell.topLeft(), cell.width(),
                                                              cell.height(), shape, 1);
            QCOMPARE(rect, cell);
        }
    }
};

QTEST_GUILESS_MAIN(TestCursorItem)
#include "test_cursor_item.moc"
