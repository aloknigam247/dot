// Tier-2 tests for command-line state produced by a real Neovim.

#include "CmdlineModel.h"
#include "IntegrationHelpers.h"

#include <QColor>
#include <QRegularExpression>
#include <QtTest>

namespace dot {

using namespace dot::test;

class TestCmdline : public QObject {
    Q_OBJECT

private:
    static std::optional<bool> externalCmdline(NvimConnector &conn) {
        const auto extCmdline = evalSync(
            conn, QStringLiteral("get(nvim_list_uis()[0], 'ext_cmdline', v:false)"), 20000);
        return extCmdline ? std::optional(extCmdline->toBool()) : std::nullopt;
    }

    static bool attach(NvimConnector &conn) {
        conn.setExtCmdline(true);
        if(!startTestNvim(conn) || !conn.attachUi(80, 24) || !waitForAttach(&conn)) return false;
        return externalCmdline(conn) == true;
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

    void runtimeToggleSwitchesCmdlineOwner() {
        NvimConnector conn;
        QVERIFY(startTestNvim(conn) && conn.attachUi(80, 24) && waitForAttach(&conn));
        QVERIFY(externalCmdline(conn) == false);
        CmdlineModel *model = conn.cmdline();

        conn.setExtCmdline(true);
        QTRY_VERIFY(externalCmdline(conn) == true);
        conn.input(QStringLiteral(":abc"));
        QTRY_COMPARE(text(conn, 0), QStringLiteral("abc"));
        QVERIFY(!conn.grid()->dumpAscii().contains(QStringLiteral(":abc")));

        conn.setExtCmdline(false);
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(model->activeLevel(), 0);
        QTRY_VERIFY(externalCmdline(conn) == false);
        conn.input(QStringLiteral("d"));
        QTRY_COMPARE(evalSync(conn, QStringLiteral("getcmdline()")),
                     std::optional<QVariant>(QStringLiteral("abcd")));
        QTRY_VERIFY(conn.grid()->dumpAscii().contains(QStringLiteral(":abcd")));
        conn.input(QStringLiteral("<Esc>"));
    }

    void togglingDuringPendingAttachReachesNeovim() {
        NvimConnector conn;
        QVERIFY(startTestNvim(conn) && conn.attachUi(80, 24));
        conn.setExtCmdline(true);
        QVERIFY(waitForAttach(&conn));
        QTRY_VERIFY(externalCmdline(conn) == true);
        conn.input(QStringLiteral(":abc"));
        QTRY_COMPARE(text(conn, 0), QStringLiteral("abc"));
        conn.input(QStringLiteral("<Esc>"));
    }

    void enablingDuringPromptDropsGridPromptIndent() {
        NvimConnector conn;
        QVERIFY(startTestNvim(conn) && conn.attachUi(80, 24) && waitForAttach(&conn));
        conn.input(QStringLiteral(":call input('Name: ')<CR>x"));
        QTRY_COMPARE(evalSync(conn, QStringLiteral("getcmdline()")),
                     std::optional<QVariant>(QStringLiteral("x")));

        conn.setExtCmdline(true);

        QTRY_COMPARE(text(conn, 0), QStringLiteral("x"));
        QCOMPARE(role(conn, 0, CmdlineModel::IndentRole).toInt(), 0);
        QCOMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QStringLiteral("Name: "));
        conn.input(QStringLiteral("<Esc>"));
    }

    void inputPromptResolvesHighlightGroup() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();
        const auto promptStyle = [&conn] {
            return role(conn, 0, CmdlineModel::PromptStyleRole).toMap();
        };
        QVERIFY(evalSync(conn, QStringLiteral("execute('hi DotPrompt guifg=#123456 gui=bold')")));

        conn.input(QStringLiteral(":echohl DotPrompt | call input('Name: ') | echohl None<CR>"));

        QTRY_COMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QStringLiteral("Name: "));
        QTRY_COMPARE(promptStyle().value(QStringLiteral("foreground")).value<QColor>(),
                     QColor(0x12, 0x34, 0x56));
        QCOMPARE(promptStyle().value(QStringLiteral("bold")).toBool(), true);
        QVERIFY(!promptStyle().contains(QStringLiteral("background")));

        QVERIFY(evalSync(conn, QStringLiteral("execute('hi DotPrompt guifg=#654321 gui=NONE')")));
        conn.input(QStringLiteral("x"));

        QTRY_COMPARE(promptStyle().value(QStringLiteral("foreground")).value<QColor>(),
                     QColor(0x65, 0x43, 0x21));
        QCOMPARE(promptStyle().value(QStringLiteral("bold")).toBool(), false);
        conn.input(QStringLiteral("<Esc>"));
        QTRY_COMPARE(model->rowCount(), 0);
    }

    void colonPromptTracksUnicodeCursor() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();
        QSignalSpy removedSpy(model, &CmdlineModel::rowsRemoved);
        const QString unicode(u"a\u00E9\U0001F600");

        conn.input(QStringLiteral(":") + unicode);

        QTRY_COMPARE(model->rowCount(), 1);
        QTRY_COMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 4);
        const QVariantList chunks{ QVariantMap{
            { QStringLiteral("attrId"), 0 },
            { QStringLiteral("highlightId"), 0 },
            { QStringLiteral("text"), unicode },
        } };
        QCOMPARE(model->activeLevel(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::ChunksRole).toList(), chunks);
        QCOMPARE(role(conn, 0, CmdlineModel::FirstCharacterRole).toString(), QStringLiteral(":"));
        QCOMPARE(role(conn, 0, CmdlineModel::HighlightIdRole).toInt(), -1);
        QCOMPARE(role(conn, 0, CmdlineModel::IndentRole).toInt(), 0);
        QCOMPARE(role(conn, 0, CmdlineModel::LevelRole).toInt(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QString());

        conn.input(QStringLiteral("<Left>"));

        QTRY_COMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 2);
        QCOMPARE(role(conn, 0, CmdlineModel::ChunksRole).toList(), chunks);

        conn.input(QStringLiteral("<Esc>"));

        QTRY_COMPARE(model->rowCount(), 0);
        QCOMPARE(model->activeLevel(), 0);
        QCOMPARE(removedSpy.count(), 1);
    }

    void searchPromptsReportFirstCharacter() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();

        for(const QString &firstCharacter: { QStringLiteral("/"), QStringLiteral("?") }) {
            conn.input(firstCharacter + QStringLiteral("foo"));

            QTRY_COMPARE(text(conn, 0), QStringLiteral("foo"));
            QCOMPARE(model->rowCount(), 1);
            QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 3);
            QCOMPARE(role(conn, 0, CmdlineModel::FirstCharacterRole).toString(), firstCharacter);
            QCOMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QString());

            conn.input(QStringLiteral("<Esc>"));
            QTRY_COMPARE(model->rowCount(), 0);
        }
    }

    void inputPromptReportsPrompt() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();
        QSignalSpy removedSpy(model, &CmdlineModel::rowsRemoved);

        conn.input(QStringLiteral(":call input('Name: ')<CR>"));

        QTRY_COMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QStringLiteral("Name: "));
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 0);
        QCOMPARE(role(conn, 0, CmdlineModel::FirstCharacterRole).toString(), QString());
        QCOMPARE(role(conn, 0, CmdlineModel::HighlightIdRole).toInt(), 0);
        QCOMPARE(role(conn, 0, CmdlineModel::LevelRole).toInt(), 1);
        QCOMPARE(text(conn, 0), QString());
        QCOMPARE(removedSpy.count(), 1);

        conn.input(QStringLiteral("x"));

        QTRY_COMPARE(text(conn, 0), QStringLiteral("x"));
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QStringLiteral("Name: "));

        conn.input(QStringLiteral("<Esc>"));

        QTRY_COMPARE(model->rowCount(), 0);
        QCOMPARE(removedSpy.count(), 2);
    }

    void secretPromptClampsMaskedCursor() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();

        conn.input(QStringLiteral(":call inputsecret('pw: ')<CR>a\u00E9"));

        // Neovim masks each character as '*' but reports the cursor in the unmasked 3-byte input.
        QTRY_COMPARE(text(conn, 0), QStringLiteral("**"));
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 2);
        QCOMPARE(role(conn, 0, CmdlineModel::PromptRole).toString(), QStringLiteral("pw: "));

        conn.input(QStringLiteral("<Left>"));

        QTRY_COMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 1);
        QCOMPARE(text(conn, 0), QStringLiteral("**"));

        conn.input(QStringLiteral("<Esc>"));
        QTRY_COMPARE(model->rowCount(), 0);
    }

    void secretPromptCursorWithinContentIsApproximate() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();

        conn.input(QStringLiteral(":call inputsecret('pw: ')<CR>\u00E9a<Left>b"));

        // The cursor follows the second '*', but Neovim sends unmasked byte 3 for "\u00E9b|a",
        // which maps to the end of "***".
        QTRY_COMPARE(text(conn, 0), QStringLiteral("***"));
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 3);

        conn.input(QStringLiteral("<Esc>"));
        QTRY_COMPARE(model->rowCount(), 0);
    }

    void nestedExpressionRestoresOuterLevel() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        CmdlineModel *model = conn.cmdline();
        QSignalSpy removedSpy(model, &CmdlineModel::rowsRemoved);

        conn.input(QStringLiteral(":<C-r>=1+1"));

        QTRY_COMPARE(text(conn, 1), QStringLiteral("1+1"));
        QTRY_COMPARE(role(conn, 0, CmdlineModel::SpecialCharacterRole).toString(),
                     QStringLiteral("\""));
        QCOMPARE(model->rowCount(), 2);
        QCOMPARE(model->activeLevel(), 2);
        QCOMPARE(role(conn, 0, CmdlineModel::FirstCharacterRole).toString(), QStringLiteral(":"));
        QCOMPARE(role(conn, 0, CmdlineModel::LevelRole).toInt(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialShiftRole).toBool(), true);
        QCOMPARE(role(conn, 1, CmdlineModel::CursorTextOffsetRole).toLongLong(), 3);
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
        QCOMPARE(role(conn, 0, CmdlineModel::CursorTextOffsetRole).toLongLong(), 1);
        QCOMPARE(role(conn, 0, CmdlineModel::SpecialCharacterRole).toString(), QString());
        QCOMPARE(removedSpy.count(), 1);

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
        QCOMPARE(model->block()->lines(),
                 QList<QVariantList>{ line(QStringLiteral("function! DotCmdlineBlock()")) });
        QCOMPARE(role(conn, 0, CmdlineModel::IndentRole).toInt(), 2);
        QCOMPARE(text(conn, 0), QString());

        conn.input(QStringLiteral("let x = 1<CR>"));

        QTRY_COMPARE(model->block()->rowCount(), 2);
        QCOMPARE(model->block()->lines().at(1), line(QStringLiteral("  let x = 1")));

        conn.input(QStringLiteral("endfunction<CR>"));

        QTRY_COMPARE(model->blockActive(), false);
        QCOMPARE(model->block()->rowCount(), 0);
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
        QSignalSpy removedSpy(model, &CmdlineModel::rowsRemoved);

        conn.input(QStringLiteral(":abc"));
        QTRY_COMPARE(text(conn, 0), QStringLiteral("abc"));

        // Neovim exits without hiding the command line, so only the connector can clear it.
        conn.command(QStringLiteral("qall!"));

        QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 10000);
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(model->activeLevel(), 0);
        QCOMPARE(removedSpy.count(), 0);
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
        QSignalSpy removedSpy(model, &CmdlineModel::rowsRemoved);
        QSignalSpy resetSpy(model, &CmdlineModel::modelReset);
        int rowsDuringHandoff = -1;
        QObject handoffContext;
        connect(&conn, &NvimConnector::attachedChanged, &handoffContext, [&] {
            if(!conn.attached()) rowsDuringHandoff = model->rowCount();
        });

        conn.input(QStringLiteral(":abc"));
        QTRY_COMPARE(text(conn, 0), QStringLiteral("abc"));

        conn.command(QStringLiteral("restart!"));

        QTRY_COMPARE_WITH_TIMEOUT(attachSpy.count(), 1, 20000);
        const auto newPid = evalSync(conn, QStringLiteral("getpid()"), 20000);
        QVERIFY(newPid && newPid->toInt() > 0 && newPid->toInt() != oldPid->toInt());
        QVERIFY(conn.attached());
        QCOMPARE(disconnectedSpy.count(), 0);
        // The old session stays visible until the replacement server connects.
        QCOMPARE(rowsDuringHandoff, 1);
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(removedSpy.count(), 0);
        QCOMPARE(resetSpy.count(), 1);
        QVERIFY(externalCmdline(conn) == true);

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
