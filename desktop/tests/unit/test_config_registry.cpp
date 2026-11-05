#include <QSignalSpy>
#include <QtTest>

#include "Config.h"

using namespace dot;

class TestConfigRegistry : public QObject {
    Q_OBJECT
private slots:
    void replacementClearsAbsentGlobalsAndPreservesCli() {
        Config cfg;
        cfg.registerOption(QStringLiteral("alpha"), ConfigType::Int, 1);
        cfg.registerOption(QStringLiteral("beta"), ConfigType::Int, 2);
        cfg.registerOption(QStringLiteral("gamma"), ConfigType::Int, 3);
        cfg.setFromGGlobal(QStringLiteral("alpha"), 11);
        cfg.setFromGGlobal(QStringLiteral("beta"), 12);
        cfg.setFromGGlobal(QStringLiteral("gamma"), 13);
        cfg.setFromCli(QStringLiteral("beta"), 22);
        QSignalSpy changed(&cfg, &Config::changed);
        connect(&cfg, &Config::changed, &cfg, [&] {
            QCOMPARE(cfg.value(QStringLiteral("alpha")).toInt(), 1);
            QCOMPARE(cfg.value(QStringLiteral("beta")).toInt(), 22);
            QCOMPARE(cfg.value(QStringLiteral("gamma")).toInt(), 23);
        });

        cfg.replaceGGlobals({ { QStringLiteral("gamma"), 23 } });

        QCOMPARE(changed.count(), 2);
        QCOMPARE(changed.at(0).at(0).toString(), QStringLiteral("alpha"));
        QCOMPARE(changed.at(1).at(0).toString(), QStringLiteral("gamma"));
        changed.clear();
        cfg.replaceGGlobals({ { QStringLiteral("gamma"), 23 } });
        QCOMPARE(changed.count(), 0);
    }

    void defaultsAndHas() {
        Config cfg;
        cfg.registerOption(QStringLiteral("opacity"), ConfigType::Float, 1.0);
        QVERIFY(cfg.has(QStringLiteral("opacity")));
        QCOMPARE(cfg.value(QStringLiteral("opacity")).toDouble(), 1.0);
    }

    void gGlobalUpdatesAndSignalDeduplicates() {
        Config cfg;
        cfg.registerOption(QStringLiteral("opacity"), ConfigType::Float, 1.0);

        QSignalSpy spy(&cfg, &Config::changed);
        cfg.setFromGGlobal(QStringLiteral("opacity"), 0.5);
        QCOMPARE(cfg.value(QStringLiteral("opacity")).toDouble(), 0.5);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("opacity"));

        // Same RESOLVED value again → no signal.
        cfg.setFromGGlobal(QStringLiteral("opacity"), 0.5);
        QCOMPARE(spy.count(), 0);
    }

    void cliOverridesGGlobal() {
        Config cfg;
        cfg.registerOption(QStringLiteral("opacity"), ConfigType::Float, 1.0);
        cfg.setFromGGlobal(QStringLiteral("opacity"), 0.5);
        QCOMPARE(cfg.value(QStringLiteral("opacity")).toDouble(), 0.5);

        cfg.setFromCli(QStringLiteral("opacity"), 0.25);
        QCOMPARE(cfg.value(QStringLiteral("opacity")).toDouble(), 0.25);

        // Setting g: again while CLI shadows → resolved value unchanged → no signal.
        QSignalSpy spy(&cfg, &Config::changed);
        cfg.setFromGGlobal(QStringLiteral("opacity"), 0.9);
        QCOMPARE(cfg.value(QStringLiteral("opacity")).toDouble(), 0.25);
        QCOMPARE(spy.count(), 0);
    }

    void unknownNameQueries() {
        Config cfg;
        QVERIFY(!cfg.has(QStringLiteral("unknown")));
        QVERIFY(!cfg.value(QStringLiteral("unknown")).isValid());
    }

    void unknownNameSettersAreNoOp() {
        Config cfg;
        QSignalSpy spy(&cfg, &Config::changed);
        cfg.setFromCli(QStringLiteral("unknown"), 1);
        cfg.setFromGGlobal(QStringLiteral("unknown"), 1);
        QCOMPARE(spy.count(), 0);
    }

    void registeredNamesAlphabetical() {
        Config cfg;
        cfg.registerOption(QStringLiteral("zeta"), ConfigType::Bool, false);
        cfg.registerOption(QStringLiteral("alpha"), ConfigType::Int, 0);
        cfg.registerOption(QStringLiteral("mu"), ConfigType::String, QString());
        const QStringList n = cfg.registeredNames();
        const QStringList expected{ "alpha", "mu", "zeta" };
        QCOMPARE(n, expected);
    }

    void duplicateRegistrationIsIgnored() {
        Config cfg;
        cfg.registerOption(QStringLiteral("opacity"), ConfigType::Float, 1.0);
        // Second registration is rejected: the original type and default survive.
        cfg.registerOption(QStringLiteral("opacity"), ConfigType::Int, 42);
        QCOMPARE(cfg.type(QStringLiteral("opacity")), ConfigType::Float);
        QCOMPARE(cfg.value(QStringLiteral("opacity")).toDouble(), 1.0);
    }

    void setFromDefaultUpdatesResolvedValueAndSignals() {
        Config cfg;
        cfg.registerOption(QStringLiteral("opacity"), ConfigType::Float, 1.0);
        QSignalSpy spy(&cfg, &Config::changed);
        cfg.setFromDefault(QStringLiteral("opacity"), 0.75);
        QCOMPARE(cfg.value(QStringLiteral("opacity")).toDouble(), 0.75);
        QCOMPARE(spy.count(), 1);
        // Unknown names are a no-op.
        cfg.setFromDefault(QStringLiteral("unknown"), 0.1);
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_GUILESS_MAIN(TestConfigRegistry)
#include "test_config_registry.moc"
