#include "CursorItem.h"
#include "GridItem.h"
#include "ImageItem.h"
#include "NvimConnector.h"
#include "support/ImageHelpers.h"
#include "support/ImageRenderHelpers.h"
#include "support/QuickRasterizer.h"

#include <atomic>
#include <limits>
#include <memory>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>
#include <QVector>

using namespace dot;
using namespace dot::test;

namespace {

void defineHighlight(HighlightTable &table, int id, QColor foreground, QColor background,
                     bool undercurl = false, const QString &roundedName = {}) {
    msgpack::sbuffer bytes;
    msgpack::packer<msgpack::sbuffer> pk(&bytes);
    pk.pack_map(6);
    pk.pack("background");
    pk.pack(static_cast<int>(background.rgb() & 0xFFFFFF));
    pk.pack("bold");
    pk.pack(true);
    pk.pack("foreground");
    pk.pack(static_cast<int>(foreground.rgb() & 0xFFFFFF));
    pk.pack("special");
    pk.pack(0x0000FF);
    pk.pack("undercurl");
    pk.pack(undercurl);
    pk.pack("underline");
    pk.pack(false);
    const auto attr = msgpack::unpack(bytes.data(), bytes.size());
    if(roundedName.isEmpty()) {
        table.defineAttr(id, attr.get());
        return;
    }
    msgpack::sbuffer infoBytes;
    msgpack::packer<msgpack::sbuffer> infoPk(&infoBytes);
    infoPk.pack_array(1);
    infoPk.pack_map(1);
    infoPk.pack("hi_name");
    infoPk.pack(roundedName.toStdString());
    const auto info = msgpack::unpack(infoBytes.data(), infoBytes.size());
    table.defineAttr(id, attr.get(), &info.get());
}

void putCells(GridModel &grid, int row, int col, const QString &text, int highlight,
              int count = 1) {
    msgpack::sbuffer bytes;
    msgpack::packer<msgpack::sbuffer> pk(&bytes);
    pk.pack_array(1);
    pk.pack_array(3);
    pk.pack(text.toStdString());
    pk.pack(highlight);
    pk.pack(count);
    const auto cells = msgpack::unpack(bytes.data(), bytes.size());
    grid.applyLine(1, row, col, cells.get());
}

QColor pixel(const QImage &image, QPointF logical, qreal dpr) {
    const QPoint point(qFloor(logical.x() * dpr), qFloor(logical.y() * dpr));
    if(!image.rect().contains(point)) {
        qWarning() << "Image probe is outside the captured image" << point << image.size();
        return {};
    }
    return image.pixelColor(point);
}

std::optional<QPoint> findColor(const QImage &image, const QRectF &logical, qreal dpr,
                                QColor color) {
    const QRect bounds = QRectF(logical.topLeft() * dpr, logical.size() * dpr)
                             .toAlignedRect()
                             .intersected(image.rect());
    for(int y = bounds.top(); y <= bounds.bottom(); ++y) {
        for(int x = bounds.left(); x <= bounds.right(); ++x) {
            if(image.pixelColor(x, y) == color) return QPoint(x, y);
        }
    }
    return std::nullopt;
}

bool hasBlue(const QImage &image, const QRectF &logical, qreal dpr) {
    const QRect bounds = QRectF(logical.topLeft() * dpr, logical.size() * dpr)
                             .toAlignedRect()
                             .intersected(image.rect());
    for(int y = bounds.top(); y <= bounds.bottom(); ++y) {
        for(int x = bounds.left(); x <= bounds.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if(color.blue() > 0 && color.blue() > color.green()) return true;
        }
    }
    return false;
}

struct Fixture {
    NvimConnector conn;
    CountingGrid grid;
    CountingImages images;
    std::unique_ptr<QuickRasterizer> raster = std::make_unique<QuickRasterizer>();

    Fixture() {
        conn.grid()->resize(20, 12);
        conn.highlights()->setDefaultColors(0xFFFFFF, 0x000000, 0x0000FF);
        grid.setConnector(&conn);
        grid.setFontName(QStringLiteral("Courier New"));
        grid.setFontSize(18);
        fit();
        images.setGrid(&grid);
        images.setModel(conn.images());
        conn.images()->queueResize(QSize(20, 12));
        conn.images()->flush();
        conn.flush();
        if(paint().isNull()) qFatal("Could not initialize the image test scene");
        fit();
        if(paint().isNull()) qFatal("Could not render the measured image test scene");
    }

    void fit() { grid.setSize(QSizeF(20 * grid.cellWidth(), 12 * grid.cellHeight())); }

    QImage paint() { return raster->render(&grid); }

    qreal dpr() const { return raster->window()->effectiveDevicePixelRatio(); }

    QQuickItem *item(quint32 id) {
        return images.findChild<QQuickItem *>(QStringLiteral("image_%1").arg(id));
    }
};

} // namespace

class TestImageRender : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QuickRasterizer::useSoftwareBackend();
        QVERIFY(QGuiApplication::primaryScreen() != nullptr);
        qDebug(
            "image object sizes (bytes): model=%zu options=%zu placement=%zu manager=%zu grid-runs=%zu cell-run=%zu",
            sizeof(ImageModel), sizeof(ImageOptions), sizeof(ImagePlacement), sizeof(ImageItem),
            sizeof(GridRuns), sizeof(CellRun));
    }

    void actualDeviceScaleAndImagePixelsAgree() {
        Fixture f;
        QVERIFY(!f.paint().isNull());
        f.fit();
        const auto id =
            createSync(*f.conn.images(), makePng(QSize(4, 4), QImage::Format_RGB32, Qt::red),
                       { .col = 3, .row = 3, .width = 4 });
        QVERIFY(id.has_value());
        const QImage image = f.paint();
        QVERIFY(!image.isNull());
        const qreal expectedDpr = qEnvironmentVariable("QT_SCALE_FACTOR", "1").toDouble();
        QVERIFY(qFuzzyCompare(f.dpr(), expectedDpr));
        QCOMPARE(image.width(), qRound(f.raster->window()->width() * f.dpr()));
        QCOMPARE(image.height(), qRound(f.raster->window()->height() * f.dpr()));
        QQuickItem *item = f.item(*id);
        QVERIFY(item);
        QCOMPARE(item->x(), 2 * f.grid.cellWidth());
        QCOMPARE(item->y(), 2 * f.grid.cellHeight());
        QCOMPARE(item->width(), 4 * f.grid.cellWidth());
        QCOMPARE(pixel(image,
                       QPointF(item->x() + item->width() / 2, item->y() + item->height() / 2),
                       f.dpr()),
                 QColor(Qt::red));
        QCOMPARE(pixel(image, QPointF(f.grid.cellWidth() / 2, f.grid.cellHeight() / 2), f.dpr()),
                 QColor(Qt::black));
    }

    void alphaAspectAndMissingDimensionsUseTheSpecifiedSizing() {
        Fixture f;
        QVERIFY(!f.paint().isNull());
        const qreal cw = f.grid.cellWidth();
        const qreal ch = f.grid.cellHeight();
        const QByteArray png =
            makePng(QSize(20, 10), QImage::Format_ARGB32, QColor(255, 0, 0, 128));
        const auto both =
            createSync(*f.conn.images(), png, { .col = 3, .height = 6, .row = 2, .width = 6 });
        QVERIFY(both.has_value());
        QImage image = f.paint();
        QVERIFY(!image.isNull());
        QQuickItem *item = f.item(*both);
        QVERIFY(item);
        QCOMPARE(item->width(), 6 * cw);
        QCOMPARE(item->height(), 3 * cw);
        QCOMPARE(item->y(), ch + (6 * ch - 3 * cw) / 2);
        QCOMPARE(pixel(image,
                       QPointF(item->x() + item->width() / 2, item->y() + item->height() / 2),
                       f.dpr()),
                 QColor(128, 0, 0));
        QCOMPARE(pixel(image, QPointF(3 * cw, ch + 1), f.dpr()), QColor(Qt::black));
        QVERIFY(f.conn.images()->remove(*both));

        const auto height = createSync(*f.conn.images(), png, { .col = 3, .height = 2, .row = 2 });
        QVERIFY(height.has_value());
        QVERIFY(!f.paint().isNull());
        QCOMPARE(f.item(*height)->height(), 2 * ch);
        QCOMPARE(f.item(*height)->width(), 4 * ch);
        QVERIFY(f.conn.images()->remove(*height));

        const auto natural = createSync(*f.conn.images(), png);
        QVERIFY(natural.has_value());
        QVERIFY(!f.paint().isNull());
        QCOMPARE(f.item(*natural)->position(), QPointF(0, 0));
        QCOMPARE(f.item(*natural)->size(), QSizeF(20, 10));
    }

    void cursorRemainsAboveTheEntireImageSubtree() {
        Fixture f;
        QVERIFY(!f.paint().isNull());
        defineHighlight(*f.conn.highlights(), 2, Qt::black, Qt::magenta);
        msgpack::sbuffer bytes;
        msgpack::packer<msgpack::sbuffer> pk(&bytes);
        pk.pack_array(1);
        pk.pack_map(6);
        pk.pack("attr_id");
        pk.pack(2);
        pk.pack("blinkoff");
        pk.pack(0);
        pk.pack("blinkon");
        pk.pack(0);
        pk.pack("blinkwait");
        pk.pack(0);
        pk.pack("cell_percentage");
        pk.pack(100);
        pk.pack("cursor_shape");
        pk.pack("block");
        const auto modes = msgpack::unpack(bytes.data(), bytes.size());
        f.conn.modeInfo()->setModes(modes.get(), true);
        f.conn.modeInfo()->setCurrentMode(QStringLiteral("normal"), 0);
        f.conn.grid()->setCursor(4, 4);

        CursorItem cursor;
        cursor.setParentItem(f.raster->window()->contentItem());
        cursor.setZ(99);
        cursor.setConnector(&f.conn);
        cursor.setCellWidth(f.grid.cellWidth());
        cursor.setCellHeight(f.grid.cellHeight());
        cursor.setBaseline(f.grid.baseline());
        cursor.setFontName(f.grid.fontName());
        cursor.setFontSize(f.grid.fontSize());
        cursor.setSize(f.grid.size());
        const QPointF probe(4.5 * f.grid.cellWidth(), 4.5 * f.grid.cellHeight());
        QCOMPARE(pixel(f.paint(), probe, f.dpr()), QColor(Qt::magenta));
        const auto id =
            createSync(*f.conn.images(), makePng(QSize(4, 4), QImage::Format_RGB32, Qt::red),
                       { .col = 1,
                         .height = 12,
                         .row = 1,
                         .width = 20,
                         .zIndex = std::numeric_limits<qint32>::max() });
        QVERIFY(id.has_value());
        const QImage image = f.paint();
        QVERIFY(!image.isNull());
        QCOMPARE(pixel(image, probe, f.dpr()), QColor(Qt::magenta));
        QVERIFY(!cursor.hasActiveFocus());
    }

    void fontLinespaceAndGridResizeUpdatePlacementWithoutUploadingAgain() {
        Fixture f;
        QVERIFY(!f.paint().isNull());
        const auto id =
            createSync(*f.conn.images(), makePng(QSize(20, 10), QImage::Format_RGB32, Qt::red),
                       { .col = 3, .row = 2, .width = 4 });
        QVERIFY(id.has_value());
        QVERIFY(!f.paint().isNull());
        QQuickItem *item = f.item(*id);
        QVERIFY(item);
        const qreal oldWidth = item->width();
        const int uploads = f.images.textureAttempts.load();
        f.grid.setFontSize(24);
        f.fit();
        QVERIFY(!f.paint().isNull());
        QVERIFY(!qFuzzyCompare(item->width(), oldWidth));
        QCOMPARE(item->x(), 2 * f.grid.cellWidth());
        QCOMPARE(item->y(), f.grid.cellHeight());
        QCOMPARE(item->width(), 4 * f.grid.cellWidth());
        const qreal oldY = item->y();
        f.conn.onOptionSet(QStringLiteral("linespace"), 4);
        f.fit();
        QVERIFY(!f.paint().isNull());
        QVERIFY(item->y() > oldY);
        QCOMPARE(item->y(), f.grid.cellHeight());
        QCOMPARE(f.images.textureAttempts.load(), uploads);
        f.conn.images()->queueResize(QSize(5, 3));
        f.conn.images()->flush();
        const QImage clipped = f.paint();
        QVERIFY(!clipped.isNull());
        QCOMPARE(item->width(), 3 * f.grid.cellWidth());
        QCOMPARE(pixel(clipped, QPointF(5.5 * f.grid.cellWidth(), item->y() + 1), f.dpr()),
                 QColor(Qt::black));
        QCOMPARE(f.images.textureAttempts.load(), uploads);
    }

    void gridLayersReactToHighlightOnlyChanges() {
        Fixture f;
        f.conn.highlights()->setRoundedHighlights({ QStringLiteral("ImagePill") });
        defineHighlight(*f.conn.highlights(), 1, Qt::white, Qt::red, false,
                        QStringLiteral("ImagePill"));
        putCells(*f.conn.grid(), 4, 4, QStringLiteral(" "), 1);
        f.conn.flush();
        const QPointF highlighted(4.5 * f.grid.cellWidth(), 4.5 * f.grid.cellHeight());
        QCOMPARE(pixel(f.paint(), highlighted, f.dpr()), QColor(Qt::red));
        QVERIFY(!f.conn.grid()->isDirty(1));
        defineHighlight(*f.conn.highlights(), 1, Qt::white, Qt::blue, false,
                        QStringLiteral("ImagePill"));
        QCOMPARE(pixel(f.paint(), highlighted, f.dpr()), QColor(Qt::blue));
        f.conn.highlights()->setDefaultColors(0xFFFFFF, 0x00FF00, 0x0000FF);
        QCOMPARE(pixel(f.paint(), QPointF(1, 1), f.dpr()), QColor(Qt::green));
        const int backgrounds = f.grid.backgrounds.load();
        const int foregrounds = f.grid.foregrounds.load();
        f.grid.setDebugOverlay(true);
        QVERIFY(!f.paint().isNull());
        QVERIFY(f.grid.backgrounds.load() > backgrounds);
        QVERIFY(f.grid.foregrounds.load() > foregrounds);
        f.grid.setDebugOverlay(false);
        QCOMPARE(pixel(f.paint(), highlighted, f.dpr()), QColor(Qt::blue));
        f.grid.setConnector(nullptr);
        QCOMPARE(pixel(f.paint(), highlighted, f.dpr()), QColor(Qt::black));
    }

    void hiddenImagesRetainPixelsUntilExplicitReplacement() {
        Fixture f;
        const int backgrounds = f.grid.backgrounds.load();
        const int foregrounds = f.grid.foregrounds.load();
        const auto id =
            createSync(*f.conn.images(), makePng(QSize(4, 4), QImage::Format_RGB32, Qt::red),
                       { .col = 200, .row = 200, .width = 4 });
        QVERIFY(id.has_value());
        QVERIFY(!f.paint().isNull());
        QQuickItem *item = f.item(*id);
        QVERIFY(item);
        QVERIFY(!item->isVisible());
        QCOMPARE(f.images.textureAttempts.load(), 0);
        const qint64 cached = f.conn.images()->cachedBytes();
        for(int iteration = 0; iteration < 2; ++iteration) {
            QVERIFY(f.conn.images()->update(*id, { .col = 3, .row = 3 }).has_value());
            const QImage image = f.paint();
            QVERIFY(!image.isNull());
            QVERIFY(item->isVisible());
            QCOMPARE(pixel(image, item->position() + QPointF(item->width() / 2, item->height() / 2),
                           f.dpr()),
                     QColor(Qt::red));
            QCOMPARE(f.images.textureAttempts.load(), 1);
            QVERIFY(f.conn.images()->update(*id, { .col = 200 }).has_value());
            QVERIFY(!f.paint().isNull());
            QVERIFY(!item->isVisible());
            QCOMPARE(f.conn.images()->cachedBytes(), cached);
        }
        QCOMPARE(f.grid.backgrounds.load(), backgrounds);
        QCOMPARE(f.grid.foregrounds.load(), foregrounds);
    }

    void imageOnlyOperationsDoNotSynchronizeAnyGridLayer() {
        Fixture f;
        QVERIFY(!f.paint().isNull());
        const int backgrounds = f.grid.backgrounds.load();
        const int foregrounds = f.grid.foregrounds.load();
        QVERIFY(backgrounds > 0);
        QVERIFY(foregrounds > 0);
        QVERIFY(!f.paint().isNull());
        QCOMPARE(f.grid.backgrounds.load(), backgrounds);
        QCOMPARE(f.grid.foregrounds.load(), foregrounds);

        const auto id =
            createSync(*f.conn.images(), makePng(QSize(4, 4), QImage::Format_RGB32, Qt::red),
                       { .col = 3, .row = 3, .width = 4 });
        QVERIFY(id.has_value());
        QVERIFY(!f.paint().isNull());
        QQuickItem *item = f.item(*id);
        QVERIFY(item);
        QCOMPARE(f.images.textureAttempts.load(), 1);
        QCOMPARE(f.grid.backgrounds.load(), backgrounds);
        QCOMPARE(f.grid.foregrounds.load(), foregrounds);
        QVERIFY(f.conn.images()->update(*id, { .col = 5, .row = 4 }).has_value());
        QVERIFY(!f.paint().isNull());
        QCOMPARE(f.item(*id), item);
        QVERIFY(f.conn.images()->update(*id, { .width = 6 }).has_value());
        QVERIFY(!f.paint().isNull());
        for(const qint32 z:
            { std::numeric_limits<qint32>::min(), qint32(-1), qint32(0), qint32(9) }) {
            QVERIFY(f.conn.images()->update(*id, { .zIndex = z }).has_value());
            QVERIFY(!f.paint().isNull());
        }
        QCOMPARE(f.images.textureAttempts.load(), 1);
        QCOMPARE(f.grid.backgrounds.load(), backgrounds);
        QCOMPARE(f.grid.foregrounds.load(), foregrounds);
        QVERIFY(f.conn.images()->remove(*id));
        QVERIFY(!f.paint().isNull());
        QCOMPARE(f.grid.backgrounds.load(), backgrounds);
        QCOMPARE(f.grid.foregrounds.load(), foregrounds);

        putCells(*f.conn.grid(), 1, 1, QStringLiteral("M"), 0);
        f.conn.flush();
        QVERIFY(!f.paint().isNull());
        QVERIFY(f.grid.backgrounds.load() > backgrounds);
        QVERIFY(f.grid.foregrounds.load() > foregrounds);
    }

    void largeGridImageUpdatesReuseTexturesAndKeepGridLayersClean() {
        constexpr int imageCount = 4;
        constexpr int updates = 100;
        qint64 updateNs = 0;
        Fixture f;
        QElapsedTimer timer;
        QVector<quint32> ids;
        ids.reserve(imageCount);
        f.grid.setFontSize(10);
        f.conn.grid()->resize(200, 60);
        f.grid.setSize(QSizeF(200 * f.grid.cellWidth(), 60 * f.grid.cellHeight()));
        f.conn.images()->queueResize(QSize(200, 60));
        f.conn.images()->flush();
        for(int row = 0; row < 60; ++row) {
            putCells(*f.conn.grid(), row, 0, QStringLiteral("M"), 0, 200);
        }
        f.conn.flush();
        QVERIFY(!f.paint().isNull());
        const int backgrounds = f.grid.backgrounds.load();
        const int foregrounds = f.grid.foregrounds.load();
        const GridRuns snapshot = buildGridRuns(*f.conn.grid(), *f.conn.highlights(), 1);
        QCOMPARE(snapshot.runs.size(), 60);
        qDebug("image workload snapshot: %lld cell runs (%lld capacity), %lld pills, "
               "%lld PUA clusters",
               static_cast<qlonglong>(snapshot.runs.size()),
               static_cast<qlonglong>(snapshot.runs.capacity()),
               static_cast<qlonglong>(snapshot.pills.size()),
               static_cast<qlonglong>(snapshot.puaClusters.size()));
        const QByteArray png = makePng(QSize(512, 256), QImage::Format_RGB32, Qt::red);
        for(int index = 0; index < imageCount; ++index) {
            const auto id =
                createSync(*f.conn.images(), png, { .col = 3 + index * 40, .row = 3, .width = 20 });
            QVERIFY(id.has_value());
            ids.append(*id);
        }
        QVERIFY(!f.paint().isNull());
        QCOMPARE(f.images.textureAttempts.load(), imageCount);
        QCOMPARE(f.conn.images()->count(), imageCount);
        QCOMPARE(f.conn.images()->cachedBytes(), qint64(imageCount) * 512 * 256 * 4);
        QCOMPARE(f.conn.images()->reservedBytes(), 0);
        for(int iteration = 0; iteration < updates; ++iteration) {
            for(int index = 0; index < imageCount; ++index) {
                timer.start();
                const auto result =
                    f.conn.images()->update(ids[index], { .col = 3 + index * 40 + iteration % 10,
                                                          .row = 3 + iteration % 20,
                                                          .width = 20 + iteration % 2,
                                                          .zIndex = iteration % 2 });
                updateNs += timer.nsecsElapsed();
                QVERIFY(result.has_value());
            }
            const QImage image = f.paint();
            QVERIFY(!image.isNull());
            for(const quint32 id: ids) {
                QQuickItem *item = f.item(id);
                QVERIFY(item);
                QCOMPARE(pixel(image,
                               item->position() + QPointF(item->width() / 2, item->height() / 2),
                               f.dpr()),
                         QColor(Qt::red));
            }
        }
        QCOMPARE(f.images.textureAttempts.load(), imageCount);
        QCOMPARE(f.grid.backgrounds.load(), backgrounds);
        QCOMPARE(f.grid.foregrounds.load(), foregrounds);
        qDebug("image workload: 200x60 grid, %d placements, %d updates, %lld decoded bytes, "
               "%lld ns total model/placement update elapsed time",
               imageCount, imageCount * updates, f.conn.images()->cachedBytes(), updateNs);
        QVERIFY(f.conn.images()->removeAll());
        QVERIFY(!f.paint().isNull());
        QCOMPARE(f.conn.images()->cachedBytes(), 0);
        QCOMPARE(f.grid.backgrounds.load(), backgrounds);
        QCOMPARE(f.grid.foregrounds.load(), foregrounds);
    }

    void roundedBackgroundAndUndercurlKeepTheirCorrectPlanes() {
        Fixture f;
        f.conn.highlights()->setRoundedHighlights({ QStringLiteral("ImagePill") });
        defineHighlight(*f.conn.highlights(), 1, Qt::white, Qt::yellow, false,
                        QStringLiteral("ImagePill"));
        putCells(*f.conn.grid(), 4, 4, QStringLiteral(" "), 1, 4);
        f.conn.flush();
        const qreal cw = f.grid.cellWidth();
        const qreal ch = f.grid.cellHeight();
        const QPointF corner(4 * cw + 0.5, 4 * ch + 0.5);
        const QPointF center(6 * cw, 4.5 * ch);
        QCOMPARE(pixel(f.paint(), corner, f.dpr()), QColor(Qt::black));
        const auto id =
            createSync(*f.conn.images(), makePng(QSize(4, 4), QImage::Format_RGB32, Qt::red),
                       { .col = 3,
                         .height = 6,
                         .row = 3,
                         .width = 10,
                         .zIndex = std::numeric_limits<qint32>::min() });
        QVERIFY(id.has_value());
        QImage image = f.paint();
        QVERIFY(!image.isNull());
        QCOMPARE(pixel(image, center, f.dpr()), QColor(Qt::yellow));
        QCOMPARE(pixel(image, corner, f.dpr()), QColor(Qt::red));
        QVERIFY(f.conn.images()->update(*id, { .zIndex = -1 }).has_value());
        QCOMPARE(pixel(f.paint(), center, f.dpr()), QColor(Qt::red));

        defineHighlight(*f.conn.highlights(), 2, Qt::white, Qt::black, true);
        putCells(*f.conn.grid(), 4, 4, QStringLiteral(" "), 2, 4);
        f.conn.flush();
        const QRectF curl(4 * cw, 5 * ch - 4, 4 * cw, 4);
        image = f.paint();
        QVERIFY(hasBlue(image, curl, f.dpr()));
        QVERIFY(f.conn.images()->update(*id, { .zIndex = 0 }).has_value());
        image = f.paint();
        QVERIFY(!hasBlue(image, curl, f.dpr()));
    }

    void sceneGraphRecreationReuploadsWithoutLosingImageState() {
        Fixture f;
        QVERIFY(!f.paint().isNull());
        const auto id =
            createSync(*f.conn.images(), makePng(QSize(4, 4), QImage::Format_RGB32, Qt::red),
                       { .col = 3, .row = 3, .width = 4 });
        QVERIFY(id.has_value());
        QVERIFY(!f.paint().isNull());
        QQuickItem *item = f.item(*id);
        const int uploads = f.images.textureAttempts.load();
        f.grid.setParentItem(nullptr);
        f.raster.reset();
        f.raster = std::make_unique<QuickRasterizer>();
        const QImage image = f.paint();
        QVERIFY(!image.isNull());
        QCOMPARE(f.item(*id), item);
        QVERIFY(f.images.textureAttempts.load() > uploads);
        QVERIFY(f.conn.images()->get(*id).has_value());
        QCOMPARE(pixel(image,
                       QPointF(item->x() + item->width() / 2, item->y() + item->height() / 2),
                       f.dpr()),
                 QColor(Qt::red));
    }

    void scrollClippingPreservesOnlyTheSurvivingSource() {
        const QColor colors[] = { Qt::red, Qt::green, Qt::blue, Qt::yellow };
        Fixture f;
        QImage stripes(4, 16, QImage::Format_RGB32);
        for(int y = 0; y < stripes.height(); ++y) {
            for(int x = 0; x < stripes.width(); ++x) stripes.setPixelColor(x, y, colors[y / 4]);
        }
        const auto id = createSync(*f.conn.images(), encodePng(stripes),
                                   { .col = 2, .height = 4, .row = 2, .width = 4 });
        QVERIFY(id.has_value());
        QImage image = f.paint();
        QVERIFY(!image.isNull());
        QQuickItem *item = f.item(*id);
        QVERIFY(item);
        const qreal ch = f.grid.cellHeight();
        const qreal x = item->x() + item->width() / 2;
        QCOMPARE(item->height(), 4 * ch);
        QCOMPARE(pixel(image, QPointF(x, 1.5 * ch), f.dpr()), QColor(Qt::red));
        f.conn.images()->queueScroll(QRectF(0, 0, 20, 10), 3);
        f.conn.images()->flush();
        image = f.paint();
        QCOMPARE(pixel(image, QPointF(x, 0.5 * ch), f.dpr()), QColor(Qt::blue));
        QCOMPARE(pixel(image, QPointF(x, 1.5 * ch), f.dpr()), QColor(Qt::yellow));
        QCOMPARE(pixel(image, QPointF(x, 2.5 * ch), f.dpr()), QColor(Qt::black));
        f.conn.images()->queueScroll(QRectF(0, 0, 20, 10), 1);
        f.conn.images()->flush();
        image = f.paint();
        QCOMPARE(pixel(image, QPointF(x, 0.5 * ch), f.dpr()), QColor(Qt::yellow));
        QCOMPARE(pixel(image, QPointF(x, 1.5 * ch), f.dpr()), QColor(Qt::black));
        f.conn.images()->queueScroll(QRectF(0, 0, 20, 10), -1);
        f.conn.images()->flush();
        image = f.paint();
        QCOMPARE(pixel(image, QPointF(x, 0.5 * ch), f.dpr()), QColor(Qt::black));
        QCOMPARE(pixel(image, QPointF(x, 1.5 * ch), f.dpr()), QColor(Qt::yellow));
        QVERIFY(f.conn.images()->update(*id, { .zIndex = 1 }).has_value());
        image = f.paint();
        QCOMPARE(pixel(image, QPointF(x, 1.5 * ch), f.dpr()), QColor(Qt::red));
        QCOMPARE(f.images.textureAttempts.load(), 1);
    }

    void signedZOrderingIncludesBoundaryAndIdTies() {
        Fixture f;
        defineHighlight(*f.conn.highlights(), 1, Qt::white, Qt::black);
        putCells(*f.conn.grid(), 4, 4, QStringLiteral("M"), 1);
        f.conn.flush();
        const QImage glyphImage = f.paint();
        QVERIFY(!glyphImage.isNull());
        const QRectF cell(4 * f.grid.cellWidth(), 4 * f.grid.cellHeight(), f.grid.cellWidth(),
                          f.grid.cellHeight());
        // A yellow background can conceal partial LCD glyph coverage.
        const auto glyph = findColor(glyphImage, cell, f.dpr(), Qt::white);
        defineHighlight(*f.conn.highlights(), 1, Qt::white, Qt::yellow);
        const QImage baseline = f.paint();
        QVERIFY(!baseline.isNull());
        const auto background = findColor(baseline, cell, f.dpr(), Qt::yellow);
        QVERIFY(glyph.has_value());
        QVERIFY(background.has_value());
        const ImageOptions options{ .col = 3,
                                    .height = 6,
                                    .row = 3,
                                    .width = 10,
                                    .zIndex = std::numeric_limits<qint32>::min() };
        const auto red = createSync(*f.conn.images(),
                                    makePng(QSize(4, 4), QImage::Format_RGB32, Qt::red), options);
        QVERIFY(red.has_value());
        QImage image = f.paint();
        QVERIFY(!image.isNull());
        QCOMPARE(image.pixelColor(*background), QColor(Qt::yellow));
        QCOMPARE(image.pixelColor(*glyph), QColor(Qt::white));
        QQuickItem *item = f.item(*red);
        QVERIFY(item);
        const QPointF defaultPoint(item->x() + 1, item->y() + 1);
        QCOMPARE(pixel(baseline, defaultPoint, f.dpr()), QColor(Qt::black));
        QCOMPARE(pixel(image, defaultPoint, f.dpr()), QColor(Qt::red));

        constexpr qint32 boundary = std::numeric_limits<qint32>::min() / 2;
        QVERIFY(f.conn.images()->update(*red, { .zIndex = boundary - 1 }).has_value());
        QCOMPARE(f.paint().pixelColor(*background), QColor(Qt::yellow));
        QVERIFY(f.conn.images()->update(*red, { .zIndex = boundary }).has_value());
        image = f.paint();
        QCOMPARE(image.pixelColor(*background), QColor(Qt::red));
        QCOMPARE(image.pixelColor(*glyph), QColor(Qt::white));
        QVERIFY(f.conn.images()->update(*red, { .zIndex = 0 }).has_value());
        QCOMPARE(f.paint().pixelColor(*glyph), QColor(Qt::red));

        ImageOptions blueOptions = options;
        blueOptions.zIndex = 0;
        const auto blue = createSync(
            *f.conn.images(), makePng(QSize(4, 4), QImage::Format_RGB32, Qt::blue), blueOptions);
        QVERIFY(blue.has_value());
        QVERIFY(*blue > *red);
        QCOMPARE(f.paint().pixelColor(*glyph), QColor(Qt::blue));
        QVERIFY(f.conn.images()->update(*red, { .zIndex = 1 }).has_value());
        QCOMPARE(f.paint().pixelColor(*glyph), QColor(Qt::red));
        QVERIFY(f.conn.images()->update(*red, { .zIndex = 0 }).has_value());
        QCOMPARE(f.paint().pixelColor(*glyph), QColor(Qt::blue));
    }

    void textureFailureIsReportedWithoutRemovingAnExistingImage() {
        Fixture f;
        QVERIFY(!f.paint().isNull());
        QSignalSpy errors(f.conn.images(), &ImageModel::error);
        const auto first =
            createSync(*f.conn.images(), makePng(QSize(4, 4), QImage::Format_RGB32, Qt::red),
                       { .col = 2, .row = 2, .width = 2 });
        QVERIFY(first.has_value());
        QVERIFY(!f.paint().isNull());
        f.images.failTextures = true;
        const auto second =
            createSync(*f.conn.images(), makePng(QSize(4, 4), QImage::Format_RGB32, Qt::blue),
                       { .col = 6, .row = 2, .width = 2 });
        QVERIFY(second.has_value());
        QVERIFY(!f.paint().isNull());
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(f.conn.images()->get(*first).has_value());
        QVERIFY(!f.conn.images()->get(*second).has_value());
        QCOMPARE(f.conn.images()->count(), qsizetype(1));
    }

    void viewDetachAndModelReplacementDoNotLeaveStaleItems() {
        Fixture f;
        QCOMPARE(f.images.grid(), &f.grid);
        QCOMPARE(f.images.model(), f.conn.images());
        QVERIFY(!f.paint().isNull());
        const auto id = createSync(*f.conn.images(), makePng());
        QVERIFY(id.has_value());
        QVERIFY(!f.paint().isNull());
        QVERIFY(f.item(*id));
        f.images.setModel(nullptr);
        QVERIFY(!f.images.model());
        QVERIFY(!f.item(*id));
        QVERIFY(f.conn.images()->get(*id).has_value());
        f.images.setModel(f.conn.images());
        QVERIFY(!f.paint().isNull());
        QVERIFY(f.item(*id));
        f.grid.setGridId(2);
        QVERIFY(!f.item(*id));
        f.grid.setGridId(1);
        QVERIFY(!f.paint().isNull());
        QVERIFY(f.item(*id));
        f.images.setGrid(nullptr);
        QVERIFY(!f.images.grid());
        QVERIFY(!f.item(*id));
        f.images.setGrid(&f.grid);
        QVERIFY(!f.paint().isNull());
        QVERIFY(f.item(*id));
    }
};

QTEST_MAIN(TestImageRender)
#include "test_image_render.moc"
