#include "ImageItem.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

namespace dot {
namespace {

struct ImageTextureNode : QSGSimpleTextureNode {
    qint64 imageKey = 0;
};

} // namespace

static QRectF targetRect(const ImagePlacement &image, qreal cellWidth, qreal cellHeight) {
    const QSizeF natural = image.pixels.size();
    QSizeF box = natural;
    QSizeF scaled = natural;
    QPointF origin(image.position.x() * cellWidth, image.position.y() * cellHeight);
    if(image.options.width && image.options.height) {
        box = QSizeF(*image.options.width * cellWidth, *image.options.height * cellHeight);
        scaled.scale(box, Qt::KeepAspectRatio);
        origin += QPointF((box.width() - scaled.width()) / 2, (box.height() - scaled.height()) / 2);
    } else if(image.options.width) {
        scaled.setWidth(*image.options.width * cellWidth);
        scaled.setHeight(natural.height() * scaled.width() / natural.width());
    } else if(image.options.height) {
        scaled.setHeight(*image.options.height * cellHeight);
        scaled.setWidth(natural.width() * scaled.height() / natural.height());
    }
    return { origin, scaled };
}

class ImagePlacementItem : public QQuickItem {
public:
    ImagePlacementItem(ImageItem *owner, quint32 id) : QQuickItem(owner), m_id(id), m_owner(owner) {
        setAcceptedMouseButtons(Qt::NoButton);
        setClip(true);
        setFlag(ItemHasContents, true);
        setObjectName(QStringLiteral("image_%1").arg(id));
    }

    void setContent(const ImagePlacement &image, const QRectF &target, const QRectF &visible) {
        const bool imageChanged = m_image.cacheKey() != image.pixels.cacheKey();
        const bool wasVisible = isVisible();
        if(imageChanged) m_image = image.pixels;
        setZ(image.options.zIndex.value_or(0));
        if(visible.isEmpty()) {
            setVisible(false);
            if(imageChanged) update();
            return;
        }
        const QRectF localTarget = target.translated(-visible.topLeft());
        const bool geometryChanged = m_textureRect != localTarget;
        m_textureRect = localTarget;
        setPosition(visible.topLeft());
        setSize(visible.size());
        setVisible(true);
        if(imageChanged || geometryChanged || !wasVisible) update();
    }

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData * /*data*/) override {
        // This item creates only ImageTextureNode roots.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        auto *node = static_cast<ImageTextureNode *>(oldNode);
        if(m_image.isNull() || m_textureRect.isEmpty() || !m_owner || !window()) {
            // no-cover:start - Qt controls the SceneGraph teardown call sequence.
            delete node;
            return nullptr;
            // no-cover:end
        }
        const qint64 imageKey = m_image.cacheKey();
        if(!node || node->imageKey != imageKey) {
            if(m_failedImageKey == imageKey) return node;
            QSGTexture *texture = m_owner->createTexture(window(), m_image);
            if(!texture) {
                delete node;
                m_failedImageKey = imageKey;
                const quint32 id = m_id;
                const QPointer<ImageItem> owner = m_owner;
                QMetaObject::invokeMethod(this, [owner, id, imageKey] {
                    if(owner) owner->reportRenderError(id, imageKey);
                }, Qt::QueuedConnection);
                return nullptr;
            }
            if(!node) {
                node = new ImageTextureNode;
                node->setOwnsTexture(true);
                node->setFiltering(QSGTexture::Linear);
            }
            node->setTexture(texture);
            node->imageKey = imageKey;
        }
        node->setRect(m_textureRect);
        return node;
    }

private:
    qint64 m_failedImageKey = 0;
    quint32 m_id;
    QImage m_image;
    QPointer<ImageItem> m_owner;
    QRectF m_textureRect;
};

ImageItem::ImageItem(QQuickItem *parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::NoButton);
}

ImageItem::~ImageItem() { clearPlacements(); }

void ImageItem::clearPlacements() {
    qDeleteAll(m_items);
    m_items.clear();
}

void ImageItem::setGrid(GridItem *grid) {
    if(m_grid == grid) return;
    if(m_grid) disconnect(m_grid, nullptr, this, nullptr);
    clearPlacements();
    m_grid = grid;
    if(m_grid) {
        connect(m_grid, &GridItem::fontChanged, this, &ImageItem::synchronize);
        connect(m_grid, &GridItem::gridIdChanged, this, &ImageItem::synchronize);
        connect(m_grid, &QQuickItem::heightChanged, this, &ImageItem::synchronize);
        connect(m_grid, &QQuickItem::widthChanged, this, &ImageItem::synchronize);
        connect(m_grid, &QObject::destroyed, this, [this] {
            m_grid = nullptr;
            clearPlacements();
            emit gridChanged();
        });
    }
    synchronize();
    emit gridChanged();
}

void ImageItem::setModel(ImageModel *model) {
    if(m_model == model) return;
    if(m_model) disconnect(m_model, nullptr, this, nullptr);
    clearPlacements();
    m_model = model;
    if(m_model) {
        connect(m_model, &ImageModel::changed, this, &ImageItem::synchronize);
        connect(m_model, &QObject::destroyed, this, [this] {
            m_model = nullptr;
            clearPlacements();
            emit modelChanged();
        });
    }
    synchronize();
    emit modelChanged();
}

void ImageItem::synchronize() {
    QElapsedTimer timer;
    static const bool profile = qEnvironmentVariableIntValue("DOT_PROFILE_PAINT") != 0;
    if(profile) timer.start();
    if(!m_grid || !m_model || m_grid->gridId() != 1) {
        clearPlacements();
        return;
    }
    const auto &images = m_model->placements();
    for(auto it = m_items.begin(); it != m_items.end();) {
        const auto image = images.constFind(it.key());
        if(image == images.cend() || image->pixels.isNull()) {
            delete it.value();
            it = m_items.erase(it);
        } else {
            ++it;
        }
    }

    const qreal cellHeight = m_grid->cellHeight();
    const qreal cellWidth = m_grid->cellWidth();
    if(cellHeight <= 0 || cellWidth <= 0) return;
    const QRectF gridBounds = QRectF(0, 0, m_model->gridSize().width() * cellWidth,
                                     m_model->gridSize().height() * cellHeight)
                                  .intersected(m_grid->boundingRect());
    for(auto it = images.cbegin(); it != images.cend(); ++it) {
        const ImagePlacement &image = it.value();
        if(image.pixels.isNull()) continue;
        ImagePlacementItem *item = m_items.value(it.key(), nullptr);
        if(!item) {
            item = new ImagePlacementItem(this, it.key());
            item->setParentItem(m_grid);
            item->QObject::setParent(this);
            m_items.insert(it.key(), item);
        }
        const QRectF target = targetRect(image, cellWidth, cellHeight);
        QRectF visible = target.intersected(gridBounds);
        if(image.clip) {
            visible = visible.intersected(
                QRectF(image.clip->x() * cellWidth, image.clip->y() * cellHeight,
                       image.clip->width() * cellWidth, image.clip->height() * cellHeight));
        }
        item->setContent(image, target, visible);
    }
    if(profile) {
        qDebug("dot image update: %lld us (%lld placements)", timer.nsecsElapsed() / 1000,
               static_cast<qlonglong>(images.size()));
    }
}

QSGTexture *ImageItem::createTexture(QQuickWindow *window, const QImage &image) {
    QElapsedTimer timer;
    static const bool profile = qEnvironmentVariableIntValue("DOT_PROFILE_PAINT") != 0;
    if(profile) timer.start();
    QSGTexture *texture = window->createTextureFromImage(image);
    if(profile) {
        qDebug("dot image texture setup: %lld us (%dx%d)", timer.nsecsElapsed() / 1000,
               image.width(), image.height());
    }
    return texture;
}

void ImageItem::reportRenderError(quint32 id, qint64 imageKey) {
    if(m_model) {
        m_model->reportRenderError(id, imageKey,
                                   QStringLiteral("Could not create the image texture"));
    }
}

} // namespace dot
