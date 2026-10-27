#ifndef CONFIGCLIREADER_H
#define CONFIGCLIREADER_H

#include <QStringList>

namespace dot {

class Config;

// Extracts `--dot-<name>[=<value>]` arguments from `args`.
//
// Side effects:
//   * For every CLI-eligible registered option, the value is parsed and
//     applied via Config::setFromCli.
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
// `rounded_highlights` is Vim-only and follows this unknown-option path.
class ConfigCliReader {
public:
    static QStringList extract(QStringList &args, Config &cfg);
};

} // namespace dot

#endif
