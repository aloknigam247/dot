#ifndef IMAGEITEM_H
#define IMAGEITEM_H

#include <QMap>
#include <QPointer>
#include <qqmlregistration.h>
#include <QQuickItem>

#include "DotMacros.h"
#include "GridItem.h"
#include "ImageModel.h"

QT_BEGIN_NAMESPACE
class QSGTexture;
QT_END_NAMESPACE

namespace dot {

class ImagePlacementItem;

class ImageItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(dot::GridItem *grid READ grid WRITE setGrid NOTIFY gridChanged)
    Q_PROPERTY(dot::ImageModel *model READ model WRITE setModel NOTIFY modelChanged)

public:
    explicit ImageItem(QQuickItem *parent = nullptr);
    ~ImageItem() override;
    DOT_DISABLE_COPY_MOVE(ImageItem)

    GridItem *grid() const { return m_grid; }
    ImageModel *model() const { return m_model; }
    void setGrid(GridItem *grid);
    void setModel(ImageModel *model);

signals:
    void gridChanged();
    void modelChanged();

protected:
    virtual QSGTexture *createTexture(QQuickWindow *window, const QImage &image);

private:
    friend class ImagePlacementItem;

    void clearPlacements();
    void reportRenderError(quint32 id, qint64 imageKey);
    void synchronize();

    QPointer<GridItem> m_grid;
    QMap<quint32, ImagePlacementItem *> m_items;
    QPointer<ImageModel> m_model;
};

} // namespace dot

#endif
