#include "AppIcon.h"

#include <QGuiApplication>
#include <QIcon>
#include <QString>

// Q_INIT_RESOURCE expands to a free function in the global namespace whose
// name encodes the .qrc basename. Forward-declare it here so we can call it
// from the Dot namespace without macro-pasting issues.
extern int qInitResources_dot();

namespace Dot {

void setupApplicationIcon(QGuiApplication & /*app*/) {
    // The .qrc is linked into dot_lib (a static library). Static-lib resources
    // are not auto-initialised in every translation unit, so we must invoke the
    // generated init explicitly before constructing the QIcon.
    qInitResources_dot();
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/dot.ico")));
}

} // namespace Dot
