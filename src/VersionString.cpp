#include "VersionString.h"

#include "Version.h"

namespace dot {

QString versionString() { return QStringLiteral("dot ") + QString::fromLatin1(DOT_VERSION); }

} // namespace dot
