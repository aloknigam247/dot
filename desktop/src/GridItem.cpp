#include "GridItem.h"
#include "CellMetrics.h"
#include "GridModel.h"
#include "GridRuns.h"
#include "HighlightTable.h"
#include "InputHandler.h"
#include "NvimConnector.h"

#include <climits>
#include <limits>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QRawFont>
#include <QRegularExpression>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QtGlobal>
#include <QtMath>
#include <QVarLengthArray>
#include <QVector>
#include <QWheelEvent>

namespace dot {

class GridBackgroundItem : public QQuickItem {
public:
    GridBackgroundItem(GridItem *owner, GridItem::BackgroundLayer layer) :
        QQuickItem(owner), m_layer(layer), m_owner(owner) {
        setAcceptedMouseButtons(Qt::NoButton);
        setFlag(ItemHasContents, true);
    }

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData * /*data*/) override {
        return m_owner->updateBackgroundNode(oldNode, m_layer);
    }

private:
    GridItem::BackgroundLayer m_layer;
    GridItem *m_owner;
};

static void parseGuifont(const QString &guifont, QString &family, qreal &size) {
    if(guifont.isEmpty()) return;
    const auto parts = guifont.split(QLatin1Char(':'));
    if(parts.isEmpty()) return;
    family = parts.first();
    family.replace(QLatin1Char('_'), QLatin1Char(' '));
    for(int i = 1; i < parts.size(); ++i) {
        const QString &p = parts.at(i);
        if(p.startsWith(QLatin1Char('h')) && p.size() > 1) {
            bool ok = false;
            const qreal v = p.mid(1).toDouble(&ok);
            if(ok) size = v;
        }
    }
}

GridItem::~GridItem() = default;

GridItem::GridItem(QQuickItem *parent) :
    QQuickItem(parent), m_backgroundCells(new GridBackgroundItem(this, BackgroundLayer::Cells)),
    m_backgroundDefault(new GridBackgroundItem(this, BackgroundLayer::Default)) {
    m_backgroundDefault->setObjectName(QStringLiteral("grid_default_background"));
    m_backgroundDefault->setZ(static_cast<qreal>(std::numeric_limits<qint32>::min()) - 1.0);
    m_backgroundCells->setObjectName(QStringLiteral("grid_cell_background"));
    m_backgroundCells->setZ((static_cast<qreal>(std::numeric_limits<qint32>::min()) / 2.0) - 0.5);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(false);
    setFlag(ItemHasContents, true);
    setFlag(ItemIsFocusScope, true);
    setActiveFocusOnTab(true);
    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setPointSizeF(m_fontSize);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setHintingPreference(QFont::PreferFullHinting);
    m_fontName = m_font.family();
    recomputeMetrics();
}

void GridItem::setGridId(int id) {
    if(m_gridId == id) return;
    m_gridId = id;
    emit gridIdChanged();
    invalidateRendering();
}

void GridItem::setConnector(NvimConnector *c) {
    if(m_conn == c) return;
    if(m_conn) {
        disconnect(m_conn, nullptr, this, nullptr);
        disconnect(m_conn->grid(), nullptr, this, nullptr);
        disconnect(m_conn->highlights(), nullptr, this, nullptr);
    }
    m_conn = c;
    if(m_conn) {
        connect(m_conn, &NvimConnector::guifontChanged, this, &GridItem::onGuifontChanged);
        connect(m_conn, &NvimConnector::linespaceChanged, this, &GridItem::onLinespaceChanged);
        connect(m_conn, &NvimConnector::flush, this, &GridItem::onFlush);
        if(const auto *g = grid()) {
            connect(g, &GridModel::sizeChanged, this, &GridItem::invalidateRendering);
        }
        if(const auto *h = hl()) {
            connect(h, &HighlightTable::changed, this, &GridItem::invalidateRendering);
        }
    }
    emit connectorChanged();
    invalidateRendering();
}

void GridItem::setFontName(const QString &name) {
    if(name == m_fontName) return;
    m_fontName = name;
    m_font.setFamily(name);
    recomputeMetrics();
    emit fontChanged();
    maybeResizeUi();
    invalidateRendering();
}

void GridItem::setFontSize(qreal pt) {
    if(qFuzzyCompare(pt, m_fontSize)) return;
    m_fontSize = pt;
    m_font.setPointSizeF(pt);
    recomputeMetrics();
    emit fontChanged();
    maybeResizeUi();
    invalidateRendering();
}

void GridItem::setDebugOverlay(bool v) {
    if(v == m_debugOverlay) return;
    m_debugOverlay = v;
    emit debugOverlayChanged();
    invalidateRendering();
}

void GridItem::recomputeMetrics() {
    // Delegated to computeCellMetrics for testability and to keep the
    // integer-snap rationale (visual-mode seams from sub-pixel boundaries)
    // in one place.
    //
    // The cellWidth chosen by computeCellMetrics (LCM-unit snapped for
    // device-pixel alignment) usually differs from the font's natural
    // horizontalAdvance for monospace characters. Without correction
    // QPainter::drawText lays each glyph at its font-native advance, so
    // over a row of N characters the visible text drifts away from the
    // cell-boundary grid by N*(cellWidth - nativeAdvance) px. Cursor and
    // click logic both use cellWidth, so the cursor block ends up over a
    // visually different character than the buffer position it represents.
    //
    // QFont::setLetterSpacing(AbsoluteSpacing, delta) adds delta px after
    // every glyph's advance. Setting delta = cellWidth - nativeAdvance
    // forces the effective advance to equal cellWidth, so drawText layout
    // and cell math agree pixel-for-pixel.
    m_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
    const QFontMetricsF fm(m_font);
    const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
    const CellMetrics cm = computeCellMetrics(fm, m_linespace, dpr);
    m_cellWidth = cm.cellWidth;
    m_cellHeight = cm.cellHeight;
    m_baseline = cm.baseline;
    const qreal nativeAdvance = fm.horizontalAdvance(QLatin1Char('M'));
    m_font.setLetterSpacing(QFont::AbsoluteSpacing, m_cellWidth - nativeAdvance);
}

void GridItem::onLinespaceChanged() {
    if(!m_conn) return;
    const int ls = m_conn->linespace();
    if(ls == m_linespace) return;
    m_linespace = ls;
    recomputeMetrics();
    emit fontChanged();
    maybeResizeUi();
    invalidateRendering();
}

QFont GridItem::buildRunFont(const HlAttr &a) const {
    QFont rf = m_font;
    rf.setWeight(a.bold ? QFont::Bold : QFont::Normal);
    rf.setItalic(a.italic);
    rf.setStrikeOut(a.strikethrough);
    return rf;
}

void GridItem::onGuifontChanged() {
    if(!m_conn) return;
    QString family = m_fontName;
    qreal size = m_fontSize;
    parseGuifont(m_conn->guifont(), family, size);
    m_fontName = family;
    m_fontSize = size;
    m_font.setFamily(family);
    m_font.setPointSizeF(size);
    recomputeMetrics();
    emit fontChanged();
    maybeResizeUi();
    invalidateRendering();
}

void GridItem::onFlush() {
    // Skip update() when nvim's batch between flushes touched no cells on
    // this grid (the common case for held j/k that doesn't scroll past the
    // viewport: nvim emits grid_cursor_goto + flush only, no grid_line).
    // The CursorItem overlay handles its own cursor-rect repaint per flush;
    // without this guard, every keystroke at 300x100 fullscreen re-ran the
    // full rows*cols paint loop on this item.
    if(auto *g = grid(); g && g->takeDirty(m_gridId)) { invalidateRendering(); }
}

GridModel *GridItem::grid() const { return m_conn ? m_conn->grid() : nullptr; }
HighlightTable *GridItem::hl() const { return m_conn ? m_conn->highlights() : nullptr; }

int GridItem::colAt(qreal x) const {
    if(m_cellWidth <= 0) return 0;
    return static_cast<int>(x / m_cellWidth);
}

int GridItem::rowAt(qreal y) const {
    if(m_cellHeight <= 0) return 0;
    return static_cast<int>(y / m_cellHeight);
}

void GridItem::maybeResizeUi() {
    if(!m_conn || width() <= 0 || height() <= 0) return;
    if(m_gridId != 1) return;
    const int cols = std::max(10, static_cast<int>(width() / m_cellWidth));
    const int rows = std::max(3, static_cast<int>(height() / m_cellHeight));
    // Delegate dedup to ResizeCoalescer (which compares against the last emitted
    // request). Guarding here against the grid model instead would skip the
    // corrective resize during a font/linespace change on :restart, when the
    // model still reads the pre-change size while an earlier resize is in flight.
    m_conn->requestResize(cols, rows);
}

void GridItem::geometryChange(const QRectF &newGeom, const QRectF &oldGeom) {
    QQuickItem::geometryChange(newGeom, oldGeom);
    if(newGeom.size() != oldGeom.size()) {
        if(m_backgroundCells) m_backgroundCells->setSize(newGeom.size());
        if(m_backgroundDefault) m_backgroundDefault->setSize(newGeom.size());
        invalidateRendering();
    }
    maybeResizeUi();
}

void GridItem::itemChange(ItemChange change, const ItemChangeData &data) {
    QQuickItem::itemChange(change, data);
    if(change != ItemSceneChange && change != ItemDevicePixelRatioHasChanged) return;
    if((change == ItemSceneChange && !data.window) || !window()) return;

    recomputeMetrics();
    emit fontChanged();
    maybeResizeUi();
    invalidateRendering();
}

void GridItem::invalidateRendering() {
    ++m_renderRevision;
    QQuickItem::update();
    if(m_backgroundCells) m_backgroundCells->update();
    if(m_backgroundDefault) m_backgroundDefault->update();
}

const GridRuns &GridItem::preparedRuns() {
    if(m_preparedRevision != m_renderRevision) {
        const GridModel *g = grid();
        const HighlightTable *h = hl();
        m_renderRuns = g && h ? buildGridRuns(*g, *h, m_gridId) : GridRuns{};
        m_preparedRevision = m_renderRevision;
    }
    return m_renderRuns;
}

QSGNode *GridItem::updateBackgroundNode(QSGNode *oldNode, BackgroundLayer layer) {
    QElapsedTimer timer;
    static const bool profile = qEnvironmentVariableIntValue("DOT_PROFILE_PAINT") != 0;
    if(profile) timer.start();
    QSGNode *root = oldNode;
    HighlightTable *h = hl();

    if(layer == BackgroundLayer::Default) {
        if(!root) {
            m_defaultBgPool.forget();
            root = new QSGNode;
        }
        m_defaultBgPool.beginFrame(root, window());
        m_defaultBgPool.add(boundingRect(), h ? h->defaultBg() : QColor(Qt::black));
        m_defaultBgPool.endFrame();
    } else {
        if(!root) {
            m_bgPool.forget();
            m_backgroundDecorationNode = nullptr;
            root = new QSGNode;
            m_bgRoot = new QSGNode;
            m_backgroundDecorationRoot = new QSGNode;
            root->appendChildNode(m_bgRoot);
            root->appendChildNode(m_backgroundDecorationRoot);
        }
        m_bgPool.beginFrame(m_bgRoot, window());
        if(grid() && h) {
            const GridRuns &runs = preparedRuns();
            const QColor defaultBg = h->defaultBg();
            for(const PillSpan &span: runs.pills) {
                if(!span.backBg.isValid() || span.backBg == defaultBg) continue;
                const QColor pillBg = h->resolved(span.hlId).bg;
                if(!pillBg.isValid() || pillBg == defaultBg) continue;
                m_bgPool.add(QRectF(span.c0 * m_cellWidth, span.row * m_cellHeight,
                                    (span.c1 - span.c0) * m_cellWidth, m_cellHeight),
                             span.backBg);
            }
            for(const CellRun &run: runs.runs) {
                if(run.fillBg) {
                    m_bgPool.add(QRectF(run.c0 * m_cellWidth, run.row * m_cellHeight,
                                        (run.c1 - run.c0) * m_cellWidth, m_cellHeight),
                                 run.bg);
                }
            }
            updateDecorations(runs, h, m_backgroundDecorationRoot, m_backgroundDecorationNode,
                              true);
        } else if(m_backgroundDecorationNode) {
            m_backgroundDecorationRoot->removeChildNode(m_backgroundDecorationNode);
            delete m_backgroundDecorationNode;
            m_backgroundDecorationNode = nullptr;
        }
        m_bgPool.endFrame();
    }
    if(profile) {
        qDebug("dot background paint: %lld us (%s)", timer.nsecsElapsed() / 1000,
               layer == BackgroundLayer::Default ? "default" : "cells");
    }
    return root;
}

void GridItem::updateDecorations(const GridRuns &runs, HighlightTable *h, QSGNode *root,
                                 QSGNode *&textureNode, bool backgrounds) {
    const QColor defaultBg = h->defaultBg();
    const QVector<PillSpan> &spans = runs.pills;

    // Union bounding box of everything this layer draws. Both features are rare
    // (pills need g:dot_rounded_highlights, undercurl needs diagnostics), so
    // the common frame finds nothing and skips the texture entirely.
    QRectF bbox;
    if(backgrounds) {
        for(const PillSpan &s: spans) {
            const QColor pillBg = h->resolved(s.hlId).bg;
            if(!pillBg.isValid() || pillBg == defaultBg) continue;
            bbox = bbox.united(QRectF(s.c0 * m_cellWidth, s.row * m_cellHeight,
                                      (s.c1 - s.c0) * m_cellWidth, m_cellHeight));
        }
    } else {
        for(const CellRun &run: runs.runs) {
            if(!run.undercurl) continue;
            bbox = bbox.united(QRectF(run.c0 * m_cellWidth,
                                      (run.row * m_cellHeight) + m_cellHeight - 4.0,
                                      (run.c1 - run.c0) * m_cellWidth, 4.0));
        }
    }

    bbox = bbox.intersected(boundingRect());
    if(bbox.isEmpty()) {
        if(textureNode) {
            root->removeChildNode(textureNode);
            delete textureNode;
            textureNode = nullptr;
        }
        return;
    }

    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    QImage img(QSize(qCeil(bbox.width() * dpr), qCeil(bbox.height() * dpr)),
               QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);

    {
        QPainter p(&img);
        p.translate(-bbox.topLeft());
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);

        if(backgrounds && !spans.empty()) {
            const qreal radius = 5.0;

            // Group spans into connected polygons. Two spans are in the same
            // group iff they sit on consecutive rows AND their column ranges
            // overlap. Row gaps or non-overlapping pairs start a new polygon.
            QVarLengthArray<int, 16> groupStart;
            groupStart.push_back(0);
            for(int i = 1; i < spans.size(); ++i) {
                const PillSpan &pa = spans[i - 1];
                const PillSpan &pb = spans[i];
                const bool sameGroup = pb.row == pa.row + 1 && pb.c0 < pa.c1 && pa.c0 < pb.c1 &&
                                       h->resolved(pb.hlId).bg == h->resolved(pa.hlId).bg;
                if(!sameGroup) groupStart.push_back(i);
            }
            groupStart.push_back(static_cast<int>(spans.size()));

            for(int gi = 0; gi + 1 < groupStart.size(); ++gi) {
                const int gs = groupStart[gi];
                const int ge = groupStart[gi + 1];
                const HlAttr a = h->resolved(spans[gs].hlId);
                if(!a.bg.isValid() || a.bg == defaultBg) continue;

                // Build the closed outline as a list of 90-degree corners in
                // clockwise order. Top edge -> right side descending (with a
                // horizontal step at every c1 mismatch) -> bottom edge -> left
                // side ascending (with a step at every c0 mismatch).
                QVarLengthArray<QPointF, 64> verts;
                verts.push_back({ spans[gs].c0 * m_cellWidth, spans[gs].row * m_cellHeight });
                verts.push_back({ spans[gs].c1 * m_cellWidth, spans[gs].row * m_cellHeight });
                for(int i = gs; i < ge - 1; ++i) {
                    if(spans[i].c1 != spans[i + 1].c1) {
                        const qreal y = (spans[i].row + 1) * m_cellHeight;
                        verts.push_back({ spans[i].c1 * m_cellWidth, y });
                        verts.push_back({ spans[i + 1].c1 * m_cellWidth, y });
                    }
                }
                verts.push_back(
                    { spans[ge - 1].c1 * m_cellWidth, (spans[ge - 1].row + 1) * m_cellHeight });
                verts.push_back(
                    { spans[ge - 1].c0 * m_cellWidth, (spans[ge - 1].row + 1) * m_cellHeight });
                for(int i = ge - 1; i > gs; --i) {
                    if(spans[i].c0 != spans[i - 1].c0) {
                        const qreal y = spans[i].row * m_cellHeight;
                        verts.push_back({ spans[i].c0 * m_cellWidth, y });
                        verts.push_back({ spans[i - 1].c0 * m_cellWidth, y });
                    }
                }

                // Round every corner with a quadratic Bezier whose control
                // point IS the corner itself. The curve always bulges TOWARD
                // the control point — that direction is outside-the-polygon
                // for convex corners (smooth outer round) and inside-the-
                // polygon for concave corners (inverse round, eating a small
                // bite into the polygon at the indent). Cap rEff at half the
                // adjacent edge length so short spans don't produce arcs that
                // overlap each other.
                QPainterPath path;
                const int nv = static_cast<int>(verts.size());
                for(int i = 0; i < nv; ++i) {
                    const QPointF &prev = verts[(i + nv - 1) % nv];
                    const QPointF &curr = verts[i];
                    const QPointF &next = verts[(i + 1) % nv];
                    const QPointF dIn = curr - prev;
                    const QPointF dOut = next - curr;
                    const qreal lenIn = qAbs(dIn.x()) + qAbs(dIn.y());
                    const qreal lenOut = qAbs(dOut.x()) + qAbs(dOut.y());
                    const qreal rEff = std::min({ radius, lenIn / 2.0, lenOut / 2.0 });
                    const QPointF pIn = curr - QPointF(dIn.x() / lenIn, dIn.y() / lenIn) * rEff;
                    const QPointF pOut =
                        curr + QPointF(dOut.x() / lenOut, dOut.y() / lenOut) * rEff;
                    if(i == 0) path.moveTo(pIn);
                    else path.lineTo(pIn);
                    path.quadTo(curr, pOut);
                }
                path.closeSubpath();
                p.fillPath(path, a.bg);
            }
        }

        if(!backgrounds) {
            for(const CellRun &run: runs.runs) {
                if(!run.undercurl) continue;
                const qreal left = run.c0 * m_cellWidth;
                const qreal right = run.c1 * m_cellWidth;
                const qreal yy = (run.row * m_cellHeight) + m_cellHeight - 1.5;
                QPainterPath path;
                path.moveTo(left, yy);
                for(int i = 0;; ++i) {
                    const qreal x = left + (i * 4.0);
                    if(x >= right) break;
                    path.quadTo(x + 1.0, yy + 2.0, x + 2.0, yy);
                    path.quadTo(x + 3.0, yy - 2.0, x + 4.0, yy);
                }
                p.setPen(run.sp);
                p.strokePath(path, QPen(run.sp));
            }
        }
    }

    auto *tex = window()->createTextureFromImage(img, QQuickWindow::TextureHasAlphaChannel);
    // QSGNode is not polymorphic (it dispatches on type() rather than a vtable), so dynamic_cast
    // is unavailable; textureNode is one we created as a QSGSimpleTextureNode.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto *node = static_cast<QSGSimpleTextureNode *>(textureNode);
    const bool isNew = node == nullptr;
    if(!node) {
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Nearest);
    }
    node->setTexture(tex);
    node->setRect(bbox);
    // The software renderer inspects newly attached nodes synchronously.
    if(isNew) {
        root->appendChildNode(node);
        textureNode = node;
    }
    node->markDirty(QSGNode::DirtyMaterial);
}

QSGNode *GridItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData * /*data*/) {
    QElapsedTimer paintTimer;
    // A null oldNode means the scene graph threw the previous tree away, so the
    // cached slot pointers are dangling. Drop them without deleting.
    if(!oldNode) {
        m_decoRoot = m_decoNode = m_textRoot = m_lineRoot = nullptr;
        m_linePool.forget();
        m_textPool.forget();
    }

    QSGNode *root = oldNode;
    if(!root) {
        root = new QSGNode;
        // Fixed foreground slots preserve decoration/glyph/underline ordering.
        m_decoRoot = new QSGNode;
        m_textRoot = new QSGNode;
        m_lineRoot = new QSGNode;
        root->appendChildNode(m_decoRoot);
        root->appendChildNode(m_textRoot);
        root->appendChildNode(m_lineRoot);
    }

    const GridModel *g = grid();
    HighlightTable *h = hl();

    m_linePool.beginFrame(m_lineRoot, window());
    m_textPool.beginFrame(m_textRoot, window());

    if(!g || !h) {
        m_linePool.endFrame();
        m_textPool.endFrame();
        if(m_decoNode) {
            m_decoRoot->removeChildNode(m_decoNode);
            delete m_decoNode;
            m_decoNode = nullptr;
        }
        return root;
    }

    // Optional micro-benchmark: enable with DOT_PROFILE_PAINT=1.
    static const bool kProfilePaint = qEnvironmentVariableIntValue("DOT_PROFILE_PAINT") != 0;
    if(kProfilePaint) paintTimer.start();

    const int cols = g->gridCols(m_gridId);
    const int rows = g->gridRows(m_gridId);
    // Hoist underline thickness: m_font is constant across the frame, so
    // constructing QFontMetricsF per underlined run would allocate per cell
    // in the hot path. Compute once and reuse.
    const qreal underlineThickness = std::max(1.0, std::round(QFontMetricsF(m_font).lineWidth()));

    // Single traversal of the grid, resolved into draw-ready runs.
    const GridRuns &runs = preparedRuns();

    // Lazy cache of per-hl_id QFont. Building a QFont and calling
    // setBold/setItalic/setStrikeOut on every run is expensive (QFontPrivate
    // detach + re-resolve). Cache keyed by hl_id is safe because
    // HighlightTable::changed() triggers a fresh frame.
    QHash<int, QFont> fontCache;
    fontCache.reserve(32);

    for(const CellRun &run: runs.runs) {
        const qreal y = run.row * m_cellHeight;
        const QRectF runRect(run.c0 * m_cellWidth, y, (run.c1 - run.c0) * m_cellWidth,
                             m_cellHeight);
        auto it = fontCache.find(run.hlId);
        if(it == fontCache.end())
            it = fontCache.insert(run.hlId, buildRunFont(h->resolved(run.hlId)));

        m_textPool.addText(run.hlId, run.text, it.value(), run.fg,
                           QPointF(run.c0 * m_cellWidth, y + m_baseline));

        if(run.underline) {
            m_linePool.add(QRectF(runRect.left(), y + m_cellHeight - underlineThickness,
                                  runRect.width(), underlineThickness),
                           run.fg);
        }
    }

    // PUA pass. Qt's shaper gives Private Use Area codepoints a zero advance
    // (QTBUG-116417), so a run containing several of them collapses into one
    // spot. Laying out each PUA cell separately at its own x sidesteps the
    // broken advance entirely — the grid supplies every position itself, so the
    // shaper is never asked to place one PUA glyph relative to another. Cells
    // outside the PUA keep the whole-run path, and with it Qt's automatic font
    // fallback (what makes U+1F600 resolve to the system emoji font).
    for(const PuaCluster &cluster: runs.puaClusters) {
        auto it = fontCache.find(cluster.hlId);
        if(it == fontCache.end())
            it = fontCache.insert(cluster.hlId, buildRunFont(h->resolved(cluster.hlId)));
        const QColor fg = h->resolved(cluster.hlId).fg;
        const qreal y = cluster.row * m_cellHeight;
        for(int c = cluster.c0; c < cluster.c1; ++c) {
            const Cell &pcell = g->cell(m_gridId, cluster.row, c);
            if(pcell.doubleWidth || pcell.text.isEmpty()) continue;
            m_textPool.addText(cluster.hlId, pcell.text, it.value(), fg,
                               QPointF(c * m_cellWidth, y + m_baseline));
        }
    }

    if(m_debugOverlay) {
        QFont overlayFont = m_font;
        overlayFont.setBold(true);
        const QString dump = g->dumpAscii(m_gridId);
        int rr = 0;
        for(const QString &line: dump.split(QLatin1Char('\n'))) {
            m_textPool.addText(INT_MIN, line, overlayFont, QColor(255, 0, 0, 200),
                               QPointF(0, (rr * m_cellHeight) + m_baseline));
            ++rr;
        }
    }

    m_linePool.endFrame();
    m_textPool.endFrame();
    updateDecorations(runs, h, m_decoRoot, m_decoNode, false);

    if(kProfilePaint) {
        const qint64 ns = paintTimer.nsecsElapsed();
        qDebug("dot paint: %lld us (%dx%d, %d hl entries cached)", ns / 1000, cols, rows,
               fontCache.size());
    }

    // Cursor is rendered by CursorItem, a sibling overlay in Shell.qml.
    return root;
}

void GridItem::keyPressEvent(QKeyEvent *ev) {
    if(!m_conn) {
        ev->ignore();
        return;
    }
    if(ev->modifiers().testFlag(Qt::ControlModifier) &&
       ev->modifiers().testFlag(Qt::ShiftModifier) && ev->key() == Qt::Key_G) {
        setDebugOverlay(!m_debugOverlay);
        ev->accept();
        return;
    }
    const QString keys = InputHandler::keyToNvim(ev);
    if(!keys.isEmpty()) {
        m_conn->input(keys);
        ev->accept();
        return;
    }
    ev->ignore();
}

void GridItem::sendMouse(QMouseEvent *ev, QEvent::Type type) {
    if(!m_conn) return;
    const InputHandler::MouseInput m = InputHandler::mouseFor(ev, type);
    if(!m.valid) return;
    const int row = rowAt(ev->position().y());
    const int col = colAt(ev->position().x());
    m_conn->inputMouse(m.button, m.action, m.modifier, m_gridId, row, col);
}

void GridItem::mousePressEvent(QMouseEvent *ev) {
    sendMouse(ev, QEvent::MouseButtonPress);
    forceActiveFocus();
    ev->accept();
}
void GridItem::mouseMoveEvent(QMouseEvent *ev) {
    sendMouse(ev, QEvent::MouseMove);
    ev->accept();
}
void GridItem::mouseReleaseEvent(QMouseEvent *ev) {
    sendMouse(ev, QEvent::MouseButtonRelease);
    ev->accept();
}

void GridItem::wheelEvent(QWheelEvent *ev) {
    if(!m_conn) {
        ev->ignore();
        return;
    }
    const auto w =
        InputHandler::wheelFor(ev->angleDelta().x(), ev->angleDelta().y(), ev->modifiers());
    if(!w.valid) {
        ev->ignore();
        return;
    }
    const int row = rowAt(ev->position().y());
    const int col = colAt(ev->position().x());
    m_conn->inputMouse(w.button, w.action, w.modifier, m_gridId, row, col);
    ev->accept();
}

} // namespace dot
