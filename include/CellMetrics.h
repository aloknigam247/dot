#ifndef CELLMETRICS_H
#define CELLMETRICS_H

#include <QFontMetricsF>
#include <QtGlobal>

namespace dot {

struct CellMetrics {
    qreal cellWidth;
    qreal cellHeight;
    qreal baseline;
};

CellMetrics computeCellMetrics(const QFontMetricsF &fm, int linespace, qreal dpr = 1.0);

} // namespace dot

#endif
