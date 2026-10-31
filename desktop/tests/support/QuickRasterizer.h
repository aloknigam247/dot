#pragma once

// Render the real scene graph without a native window, so desktop size cannot
// constrain pixel probes. Select the software backend before constructing one.

#include <QDebug>
#include <QImage>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>

namespace dot {

class QuickRasterizer {
public:
    static void useSoftwareBackend() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    QImage render(QQuickItem *item) {
        const int w = static_cast<int>(item->width());
        const int h = static_cast<int>(item->height());
        item->setParentItem(m_window.contentItem());
        m_window.resize(w, h);
        const qreal dpr = m_window.devicePixelRatio();
        const QSize size(qRound(w * dpr), qRound(h * dpr));
        if(m_image.isNull() || m_image.size() != size ||
           !qFuzzyCompare(m_image.devicePixelRatio(), dpr)) {
            m_image = QImage(size, QImage::Format_ARGB32_Premultiplied);
            if(m_image.isNull()) {
                qWarning() << "Could not allocate the scene graph capture" << size;
                return {};
            }
            m_image.setDevicePixelRatio(dpr);
            m_image.fill(Qt::transparent);
            QQuickRenderTarget target = QQuickRenderTarget::fromPaintDevice(&m_image);
            target.setDevicePixelRatio(dpr);
            m_window.setRenderTarget(target);
        }
        m_control.polishItems();
        m_control.sync();
        m_control.render();
        return m_image;
    }

    // True when one rendered pixel equals one logical pixel. Pixel-probe tests
    // address the image in logical coordinates and goldens are stored at 1:1,
    // so a scaled surface must fail the test rather than silently shift every
    // probe. CTest pins QT_ENABLE_HIGHDPI_SCALING=0 for those targets.
    bool isUnscaled() const {
        return qFuzzyCompare(m_window.effectiveDevicePixelRatio(), qreal(1));
    }

    QQuickWindow *window() { return &m_window; }

private:
    // The paint device must outlive the scene graph.
    QImage m_image;
    QQuickRenderControl m_control;
    QQuickWindow m_window{ &m_control };
};

} // namespace dot
