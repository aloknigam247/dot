#ifndef CONFIGCLIREADER_H
#define CONFIGCLIREADER_H

#include <QStringList>

namespace Dot {

class Config;

// Extracts `--dot-<name>[=<value>]` arguments from `args`.
//
// Side effects:
//   * For every recognised option (registered in `cfg` with a matching type),
//     the value is parsed and applied via Config::setFromCli.
//   * The corresponding entry is removed from `args` so the remainder can be
//     forwarded to nvim verbatim.
//
// Forms accepted:
//   --dot-name=value   — value parsed per registered ConfigType
//   --dot-name         — Bool only; equivalent to value=true
//
// Bool values accept: 1/0, true/false, yes/no, on/off (case-insensitive).
// StringList values are comma-separated.
//
// Unknown dot options are logged via qDebug and removed from args.
class ConfigCliReader {
public:
    static QStringList extract(QStringList &args, Config &cfg);
};

} // namespace Dot

#endif
