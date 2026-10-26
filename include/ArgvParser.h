#ifndef ARGVPARSER_H
#define ARGVPARSER_H

#include <QStringList>

namespace dot {

struct DotArgs {
    QStringList editorArgs;
    bool helpRequested = false;
    bool versionRequested = false;
    // `nvim -` reads the file content from stdin. With dot, nvim --embed
    // already owns its own stdin (the RPC pipe), so the `-` token is consumed
    // here: main.cpp reads dot's stdin and pushes the bytes into the first
    // buffer over RPC after attach.
    bool stdinAsBuffer = false;
};

// Consumes help, version and stdin flags, and normalizes the remaining argv.
// ConfigCliReader must extract dot options before editorArgs reaches nvim.
DotArgs parseArgv(int argc, char **argv);

} // namespace dot

#endif
