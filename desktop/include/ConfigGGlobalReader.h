#ifndef CONFIGGGLOBALREADER_H
#define CONFIGGGLOBALREADER_H

#include <QHash>
#include <QString>
#include <QVariant>

namespace dot {

class Config;
class NvimConnector;

// Replaces the g-global source only after a complete successful snapshot.
// Missing globals clear their source; stale or failed reads leave Config unchanged.
class ConfigGGlobalReader {
public:
    static void read(NvimConnector &connector, Config &cfg);
    static void readFromMap(const QHash<QString, QVariant> &vars, Config &cfg);
};

} // namespace dot

#endif
