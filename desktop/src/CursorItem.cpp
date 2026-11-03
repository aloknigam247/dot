#include "CursorItem.h"

#include "GridModel.h"
#include "HighlightTable.h"
#include "NvimConnector.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QFontDatabase>
#include <QGlyphRun>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QPointF>
#include <QQuickWindow>
#include <QRawFont>
#include <QSGSimpleTextureNode>

namespace dot {

class CursorBackgroundNode final : public QSGNode {
public:
    CursorBackgroundNode() = default;
    ~CursorBackgroundNode() override = default;
    DOT_DISABLE_COPY_MOVE(CursorBackgroundNode)

    void hide() {
        if(m_textureNode) m_textureNode->setRect({});
    }

    bool update(const QRectF &rect, const QColor &color, qreal radius, CursorItem &owner) {
        QQuickWindow *window = owner.window();
        const Key key{ color, window->effectiveDevicePixelRatio(), radius, rect.size() };
        if(key != m_key) {
            m_key = key;
            m_valid = false;
            const qreal pixelHeight = std::ceil(key.size.height() * key.dpr);
            const qreal pixelWidth = std::ceil(key.size.width() * key.dpr);
            if(!std::isfinite(pixelHeight) || !std::isfinite(pixelWidth) || pixelHeight <= 0 ||
               pixelWidth <= 0 || pixelHeight > std::numeric_limits<int>::max() ||
               pixelWidth > std::numeric_limits<int>::max()) {
                qWarning("Cursor texture dimensions exceed image limits");
                return false;
            }
            QImage image = owner.createCursorImage(
                QSize(static_cast<int>(pixelWidth), static_cast<int>(pixelHeight)));
            if(image.isNull()) {
                qWarning("Could not allocate the rounded cursor image");
                return false;
            }
            image.setDevicePixelRatio(key.dpr);
            image.fill(Qt::transparent);
            {
                QPainter painter(&image);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setBrush(color);
                painter.setPen(Qt::NoPen);
                painter.drawRoundedRect(QRectF(QPointF(), key.size), radius, radius);
            }
            auto *texture = owner.createCursorTexture(window, image);
            if(!texture) {
                qWarning("Could not create the rounded cursor texture");
                return false;
            }
            const bool isNew = m_textureNode == nullptr;
            if(isNew) {
                m_textureNode = new QSGSimpleTextureNode;
                m_textureNode->setOwnsTexture(true);
                m_textureNode->setFiltering(QSGTexture::Linear);
            }
            m_textureNode->setTexture(texture);
            m_textureNode->setSourceRect(0, 0, key.size.width() * key.dpr,
                                         key.size.height() * key.dpr);
            m_textureNode->setRect(rect);
            if(isNew) {
                appendChildNode(m_textureNode);
                if(qEnvironmentVariableIntValue("DOT_PROFILE_PAINT") != 0) {
                    qDebug("dot cursor cache: owner=%zu node=%zu key=%zu image=%lld bytes",
                           sizeof(CursorBackgroundNode), sizeof(QSGSimpleTextureNode), sizeof(Key),
                           static_cast<qint64>(image.sizeInBytes()));
                }
            }
            m_valid = true;
        }
        if(m_valid) m_textureNode->setRect(rect);
        return m_valid;
    }

private:
    struct Key {
        QColor color;
        qreal dpr = 0;
        qreal radius = 0;
        QSizeF size;

        bool operator==(const Key &) const = default;
    };

    Key m_key;
    QSGSimpleTextureNode *m_textureNode = nullptr;
    bool m_valid = false;
};

CursorItem::CursorItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    // Cursor item must never take focus — qml/AGENTS.md mandates focus stay
    // on the long-lived baseGrid in Shell.qml so it survives Repeater rebuilds.
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
    // The item emits geometry only where the cursor is; everywhere else it
    // contributes no nodes at all, so the grid below composites through.

    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setPointSizeF(m_fontSize);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setHintingPreference(QFont::PreferFullHinting);
    m_fontName = m_font.family();

    m_clock.start();
    m_blink.notifyActivity(m_clock.elapsed());

    m_blinkTimer.setSingleShot(true);
    connect(&m_blinkTimer, &QTimer::timeout, this, &CursorItem::onBlinkTimeout);
}

CursorItem::~CursorItem() = default;

QImage CursorItem::createCursorImage(const QSize &size) {
    return { size, QImage::Format_ARGB32_Premultiplied };
}

QSGTexture *CursorItem::createCursorTexture(QQuickWindow *window, const QImage &image) {
    return window->createTextureFromImage(image, QQuickWindow::TextureHasAlphaChannel);
}

void CursorItem::setConnector(NvimConnector *c) {
    if(m_conn == c) return;
    if(m_conn) {
        disconnect(m_conn, nullptr, this, nullptr);
        disconnect(m_conn->colorPalette(), nullptr, this, nullptr);
    }
    m_conn = c;
    if(m_conn) {
        connect(m_conn, &NvimConnector::flush, this, &CursorItem::onFlush);
        connect(colorPalette(), &ColorPalette::paletteChanged, this, &CursorItem::scheduleRepaint);
        if(const auto *g = grid()) {
            connect(g, &GridModel::cursorChanged, this, &CursorItem::onCursorActivity);
        }
        if(const auto *h = hl()) {
            connect(h, &HighlightTable::changed, this, &CursorItem::scheduleRepaint);
        }
        if(const auto *m = mode()) {
            connect(m, &ModeInfo::currentChanged, this, &CursorItem::onModeBlinkChanged);
            onModeBlinkChanged();
        }
    }
    emit connectorChanged();
    scheduleRepaint();
}

void CursorItem::setCellWidth(qreal v) {
    if(qFuzzyCompare(v, m_cellWidth)) return;
    m_cellWidth = v;
    emit metricsChanged();
    update();
}

void CursorItem::setCellHeight(qreal v) {
    if(qFuzzyCompare(v, m_cellHeight)) return;
    m_cellHeight = v;
    emit metricsChanged();
    update();
}

void CursorItem::setBaseline(qreal v) {
    if(qFuzzyCompare(v, m_baseline)) return;
    m_baseline = v;
    emit metricsChanged();
    update();
}

void CursorItem::setFontName(const QString &v) {
    if(v == m_fontName) return;
    m_fontName = v;
    rebuildFont();
    emit fontChanged();
    update();
}

void CursorItem::setFontSize(qreal v) {
    if(qFuzzyCompare(v, m_fontSize)) return;
    m_fontSize = v;
    rebuildFont();
    emit fontChanged();
    update();
}

void CursorItem::setCornerRadius(qreal radius) {
    if(!std::isfinite(radius) || radius < 0) {
        qWarning("Cursor corner radius must be finite and non-negative");
        return;
    }
    if(m_cornerRadius == radius) return;
    m_cornerRadius = radius;
    emit cornerRadiusChanged();
    update();
}

void CursorItem::rebuildFont() {
    m_font.setFamily(m_fontName);
    m_font.setPointSizeF(m_fontSize);
}

ColorPalette *CursorItem::colorPalette() const { return m_conn ? m_conn->colorPalette() : nullptr; }
GridModel *CursorItem::grid() const { return m_conn ? m_conn->grid() : nullptr; }
HighlightTable *CursorItem::hl() const { return m_conn ? m_conn->highlights() : nullptr; }
ModeInfo *CursorItem::mode() const { return m_conn ? m_conn->modeInfo() : nullptr; }

QRectF CursorItem::cursorRectFor(int row, int col, qreal cellWidth, qreal cellHeight,
                                 CursorShape shape, int cellPercentage) {
    return cursorRectAtPixel(QPointF(col * cellWidth, row * cellHeight), cellWidth, cellHeight,
                             shape, cellPercentage);
}

QRectF CursorItem::cursorRectAtPixel(QPointF cellTopLeft, qreal cellWidth, qreal cellHeight,
                                     CursorShape shape, int cellPercentage) {
    if(!std::isfinite(cellTopLeft.x()) || !std::isfinite(cellTopLeft.y()) ||
       !std::isfinite(cellWidth) || !std::isfinite(cellHeight) || cellWidth <= 0 ||
       cellHeight <= 0 || !std::isfinite(cellTopLeft.x() + cellWidth) ||
       !std::isfinite(cellTopLeft.y() + cellHeight)) {
        return {};
    }
    const qreal fraction = std::clamp(cellPercentage, 1, 100) / 100.0;
    QRectF rect(cellTopLeft, QSizeF(cellWidth, cellHeight));
    if(shape == CursorShape::Vertical) {
        rect.setWidth(std::min(cellWidth, std::max(1.0, cellWidth * fraction)));
    } else if(shape == CursorShape::Horizontal) {
        const qreal height = std::min(cellHeight, std::max(1.0, cellHeight * fraction));
        rect.setTop(rect.bottom() - height);
        rect.setHeight(height);
    }
    return rect;
}

qreal CursorItem::cornerRadiusFor(const QRectF &rect, qreal radius) {
    if(rect.isEmpty() || !std::isfinite(rect.width()) || !std::isfinite(rect.height()) ||
       !std::isfinite(radius) || radius <= 0) {
        return 0;
    }
    return std::min({ radius, rect.width() / 2.0, rect.height() / 2.0 });
}

void CursorItem::scheduleRepaint() {
    // QQuickItem has no partial-invalidation entry point, and it does not need
    // one: the item's whole output is a couple of quads plus one glyph, so a
    // full rebuild is cheaper than tracking dirty rects was under
    // QQuickPaintedItem (where a partial update still re-rasterised a texture).
    update();
}

void CursorItem::onCursorActivity() {
    m_blink.notifyActivity(m_clock.elapsed());
    rescheduleBlink();
    scheduleRepaint();
}

void CursorItem::onModeBlinkChanged() {
    if(const auto *m = mode()) {
        m_blink.setBlinkParams(m->blinkWait(), m->blinkOn(), m->blinkOff());
    } else {
        m_blink.setBlinkParams(0, 0, 0);
    }
    m_blink.notifyActivity(m_clock.elapsed());
    rescheduleBlink();
    scheduleRepaint();
}

void CursorItem::onBlinkTimeout() {
    // Single-cell repaint — cursor cell only, no blink reset.
    update();
    rescheduleBlink();
}

void CursorItem::onFlush() {
    // The cell under the cursor may have changed content (e.g. `r<char>` in
    // normal mode does not move the cursor). Repaint so the block-mode glyph
    // stays in sync with the underlying cell.
    update();
}

void CursorItem::rescheduleBlink() {
    const qint64 now = m_clock.elapsed();
    const qint64 next = m_blink.nextChangeMs(now);
    if(next == CursorBlinkState::kNoChange) {
        m_blinkTimer.stop();
        return;
    }
    const qint64 delay = std::max<qint64>(1, next - now);
    m_blinkTimer.start(static_cast<int>(delay));
}

QSGNode *CursorItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData * /*data*/) {
    bool roundedVisible = false;
    // A null oldNode means the scene graph destroyed the previous tree, so the
    // cached slot pointers are dangling. Drop them without deleting.
    if(!oldNode) {
        m_blockRoot = m_textRoot = m_lineRoot = nullptr;
        m_blockPool.forget();
        m_linePool.forget();
        m_textPool.forget();
        m_roundedBackground = nullptr;
    }

    QSGNode *root = oldNode;
    if(!root) {
        root = new QSGNode;
        // Three fixed slots, created once: the cursor block, the carried glyph
        // above it, then the underline on top. Same layering GridItem uses, so
        // a descender crossing an underline looks the same under the cursor as
        // it does in the grid.
        m_blockRoot = new QSGNode;
        m_textRoot = new QSGNode;
        m_lineRoot = new QSGNode;
        root->appendChildNode(m_blockRoot);
        root->appendChildNode(m_textRoot);
        root->appendChildNode(m_lineRoot);
    }

    m_blockPool.beginFrame(m_blockRoot, window());
    m_linePool.beginFrame(m_lineRoot, window());
    m_textPool.beginFrame(m_textRoot, window());

    // Every early return still has to commit an empty frame, otherwise the
    // previous frame's cursor stays on screen.
    const auto commit = [&] {
        if(m_roundedBackground && !roundedVisible) m_roundedBackground->hide();
        m_blockPool.endFrame();
        m_linePool.endFrame();
        m_textPool.endFrame();
        return root;
    };

    const ColorPalette *colors = colorPalette();
    const GridModel *g = grid();
    const HighlightTable *h = hl();
    if(!colors || !colors->cursorValid() || !g || !h || !window()) return commit();
    const ModeInfo *m = mode();

    const int active = g->activeGrid();
    const int localRow = g->cursorRowOf(active);
    const int localCol = g->cursorColOf(active);
    if(localRow < 0 || localRow >= g->gridRows(active) || localCol < 0 ||
       localCol >= g->gridCols(active)) {
        return commit();
    }

    const bool cursorOn = m_blink.isOn(m_clock.elapsed());
    const bool drawCursor = cursorOn || !m || !m->cursorStyleEnabled();
    if(!drawCursor) return commit();

    const QColor curColor = colors->cursorBackground();

    const CursorShape shape = m && m->cursorStyleEnabled()
                                  ? static_cast<CursorShape>(m->cursorShapeInt())
                                  : CursorShape::Block;

    const QPointF drawPos(localCol * m_cellWidth, localRow * m_cellHeight);
    const QRectF rect =
        cursorRectAtPixel(drawPos, m_cellWidth, m_cellHeight, shape, m ? m->cellPercentage() : 100);
    if(rect.isEmpty()) return commit();
    const qreal radius = cornerRadiusFor(rect, m_cornerRadius);
    if(radius > 0) {
        if(!m_roundedBackground) {
            m_roundedBackground = new CursorBackgroundNode;
            m_blockRoot->appendChildNode(m_roundedBackground);
        }
        roundedVisible = m_roundedBackground->update(rect, curColor, radius, *this);
        if(!roundedVisible) m_blockPool.add(rect.intersected(boundingRect()), curColor);
    } else {
        if(m_roundedBackground) {
            m_blockRoot->removeChildNode(m_roundedBackground);
            delete m_roundedBackground;
            m_roundedBackground = nullptr;
        }
        m_blockPool.add(rect.intersected(boundingRect()), curColor);
    }

    if(shape == CursorShape::Block) {
        const Cell &cell = g->cell(active, localRow, localCol);
        if(!cell.text.isEmpty()) {
            // Preserve the target cell's font style (italic/bold/strikethrough)
            // so the cursor glyph matches how it renders in the grid below.
            // Without this, an italic word would lose its slant under the
            // block cursor.
            const HlAttr cellAttr = h->resolved(cell.hlId);
            QFont carryFont = m_font;
            carryFont.setItalic(cellAttr.italic);
            carryFont.setWeight(cellAttr.bold ? QFont::Bold : QFont::Normal);
            carryFont.setStrikeOut(cellAttr.strikethrough);

            // A single cell at an explicit x, so the PUA zero-advance defect
            // (QTBUG-116417) that collapses multi-glyph runs cannot bite here:
            // nothing is ever positioned relative to this glyph.
            m_textPool.addText(0, cell.text, carryFont, colors->cursorForeground(),
                               QPointF(drawPos.x(), drawPos.y() + m_baseline));

            if(cellAttr.underline) {
                // The cursor block above overwrote the grid's underline at this
                // cell, so redraw it on top of the block.
                const qreal thickness =
                    std::max(1.0, std::round(QFontMetricsF(carryFont).lineWidth()));
                m_linePool.add(QRectF(drawPos.x(), drawPos.y() + m_cellHeight - thickness,
                                      m_cellWidth, thickness)
                                   .intersected(boundingRect()),
                               colors->cursorForeground());
            }
        }
    }

    return commit();
}

} // namespace dot
