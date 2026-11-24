// Tier-2 tests for command-line state produced by a real Neovim. Tests enable ext_cmdline on their
// own UI channel because production attaches keep the grid-rendered command line.

#include "CmdlineModel.h"
#include "IntegrationHelpers.h"

#include <QRegularExpression>
#include <QtTest>

namespace dot {

using namespace dot::test;

class TestCmdline : public QObject {
    Q_OBJECT

private:
    static bool enableExtCmdline(NvimConnector &conn) {
        conn.m_rpc->notify(QStringLiteral("nvim_ui_set_option"),
                           [](msgpack::packer<msgpack::sbuffer> &pk) {
            pk.pack_array(2);
            pk.pack(std::string("ext_cmdline"));
            pk.pack(true);
        });
        const auto extCmdline =
            evalSync(conn, QStringLiteral("get(nvim_list_uis()[0], 'ext_cmdline', v:false)"), 5000);
        return extCmdline && extCmdline->toBool();
    }

    static bool attach(NvimConnector &conn) {
        if(!startTestNvim(conn) || !conn.attachUi(80, 24) || !waitForAttach(&conn)) return false;
        return enableExtCmdline(conn);
    }

    static QVariant role(NvimConnector &conn, int row, int role) {
        CmdlineModel *model = conn.cmdline();
        return model->data(model->index(row), role);
    }

    static QString text(NvimConnector &conn, int row) {
        QString result;
        const QVariantList chunks = role(conn, row, CmdlineModel::ChunksRole).toList();
        for(const QVariant &chunk: chunks)
            result += chunk.toMap().value(QStringLiteral("text")).toString();
        return result;
    }

private slots:
    void init() { QTest::failOnWarning(QRegularExpression(QStringLiteral("^Invalid cmdline_"))); }

    void colonPromptTracksUnicodeCursor() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();
        QSignalSpy hiddenSpy(model, &CmdlineModel::cmdlineHidden);
        const QString unicode(u"a\u00E9\U0001F600");

        conn.input(QStringLiteral(":") + unicode);

        QTRY_COMPARE(model->rowCount(), 1);
        QTRY_COMPARE(role(conn, 0, CmdlineModel::CursorByteOffsetRole).toLongLong(), 7);
        const QVariantList chunks{ QVariantMap{
            { QStringLiteral("attrId"), 0 },
            { QStringLiteral("highlightId"), 0 },
            { QStringLiteral("text"), unicode },
        } };
        QCOMPARE(model->activeLevel(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::ChunksRole).toList(), chunks);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 4);
        QCOMPARE(role(conn, 0, CmdlineModel::FirstCharacterRole).toString(), QStringLiteral(":"));
        QCOMPARE(role(conn, 0, CmdlineModel::HighlightIdRole).toInt(), -1);
        QCOMPARE(role(conn, 0, CmdlineModel::IndentRole).toInt(), 0);
        QCOMPARE(role(conn, 0, CmdlineModel::LevelRole).toInt(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QString());

        conn.input(QStringLiteral("<Left>"));

        QTRY_COMPARE(role(conn, 0, CmdlineModel::CursorByteOffsetRole).toLongLong(), 3);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 2);
        QCOMPARE(role(conn, 0, CmdlineModel::ChunksRole).toList(), chunks);

        conn.input(QStringLiteral("<Esc>"));

        QTRY_COMPARE(model->rowCount(), 0);
        QCOMPARE(model->activeLevel(), 0);
        QCOMPARE(hiddenSpy.count(), 1);
        QCOMPARE(hiddenSpy.at(0), QVariantList({ 1, true }));
    }

    void searchPromptsReportFirstCharacter() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();

        for(const QString &firstCharacter: { QStringLiteral("/"), QStringLiteral("?") }) {
            conn.input(firstCharacter + QStringLiteral("foo"));

            QTRY_COMPARE(text(conn, 0), QStringLiteral("foo"));
            QCOMPARE(model->rowCount(), 1);
            QCOMPARE(role(conn, 0, CmdlineModel::CursorByteOffsetRole).toLongLong(), 3);
            QCOMPARE(role(conn, 0, CmdlineModel::FirstCharacterRole).toString(), firstCharacter);
            QCOMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QString());

            conn.input(QStringLiteral("<Esc>"));
            QTRY_COMPARE(model->rowCount(), 0);
        }
    }

    void typeaheadHideOfUnshownLevelIsIgnored() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();
        QSignalSpy hiddenSpy(model, &CmdlineModel::cmdlineHidden);

        // Typeahead lets Neovim leave the command line before it redraws, so it hides an unshown
        // level.
        conn.input(QStringLiteral(":<Esc>"));
        conn.input(QStringLiteral(":abc"));

        QTRY_COMPARE(text(conn, 0), QStringLiteral("abc"));
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->activeLevel(), 1);
        QCOMPARE(hiddenSpy.count(), 0);
    }

    void inputPromptReportsPrompt() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();
        QSignalSpy hiddenSpy(model, &CmdlineModel::cmdlineHidden);

        conn.input(QStringLiteral(":call input('Name: ')<CR>"));

        QTRY_COMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QStringLiteral("Name: "));
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorByteOffsetRole).toLongLong(), 0);
        QCOMPARE(role(conn, 0, CmdlineModel::FirstCharacterRole).toString(), QString());
        QCOMPARE(role(conn, 0, CmdlineModel::HighlightIdRole).toInt(), 0);
        QCOMPARE(role(conn, 0, CmdlineModel::LevelRole).toInt(), 1);
        QCOMPARE(text(conn, 0), QString());
        QCOMPARE(hiddenSpy.count(), 1);
        QCOMPARE(hiddenSpy.at(0), QVariantList({ 1, false }));

        conn.input(QStringLiteral("x"));

        QTRY_COMPARE(text(conn, 0), QStringLiteral("x"));
        QCOMPARE(role(conn, 0, CmdlineModel::CursorByteOffsetRole).toLongLong(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QStringLiteral("Name: "));

        conn.input(QStringLiteral("<Esc>"));

        QTRY_COMPARE(model->rowCount(), 0);
        QCOMPARE(hiddenSpy.count(), 2);
        QCOMPARE(hiddenSpy.at(1), QVariantList({ 1, true }));
    }

    void nestedExpressionRestoresOuterLevel() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();
        QSignalSpy hiddenSpy(model, &CmdlineModel::cmdlineHidden);

        conn.input(QStringLiteral(":<C-r>=1+1"));

        QTRY_COMPARE(text(conn, 1), QStringLiteral("1+1"));
        QTRY_COMPARE(role(conn, 0, CmdlineModel::SpecialCharacterRole).toString(),
                     QStringLiteral("\""));
        QCOMPARE(model->rowCount(), 2);
        QCOMPARE(model->activeLevel(), 2);
        QCOMPARE(role(conn, 0, CmdlineModel::FirstCharacterRole).toString(), QStringLiteral(":"));
        QCOMPARE(role(conn, 0, CmdlineModel::LevelRole).toInt(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialShiftRole).toBool(), true);
        QCOMPARE(role(conn, 1, CmdlineModel::CursorByteOffsetRole).toLongLong(), 3);
        QCOMPARE(role(conn, 1, CmdlineModel::FirstCharacterRole).toString(), QStringLiteral("="));
        QCOMPARE(role(conn, 1, CmdlineModel::LevelRole).toInt(), 2);
        const QVariantList chunks = role(conn, 1, CmdlineModel::ChunksRole).toList();
        QCOMPARE(chunks.size(), 3);
        const int numberHighlight =
            chunks.constFirst().toMap().value(QStringLiteral("highlightId")).toInt();

        conn.input(QStringLiteral("<CR>"));

        QTRY_COMPARE(model->rowCount(), 1);
        QTRY_COMPARE(text(conn, 0), QStringLiteral("2"));
        QCOMPARE(model->activeLevel(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorByteOffsetRole).toLongLong(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialCharacterRole).toString(), QString());
        QCOMPARE(hiddenSpy.count(), 1);
        QCOMPARE(hiddenSpy.at(0), QVariantList({ 2, false }));

        conn.input(QStringLiteral("<Esc>"));
        QTRY_COMPARE(model->rowCount(), 0);

        const auto highlightName =
            evalSync(conn, QStringLiteral("synIDattr(%1, 'name')").arg(numberHighlight));
        QVERIFY(highlightName);
        QCOMPARE(highlightName->toString(), QStringLiteral("NvimNumber"));
    }

    void literalInsertReportsSpecialCharacter() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();

        conn.input(QStringLiteral(":<C-v>"));

        QTRY_COMPARE(role(conn, 0, CmdlineModel::SpecialCharacterRole).toString(),
                     QStringLiteral("^"));
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialShiftRole).toBool(), true);

        conn.input(QStringLiteral("a"));

        QTRY_COMPARE(text(conn, 0), QStringLiteral("a"));
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialCharacterRole).toString(), QString());
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialShiftRole).toBool(), false);

        conn.input(QStringLiteral("<Esc>"));
        QTRY_COMPARE(model->rowCount(), 0);
    }

    void functionDefinitionPublishesBlock() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();
        const auto line = [](const QString &text) {
            return QVariantList{ QVariantMap{
                { QStringLiteral("attrId"), 0 },
                { QStringLiteral("highlightId"), 0 },
                { QStringLiteral("text"), text },
            } };
        };

        conn.input(QStringLiteral(":function! DotCmdlineBlock()<CR>"));

        QTRY_COMPARE(model->blockActive(), true);
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(model->blockLines().size(), 1);
        QCOMPARE(model->blockLines().at(0).toList(),
                 line(QStringLiteral("function! DotCmdlineBlock()")));
        QCOMPARE(role(conn, 0, CmdlineModel::IndentRole).toInt(), 2);
        QCOMPARE(text(conn, 0), QString());

        conn.input(QStringLiteral("let x = 1<CR>"));

        QTRY_COMPARE(model->blockLines().size(), 2);
        QCOMPARE(model->blockLines().at(1), QVariant(line(QStringLiteral("  let x = 1"))));

        conn.input(QStringLiteral("endfunction<CR>"));

        QTRY_COMPARE(model->blockActive(), false);
        QCOMPARE(model->blockLines(), QVariantList());
        QCOMPARE(model->rowCount(), 0);
        const auto defined = evalSync(conn, QStringLiteral("exists('*DotCmdlineBlock')"));
        QVERIFY(defined);
        QCOMPARE(defined->toInt(), 1);
    }

    void disconnectClearsLiveSession() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();
        QSignalSpy disconnectedSpy(&conn, &NvimConnector::disconnected);
        QSignalSpy hiddenSpy(model, &CmdlineModel::cmdlineHidden);

        conn.input(QStringLiteral(":abc"));
        QTRY_COMPARE(text(conn, 0), QStringLiteral("abc"));

        // Neovim exits without hiding the command line, so only the connector can clear it.
        conn.command(QStringLiteral("qall!"));

        QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 10000);
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(model->activeLevel(), 0);
        QCOMPARE(hiddenSpy.count(), 0);
        QVERIFY(!conn.attached());
    }

    void restartDoesNotResurrectSession() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        const auto apiLevel = evalSync(conn, QStringLiteral("api_info().version.api_level"));
        QVERIFY(apiLevel);
        if(apiLevel->toInt() < 14) QSKIP("nvim API level < 14: :restart UI event unavailable");
        const auto oldPid = evalSync(conn, QStringLiteral("getpid()"));
        QVERIFY(oldPid && oldPid->toInt() > 0);
        CmdlineModel *model = conn.cmdline();
        QSignalSpy attachSpy(&conn, &NvimConnector::attachComplete);
        QSignalSpy disconnectedSpy(&conn, &NvimConnector::disconnected);

        conn.input(QStringLiteral(":abc"));
        QTRY_COMPARE(text(conn, 0), QStringLiteral("abc"));

        conn.command(QStringLiteral("restart!"));

        QTRY_COMPARE_WITH_TIMEOUT(attachSpy.count(), 1, 20000);
        const auto newPid = evalSync(conn, QStringLiteral("getpid()"));
        QVERIFY(newPid && newPid->toInt() > 0 && newPid->toInt() != oldPid->toInt());
        QVERIFY(conn.attached());
        QCOMPARE(disconnectedSpy.count(), 0);
        QCOMPARE(model->rowCount(), 0);
        QVERIFY(enableExtCmdline(conn));

        conn.input(QStringLiteral(":xyz"));

        QTRY_COMPARE(text(conn, 0), QStringLiteral("xyz"));
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->activeLevel(), 1);

        conn.input(QStringLiteral("<Esc>"));
        QTRY_COMPARE(model->rowCount(), 0);
    }
};

} // namespace dot

QTEST_GUILESS_MAIN(dot::TestCmdline)
#include "test_cmdline.moc"
