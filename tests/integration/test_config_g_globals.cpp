#include <QSignalSpy>
#include <QtTest>

#include "Config.h"
#include "ConfigCliReader.h"
#include "ConfigGGlobalReader.h"
#include "IntegrationHelpers.h"

using namespace dot;
using namespace dot::test;

class TestConfigGGlobals : public QObject {
    Q_OBJECT
private slots:
    void readsGGlobalSetViaForwardedCmd() {
        Config cfg;
        cfg.registerOption(QStringLiteral("test_value"), ConfigType::Float, 0.0);

        NvimConnector conn;
        const QStringList extra{
            QStringLiteral("--cmd"),
            QStringLiteral("let g:dot_test_value = 0.42"),
        };
        QVERIFY(startTestNvim(conn, extra));
        QSignalSpy completeSpy(&conn, &NvimConnector::attachComplete);
        QVERIFY(conn.attachUi(80, 24));
        QVERIFY(completeSpy.wait(5000));

        QSignalSpy changedSpy(&cfg, &Config::changed);
        ConfigGGlobalReader::read(conn, cfg);
        QVERIFY(changedSpy.wait(5000));

        QCOMPARE(cfg.value(QStringLiteral("test_value")).toDouble(), 0.42);
    }

    void roundedHighlightsGlobalSurvivesRetiredCliOption() {
        QStringList args{
            QStringLiteral("--dot-rounded_highlights=CliOnly"),
            QStringLiteral("--cmd"),
            QStringLiteral("let g:dot_rounded_highlights = ['FloatBorder', 'NormalFloat']"),
        };
        Config cfg;
        NvimConnector conn;
        const QString key = QStringLiteral("rounded_highlights");
        const QStringList vimGroups{
            QStringLiteral("FloatBorder"),
            QStringLiteral("NormalFloat"),
        };
        cfg.registerOption(key, ConfigType::StringList, QStringList{});

        QTest::ignoreMessage(
            QtDebugMsg,
            "ConfigCliReader: unknown --dot option: \"--dot-rounded_highlights=CliOnly\"");
        ConfigCliReader::extract(args, cfg);

        QSignalSpy completeSpy(&conn, &NvimConnector::attachComplete);
        QVERIFY(startTestNvim(conn, args));
        QVERIFY(conn.attachUi(80, 24));
        QVERIFY(completeSpy.wait(5000));

        QSignalSpy changedSpy(&cfg, &Config::changed);
        ConfigGGlobalReader::read(conn, cfg);
        QTRY_COMPARE_WITH_TIMEOUT(cfg.value(key).toStringList(), vimGroups, 5000);
        QCOMPARE(changedSpy.count(), 1);
        QCOMPARE(changedSpy.constFirst().constFirst().toString(), key);
    }
};

QTEST_GUILESS_MAIN(TestConfigGGlobals)
#include "test_config_g_globals.moc"
