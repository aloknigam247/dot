#pragma once

#include <algorithm>
#include <QQuickWindow>
#include <QtMath>
#include <QtTest>

#include "CellMetrics.h"
#include "GridItem.h"
#include "ResizeCoalescer.h"

namespace dot::test {

struct PaneOracle {
    qreal bandHeight;
    QSize cells;
    QRectF chatPane;
    QRectF chatToggle;
    QRectF editorPane;
    QRectF grid;
};

inline QSize cellsOracle(QSizeF bounds, const CellMetrics &metrics) {
    return { std::max(12, qFloor(bounds.width() / metrics.cellWidth + 1e-6)),
             std::max(3, qFloor(bounds.height() / metrics.cellHeight + 1e-6)) };
}

inline QPointF gridOriginOracle(const CellMetrics &metrics, qreal dpr) {
    return { qCeil(10 * dpr) / dpr, qCeil((10 + metrics.cellHeight + 8) * dpr) / dpr };
}

inline PaneOracle paneOracle(QSizeF window, bool chatVisible, const CellMetrics &metrics,
                             qreal dpr) {
    const qreal allocation = chatVisible ? std::min(360, qFloor(window.width() / 2)) : 0;
    const qreal band = metrics.cellHeight + 8;
    const qreal height = std::max(0.0, window.height() - band - 8);
    const QPointF origin = gridOriginOracle(metrics, dpr);
    const QRectF grid(origin, QSizeF(std::max(0.0, window.width() - allocation - 10 - origin.x()),
                                     std::max(0.0, window.height() - 10 - origin.y())));
    return { band,
             cellsOracle(grid.size(), metrics),
             { window.width() - allocation, 4 + band, std::max(0.0, allocation - 4), height },
             { window.width() - 4 - band, 0, band, band },
             { 4, 4 + band, std::max(0.0, window.width() - allocation - 8), height },
             grid };
}

inline QSize windowSizeOracle(int cols, int rows, bool chatVisible, const CellMetrics &metrics,
                              qreal dpr) {
    const QPointF origin = gridOriginOracle(metrics, dpr);
    const int extent = qCeil(cols * metrics.cellWidth + origin.x() + 10);
    return { extent + (chatVisible ? std::min(360, extent) : 0),
             qCeil(rows * metrics.cellHeight + origin.y() + 10) };
}

inline CellMetrics fontMetricsOracle(const GridItem &item, int linespace, qreal dpr) {
    QFont font = item.textFont();
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    return computeCellMetrics(QFontMetricsF(font), linespace, dpr);
}

inline QRectF sceneRect(const QQuickItem *item) {
    return item->mapRectToScene(item->boundingRect());
}

inline void comparePaneScene(QQuickWindow *window, const PaneOracle &expected, bool chatVisible) {
    auto *chat = window->findChild<QQuickItem *>(QStringLiteral("chatPanel"));
    auto *editor = window->findChild<QQuickItem *>(QStringLiteral("editorPane"));
    auto *grid = window->findChild<GridItem *>();
    auto *input = window->findChild<QQuickItem *>(QStringLiteral("chatInput"));
    auto *row = window->findChild<QQuickItem *>(QStringLiteral("chatInputRow"));
    auto *shell = window->findChild<QQuickItem *>(QStringLiteral("shell"));
    auto *toggle = window->findChild<QQuickItem *>(QStringLiteral("chatToggle"));
    QVERIFY(chat && editor && grid && input && row && shell && toggle);
    QCOMPARE(chat->isVisible(), chatVisible);
    QCOMPARE(sceneRect(chat), expected.chatPane);
    QCOMPARE(sceneRect(editor), expected.editorPane);
    QCOMPARE(sceneRect(shell), expected.grid);
    QCOMPARE(sceneRect(toggle), expected.chatToggle);
    QCOMPARE(window->activeFocusItem(), chatVisible ? input : grid);
    if(chatVisible) {
        const QRectF pane = expected.chatPane;
        const qreal h = std::max(0.0, std::min(expected.bandHeight, pane.height() - 4));
        QCOMPARE(row->height(), h);
        if(h > 0)
            QTRY_COMPARE(sceneRect(row),
                         QRectF(pane.x() + 2, pane.bottom() - 2 - h, pane.width() - 4, h));
    }
}

inline QSize liveCells(const NvimConnector &conn) {
    return { conn.grid()->gridCols(1), conn.grid()->gridRows(1) };
}

inline void checkNoResizeEcho(NvimConnector &conn) {
    QSignalSpy spy(conn.resizeCoalescer(), &ResizeCoalescer::resizeRequested);
    QTest::qWait(1);
    QTest::qWait(1);
    conn.resizeCoalescer()->flushNow();
    QCOMPARE(spy.count(), 0);
}

} // namespace dot::test
