#ifndef IMAGEMODEL_H
#define IMAGEMODEL_H

#include <expected>
#include <functional>
#include <optional>
#include <QByteArray>
#include <QImage>
#include <QMap>
#include <QObject>
#include <QPointF>
#include <qqmlregistration.h>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QThreadPool>
#include <QVarLengthArray>

#include "DotMacros.h"

namespace dot {

struct ImageOptions {
    std::optional<qint32> col;
    std::optional<qint32> height;
    std::optional<qint32> row;
    std::optional<qint32> width;
    std::optional<qint32> zIndex;

    bool operator==(const ImageOptions &) const = default;

    void merge(const ImageOptions &other);
    std::expected<void, QString> validate() const;
};

struct ImagePlacement {
    std::optional<QRectF> clip;
    ImageOptions options;
    QImage pixels;
    QPointF position;
    std::optional<QRectF> scrollRegion;
};

class ImageModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by NvimConnector")

public:
    using CreateCallback = std::function<void(std::expected<quint32, QString>)>;

    static constexpr qint64 kMaxCacheBytes = qint64{ 256 } * 1024 * 1024;
    static constexpr qsizetype kMaxEncodedBytes = qsizetype{ 16 } * 1024 * 1024;
    static constexpr qint64 kMaxImageBytes = qint64{ 64 } * 1024 * 1024;
    static constexpr qsizetype kMaxImages = 128;

    explicit ImageModel(QObject *parent = nullptr);
    ~ImageModel() override;
    DOT_DISABLE_COPY_MOVE(ImageModel)

    void create(QByteArray png, const ImageOptions &options, CreateCallback callback);
    void cancelPending();
    std::optional<ImageOptions> get(quint32 id) const;
    bool remove(quint32 id);
    bool removeAll();
    std::expected<quint32, QString> update(quint32 id, const ImageOptions &options);

    void flush();
    void queueResize(const QSize &size);
    void queueScroll(const QRectF &region, int rows);
    void reportRenderError(quint32 id, qint64 imageKey, const QString &message);
    void reset();

    qint64 cachedBytes() const { return m_cachedBytes; }
    qsizetype count() const { return m_images.size(); }
    QSize gridSize() const { return m_gridSize; }
    const QMap<quint32, ImagePlacement> &placements() const { return m_images; }
    qint64 reservedBytes() const { return m_reservedBytes; }

signals:
    void changed();
    void error(const QString &message);

private:
    friend class TestImageApi;
    friend class TestImageModel;

    struct PendingDecode {
        CreateCallback callback;
        quint32 id = 0;
    };

    struct Scroll {
        QRectF region;
        int rows = 0;
    };

    static std::expected<QImage, QString> decode(const QByteArray &png);
    static std::expected<qint64, QString> inspect(const QByteArray &png);
    static bool scroll(ImagePlacement &image, const Scroll &operation);

    CreateCallback cancelDecode();
    void finishDecode(quint64 generation, quint32 id, std::expected<QImage, QString> result,
                      qint64 elapsedNs);

    qint64 m_cachedBytes = 0;
    quint64 m_generation = 0;
    QSize m_gridSize;
    QMap<quint32, ImagePlacement> m_images;
    quint64 m_nextId = 1;
    std::optional<PendingDecode> m_pendingDecode;
    std::optional<QSize> m_pendingGridSize;
    QThreadPool m_pool;
    qint64 m_reservedBytes = 0;
    QVarLengthArray<Scroll, 8> m_scrolls;
};

} // namespace dot

#endif
