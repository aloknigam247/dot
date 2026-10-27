#include <QStringList>
#include <QtTest>

#include <array>
#include <string>
#include <vector>

#include "ArgvParser.h"
#include "Config.h"
#include "ConfigCliReader.h"

using namespace dot;

namespace {

// Build a fake argv array from a QStringList. Owns the underlying byte
// storage in `storage` so the returned char* array stays valid for the
// duration of the test scope.
struct FakeArgv {
    std::vector<std::string> storage;
    std::vector<char *> ptrs;

    explicit FakeArgv(const QStringList &tokens) {
        storage.reserve(tokens.size());
        ptrs.reserve(tokens.size());
        for(const QString &t: tokens) {
            storage.push_back(t.toLocal8Bit().toStdString());
            ptrs.push_back(storage.back().data());
        }
    }

    int argc() { return static_cast<int>(ptrs.size()); }
    char **argv() { return ptrs.data(); }
};

} // namespace

class TestArgvParser : public QObject {
    Q_OBJECT

private slots:
    void noArgs() {
        FakeArgv av{ { QStringLiteral("dot") } };
        const auto r = parseArgv(av.argc(), av.argv());
        QVERIFY(r.editorArgs.isEmpty());
        QVERIFY(!r.helpRequested);
        QVERIFY(!r.versionRequested);
    }

    void singleFile() {
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("foo.txt") } };
        const auto r = parseArgv(av.argc(), av.argv());
        QCOMPARE(r.editorArgs, QStringList{ QStringLiteral("foo.txt") });
    }

    void splitOptionMultipleFiles() {
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("-O"), QStringLiteral("a.txt"),
                       QStringLiteral("b.txt") } };
        const auto r = parseArgv(av.argc(), av.argv());
        const QStringList expected{ QStringLiteral("-O"), QStringLiteral("a.txt"),
                                    QStringLiteral("b.txt") };
        QCOMPARE(r.editorArgs, expected);
    }

    void plusLineNumber() {
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("+10"), QStringLiteral("foo.txt") } };
        const auto r = parseArgv(av.argc(), av.argv());
        const QStringList expected{ QStringLiteral("+10"), QStringLiteral("foo.txt") };
        QCOMPARE(r.editorArgs, expected);
    }

    void dashCWithCommand() {
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("-c"), QStringLiteral("set number"),
                       QStringLiteral("foo.txt") } };
        const auto r = parseArgv(av.argc(), av.argv());
        const QStringList expected{ QStringLiteral("-c"), QStringLiteral("set number"),
                                    QStringLiteral("foo.txt") };
        QCOMPARE(r.editorArgs, expected);
    }

    void longHelp() {
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("--help") } };
        const auto r = parseArgv(av.argc(), av.argv());
        QVERIFY(r.helpRequested);
        QVERIFY(r.editorArgs.isEmpty());
    }

    void shortHelp() {
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("-h") } };
        const auto r = parseArgv(av.argc(), av.argv());
        QVERIFY(r.helpRequested);
        QVERIFY(r.editorArgs.isEmpty());
    }

    void longVersion() {
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("--version") } };
        const auto r = parseArgv(av.argc(), av.argv());
        QVERIFY(r.versionRequested);
        QVERIFY(r.editorArgs.isEmpty());
    }

    void shortVersion() {
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("-v") } };
        const auto r = parseArgv(av.argc(), av.argv());
        QVERIFY(r.versionRequested);
        QVERIFY(r.editorArgs.isEmpty());
    }

    void pathWithSpaces() {
        const QString path = QStringLiteral("C:\\Program Files\\foo.txt");
        FakeArgv av{ { QStringLiteral("dot"), path } };
        const auto r = parseArgv(av.argc(), av.argv());
        QCOMPARE(r.editorArgs, QStringList{ path });
    }

    void dotOptionsRetainedForConfig() {
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("--dot-future-option"),
                       QStringLiteral("foo.txt") } };
        const auto r = parseArgv(av.argc(), av.argv());
        const QStringList expected{ QStringLiteral("--dot-future-option"),
                                    QStringLiteral("foo.txt") };
        QCOMPARE(r.editorArgs, expected);
    }

    void dashStdinConsumedAsBuffer() {
        // A lone `-` is claimed by dot (nvim's stdin is the RPC channel) and
        // must not be forwarded.
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("-"), QStringLiteral("foo.txt") } };
        const auto r = parseArgv(av.argc(), av.argv());
        QVERIFY(r.stdinAsBuffer);
        QCOMPARE(r.editorArgs, QStringList{ QStringLiteral("foo.txt") });
    }

    // PowerShell 7 leaves outer double-quotes on argv entries containing `.`,
    // so `.\tasks.md` arrives as `".\tasks.md"`. Strip the matching pair so
    // the file path forwarded to nvim is the bare path, not a quoted literal.
    void stripsOuterQuotesFromQuotedPath() {
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("\".\\tasks.md\"") } };
        const auto r = parseArgv(av.argc(), av.argv());
        QCOMPARE(r.editorArgs, QStringList{ QStringLiteral(".\\tasks.md") });
    }

    // pwsh's worst case: it crushes ALL args into a single quoted token like
    //   ".\tasks.md" "-O" ".\CLAUDE.md"
    // Detect the `" "` separator pattern and split back into the original args.
    void splitsPwshMangledMultiArg() {
        FakeArgv av{ { QStringLiteral("dot"),
                       QStringLiteral("\".\\tasks.md\" \"-O\" \".\\CLAUDE.md\"") } };
        const auto r = parseArgv(av.argc(), av.argv());
        const QStringList expected{ QStringLiteral(".\\tasks.md"), QStringLiteral("-O"),
                                    QStringLiteral(".\\CLAUDE.md") };
        QCOMPARE(r.editorArgs, expected);
    }

    void splitsPwshMangledWithDashCAndSpaceyValue() {
        // `dot -c "set number" foo.txt` arriving from pwsh as one crushed arg.
        FakeArgv av{ { QStringLiteral("dot"),
                       QStringLiteral("\"-c\" \"set number\" \"foo.txt\"") } };
        const auto r = parseArgv(av.argc(), av.argv());
        const QStringList expected{ QStringLiteral("-c"), QStringLiteral("set number"),
                                    QStringLiteral("foo.txt") };
        QCOMPARE(r.editorArgs, expected);
    }

    void leavesUnmatchedQuoteAlone() {
        // Single trailing quote — not a wrapping pair, leave as-is so we
        // don't mangle legitimate filenames containing a quote.
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("weird\"name.txt") } };
        const auto r = parseArgv(av.argc(), av.argv());
        QCOMPARE(r.editorArgs, QStringList{ QStringLiteral("weird\"name.txt") });
    }

    void preservesSingleQuotedPathWithSpace() {
        // Legit case: a path with a space inside, quoted properly so it
        // arrives as `"C:\Program Files\foo.txt"`. Outer quotes strip; no
        // `" "` inside the inner string, so we don't try to split.
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("\"C:\\Program Files\\foo.txt\"") } };
        const auto r = parseArgv(av.argc(), av.argv());
        QCOMPARE(r.editorArgs, QStringList{ QStringLiteral("C:\\Program Files\\foo.txt") });
    }

    void configOptionsReachRegistryBeforeForwarding() {
        Config cfg;
        cfg.registerOption(QStringLiteral("fallback"), ConfigType::StringList, QStringList{});
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("--dot-fallback=FirstFont,SecondFont"),
                       QStringLiteral("--dot-unknown=value"), QStringLiteral("foo.txt") } };
        const auto parsed = parseArgv(av.argc(), av.argv());
        QStringList forwarded = parsed.editorArgs;
        ConfigCliReader::extract(forwarded, cfg);
        const QStringList expected{ QStringLiteral("FirstFont"), QStringLiteral("SecondFont") };
        QCOMPARE(cfg.value(QStringLiteral("fallback")).toStringList(), expected);
        QCOMPARE(forwarded, QStringList{ QStringLiteral("foo.txt") });

        cfg.setFromGGlobal(QStringLiteral("fallback"),
                           QStringList{ QStringLiteral("DifferentFont") });
        QCOMPARE(cfg.value(QStringLiteral("fallback")).toStringList(), expected);
    }

    void unrelatedPrefixIsNotConsumedOrApplied() {
        Config cfg;
        cfg.registerOption(QStringLiteral("padding"), ConfigType::Int, 0);
        FakeArgv av{ { QStringLiteral("dot"), QStringLiteral("--previous-app-padding=9"),
                       QStringLiteral("foo.txt") } };
        const auto parsed = parseArgv(av.argc(), av.argv());
        QStringList forwarded = parsed.editorArgs;
        ConfigCliReader::extract(forwarded, cfg);
        const QStringList expected{ QStringLiteral("--previous-app-padding=9"),
                                    QStringLiteral("foo.txt") };
        QCOMPARE(forwarded, expected);
        QCOMPARE(cfg.value(QStringLiteral("padding")).toInt(), 0);
    }

    void acceptsConstArguments() {
        const std::array args{ "dot", "--help", "file.txt" };
        const auto result = parseArgv(static_cast<int>(args.size()), args.data());
        QVERIFY(result.helpRequested);
        QCOMPARE(result.editorArgs, QStringList{ QStringLiteral("file.txt") });
    }
};

QTEST_GUILESS_MAIN(TestArgvParser)
#include "test_argv_parser.moc"
