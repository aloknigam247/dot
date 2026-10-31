#include "ImageModel.h"
#include "support/ImageHelpers.h"

#include <array>
#include <limits>
#include <memory>
#include <QCoreApplication>
#include <QPointer>
#include <QSignalSpy>
#include <QtTest>

namespace dot {

using test::CreateResult;
using test::createSync;
using test::makePng;

class TestImageModel : public QObject {
    Q_OBJECT

private slots:
    void callbacksCanDestroyTheModel() {
        bool completed = false;
        auto *model = new ImageModel;
        QPointer<ImageModel> guard(model);
        model->create(makePng(), {}, [&](CreateResult result) {
            QVERIFY(result.has_value());
            completed = true;
            delete model;
        });
        QTRY_VERIFY(completed);
        QVERIFY(guard.isNull());

        completed = false;
        model = new ImageModel;
        guard = model;
        model->create(makePng(), {}, [&](CreateResult result) {
            QVERIFY(!result.has_value());
            completed = true;
            delete model;
        });
        QVERIFY(model->removeAll());
        QVERIFY(completed);
        QVERIFY(guard.isNull());
    }

    void cancelledDecodeCannotPopulateANewSession() {
        bool cancelled = false;
        ImageModel model;
        model.create(makePng(), {}, [&](CreateResult result) {
            QVERIFY(!result.has_value());
            QVERIFY(result.error().contains(QStringLiteral("cancelled")));
            cancelled = true;
        });
        QCOMPARE(model.count(), qsizetype(1));
        QVERIFY(!model.get(1).has_value());
        model.queueResize(QSize(80, 24));
        model.reset();
        QVERIFY(cancelled);
        QCOMPARE(model.count(), qsizetype(0));
        QCOMPARE(model.cachedBytes(), qint64(0));
        QCOMPARE(model.reservedBytes(), qint64(0));
        QVERIFY(model.gridSize().isEmpty());

        const auto id = createSync(model, makePng(QSize(2, 2), QImage::Format_ARGB32, Qt::green));
        QVERIFY(id.has_value());
        QCOMPARE(*id, quint32(1));
        QCoreApplication::processEvents();
        QCOMPARE(model.count(), qsizetype(1));
        QCOMPARE(model.placements().constFind(*id)->pixels.pixelColor(0, 0), QColor(Qt::green));
    }

    void clippedPlacementFollowsItsRememberedRegion() {
        ImageModel model;
        const QRectF region(0, 0, 10, 10);
        const auto id =
            createSync(model, makePng(), { .col = 2, .height = 4, .row = 2, .width = 4 });
        QVERIFY(id.has_value());

        const auto visible = [&] {
            const auto &image = model.placements().constFind(*id).value();
            return QRectF(image.position, QSizeF(4, 4)).intersected(image.clip.value_or(region));
        };
        QCOMPARE(visible(), QRectF(1, 1, 4, 4));
        model.queueScroll(region, 3);
        QCOMPARE(visible(), QRectF(1, 1, 4, 4));
        model.flush();
        QCOMPARE(model.placements().constFind(*id)->position, QPointF(1, -2));
        QCOMPARE(visible(), QRectF(1, 0, 4, 2));
        model.queueScroll(region, 1);
        model.flush();
        QCOMPARE(model.placements().constFind(*id)->position, QPointF(1, -3));
        QCOMPARE(visible(), QRectF(1, 0, 4, 1));
        model.queueScroll(region, -1);
        model.flush();
        QCOMPARE(visible(), QRectF(1, 1, 4, 1));

        const ImageOptions requested = *model.get(*id);
        QCOMPARE(requested.row, std::optional<qint32>(2));
        QVERIFY(model.update(*id, { .zIndex = 1 }).has_value());
        QCOMPARE(visible(), QRectF(1, 1, 4, 4));

        model.queueScroll(region, 20);
        model.flush();
        QVERIFY(visible().isEmpty());
        const QPointF discardedPosition = model.placements().constFind(*id)->position;
        model.queueScroll(region, -20);
        model.flush();
        QCOMPARE(model.placements().constFind(*id)->position, discardedPosition);
        QVERIFY(visible().isEmpty());
        QVERIFY(model.update(*id, {}).has_value());
        QCOMPARE(visible(), QRectF(1, 1, 4, 4));
    }

    void concurrentCreationIsRejectedWithoutLosingTheFirst() {
        ImageModel model;
        const auto first = std::make_shared<std::optional<CreateResult>>();
        std::optional<CreateResult> second;
        const QByteArray png = makePng();
        model.create(png, {}, [first](CreateResult result) { *first = std::move(result); });
        model.create(png, {}, [&](CreateResult result) { second = std::move(result); });
        QVERIFY(second.has_value());
        QVERIFY(!second->has_value());
        QVERIFY(second->error().contains(QStringLiteral("already being decoded")));
        QCOMPARE(model.count(), qsizetype(1));
        QTRY_VERIFY(first->has_value());
        QVERIFY(first->value().has_value());
        QCOMPARE(model.reservedBytes(), qint64(0));
        QCOMPARE(model.count(), qsizetype(1));
    }

    void decodedCacheLimitPreservesExistingImages() {
        ImageModel model;
        const QByteArray png = makePng(QSize(4096, 4096));
        QVERIFY(!png.isEmpty());
        std::array<quint32, 4> ids{};
        for(quint32 &id: ids) {
            const auto created = createSync(model, png);
            QVERIFY2(created.has_value(), qPrintable(created ? QString{} : created.error()));
            id = *created;
        }
        QCOMPARE(model.cachedBytes(), ImageModel::kMaxCacheBytes);
        const auto rejected = createSync(model, makePng());
        QVERIFY(!rejected.has_value());
        QVERIFY(rejected.error().contains(QStringLiteral("cache is full")));
        QCOMPARE(model.count(), qsizetype(ids.size()));
        QCOMPARE(model.cachedBytes(), ImageModel::kMaxCacheBytes);
        for(const quint32 id: ids) {
            QVERIFY(model.get(id).has_value());
            QCOMPARE(model.placements().constFind(id)->pixels.pixelColor(0, 0), QColor(17, 34, 51));
        }
        QVERIFY(model.remove(ids.front()));
        QVERIFY(createSync(model, makePng()).has_value());
    }

    void decodedResultCannotExceedTheCacheBudget() {
        std::optional<CreateResult> result;
        ImageModel model;
        const auto first = createSync(model, makePng());
        QVERIFY(first.has_value());
        const qint64 cached = model.cachedBytes();
        model.create(makePng(), {}, [&](CreateResult decoded) { result = std::move(decoded); });
        QVERIFY(model.reservedBytes() > 0);
        model.m_cachedBytes = ImageModel::kMaxCacheBytes;
        QTRY_VERIFY(result.has_value());
        QVERIFY(!result->has_value());
        QVERIFY(result->error().contains(QStringLiteral("cache is full")));
        QCOMPARE(model.count(), qsizetype(1));
        QCOMPARE(model.reservedBytes(), qint64(0));
        QCOMPARE(model.cachedBytes(), ImageModel::kMaxCacheBytes);
        model.m_cachedBytes = cached;
        QVERIFY(model.get(*first).has_value());
        QCOMPARE(model.placements().constFind(*first)->pixels.pixelColor(0, 0), QColor(17, 34, 51));
        QVERIFY(model.remove(*first));
        QCOMPARE(model.cachedBytes(), qint64(0));
    }

    void destructionJoinsWorkerAndDropsItsCallback() {
        bool called = false;
        {
            ImageModel model;
            model.create(makePng(), {}, [&](CreateResult) { called = true; });
        }
        QCoreApplication::processEvents();
        QVERIFY(!called);
    }

    void encodedSizeAndPngErrorsLeaveNoReservation() {
        ImageModel model;
        const QByteArray png = makePng();
        const std::array<QByteArray, 4> invalid{ QByteArray{}, QByteArrayLiteral("GIF89a"),
                                                 QByteArrayLiteral("\x89PNG\r\n\x1a\n"),
                                                 png.left(png.indexOf("IDAT") + 8) };
        for(const auto &bytes: invalid) {
            const auto result = createSync(model, bytes);
            QVERIFY(!result.has_value());
            QCOMPARE(model.count(), qsizetype(0));
            QCOMPARE(model.reservedBytes(), qint64(0));
            QCOMPARE(model.cachedBytes(), qint64(0));
        }

        QByteArray padded = png;
        padded.resize(ImageModel::kMaxEncodedBytes, '\0');
        const auto boundary = createSync(model, padded);
        QVERIFY2(boundary.has_value(), qPrintable(boundary ? QString{} : boundary.error()));
        padded.append('\0');
        const auto tooLarge = createSync(model, padded);
        QVERIFY(!tooLarge.has_value());
        QVERIFY(tooLarge.error().contains(QStringLiteral("encoded image limit")));
        QCOMPARE(model.count(), qsizetype(1));
    }

    void highBitDepthAndAlphaAreDecoded() {
        ImageModel model;
        const QByteArray highDepth = makePng(QSize(4, 4), QImage::Format_RGBA64);
        QVERIFY(highDepth.size() > 24);
        QCOMPARE(static_cast<unsigned char>(highDepth.at(24)), 16);
        const auto highId = createSync(model, highDepth);
        QVERIFY(highId.has_value());
        QCOMPARE(model.placements().constFind(*highId)->pixels.format(),
                 QImage::Format_ARGB32_Premultiplied);

        const auto alphaId =
            createSync(model, makePng(QSize(4, 4), QImage::Format_ARGB32, QColor(255, 0, 0, 128)));
        QVERIFY(alphaId.has_value());
        QCOMPARE(model.placements().constFind(*alphaId)->pixels.pixelColor(0, 0),
                 QColor(255, 0, 0, 128));

        const QByteArray oversized = makePng(QSize(4096, 4096), QImage::Format_RGBA64);
        const auto rejected = createSync(model, oversized);
        QVERIFY(!rejected.has_value());
        QVERIFY(rejected.error().contains(QStringLiteral("decoded image limit")));
        QCOMPARE(model.count(), qsizetype(2));
        QCOMPARE(model.reservedBytes(), qint64(0));
        const auto decoded = ImageModel::decode(oversized);
        QVERIFY(!decoded.has_value());
        QVERIFY(decoded.error().contains(QStringLiteral("decoded image limit")));
    }

    void idsNeverWrapOrReuseDeletedHandles() {
        ImageModel model;
        model.m_nextId = std::numeric_limits<quint32>::max();
        const auto id = createSync(model, makePng());
        QVERIFY(id.has_value());
        QCOMPARE(*id, std::numeric_limits<quint32>::max());
        QVERIFY(model.remove(*id));
        const auto exhausted = createSync(model, makePng());
        QVERIFY(!exhausted.has_value());
        QVERIFY(exhausted.error().contains(QStringLiteral("exhausted")));
        QCOMPARE(model.count(), qsizetype(0));
    }

    void imageCountLimitRejectsWithoutEviction() {
        ImageModel model;
        const QByteArray png = makePng(QSize(1, 1));
        for(qsizetype i = 0; i < ImageModel::kMaxImages; ++i) {
            QVERIFY(createSync(model, png).has_value());
        }
        const auto rejected = createSync(model, png);
        QVERIFY(!rejected.has_value());
        QVERIFY(rejected.error().contains(QStringLiteral("image count limit")));
        QCOMPARE(model.count(), ImageModel::kMaxImages);
        QVERIFY(model.get(1).has_value());
        QVERIFY(model.remove(1));
        const auto next = createSync(model, png);
        QVERIFY(next.has_value());
        QVERIFY(*next > static_cast<quint32>(ImageModel::kMaxImages));
        QCOMPARE(model.count(), ImageModel::kMaxImages);
    }

    void optionsAreValidatedMergedAndCopied() {
        ImageModel model;
        QSignalSpy changes(&model, &ImageModel::changed);
        const std::array<ImageOptions, 4> invalid{ ImageOptions{ .col = 0 },
                                                   ImageOptions{ .height = -1 },
                                                   ImageOptions{ .row = -1 },
                                                   ImageOptions{ .width = 0 } };
        for(const auto &options: invalid) {
            QVERIFY(!createSync(model, makePng(), options).has_value());
        }
        QCOMPARE(model.count(), qsizetype(0));

        const ImageOptions initial{ .col = 3, .height = 4, .row = 2, .width = 6, .zIndex = -7 };
        const auto id = createSync(model, makePng(), initial);
        QVERIFY(id.has_value());
        auto copy = model.get(*id);
        QVERIFY(copy.has_value());
        copy->row = 99;
        QCOMPARE(model.get(*id)->row, initial.row);
        QVERIFY(model.update(*id, { .col = 5, .height = 8, .row = 6, .width = 10, .zIndex = 0 })
                    .has_value());
        QCOMPARE(model.get(*id)->zIndex, std::optional<qint32>(0));
        QCOMPARE(model.get(*id)->width, std::optional<qint32>(10));
        const auto before = model.get(*id);
        QVERIFY(!model.update(*id, { .row = 0 }).has_value());
        QCOMPARE(model.get(*id), before);
        QVERIFY(!model.update(0, {}).has_value());
        QVERIFY(!model.get(0).has_value());
        QVERIFY(!model.remove(0));
        const qsizetype count = changes.count();
        QVERIFY(model.update(*id, {}).has_value());
        QCOMPARE(changes.count(), count);
        QVERIFY(model.removeAll());
        QVERIFY(!model.removeAll());
        QCOMPARE(model.cachedBytes(), qint64(0));
        QCOMPARE(model.count(), qsizetype(0));
    }

    void pendingCreationParticipatesInScrollAndDeletion() {
        ImageModel model;
        const auto result = std::make_shared<std::optional<CreateResult>>();
        model.create(makePng(), { .col = 2, .row = 4 },
                     [result](CreateResult value) { *result = std::move(value); });
        model.queueScroll(QRectF(0, 0, 10, 10), 1);
        model.flush();
        QTRY_VERIFY(result->has_value());
        QVERIFY(result->value().has_value());
        const quint32 id = result->value().value();
        QCOMPARE(model.placements().constFind(id)->position, QPointF(1, 2));

        bool cancelled = false;
        model.create(makePng(), {}, [&](CreateResult value) {
            QVERIFY(!value.has_value());
            cancelled = true;
        });
        QVERIFY(model.remove(id + 1));
        QVERIFY(cancelled);
        QCoreApplication::processEvents();
        QCOMPARE(model.count(), qsizetype(1));
        QVERIFY(model.get(id).has_value());
    }

    void renderFailureIsExplicitAndStaleFailureIsIgnored() {
        ImageModel model;
        QSignalSpy errors(&model, &ImageModel::error);
        const auto id = createSync(model, makePng());
        QVERIFY(id.has_value());
        const qint64 key = model.placements().constFind(*id)->pixels.cacheKey();
        model.reportRenderError(*id, key + 1, QStringLiteral("obsolete"));
        model.reportRenderError(*id + 1, key, QStringLiteral("unknown"));
        QCOMPARE(errors.count(), 0);
        QVERIFY(model.get(*id).has_value());
        model.reportRenderError(*id, key, QStringLiteral("texture allocation failed"));
        QCOMPARE(errors.count(), 1);
        QVERIFY(
            errors.at(0).at(0).toString().contains(QStringLiteral("texture allocation failed")));
        QVERIFY(!model.get(*id).has_value());
        QCOMPARE(model.cachedBytes(), qint64(0));
    }

    void resizePublishesAtFlushAndInvalidRegionsAreReported() {
        ImageModel model;
        QSignalSpy changes(&model, &ImageModel::changed);
        QSignalSpy errors(&model, &ImageModel::error);
        model.queueResize(QSize(80, 24));
        QVERIFY(model.gridSize().isEmpty());
        model.flush();
        QCOMPARE(model.gridSize(), QSize(80, 24));
        QCOMPARE(changes.count(), 0);
        QVERIFY(createSync(model, makePng()).has_value());
        changes.clear();
        model.queueResize(QSize(90, 30));
        model.flush();
        QCOMPARE(changes.count(), 1);
        changes.clear();
        model.queueResize(QSize(90, 30));
        model.queueScroll(QRectF(0, 0, 10, 10), 0);
        model.flush();
        QCOMPARE(changes.count(), 0);
        model.queueResize(QSize(0, 30));
        model.queueScroll(QRectF(), 1);
        QCOMPARE(errors.count(), 2);
        QCOMPARE(model.gridSize(), QSize(90, 30));
    }

    void scrollOwnershipDoesNotFollowUnrelatedOrPartialRegions() {
        ImageModel model;
        const QRectF original(0, 0, 10, 10);
        const auto id = createSync(model, makePng(), { .col = 2, .row = 2 });
        const auto border = createSync(model, makePng(), { .col = 11, .row = 2 });
        QVERIFY(id.has_value());
        QVERIFY(border.has_value());
        model.queueScroll(original, 3);
        model.flush();
        QCOMPARE(model.placements().constFind(*border)->position, QPointF(10, 1));
        const QPointF position = model.placements().constFind(*id)->position;
        const auto clip = model.placements().constFind(*id)->clip;
        model.queueScroll(QRectF(5, 0, 10, 10), 1);
        model.queueScroll(QRectF(20, 0, 10, 10), 1);
        model.flush();
        QCOMPARE(model.placements().constFind(*id)->position, position);
        QCOMPARE(model.placements().constFind(*id)->clip, clip);
        model.queueScroll(QRectF(0, 0, 20, 20), 1);
        model.flush();
        QCOMPARE(model.placements().constFind(*id)->position, QPointF(1, -3));
        QCOMPARE(model.placements().constFind(*id)->scrollRegion,
                 std::optional<QRectF>(QRectF(0, 0, 20, 20)));
    }
};

} // namespace dot

QTEST_GUILESS_MAIN(dot::TestImageModel)
#include "test_image_model.moc"
