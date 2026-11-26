#pragma once

#include <private/qhighdpiscaling_p.h>
#include <qpa/qwindowsysteminterface.h>
#include <QWindow>

namespace dot::test {

inline void deliverPlatformGeometry(QWindow *window, const QRect &rect) {
    window->setGeometry(rect);
    QWindowSystemInterface::handleGeometryChange<QWindowSystemInterface::SynchronousDelivery>(
        window, QHighDpi::toNativeWindowGeometry(rect, window));
}

} // namespace dot::test
