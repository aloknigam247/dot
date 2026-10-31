#include "ImageModel.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <QBuffer>
#include <QDebug>
#include <QElapsedTimer>
#include <QImageReader>
#include <QMetaObject>
#include <QPixelFormat>
#include <QPointer>

namespace dot {

void ImageOptions::merge(const ImageOptions &other) {
    if(other.col) col = other.col;
    if(other.height) height = other.height;
    if(other.row) row = other.row;
    if(other.width) width = other.width;
    if(other.zIndex) zIndex = other.zIndex;
}

std::expected<void, QString> ImageOptions::validate() const {
    if((col && *col <= 0) || (row && *row <= 0)) {
        return std::unexpected(QStringLiteral("Image row and col must be positive integers"));
    }
    if((height && *height <= 0) || (width && *width <= 0)) {
        return std::unexpected(QStringLiteral("Image width and height must be positive integers"));
    }
    return {};
}

ImageModel::ImageModel(QObject *parent) : QObject(parent) { m_pool.setMaxThreadCount(1); }

ImageModel::~ImageModel() {
    ++m_generation;
    m_pool.clear();
    m_pool.waitForDone();
}

std::expected<qint64, QString> ImageModel::inspect(const QByteArray &png) {
    if(png.size() > kMaxEncodedBytes) {
        return std::unexpected(
            QStringLiteral("PNG exceeds the encoded image limit (%1 bytes)").arg(kMaxEncodedBytes));
    }
    if(!png.startsWith(QByteArrayLiteral("\x89PNG\r\n\x1a\n"))) {
        return std::unexpected(QStringLiteral("Image data must be PNG bytes"));
    }

    QBuffer buffer;
    buffer.setData(png);
    QImageReader reader(&buffer, "png");
    reader.setAutoDetectImageFormat(false);
    const QSize size = reader.size();
    if(size.width() <= 0 || size.height() <= 0) {
        return std::unexpected(QStringLiteral("Invalid PNG: %1").arg(reader.errorString()));
    }

    const int reportedDepth = QImage::toPixelFormat(reader.imageFormat()).bitsPerPixel();
    const qint64 depth = reportedDepth > 0 ? std::max(32, reportedDepth) : 64;
    const qint64 rowBytes = (((qint64{ size.width() } * depth) + 31) / 32) * 4;
    if(rowBytes > kMaxImageBytes / size.height()) {
        return std::unexpected(
            QStringLiteral("PNG dimensions exceed the decoded image limit (%1 bytes)")
                .arg(kMaxImageBytes));
    }
    return rowBytes * size.height();
}

std::expected<QImage, QString> ImageModel::decode(const QByteArray &png) {
    QBuffer buffer;
    buffer.setData(png);
    QImageReader reader(&buffer, "png");
    reader.setAutoDetectImageFormat(false);
    QImage image = reader.read();
    if(image.isNull()) {
        return std::unexpected(QStringLiteral("Invalid PNG: %1").arg(reader.errorString()));
    }
    if(image.sizeInBytes() > kMaxImageBytes) {
        return std::unexpected(
            QStringLiteral("PNG exceeds the decoded image limit (%1 bytes)").arg(kMaxImageBytes));
    }

    image = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                          : QImage::Format_RGB32);
    if(image.isNull()) {
        // no-cover:start - Qt allocation failure cannot be induced safely.
        return std::unexpected(QStringLiteral("Could not allocate decoded image storage"));
        // no-cover:end
    }
    return image;
}

void ImageModel::create(QByteArray png, const ImageOptions &options, CreateCallback callback) {
    ImagePlacement image;
    if(const auto valid = options.validate(); !valid) {
        callback(std::unexpected(valid.error()));
        return;
    }
    if(m_pendingDecode) {
        callback(std::unexpected(
            QStringLiteral("An image is already being decoded; retry after it completes")));
        return;
    }
    if(m_images.size() >= kMaxImages) {
        callback(std::unexpected(
            QStringLiteral("The image count limit (%1) has been reached").arg(kMaxImages)));
        return;
    }
    if(m_nextId > std::numeric_limits<quint32>::max()) {
        callback(std::unexpected(QStringLiteral("Image IDs are exhausted for this session")));
        return;
    }

    const auto required = inspect(png);
    if(!required) {
        callback(std::unexpected(required.error()));
        return;
    }
    if(*required > kMaxCacheBytes - m_cachedBytes) {
        callback(std::unexpected(
            QStringLiteral("The decoded image cache is full (%1 bytes)").arg(kMaxCacheBytes)));
        return;
    }

    const quint64 generation = m_generation;
    const auto id = static_cast<quint32>(m_nextId++);
    image.options = options;
    image.position = QPointF(options.col.value_or(1) - 1, options.row.value_or(1) - 1);
    m_images.insert(id, image);
    m_pendingDecode = PendingDecode{ std::move(callback), id };
    m_reservedBytes = *required;

    m_pool.start([this, generation, id, png = std::move(png)] {
        QElapsedTimer timer;
        timer.start();
        auto result = decode(png);
        const qint64 elapsedNs = timer.nsecsElapsed();
        QMetaObject::invokeMethod(
            this, [this, generation, id, result = std::move(result), elapsedNs]() mutable {
            finishDecode(generation, id, std::move(result), elapsedNs);
        }, Qt::QueuedConnection);
    });
}

void ImageModel::finishDecode(quint64 generation, quint32 id, std::expected<QImage, QString> result,
                              qint64 elapsedNs) {
    if(generation != m_generation || !m_pendingDecode || m_pendingDecode->id != id) return;

    const QPointer<ImageModel> guard(this);
    const CreateCallback callback = std::move(m_pendingDecode->callback);
    m_pendingDecode.reset();
    m_reservedBytes = 0;
    auto it = m_images.find(id);
    if(!result) {
        m_images.erase(it);
        callback(std::unexpected(result.error()));
        return;
    }
    if(result->sizeInBytes() > kMaxCacheBytes - m_cachedBytes) {
        m_images.erase(it);
        callback(std::unexpected(
            QStringLiteral("The decoded image cache is full (%1 bytes)").arg(kMaxCacheBytes)));
        return;
    }

    it->pixels = std::move(*result);
    m_cachedBytes += it->pixels.sizeInBytes();
    if(qEnvironmentVariableIntValue("DOT_PROFILE_PAINT") != 0) {
        qDebug("dot image decode: %lld us (%lld bytes cached)", elapsedNs / 1000, m_cachedBytes);
    }
    callback(id);
    if(guard) emit changed();
}

std::optional<ImageOptions> ImageModel::get(quint32 id) const {
    const auto it = m_images.constFind(id);
    if(it == m_images.cend() || it->pixels.isNull()) return std::nullopt;
    return it->options;
}

std::expected<quint32, QString> ImageModel::update(quint32 id, const ImageOptions &options) {
    if(const auto valid = options.validate(); !valid) return std::unexpected(valid.error());
    auto it = m_images.find(id);
    if(it == m_images.end() || it->pixels.isNull()) {
        return std::unexpected(QStringLiteral("Unknown image ID: %1").arg(id));
    }

    ImageOptions merged = it->options;
    merged.merge(options);
    const QPointF position(merged.col.value_or(1) - 1, merged.row.value_or(1) - 1);
    if(merged == it->options && position == it->position && !it->clip && !it->scrollRegion) {
        return id;
    }
    it->options = merged;
    it->position = position;
    it->clip.reset();
    it->scrollRegion.reset();
    emit changed();
    return id;
}

ImageModel::CreateCallback ImageModel::cancelDecode() {
    CreateCallback callback;
    ++m_generation;
    m_pool.clear();
    if(m_pendingDecode) callback = std::move(m_pendingDecode->callback);
    m_pendingDecode.reset();
    m_reservedBytes = 0;
    return callback;
}

void ImageModel::cancelPending() {
    if(m_pendingDecode) remove(m_pendingDecode->id);
}

bool ImageModel::remove(quint32 id) {
    CreateCallback callback;
    const QPointer<ImageModel> guard(this);
    const auto it = m_images.find(id);
    if(it == m_images.end()) return false;
    if(m_pendingDecode && m_pendingDecode->id == id) callback = cancelDecode();
    m_cachedBytes -= it->pixels.sizeInBytes();
    m_images.erase(it);
    if(callback) callback(std::unexpected(QStringLiteral("Image creation was cancelled")));
    if(guard) emit changed();
    return true;
}

bool ImageModel::removeAll() {
    const QPointer<ImageModel> guard(this);
    const bool found = !m_images.isEmpty();
    const CreateCallback callback = cancelDecode();
    m_images.clear();
    m_cachedBytes = 0;
    m_scrolls.clear();
    if(callback) callback(std::unexpected(QStringLiteral("Image creation was cancelled")));
    if(guard && found) emit changed();
    return found;
}

void ImageModel::queueResize(const QSize &size) {
    if(size.width() <= 0 || size.height() <= 0) {
        emit error(QStringLiteral("Invalid image grid dimensions"));
        return;
    }
    m_pendingGridSize = size;
}

void ImageModel::queueScroll(const QRectF &region, int rows) {
    if(!region.isValid()) {
        emit error(QStringLiteral("Invalid image scroll region"));
        return;
    }
    if(rows == 0 || m_images.isEmpty()) return;
    m_scrolls.append(Scroll{ region, rows });
}

bool ImageModel::scroll(ImagePlacement &image, const Scroll &operation) {
    if(image.clip && image.clip->isEmpty()) return false;
    const QRectF &region = operation.region;
    const QPointF &position = image.position;
    const bool containsAnchor = position.x() >= region.left() && position.x() < region.right() &&
                                position.y() >= region.top() && position.y() < region.bottom();
    const bool containsOwner = image.scrollRegion && region.contains(*image.scrollRegion);
    if(!containsAnchor && !containsOwner) return false;

    const qreal dy = -static_cast<qreal>(operation.rows);
    const QRectF retained = image.clip.value_or(region).intersected(region);
    image.clip = retained.translated(0, dy).intersected(region);
    image.position.ry() += dy;
    image.scrollRegion = region;
    return true;
}

void ImageModel::flush() {
    bool dirty = false;
    if(m_pendingGridSize) {
        dirty = m_gridSize != *m_pendingGridSize && !m_images.isEmpty();
        m_gridSize = *m_pendingGridSize;
        m_pendingGridSize.reset();
    }
    for(const Scroll &operation: m_scrolls) {
        for(auto it = m_images.begin(); it != m_images.end(); ++it) {
            dirty = scroll(it.value(), operation) || dirty;
        }
    }
    m_scrolls.clear();
    if(dirty) emit changed();
}

void ImageModel::reportRenderError(quint32 id, qint64 imageKey, const QString &message) {
    const QPointer<ImageModel> guard(this);
    const auto it = m_images.constFind(id);
    if(it == m_images.cend() || it->pixels.cacheKey() != imageKey) return;
    remove(id);
    if(guard)
        emit error(QStringLiteral("Image %1 could not be displayed: %2").arg(id).arg(message));
}

void ImageModel::reset() {
    m_gridSize = {};
    m_nextId = 1;
    m_pendingGridSize.reset();
    removeAll();
}

} // namespace dot
