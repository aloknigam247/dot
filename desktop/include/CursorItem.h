#ifndef CURSORITEM_H
#define CURSORITEM_H

#include <QElapsedTimer>
#include <QFont>
#include <QPointer>
#include <QPointF>
#include <qqmlregistration.h>
#include <QQuickItem>
#include <QRectF>
#include <QSGNode>
#include <QTimer>

#include "CursorBlinkState.h"
#include "ModeInfo.h"
#include "NvimConnector.h"
#include "RectNodePool.h"
#include "TextNodePool.h"

QT_BEGIN_NAMESPACE
class QImage;
class QSGTexture;
QT_END_NAMESPACE

namespace dot {

class CursorBackgroundNode;
class GridModel;
class HighlightTable;

// Cursor activity updates this overlay without dirtying the grid's cell content.
class CursorItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(
        dot::NvimConnector *connector READ connector WRITE setConnector NOTIFY connectorChanged)
    Q_PROPERTY(qreal cellWidth READ cellWidth WRITE setCellWidth NOTIFY metricsChanged)
    Q_PROPERTY(qreal cellHeight READ cellHeight WRITE setCellHeight NOTIFY metricsChanged)
    // Renamed from "baseline" — QQuickItem has a FINAL member with that name
    // that hides the Q_PROPERTY override and makes it read-only from QML.
    Q_PROPERTY(qreal cellBaseline READ baseline WRITE setBaseline NOTIFY metricsChanged)
    Q_PROPERTY(QString fontName READ fontName WRITE setFontName NOTIFY fontChanged)
    Q_PROPERTY(qreal fontSize READ fontSize WRITE setFontSize NOTIFY fontChanged)
    Q_PROPERTY(
        qreal cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY cornerRadiusChanged)

public:
    explicit CursorItem(QQuickItem *parent = nullptr);
    ~CursorItem() override;

    NvimConnector *connector() const { return m_conn; }
    void setConnector(NvimConnector *c);

    qreal cellWidth() const { return m_cellWidth; }
    qreal cellHeight() const { return m_cellHeight; }
    qreal baseline() const { return m_baseline; }
    void setCellWidth(qreal v);
    void setCellHeight(qreal v);
    void setBaseline(qreal v);

    QString fontName() const { return m_fontName; }
    qreal fontSize() const { return m_fontSize; }
    void setFontName(const QString &v);
    void setFontSize(qreal v);

    qreal cornerRadius() const { return m_cornerRadius; }
    void setCornerRadius(qreal radius);

    static qreal cornerRadiusFor(const QRectF &rect, qreal radius);

    // Pure rect math, static so unit tests can exercise it without
    // instantiating a QQuickItem (which needs a QGuiApplication).
    // row/col are cell coordinates within the item's own coordinate space.
    static QRectF cursorRectFor(int row, int col, qreal cellWidth, qreal cellHeight,
                                CursorShape shape, int cellPercentage);
    // cellTopLeft uses the same coordinate space as cursorRectFor's output.
    static QRectF cursorRectAtPixel(QPointF cellTopLeft, qreal cellWidth, qreal cellHeight,
                                    CursorShape shape, int cellPercentage);

protected:
    virtual QImage createCursorImage(const QSize &size);
    virtual QSGTexture *createCursorTexture(QQuickWindow *window, const QImage &image);
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

signals:
    void connectorChanged();
    void metricsChanged();
    void fontChanged();
    void cornerRadiusChanged();

private:
    friend class CursorBackgroundNode;

    Q_SLOT void onCursorActivity();   // GridModel::cursorChanged — restart blink + repaint
    Q_SLOT void onModeBlinkChanged(); // ModeInfo::currentChanged — update blink params + repaint
    Q_SLOT void onBlinkTimeout();     // QTimer timeout — toggle visibility, dirty current rect
    Q_SLOT void onFlush(); // NvimConnector::flush — content under cursor may have changed

    GridModel *grid() const;
    HighlightTable *hl() const;
    ModeInfo *mode() const;

    void scheduleRepaint();
    void rescheduleBlink();
    void rebuildFont();

    QPointer<NvimConnector> m_conn;
    qreal m_cellWidth = 8.0;
    qreal m_cellHeight = 16.0;
    qreal m_baseline = 12.0;
    QString m_fontName;
    qreal m_fontSize = 14.0;
    QFont m_font;
    CursorBlinkState m_blink;
    QElapsedTimer m_clock;
    QTimer m_blinkTimer;

    // Scene-graph state. Owned by the returned root node, so these are
    // observing pointers only and are dropped, never deleted, when
    // updatePaintNode is handed a null oldNode. Render thread only.
    QSGNode *m_blockRoot = nullptr;
    QSGNode *m_textRoot = nullptr;
    QSGNode *m_lineRoot = nullptr;
    RectNodePool m_blockPool;
    RectNodePool m_linePool;
    TextNodePool m_textPool;
    qreal m_cornerRadius = 0;
    CursorBackgroundNode *m_roundedBackground = nullptr;
};

} // namespace dot

#endif
