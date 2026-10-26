#include <QtTest>

#include "Config.h"
#include "ConfigCliReader.h"

using namespace dot;

class TestConfigCliReader : public QObject {
    Q_OBJECT
private slots:
    void floatParsedAndArgConsumed() {
        Config cfg;
        cfg.registerOption(QStringLiteral("opacity"), ConfigType::Float, 1.0);
        QStringList args{ QStringLiteral("--dot-opacity=0.5") };
        ConfigCliReader::extract(args, cfg);
        QCOMPARE(cfg.value(QStringLiteral("opacity")).toDouble(), 0.5);
        QVERIFY(args.isEmpty());
    }

    void boolBareFlagDefaultsTrue() {
        Config cfg;
        cfg.registerOption(QStringLiteral("frameless"), ConfigType::Bool, false);
        QStringList args{ QStringLiteral("--dot-frameless") };
        ConfigCliReader::extract(args, cfg);
        QCOMPARE(cfg.value(QStringLiteral("frameless")).toBool(), true);
        QVERIFY(args.isEmpty());
    }

    void boolExplicitValueForms() {
        Config cfg;
        cfg.registerOption(QStringLiteral("frameless"), ConfigType::Bool, true);

        QStringList args{ QStringLiteral("--dot-frameless=false") };
        ConfigCliReader::extract(args, cfg);
        QCOMPARE(cfg.value(QStringLiteral("frameless")).toBool(), false);

        QStringList args2{ QStringLiteral("--dot-frameless=on") };
        ConfigCliReader::extract(args2, cfg);
        QCOMPARE(cfg.value(QStringLiteral("frameless")).toBool(), true);

        QStringList args3{ QStringLiteral("--dot-frameless=0") };
        ConfigCliReader::extract(args3, cfg);
        QCOMPARE(cfg.value(QStringLiteral("frameless")).toBool(), false);
    }

    void intMixedWithForwardedArg() {
        Config cfg;
        cfg.registerOption(QStringLiteral("padding"), ConfigType::Int, 0);
        QStringList args{ QStringLiteral("--dot-padding=10"), QStringLiteral("foo.txt") };
        ConfigCliReader::extract(args, cfg);
        QCOMPARE(cfg.value(QStringLiteral("padding")).toInt(), 10);
        QCOMPARE(args, QStringList{ QStringLiteral("foo.txt") });
    }

    void stringValue() {
        Config cfg;
        cfg.registerOption(QStringLiteral("theme"), ConfigType::String, QStringLiteral("light"));
        QStringList args{ QStringLiteral("--dot-theme=dark") };
        ConfigCliReader::extract(args, cfg);
        QCOMPARE(cfg.value(QStringLiteral("theme")).toString(), QStringLiteral("dark"));
    }

    void stringListSplitsOnComma() {
        Config cfg;
        cfg.registerOption(QStringLiteral("fallback"), ConfigType::StringList, QStringList{});
        QStringList args{ QStringLiteral("--dot-fallback=a,b,c") };
        ConfigCliReader::extract(args, cfg);
        const QStringList expected{ "a", "b", "c" };
        QCOMPARE(cfg.value(QStringLiteral("fallback")).toStringList(), expected);
    }

    void unknownOptionIgnored() {
        Config cfg;
        cfg.registerOption(QStringLiteral("padding"), ConfigType::Int, 0);
        QStringList args{ QStringLiteral("--dot-unknown=x") };
        ConfigCliReader::extract(args, cfg);
        QCOMPARE(cfg.value(QStringLiteral("padding")).toInt(), 0);
    }

    void malformedValueLeavesDefault() {
        Config cfg;
        cfg.registerOption(QStringLiteral("padding"), ConfigType::Int, 7);
        QStringList args{ QStringLiteral("--dot-padding=notanumber") };
        ConfigCliReader::extract(args, cfg);
        QCOMPARE(cfg.value(QStringLiteral("padding")).toInt(), 7);
    }

    void unrecognisedBoolWordLeavesDefault() {
        Config cfg;
        cfg.registerOption(QStringLiteral("frameless"), ConfigType::Bool, false);
        QStringList args{ QStringLiteral("--dot-frameless=maybe") };
        ConfigCliReader::extract(args, cfg);
        QCOMPARE(cfg.value(QStringLiteral("frameless")).toBool(), false);
    }

    void nonBoolOptionWithoutValueIsRejected() {
        Config cfg;
        cfg.registerOption(QStringLiteral("padding"), ConfigType::Int, 3);
        QStringList args{ QStringLiteral("--dot-padding") }; // no "=value"
        ConfigCliReader::extract(args, cfg);
        QCOMPARE(cfg.value(QStringLiteral("padding")).toInt(), 3);
        QVERIFY(args.isEmpty()); // still consumed, not forwarded
    }

    void unrelatedPrefixDoesNotApply() {
        Config cfg;
        cfg.registerOption(QStringLiteral("padding"), ConfigType::Int, 0);
        QStringList args{ QStringLiteral("--previous-app-padding=9") };
        ConfigCliReader::extract(args, cfg);
        QCOMPARE(args, QStringList{ QStringLiteral("--previous-app-padding=9") });
        QCOMPARE(cfg.value(QStringLiteral("padding")).toInt(), 0);
    }
};

QTEST_GUILESS_MAIN(TestConfigCliReader)
#include "test_config_cli_reader.moc"
