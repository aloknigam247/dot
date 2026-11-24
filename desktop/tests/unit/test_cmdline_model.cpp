// Tier-1 tests for command-line redraw decoding. Events go through the production
// NvimConnector redraw parser into its owned CmdlineModel without starting Neovim.

#include <QAbstractItemModelTester>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QtTest>

#include "CmdlineModel.h"
#include "NvimConnector.h"

#include <msgpack.hpp>

#include <string>
#include <tuple>
#include <vector>

namespace dot {

namespace {

using Chunk = std::tuple<int, std::string, int>;
using Line = std::vector<Chunk>;

const std::string kUnicodeUtf8 = "a\xC3\xA9\xF0\x9F\x98\x80";

QString unicodeText() { return QString(u"a\u00E9\U0001F600"); }

template <typename T>
QByteArray redrawInvocation(const char *name, const T &arguments) {
    msgpack::sbuffer buffer;
    msgpack::pack(buffer, std::make_tuple(std::make_tuple(std::string(name), arguments)));
    return QByteArray(buffer.data(), static_cast<qsizetype>(buffer.size()));
}

template <typename... Args>
QByteArray redrawEvent(const char *name, const Args &...args) {
    return redrawInvocation(name, std::make_tuple(args...));
}

QByteArray showEvent(const Line &content, int position, const std::string &firstCharacter,
                     int level, int indent = 0) {
    return redrawEvent("cmdline_show", content, position, firstCharacter, std::string(), indent,
                       level, -1);
}

QVariantMap chunkMap(int attrId, const QString &text, int highlightId) {
    return {
        { QStringLiteral("attrId"), attrId },
        { QStringLiteral("highlightId"), highlightId },
        { QStringLiteral("text"), text },
    };
}

} // namespace

class TestCmdlineModel : public QObject {
    Q_OBJECT

private:
    static void dispatch(NvimConnector &conn, const QByteArray &batch) {
        const msgpack::object_handle handle = msgpack::unpack(batch.constData(), batch.size());
        conn.handleRedraw(handle.get());
    }

    static QVariant role(NvimConnector &conn, int row, int role) {
        CmdlineModel *model = conn.cmdline();
        return model->data(model->index(row), role);
    }

    static void verifyUnicodeSession(NvimConnector &conn) {
        CmdlineModel *model = conn.cmdline();
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->activeLevel(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::ChunksRole).toList(),
                 QVariantList{ chunkMap(0, unicodeText(), 0) });
        QCOMPARE(role(conn, 0, CmdlineModel::CursorByteOffsetRole).toLongLong(), 7);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 4);
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialCharacterRole).toString(), QString());
        QCOMPARE(model->blockActive(), false);
    }

private slots:
    void init() { QTest::failOnWarning(QRegularExpression(QStringLiteral("^Invalid cmdline_"))); }

    void showDecodesAllFields() {
        NvimConnector conn;
        CmdlineModel *model = conn.cmdline();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy activeLevelSpy(model, &CmdlineModel::activeLevelChanged);

        dispatch(conn, redrawEvent("cmdline_show", Line{ { 54, "1", 313 }, { 69, "+", 282 } }, 2,
                                   std::string("="), std::string("Name: "), 2, 1, 7));

        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->activeLevel(), 1);
        QCOMPARE(activeLevelSpy.count(), 1);
        const QVariantList expectedChunks{ chunkMap(54, QStringLiteral("1"), 313),
                                           chunkMap(69, QStringLiteral("+"), 282) };
        QCOMPARE(role(conn, 0, CmdlineModel::ChunksRole).toList(), expectedChunks);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorByteOffsetRole).toLongLong(), 2);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 2);
        QCOMPARE(role(conn, 0, CmdlineModel::FirstCharacterRole).toString(), QStringLiteral("="));
        QCOMPARE(role(conn, 0, CmdlineModel::HighlightIdRole).toInt(), 7);
        QCOMPARE(role(conn, 0, CmdlineModel::IndentRole).toInt(), 2);
        QCOMPARE(role(conn, 0, CmdlineModel::LevelRole).toInt(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QStringLiteral("Name: "));
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialCharacterRole).toString(), QString());
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialShiftRole).toBool(), false);
        QSignalSpy dataSpy(model, &CmdlineModel::dataChanged);
        QSignalSpy insertSpy(model, &CmdlineModel::rowsInserted);

        dispatch(conn, redrawEvent("cmdline_show", Line{ { 0, "", 0 } }, 0, std::string(":"), false,
                                   0, 1, -1));
        QCOMPARE(insertSpy.count(), 0);
        QCOMPARE(dataSpy.count(), 1);
        QCOMPARE(dataSpy.at(0).at(0).toModelIndex(), model->index(0));
        QCOMPARE(dataSpy.at(0).at(1).toModelIndex(), model->index(0));
        QCOMPARE(dataSpy.at(0).at(2).value<QList<int>>(), QList<int>());
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::ChunksRole).toList(),
                 QVariantList{ chunkMap(0, QString(), 0) });
        QCOMPARE(role(conn, 0, CmdlineModel::HighlightIdRole).toInt(), -1);
        QCOMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QString());
        QCOMPARE(activeLevelSpy.count(), 1);
    }

    void unicodeCursorPreservesByteAndTextDomains() {
        NvimConnector conn;
        dispatch(conn, showEvent(Line{ { 0, kUnicodeUtf8, 0 } }, 7, ":", 1));
        verifyUnicodeSession(conn);

        const std::vector<std::pair<int, int>> boundaries{ { 0, 0 }, { 1, 1 }, { 3, 2 }, { 7, 4 } };
        for(const auto &[byteOffset, textOffset]: boundaries) {
            dispatch(conn, redrawEvent("cmdline_pos", byteOffset, 1));
            QCOMPARE(role(conn, 0, CmdlineModel::CursorByteOffsetRole).toLongLong(), byteOffset);
            QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), textOffset);
        }

        QTest::ignoreMessage(
            QtWarningMsg, "Invalid cmdline_pos: cursor byte offset 2 is inside a UTF-8 sequence");
        dispatch(conn, redrawEvent("cmdline_pos", 2, 1));
        QTest::ignoreMessage(
            QtWarningMsg, "Invalid cmdline_pos: cursor byte offset 8 is outside 7 content bytes");
        dispatch(conn, redrawEvent("cmdline_pos", 8, 1));
        verifyUnicodeSession(conn);
    }

    void invalidUtf8DecodesEachByteAsReplacement() {
        NvimConnector conn;
        dispatch(conn, showEvent(Line{ { 0, std::string("\xE0\x80\x80") + "a", 0 } }, 2, ":", 1));

        const QString replacement(QChar::ReplacementCharacter);
        QCOMPARE(role(conn, 0, CmdlineModel::ChunksRole).toList(),
                 QVariantList{ chunkMap(0, replacement.repeated(3) + u'a', 0) });
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 2);
        dispatch(conn, redrawEvent("cmdline_pos", 4, 1));
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 4);
    }

    void positionOnlyKeepsChunks() {
        NvimConnector conn;
        CmdlineModel *model = conn.cmdline();
        dispatch(conn, showEvent(Line{ { 0, "ab", 0 }, { 1, "cd", 2 } }, 4, ":", 1));
        const QVariant chunks = role(conn, 0, CmdlineModel::ChunksRole);
        QSignalSpy dataSpy(model, &CmdlineModel::dataChanged);
        QSignalSpy insertSpy(model, &CmdlineModel::rowsInserted);

        dispatch(conn, redrawEvent("cmdline_pos", 3, 1));

        QCOMPARE(dataSpy.count(), 1);
        const QList<int> cursorRoles{ CmdlineModel::CursorByteOffsetRole,
                                      CmdlineModel::CursorTextOffsetRole };
        QCOMPARE(dataSpy.at(0).at(2).value<QList<int>>(), cursorRoles);
        QCOMPARE(insertSpy.count(), 0);
        QCOMPARE(role(conn, 0, CmdlineModel::ChunksRole), chunks);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorByteOffsetRole).toLongLong(), 3);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 3);

        dispatch(conn, redrawEvent("cmdline_pos", 3, 1));
        QCOMPARE(dataSpy.count(), 1);
    }

    void nestedPromptRestoresOuterLevel() {
        NvimConnector conn;
        CmdlineModel *model = conn.cmdline();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy hiddenSpy(model, &CmdlineModel::cmdlineHidden);

        // Neovim sends the inner level before redrawing the outer one.
        dispatch(conn, showEvent(Line{ { 0, "1+1", 0 } }, 3, "=", 2));
        dispatch(conn, showEvent(Line{ { 0, "abc", 0 } }, 3, ":", 1));

        QCOMPARE(model->rowCount(), 2);
        QCOMPARE(model->activeLevel(), 2);
        QCOMPARE(role(conn, 0, CmdlineModel::LevelRole).toInt(), 1);
        QCOMPARE(role(conn, 1, CmdlineModel::LevelRole).toInt(), 2);
        QCOMPARE(role(conn, 1, CmdlineModel::FirstCharacterRole).toString(), QStringLiteral("="));

        dispatch(conn, redrawEvent("cmdline_hide", 2, false));

        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->activeLevel(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::ChunksRole).toList(),
                 QVariantList{ chunkMap(0, QStringLiteral("abc"), 0) });
        QCOMPARE(role(conn, 0, CmdlineModel::FirstCharacterRole).toString(), QStringLiteral(":"));
        QCOMPARE(hiddenSpy.count(), 1);
        QCOMPARE(hiddenSpy.at(0), QVariantList({ 2, false }));
    }

    void specialCharacterClearedByShow() {
        NvimConnector conn;
        CmdlineModel *model = conn.cmdline();
        dispatch(conn, showEvent(Line{ { 0, "", 0 } }, 0, ":", 1));
        QSignalSpy dataSpy(model, &CmdlineModel::dataChanged);

        dispatch(conn, redrawEvent("cmdline_special_char", std::string("\""), true, 1));

        QCOMPARE(role(conn, 0, CmdlineModel::SpecialCharacterRole).toString(),
                 QStringLiteral("\""));
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialShiftRole).toBool(), true);
        QCOMPARE(dataSpy.count(), 1);
        const QList<int> specialRoles{ CmdlineModel::SpecialCharacterRole,
                                       CmdlineModel::SpecialShiftRole };
        QCOMPARE(dataSpy.at(0).at(2).value<QList<int>>(), specialRoles);

        dispatch(conn, showEvent(Line{ { 0, "2", 0 } }, 1, ":", 1));

        QCOMPARE(role(conn, 0, CmdlineModel::SpecialCharacterRole).toString(), QString());
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialShiftRole).toBool(), false);
    }

    void hideReportsAbort() {
        NvimConnector conn;
        CmdlineModel *model = conn.cmdline();
        QSignalSpy activeLevelSpy(model, &CmdlineModel::activeLevelChanged);
        QSignalSpy hiddenSpy(model, &CmdlineModel::cmdlineHidden);
        dispatch(conn, showEvent(Line{ { 0, "abc", 0 } }, 3, ":", 1));

        dispatch(conn, redrawEvent("cmdline_hide", 3, true));
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(hiddenSpy.count(), 0);

        dispatch(conn, redrawEvent("cmdline_hide", 1, true));

        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(model->activeLevel(), 0);
        QCOMPARE(activeLevelSpy.count(), 2);
        QCOMPARE(hiddenSpy.count(), 1);
        QCOMPARE(hiddenSpy.at(0), QVariantList({ 1, true }));
    }

    void blockAppendHideLifecycle() {
        NvimConnector conn;
        CmdlineModel *model = conn.cmdline();
        QSignalSpy blockSpy(model, &CmdlineModel::blockChanged);

        QTest::ignoreMessage(QtWarningMsg, "Invalid cmdline_block_append: no block is visible");
        dispatch(conn, redrawEvent("cmdline_block_append", Line{ { 0, "orphan", 0 } }));
        QCOMPARE(model->blockActive(), false);
        QCOMPARE(blockSpy.count(), 0);

        dispatch(conn, redrawEvent("cmdline_block_show",
                                   std::vector<Line>{ Line{ { 0, "function! Foo()", 0 } } }));
        dispatch(conn, showEvent(Line{ { 0, "", 0 } }, 0, ":", 1, 2));
        dispatch(conn, redrawEvent("cmdline_hide", 1, false));
        dispatch(conn, redrawEvent("cmdline_block_append",
                                   Line{ { 0, "  let x", 0 }, { 5, " = 1", 9 } }));

        QCOMPARE(model->blockActive(), true);
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(blockSpy.count(), 2);
        const QVariantList expectedBlock{
            QVariantList{ chunkMap(0, QStringLiteral("function! Foo()"), 0) },
            QVariantList{ chunkMap(0, QStringLiteral("  let x"), 0),
                          chunkMap(5, QStringLiteral(" = 1"), 9) },
        };
        QCOMPARE(model->blockLines(), expectedBlock);

        dispatch(conn, redrawEvent("cmdline_block_hide"));

        QCOMPARE(model->blockActive(), false);
        QCOMPARE(model->blockLines(), QVariantList());
        QCOMPARE(blockSpy.count(), 3);
    }

    void malformedEventsKeepLastValidState_data() {
        QTest::addColumn<QByteArray>("batch");
        QTest::addColumn<QString>("warning");

        const QString showShape = QStringLiteral(
            "Invalid cmdline_show: expected content, pos, firstc, prompt, indent, level, and hl_id");
        const Line valid{ { 0, "x", 0 } };
        QTest::newRow("show arguments not array")
            << redrawInvocation("cmdline_show", 1)
            << QStringLiteral("Invalid cmdline_show: expected an argument array");
        QTest::newRow("show missing hl_id")
            << redrawEvent("cmdline_show", valid, 1, std::string(":"), std::string(), 0, 1)
            << showShape;
        QTest::newRow("show prompt true")
            << redrawEvent("cmdline_show", valid, 1, std::string(":"), true, 0, 1, -1) << showShape;
        QTest::newRow("show chunk missing hl_id")
            << redrawEvent("cmdline_show", std::vector<std::tuple<int, std::string>>{ { 0, "x" } },
                           1, std::string(":"), std::string(), 0, 1, -1)
            << showShape;
        QTest::newRow("show chunk text integer")
            << redrawEvent("cmdline_show", std::vector<std::tuple<int, int, int>>{ { 0, 1, 0 } }, 1,
                           std::string(":"), std::string(), 0, 1, -1)
            << showShape;
        QTest::newRow("show negative indent")
            << redrawEvent("cmdline_show", valid, 1, std::string(":"), std::string(), -1, 1, -1)
            << showShape;
        QTest::newRow("show level zero")
            << showEvent(valid, 1, ":", 0)
            << QStringLiteral("Invalid cmdline_show: level 0 is not positive");
        QTest::newRow("show cursor inside sequence")
            << showEvent(Line{ { 0, kUnicodeUtf8, 0 } }, 2, ":", 1)
            << QStringLiteral(
                   "Invalid cmdline_show: cursor byte offset 2 is inside a UTF-8 sequence");
        QTest::newRow("pos invisible level")
            << redrawEvent("cmdline_pos", 0, 3)
            << QStringLiteral("Invalid cmdline_pos: level 3 is not visible");
        QTest::newRow("pos string offset")
            << redrawEvent("cmdline_pos", std::string("0"), 1)
            << QStringLiteral("Invalid cmdline_pos: expected cursor byte offset and level");
        QTest::newRow("special shift integer")
            << redrawEvent("cmdline_special_char", std::string("^"), 1, 1)
            << QStringLiteral(
                   "Invalid cmdline_special_char: expected character, shift flag, and level");
        QTest::newRow("special invisible level")
            << redrawEvent("cmdline_special_char", std::string("^"), true, 2)
            << QStringLiteral("Invalid cmdline_special_char: level 2 is not visible");
        QTest::newRow("hide missing abort")
            << redrawEvent("cmdline_hide", 1)
            << QStringLiteral("Invalid cmdline_hide: expected level and abort flag");
        QTest::newRow("block show line not array")
            << redrawEvent("cmdline_block_show", std::vector<int>{ 1 })
            << QStringLiteral("Invalid cmdline_block_show: expected an array of chunk lines");
    }

    void malformedEventsKeepLastValidState() {
        QFETCH(QByteArray, batch);
        QFETCH(QString, warning);
        NvimConnector conn;
        dispatch(conn, showEvent(Line{ { 0, kUnicodeUtf8, 0 } }, 7, ":", 1));

        QTest::ignoreMessage(QtWarningMsg, warning.toUtf8().constData());
        dispatch(conn, batch);

        verifyUnicodeSession(conn);
    }

    void resetClearsAllLevels() {
        for(const bool disconnect: { false, true }) {
            NvimConnector conn;
            CmdlineModel *model = conn.cmdline();
            dispatch(conn, showEvent(Line{ { 0, "abc", 0 } }, 3, ":", 1));
            dispatch(conn, showEvent(Line{ { 0, "1", 0 } }, 1, "=", 2));
            dispatch(conn, redrawEvent("cmdline_block_show",
                                       std::vector<Line>{ Line{ { 0, "function! Foo()", 0 } } }));
            QSignalSpy activeLevelSpy(model, &CmdlineModel::activeLevelChanged);
            QSignalSpy blockSpy(model, &CmdlineModel::blockChanged);
            QSignalSpy resetSpy(model, &CmdlineModel::modelReset);

            if(disconnect) conn.onRpcDisconnected();
            else conn.resetUiState();

            QCOMPARE(model->rowCount(), 0);
            QCOMPARE(model->activeLevel(), 0);
            QCOMPARE(model->blockActive(), false);
            QCOMPARE(model->blockLines(), QVariantList());
            QCOMPARE(activeLevelSpy.count(), 1);
            QCOMPARE(blockSpy.count(), 1);
            QCOMPARE(resetSpy.count(), 1);
        }
    }
};

} // namespace dot

QTEST_MAIN(dot::TestCmdlineModel)
#include "test_cmdline_model.moc"
