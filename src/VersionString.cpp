#include "VersionString.h"

#include "Version.h"

namespace Dot {

QString versionString() { return QStringLiteral("dot ") + QString::fromLatin1(DOT_VERSION); }

} // namespace Dot
