#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItemGrabResult>
#include <QSGRectangleNode>
#include <QSGSimpleTextureNode>
#include <QtTest>

#include "CursorItem.h"
#include "GridItem.h"
#include "GridModel.h"
#include "HighlightTable.h"
#include "IntegrationHelpers.h"
#include "ModeInfo.h"
#include "NvimConnector.h"
#include "support/QuickRasterizer.h"

using namespace dot;
using namespace dot::test;

namespace {

struct CursorFrame {
    quintptr backgroundIdentity = 0;
    QRectF backgroundRect;
    qreal dpr = 0;
    int glyphNodes = 0;
    quintptr rootIdentity = 0;
    QRectF sourceRect;
    quintptr textureIdentity = 0;
    QSize textureSize;
    int underlineNodes = 0;
    qint64 updateNs = 0;
};

void observeBackground(QSGNode *parent, CursorFrame &frame) {
    for(QSGNode *node = parent->firstChild(); node; node = node->nextSibling()) {
        if(const auto *texture = dynamic_cast<QSGSimpleTextureNode *>(node)) {
            frame.backgroundIdentity = reinterpret_cast<quintptr>(texture);
            frame.backgroundRect = texture->rect();
            frame.sourceRect = texture->sourceRect();
            frame.textureIdentity = reinterpret_cast<quintptr>(texture->texture());
            if(texture->texture()) frame.textureSize = texture->texture()->textureSize();
        } else if(const auto *rectangle = dynamic_cast<QSGRectangleNode *>(node)) {
            frame.backgroundIdentity = reinterpret_cast<quintptr>(rectangle);
            frame.backgroundRect = rectangle->rect();
        }
        observeBackground(node, frame);
    }
}

class ObservedCursor : public CursorItem {
public:
    CursorFrame frame;

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override {
        QElapsedTimer timer;
        timer.start();
        QSGNode *root = CursorItem::updatePaintNode(oldNode, data);
        const qint64 elapsed = timer.nsecsElapsed();
        frame = {};
        frame.dpr = window()->effectiveDevicePixelRatio();
        frame.rootIdentity = reinterpret_cast<quintptr>(root);
        frame.updateNs = elapsed;
        if(!root) return root;
        QSGNode *background = root->firstChild();
        observeBackground(background, frame);
        frame.glyphNodes = background->nextSibling()->childCount();
        frame.underlineNodes = background->nextSibling()->nextSibling()->childCount();
        return root;
    }
};

template <typename Predicate>
bool waitUntil(Predicate &&predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while(!predicate()) {
        if(timer.elapsed() >= timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

QRect colorBounds(const QImage &image, const QColor &color) {
    QRect bounds;
    for(int y = 0; y < image.height(); ++y) {
        for(int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if(pixel.alpha() != 0 && pixel.rgb() == color.rgb()) {
                bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    return bounds;
}

QImage grabItem(QQuickItem *item) {
    const auto grab = item->grabToImage();
    if(!grab || !waitUntil([&] { return !grab->image().isNull(); })) return {};
    return grab->image().copy();
}

struct CursorFixture {
    NvimConnector conn;
    std::unique_ptr<QuickRasterizer> raster = std::make_unique<QuickRasterizer>();
    QQuickItem scene;
    GridItem textGrid{ &scene };
    ObservedCursor cursor;

    CursorFixture() {
        raster->window()->setColor(Qt::transparent);
        conn.highlights()->setDefaultColors(0xff00ff, 0x000000, 0xff00ff);
        conn.grid()->resize(12, 4);
        cursor.setConnector(&conn);
        cursor.setCellHeight(20);
        cursor.setCellWidth(10);
        cursor.setBaseline(14);
        cursor.setCornerRadius(2);
        cursor.setFontName(QStringLiteral("Courier New"));
        cursor.setFontSize(12);
        cursor.setSize(QSizeF(160, 120));
        conn.grid()->setCursor(1, 0, 2);
    }

    QImage paint(std::optional<qreal> dpr = {}) {
        cursor.update();
        return raster->render(&cursor, dpr).copy();
    }

    QImage paintLayers() {
        textGrid.setFontName(cursor.fontName());
        textGrid.setFontSize(cursor.fontSize());
        textGrid.setConnector(&conn);
        cursor.setCellHeight(textGrid.cellHeight());
        cursor.setCellWidth(textGrid.cellWidth());
        cursor.setBaseline(textGrid.baseline());
        const QSizeF size(conn.grid()->cols() * textGrid.cellWidth(),
                          conn.grid()->rows() * textGrid.cellHeight());
        scene.setSize(size);
        textGrid.setSize(size);
        cursor.setSize(size);
        cursor.setParentItem(&scene);
        cursor.setZ(99);
        cursor.update();
        return raster->render(&scene).copy();
    }

    bool start(bool underline = false) {
        if(!startTestNvim(conn) || !conn.attachUi(40, 12) || !waitForAttach(&conn)) return false;
        conn.execLua(QStringLiteral(R"lua(
vim.api.nvim_set_hl(0, 'Normal', { bg = 0x000000, fg = 0xffffff })
vim.api.nvim_set_hl(0, 'DotTestCursor', { bg = 0xff00ff, fg = 0x000000 })
vim.api.nvim_set_hl(0, 'DotTestUnderline', { fg = 0xffffff, underline = true })
vim.o.guicursor = 'a:block-DotTestCursor-blinkwait0-blinkon0-blinkoff0'
vim.api.nvim_buf_set_lines(0, 0, -1, false, { '%1', 'another line' })
vim.fn.matchadd('DotTestUnderline', 'X')
vim.api.nvim_win_set_cursor(0, { 1, 2 })
vim.bo.modified = false
)lua")
                         .arg(underline ? QStringLiteral("x X   y") : QStringLiteral("x     y")));
        return waitUntil([&] {
            if(conn.grid()->cursorRow() != 0 || conn.grid()->cursorCol() != 2 ||
               !conn.modeInfo()->cursorStyleEnabled()) {
                return false;
            }
            const Cell &cell = conn.grid()->cell(1, 0, 2);
            return cell.text == (underline ? QStringLiteral("X") : QStringLiteral(" ")) &&
                   (!underline || conn.highlights()->resolved(cell.hlId).underline) &&
                   conn.highlights()->resolved(conn.modeInfo()->attrId()).bg == QColor(Qt::magenta);
        });
    }
};

} // namespace

class TestCursorShape : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        if(qEnvironmentVariableIsSet("DOT_TEST_NATIVE_CURSOR")) {
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
        } else {
            QuickRasterizer::useSoftwareBackend();
        }
        qRegisterMetaType<dot::Notification>("dot::Notification");
        qRegisterMetaType<dot::ObjectHandlePtr>("dot::ObjectHandlePtr");
        qInfo(
            "Object sizes: CursorItem=%zu ModeInfo=%zu QSGSimpleTextureNode=%zu QColor=%zu QSizeF=%zu",
            sizeof(CursorItem), sizeof(ModeInfo), sizeof(QSGSimpleTextureNode), sizeof(QColor),
            sizeof(QSizeF));
    }

    void benchmarkFrames_data() {
        QTest::addColumn<bool>("cold");
        QTest::addColumn<qreal>("radius");
        QTest::newRow("cold-flat") << true << qreal(0);
        QTest::newRow("cold-rounded") << true << qreal(2);
        QTest::newRow("warm-flat") << false << qreal(0);
        QTest::newRow("warm-rounded") << false << qreal(2);
    }

    void benchmarkFrames() {
        QFETCH(bool, cold);
        QFETCH(qreal, radius);

        constexpr int samples = 256;
        NvimConnector conn;
        QuickRasterizer raster;
        ObservedCursor cursor;
        std::array<qint64, samples> renderTimes{};
        std::array<qint64, samples> updateTimes{};
        conn.highlights()->setDefaultColors(0xffffff, 0x000000, 0xffffff);
        conn.grid()->resize(12, 4);
        cursor.setConnector(&conn);
        cursor.setCellHeight(20);
        cursor.setCellWidth(10);
        cursor.setSize(QSizeF(120, 80));
        if(radius > 0) QVERIFY(cursor.setProperty("cornerRadius", radius));
        conn.grid()->setCursor(1, 1, 2);
        QVERIFY(!raster.render(&cursor).isNull());
        const quintptr backgroundIdentity = cursor.frame.backgroundIdentity;
        const quintptr textureIdentity = cursor.frame.textureIdentity;
        QVERIFY(backgroundIdentity != 0);
        if(radius > 0) QVERIFY(textureIdentity != 0);

        for(int i = 0; i < samples; ++i) {
            if(cold) cursor.setCellWidth(i % 2 == 0 ? 14 : 10);
            conn.grid()->setCursor(1, 1, i % 2 == 0 ? 6 : 2);
            cursor.update();
            QElapsedTimer timer;
            timer.start();
            const QImage image = raster.render(&cursor);
            renderTimes[i] = timer.nsecsElapsed();
            updateTimes[i] = cursor.frame.updateNs;
            QVERIFY(!image.isNull());
            QCOMPARE(cursor.frame.backgroundRect.size(),
                     QSizeF(cursor.cellWidth(), cursor.cellHeight()));
            QCOMPARE(cursor.frame.backgroundIdentity, backgroundIdentity);
            if(!cold) QCOMPARE(cursor.frame.textureIdentity, textureIdentity);
        }
        std::sort(renderTimes.begin(), renderTimes.end());
        std::sort(updateTimes.begin(), updateTimes.end());
        qInfo(
            "Cursor benchmark: cold=%d radius=%.1f samples=%d update_median_ns=%lld render_median_ns=%lld",
            cold, radius, samples, updateTimes[samples / 2], renderTimes[samples / 2]);
    }

    void metricsChangeRetainsCellAnchor() {
        NvimConnector conn;
        QuickRasterizer raster;
        ObservedCursor cursor;
        conn.grid()->resize(12, 4);
        cursor.setConnector(&conn);
        cursor.setCellHeight(20);
        cursor.setCellWidth(10);
        cursor.setSize(QSizeF(180, 140));
        conn.grid()->setCursor(1, 2, 3);
        QVERIFY(!raster.render(&cursor).isNull());
        QCOMPARE(cursor.frame.backgroundRect, QRectF(30, 40, 10, 20));

        cursor.setCellHeight(30);
        cursor.setCellWidth(14);

        QVERIFY(!raster.render(&cursor).isNull());
        QCOMPARE(cursor.frame.backgroundRect, QRectF(3 * 14, 2 * 30, 14, 30));
    }

    void blinkHidesAndReusesRoundedBackground() {
        CursorFixture fixture;
        QVERIFY(fixture.start());
        fixture.conn.command(
            QStringLiteral("set guicursor=a:ver40-DotTestCursor-blinkwait30-blinkon60-blinkoff60"));
        QTRY_COMPARE(fixture.conn.modeInfo()->cellPercentage(), 40);
        QTRY_COMPARE(fixture.conn.modeInfo()->blinkOn(), 60);
        QVERIFY(!fixture.paint().isNull());
        const quintptr identity = fixture.cursor.frame.textureIdentity;
        QVERIFY(identity != 0);
        QVERIFY(waitUntil([&] {
            fixture.paint();
            return fixture.cursor.frame.backgroundRect.isEmpty();
        }));
        QCOMPARE(fixture.cursor.frame.glyphNodes, 0);
        QCOMPARE(fixture.cursor.frame.textureIdentity, identity);
        QVERIFY(waitUntil([&] {
            fixture.paint();
            return !fixture.cursor.frame.backgroundRect.isEmpty();
        }));
        QCOMPARE(fixture.cursor.frame.textureIdentity, identity);
        QCOMPARE(fixture.cursor.frame.backgroundRect, QRectF(20, 0, 4, 20));
    }

    void cornerChangesNotifyAndReleaseTheTexture() {
        CursorFixture fixture;
        QVERIFY(!fixture.paint().isNull());
        const quintptr originalTexture = fixture.cursor.frame.textureIdentity;
        QSignalSpy spy(&fixture.cursor, &CursorItem::cornerRadiusChanged);
        fixture.cursor.setCornerRadius(3);
        fixture.cursor.setCornerRadius(3);
        QCOMPARE(spy.count(), 1);
        QVERIFY(!fixture.paint().isNull());
        QVERIFY(fixture.cursor.frame.textureIdentity != originalTexture);
        for(qreal invalid: { qreal(-1), std::numeric_limits<qreal>::infinity(),
                             std::numeric_limits<qreal>::quiet_NaN() }) {
            QTest::ignoreMessage(QtWarningMsg,
                                 "Cursor corner radius must be finite and non-negative");
            fixture.cursor.setCornerRadius(invalid);
            QCOMPARE(fixture.cursor.cornerRadius(), 3.0);
        }
        QCOMPARE(spy.count(), 1);
        fixture.cursor.setCornerRadius(0);
        QVERIFY(!fixture.paint().isNull());
        QCOMPARE(fixture.cursor.frame.textureIdentity, quintptr(0));
        QCOMPARE(fixture.cursor.frame.backgroundRect.size(), QSizeF(10, 20));
        QCOMPARE(spy.count(), 2);
    }

    void dirtyGridMovementStillSnapsWithoutConsumingDirty() {
        CursorFixture fixture;
        QVERIFY(!fixture.paint().isNull());
        fixture.conn.grid()->clear(1);
        fixture.conn.grid()->setCursor(1, 0, 3);

        QVERIFY(!fixture.paint().isNull());

        QCOMPARE(fixture.cursor.frame.backgroundRect, QRectF(30, 0, 10, 20));
        QVERIFY(fixture.conn.grid()->isDirty(1));
        QVERIFY(fixture.conn.grid()->takeDirty(1));
        QVERIFY(!fixture.conn.grid()->isDirty(1));
    }

    void disabledStylingUsesASolidBlockAfterBlinkingBar() {
        CursorFixture fixture;
        QVERIFY(fixture.start(true));
        fixture.conn.command(
            QStringLiteral("set guicursor=a:ver40-DotTestCursor-blinkwait30-blinkon60-blinkoff60"));
        QTRY_COMPARE(fixture.conn.modeInfo()->cellPercentage(), 40);
        QVERIFY(waitUntil([&] {
            fixture.paint();
            return fixture.cursor.frame.backgroundRect.isEmpty();
        }));
        fixture.conn.command(QStringLiteral("set guicursor="));
        QTRY_VERIFY(!fixture.conn.modeInfo()->cursorStyleEnabled());
        for(int phase = 0; phase < 4; ++phase) {
            QTest::qWait(50);
            QVERIFY(!fixture.paint().isNull());
            QCOMPARE(fixture.cursor.frame.backgroundRect.size(), QSizeF(10, 20));
            QVERIFY(fixture.cursor.frame.glyphNodes > 0);
        }
    }

    void fractionalGeometryUsesTheUnpaddedTextureSource() {
        CursorFixture fixture;
        fixture.cursor.setCellHeight(20.5);
        fixture.cursor.setCellWidth(10.25);
        fixture.conn.grid()->setCursor(1, 0, 0);
        QVERIFY(!fixture.paint(1.5).isNull());

        QCOMPARE(fixture.cursor.frame.dpr, 1.5);
        QCOMPARE(fixture.cursor.frame.backgroundRect, QRectF(0, 0, 10.25, 20.5));
        QCOMPARE(fixture.cursor.frame.sourceRect, QRectF(0, 0, 10.25 * 1.5, 20.5 * 1.5));
        QCOMPARE(fixture.cursor.frame.textureSize, QSize(qCeil(10.25 * 1.5), qCeil(20.5 * 1.5)));
    }

    void invalidGeometryClearsEveryLayerAndRecovers() {
        CursorFixture fixture;
        QVERIFY(fixture.start(true));
        QVERIFY(!fixture.paint().isNull());
        QVERIFY(fixture.cursor.frame.glyphNodes > 0);
        QVERIFY(fixture.cursor.frame.underlineNodes > 0);
        const quintptr texture = fixture.cursor.frame.textureIdentity;
        fixture.cursor.setCellWidth(std::numeric_limits<qreal>::quiet_NaN());

        const QImage empty = fixture.paint();

        QVERIFY(!empty.isNull());
        QVERIFY(fixture.cursor.frame.backgroundRect.isEmpty());
        QCOMPARE(fixture.cursor.frame.glyphNodes, 0);
        QCOMPARE(fixture.cursor.frame.underlineNodes, 0);
        QVERIFY(colorBounds(empty, Qt::black).isEmpty());
        QVERIFY(colorBounds(empty, Qt::magenta).isEmpty());
        QCOMPARE(fixture.cursor.frame.textureIdentity, texture);
        fixture.cursor.setCellWidth(10);
        QVERIFY(!fixture.paint().isNull());
        QCOMPARE(fixture.cursor.frame.backgroundRect, QRectF(20, 0, 10, 20));
        QCOMPARE(fixture.cursor.frame.textureIdentity, texture);
        QVERIFY(fixture.cursor.frame.glyphNodes > 0);

        fixture.conn.grid()->setCursor(1, -1, -1);
        QVERIFY(!fixture.paint().isNull());
        QVERIFY(fixture.cursor.frame.backgroundRect.isEmpty());
        QCOMPARE(fixture.cursor.frame.glyphNodes, 0);
        fixture.cursor.setConnector(nullptr);
        QVERIFY(!fixture.paint().isNull());
        QVERIFY(fixture.cursor.frame.backgroundRect.isEmpty());
    }

    void metricChangesCancelObsoletePixelInterpolation() {
        CursorFixture fixture;
        QVERIFY(!fixture.paint().isNull());
        fixture.conn.grid()->takeDirty(1);
        fixture.conn.grid()->setCursor(1, 0, 3);
        QVERIFY(!fixture.paint().isNull());
        QVERIFY(fixture.cursor.frame.backgroundRect.left() < 30);

        fixture.cursor.setCellWidth(14);
        fixture.cursor.setCellHeight(30);

        QVERIFY(!fixture.paint().isNull());
        QCOMPARE(fixture.cursor.frame.backgroundRect, QRectF(3 * 14, 0, 14, 30));
        QTest::qWait(100);
        QVERIFY(!fixture.paint().isNull());
        QCOMPARE(fixture.cursor.frame.backgroundRect, QRectF(3 * 14, 0, 14, 30));
    }

    void qmlBindingKeepsFocusAcrossModeFontAndRestart_data() {
        QTest::addColumn<bool>("light");
        QTest::newRow("dark") << false;
        QTest::newRow("light") << true;
    }

    void qmlBindingKeepsFocusAcrossModeFontAndRestart() {
        NvimConnector conn;
        QQmlApplicationEngine engine;
        QFETCH(bool, light);
        const QString background = light ? QStringLiteral("#f0f0f0") : QStringLiteral("#101010");
        const QString foreground = light ? QStringLiteral("#202020") : QStringLiteral("#d0d0d0");
        const QString theme = light ? QStringLiteral("light") : QStringLiteral("dark");
        QVERIFY(startTestNvim(conn));
        engine.rootContext()->setContextProperty(QStringLiteral("$connector"), &conn);
        engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(waitForAttach(&conn));
        auto *cursor = window->findChild<CursorItem *>();
        auto *grid = window->findChild<GridItem *>();
        auto *shell = window->findChild<QQuickItem *>(QStringLiteral("shell"));
        QVERIFY(cursor && grid && shell);
        auto *metrics = shell->property("metrics").value<QObject *>();
        QVERIFY(metrics);
        QCOMPARE(cursor->cornerRadius(), metrics->property("cursorCornerRadius").toReal());
        QCOMPARE(cursor->cornerRadius(), 2.0);
        conn.execLua(QStringLiteral(R"lua(
vim.o.background = '%3'
vim.api.nvim_set_hl(0, 'Normal', { bg = '%1', fg = '%2' })
vim.api.nvim_set_hl(0, 'DotTestCursor', { bg = 0xff00ff, fg = 0x000000 })
vim.api.nvim_buf_set_lines(0, 0, -1, false, { 'x       y', 'another line' })
vim.api.nvim_win_set_cursor(0, { 1, 2 })
vim.bo.modified = false
vim.o.guicursor = 'a:ver80-DotTestCursor-blinkwait0-blinkon0-blinkoff0'
)lua")
                         .arg(background, foreground, theme));
        QTRY_COMPARE(conn.modeInfo()->cellPercentage(), 80);
        QTRY_COMPARE(conn.grid()->cursorCol(), 2);
        for(int fontSize: { 12, 24 }) {
            conn.command(QStringLiteral("set guifont=Courier\\ New:h%1 linespace=4").arg(fontSize));
            QTRY_COMPARE(grid->fontSize(), qreal(fontSize));
            QCOMPARE(cursor->cellHeight(), grid->cellHeight());
            QCOMPARE(cursor->cellWidth(), grid->cellWidth());
            QCOMPARE(cursor->cornerRadius(), 2.0);
            QCOMPARE(window->activeFocusItem(), grid);
            QVERIFY(!cursor->hasActiveFocus());
            const QImage image = grabItem(cursor);
            QVERIFY(!image.isNull());
            const QRect bounds = colorBounds(image, Qt::magenta);
            QVERIFY(!bounds.isEmpty());
            const qreal scaleX = image.width() / cursor->width();
            const qreal scaleY = image.height() / cursor->height();
            QVERIFY(qAbs(bounds.left() - 2 * grid->cellWidth() * scaleX) <= 1);
            QVERIFY(qAbs(bounds.width() - grid->cellWidth() * 0.8 * scaleX) <= 2);
            QVERIFY(qAbs(bounds.height() - grid->cellHeight() * scaleY) <= 2);
        }
        qInfo() << "Cursor QML backend:" << window->rendererInterface()->graphicsApi()
                << "effective DPR:" << window->effectiveDevicePixelRatio();
        if(qEnvironmentVariableIsSet("DOT_TEST_NATIVE_CURSOR")) {
            QCOMPARE(window->rendererInterface()->graphicsApi(), QSGRendererInterface::Direct3D11);
        } else {
            QCOMPARE(window->rendererInterface()->graphicsApi(), QSGRendererInterface::Software);
        }
        conn.command(
            QStringLiteral("set guicursor=n:block-DotTestCursor-blinkwait0-blinkon0-blinkoff0"
                           ",i:ver40-DotTestCursor-blinkwait0-blinkon0-blinkoff0"
                           ",r:hor30-DotTestCursor-blinkwait0-blinkon0-blinkoff0"));
        QTRY_COMPARE(conn.modeInfo()->cursorShapeInt(), static_cast<int>(CursorShape::Block));
        QTest::keyClick(window, Qt::Key_I);
        QTRY_COMPARE(conn.modeInfo()->currentName(), QStringLiteral("insert"));
        QTRY_COMPARE(conn.modeInfo()->cellPercentage(), 40);
        QCOMPARE(conn.modeInfo()->cursorShapeInt(), static_cast<int>(CursorShape::Vertical));
        const QImage insertImage = grabItem(cursor);
        QVERIFY(!insertImage.isNull());
        const QRect insertBounds = colorBounds(insertImage, Qt::magenta);
        QVERIFY(!insertBounds.isEmpty());
        const qreal insertScale = insertImage.width() / cursor->width();
        QVERIFY(qAbs(insertBounds.width() - grid->cellWidth() * 0.4 * insertScale) <= 2);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(conn.modeInfo()->currentName(), QStringLiteral("normal"));
        QTest::keyClick(window, Qt::Key_R, Qt::ShiftModifier);
        QTRY_COMPARE(conn.modeInfo()->currentName(), QStringLiteral("replace"));
        QTRY_COMPARE(conn.modeInfo()->cellPercentage(), 30);
        QCOMPARE(conn.modeInfo()->cursorShapeInt(), static_cast<int>(CursorShape::Horizontal));
        const QImage replaceImage = grabItem(cursor);
        QVERIFY(!replaceImage.isNull());
        const QRect replaceBounds = colorBounds(replaceImage, Qt::magenta);
        QVERIFY(!replaceBounds.isEmpty());
        const qreal replaceScale = replaceImage.height() / cursor->height();
        QVERIFY(qAbs(replaceBounds.height() - grid->cellHeight() * 0.3 * replaceScale) <= 2);
        QVERIFY(qAbs(replaceBounds.bottom() + 1 -
                     (conn.grid()->cursorRow() + 1) * grid->cellHeight() * replaceScale) <= 1);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(conn.modeInfo()->currentName(), QStringLiteral("normal"));
        conn.command(
            QStringLiteral("set guicursor=a:ver80-DotTestCursor-blinkwait0-blinkon0-blinkoff0"));
        QTRY_COMPARE(conn.modeInfo()->cellPercentage(), 80);
        const int oldColumn = conn.grid()->cursorCol();
        QTest::keyClick(window, Qt::Key_L);
        QTRY_COMPARE(conn.grid()->cursorCol(), oldColumn + 1);
        QCOMPARE(window->activeFocusItem(), grid);
        QTRY_VERIFY(!conn.grid()->isDirty(1));

        conn.command(QStringLiteral(
            "set guicursor=a:ver80-DotTestCursor-blinkwait60-blinkon120-blinkoff120"));
        QTRY_COMPARE(conn.modeInfo()->blinkOn(), 120);
        const QImage blinkImage = grabItem(cursor);
        QVERIFY(!blinkImage.isNull());
        const qreal blinkScaleX = blinkImage.width() / cursor->width();
        const qreal blinkScaleY = blinkImage.height() / cursor->height();
        const QRect blinkRegion =
            QRectF(conn.grid()->cursorCol() * grid->cellWidth() * blinkScaleX,
                   conn.grid()->cursorRow() * grid->cellHeight() * blinkScaleY,
                   grid->cellWidth() * blinkScaleX, grid->cellHeight() * blinkScaleY)
                .toAlignedRect();
        QVERIFY(!blinkRegion.isEmpty());
        QVERIFY(blinkImage.rect().contains(blinkRegion));
        QVERIFY(waitUntil([&] {
            const QImage image = grabItem(cursor);
            return !image.isNull() && colorBounds(image.copy(blinkRegion), Qt::magenta).isEmpty();
        }));
        QVERIFY(waitUntil([&] {
            const QImage image = grabItem(cursor);
            return !image.isNull() && !colorBounds(image.copy(blinkRegion), Qt::magenta).isEmpty();
        }));
        conn.command(
            QStringLiteral("set guicursor=a:ver80-DotTestCursor-blinkwait0-blinkon0-blinkoff0"));
        QTRY_COMPARE(conn.modeInfo()->blinkOn(), 0);
        QCOMPARE(window->activeFocusItem(), grid);

        const auto oldPid = evalSync(conn, QStringLiteral("getpid()"));
        QVERIFY(oldPid.has_value());
        conn.command(QStringLiteral("restart!"));
        QVERIFY(waitUntil([&] {
            return conn.attached() && conn.modeInfo()->currentName() == QStringLiteral("normal") &&
                   conn.modeInfo()->cellPercentage() != 80;
        }, 20000));
        const auto newPid = evalSync(conn, QStringLiteral("getpid()"));
        QVERIFY(newPid.has_value());
        QVERIFY(newPid->toInt() != oldPid->toInt());
        QCOMPARE(window->findChild<CursorItem *>(), cursor);
        QCOMPARE(window->activeFocusItem(), grid);
        QCOMPARE(cursor->cornerRadius(), 2.0);
        const QImage restarted = grabItem(cursor);
        QVERIFY(!restarted.isNull());
        QVERIFY(colorBounds(restarted, Qt::magenta).isEmpty());
    }

    void realGuicursorChangesRenderedGeometry() {
        struct ShapeCase {
            const char *option;
            int percentage;
            QRectF rect;
            CursorShape shape;
        };
        const ShapeCase cases[]{
            { "block", 0, QRectF(20, 0, 10, 20), CursorShape::Block },
            { "ver20", 20, QRectF(20, 0, 2, 20), CursorShape::Vertical },
            { "ver80", 80, QRectF(20, 0, 8, 20), CursorShape::Vertical },
            { "hor20", 20, QRectF(20, 16, 10, 4), CursorShape::Horizontal },
            { "hor80", 80, QRectF(20, 4, 10, 16), CursorShape::Horizontal },
        };
        CursorFixture fixture;
        QVERIFY(fixture.start());
        QSignalSpy spy(fixture.conn.modeInfo(), &ModeInfo::currentChanged);
        for(const ShapeCase &test: cases) {
            fixture.conn.command(
                QStringLiteral("set guicursor=a:%1-DotTestCursor-blinkwait0-blinkon0-blinkoff0")
                    .arg(test.option));
            QTRY_COMPARE(fixture.conn.modeInfo()->cursorShapeInt(), static_cast<int>(test.shape));
            if(test.shape != CursorShape::Block) {
                QTRY_COMPARE(fixture.conn.modeInfo()->cellPercentage(), test.percentage);
            }
            QCOMPARE(fixture.conn.modeInfo()->currentName(), QStringLiteral("normal"));
            QCOMPARE(fixture.conn.grid()->cursorRow(), 0);
            QCOMPARE(fixture.conn.grid()->cursorCol(), 2);
            const QImage image = fixture.paint();
            QVERIFY(!image.isNull());
            QCOMPARE(fixture.cursor.frame.backgroundRect, test.rect);
            QCOMPARE(colorBounds(image, Qt::magenta), test.rect.toAlignedRect());
        }
        QVERIFY(!spy.isEmpty());
    }

    void resetAndSceneGraphRecreationDiscardOldGeometry() {
        CursorFixture fixture;
        QVERIFY(fixture.start());
        fixture.conn.command(
            QStringLiteral("set guicursor=a:ver80-DotTestCursor-blinkwait0-blinkon0-blinkoff0"));
        QTRY_COMPARE(fixture.conn.modeInfo()->cellPercentage(), 80);
        QVERIFY(!fixture.paint().isNull());
        QCOMPARE(fixture.cursor.frame.backgroundRect.width(), 8.0);
        fixture.conn.modeInfo()->reset();
        QVERIFY(!fixture.paint().isNull());
        QCOMPARE(fixture.cursor.frame.backgroundRect.size(), QSizeF(10, 20));
        fixture.conn.grid()->reset();
        QVERIFY(!fixture.paint().isNull());
        QVERIFY(fixture.cursor.frame.backgroundRect.isEmpty());
        QCOMPARE(fixture.cursor.frame.glyphNodes, 0);
        fixture.conn.grid()->resize(12, 4);
        fixture.conn.grid()->setCursor(1, 1, 2);
        fixture.cursor.setParentItem(nullptr);
        fixture.raster.reset();
        fixture.raster = std::make_unique<QuickRasterizer>();
        fixture.raster->window()->setColor(Qt::transparent);

        QVERIFY(!fixture.paint().isNull());

        QCOMPARE(fixture.cursor.frame.backgroundRect, QRectF(20, 20, 10, 20));
        QVERIFY(fixture.cursor.frame.textureIdentity != 0);
    }

    void roundedCornersAndActualDprChangesReachPixels() {
        CursorFixture fixture;
        QVERIFY(fixture.start());
        fixture.conn.command(QStringLiteral(
            "set guicursor=a:ver100-DotTestCursor-blinkwait0-blinkon0-blinkoff0 | normal! gg0"));
        QTRY_COMPARE(fixture.conn.modeInfo()->cursorShapeInt(),
                     static_cast<int>(CursorShape::Vertical));
        QTRY_COMPARE(fixture.conn.modeInfo()->cellPercentage(), 100);
        QTRY_COMPARE(fixture.conn.grid()->cursorCol(), 0);
        fixture.cursor.setCellWidth(20);
        fixture.cursor.setCellHeight(40);
        quintptr texture = 0;
        for(qreal dpr: { 1.0, 1.5, 2.0 }) {
            const QImage image = fixture.paint(dpr);
            QVERIFY(!image.isNull());
            QCOMPARE(fixture.cursor.frame.glyphNodes, 0);
            QCOMPARE(fixture.raster->window()->effectiveDevicePixelRatio(), dpr);
            QCOMPARE(fixture.cursor.frame.dpr, dpr);
            QCOMPARE(fixture.cursor.frame.backgroundRect, QRectF(0, 0, 20, 40));
            QCOMPARE(fixture.cursor.frame.textureSize, QSize(qCeil(20 * dpr), qCeil(40 * dpr)));
            QVERIFY(fixture.cursor.frame.textureIdentity != texture);
            texture = fixture.cursor.frame.textureIdentity;
            QVERIFY2(image.pixelColor(0, 0).alpha() < 255,
                     qPrintable(QStringLiteral("DPR %1: corner %2")
                                    .arg(dpr)
                                    .arg(image.pixelColor(0, 0).name(QColor::HexArgb))));
            QCOMPARE(image.pixelColor(qRound(10 * dpr), 0), QColor(Qt::magenta));
            QCOMPARE(image.pixelColor(qRound(10 * dpr), qRound(20 * dpr)), QColor(Qt::magenta));
        }
        fixture.cursor.setCornerRadius(0);
        const QImage flat = fixture.paint(2);
        QCOMPARE(flat.pixelColor(0, 0), QColor(Qt::magenta));
        QCOMPARE(fixture.cursor.frame.textureIdentity, quintptr(0));
    }

    void shapeChangeReusesAndClearsCursorNodes() {
        CursorFixture fixture;
        QVERIFY(fixture.start(true));
        const QImage block = fixture.paint();
        QVERIFY(!block.isNull());
        QVERIFY(fixture.cursor.frame.glyphNodes > 0);
        QVERIFY(fixture.cursor.frame.underlineNodes > 0);
        QVERIFY(!colorBounds(block, Qt::black).isEmpty());
        fixture.cursor.setVisible(false);
        const QImage underlay = fixture.paintLayers();
        QVERIFY(!underlay.isNull());
        fixture.cursor.setVisible(true);
        QVERIFY(!fixture.paintLayers().isNull());
        const QRect cell = fixture.cursor.frame.backgroundRect.toAlignedRect();
        QVERIFY(!cell.isEmpty());
        QVERIFY(fixture.cursor.frame.glyphNodes > 0);
        QVERIFY(fixture.cursor.frame.underlineNodes > 0);
        QVERIFY(!colorBounds(underlay.copy(cell), Qt::white).isEmpty());
        const quintptr background = fixture.cursor.frame.backgroundIdentity;
        const quintptr root = fixture.cursor.frame.rootIdentity;
        QVERIFY(background != 0 && root != 0);
        for(const QString &option: { QStringLiteral("ver20"), QStringLiteral("hor20") }) {
            fixture.conn.command(
                QStringLiteral("set guicursor=a:%1-DotTestCursor-blinkwait0-blinkon0-blinkoff0")
                    .arg(option));
            const CursorShape expected = option.startsWith(QStringLiteral("ver"))
                                             ? CursorShape::Vertical
                                             : CursorShape::Horizontal;
            QTRY_COMPARE(fixture.conn.modeInfo()->cursorShapeInt(), static_cast<int>(expected));
            const QImage slim = fixture.paintLayers();
            QVERIFY(!slim.isNull());
            QCOMPARE(fixture.cursor.frame.backgroundIdentity, background);
            QCOMPARE(fixture.cursor.frame.rootIdentity, root);
            QCOMPARE(fixture.cursor.frame.glyphNodes, 0);
            QCOMPARE(fixture.cursor.frame.underlineNodes, 0);
            QCOMPARE(fixture.conn.grid()->cell(1, 0, 2).text, QStringLiteral("X"));
            const QRect cursorRect = fixture.cursor.frame.backgroundRect.toAlignedRect();
            QVERIFY(!cursorRect.isEmpty());
            QVERIFY(cell.contains(cursorRect));
            QVERIFY(cursorRect != cell);
            QVERIFY(!colorBounds(slim.copy(cursorRect), Qt::magenta).isEmpty());
            for(int y = cell.top(); y <= cell.bottom(); ++y) {
                for(int x = cell.left(); x <= cell.right(); ++x) {
                    if(!cursorRect.contains(x, y)) QCOMPARE(slim.pixel(x, y), underlay.pixel(x, y));
                }
            }
        }
    }

    void warmMovementReusesTextureButColorChangesReplaceIt() {
        CursorFixture fixture;
        QVERIFY(!fixture.paint().isNull());
        const quintptr texture = fixture.cursor.frame.textureIdentity;
        fixture.conn.grid()->setCursor(1, 1, 6);
        const QImage moved = fixture.paint();
        QVERIFY(!moved.isNull());
        QCOMPARE(fixture.cursor.frame.textureIdentity, texture);
        QCOMPARE(colorBounds(moved, Qt::magenta), QRect(60, 20, 10, 20));
        fixture.conn.highlights()->setDefaultColors(0x00ffff, 0x000000, 0x00ffff);
        const QImage recolored = fixture.paint();
        QVERIFY(!recolored.isNull());
        QVERIFY(fixture.cursor.frame.textureIdentity != texture);
        QVERIFY(colorBounds(recolored, Qt::magenta).isEmpty());
        QCOMPARE(colorBounds(recolored, Qt::cyan), QRect(60, 20, 10, 20));
    }
};

QTEST_MAIN(TestCursorShape)
#include "test_cursor_shape.moc"
