#include <QSignalSpy>
#include <QtTest>

#include "Config.h"
#include "ConfigGGlobalReader.h"

using namespace Dot;

class TestConfigGReader : public QObject {
    Q_OBJECT
private slots:
    void appliesRegisteredVarsAndSkipsUnknown() {
        Config cfg;
        cfg.registerOption(QStringLiteral("opacity"), ConfigType::Float, 1.0);
        cfg.registerOption(QStringLiteral("padding"), ConfigType::Int, 0);

        QHash<QString, QVariant> vars;
        vars.insert(QStringLiteral("dot_opacity"), 0.7);
        vars.insert(QStringLiteral("dot_padding"), 8);
        vars.insert(QStringLiteral("dot_unknown_var"), 42);

        ConfigGGlobalReader::readFromMap(vars, cfg);

        QCOMPARE(cfg.value(QStringLiteral("opacity")).toDouble(), 0.7);
        QCOMPARE(cfg.value(QStringLiteral("padding")).toInt(), 8);
        QVERIFY(!cfg.has(QStringLiteral("unknown_var")));
    }

    void typeCoercionForStringList() {
        Config cfg;
        cfg.registerOption(QStringLiteral("fallback"), ConfigType::StringList, QStringList{});
        QHash<QString, QVariant> vars;
        vars.insert(QStringLiteral("dot_fallback"), QVariant(QStringList{ "a", "b", "c" }));
        ConfigGGlobalReader::readFromMap(vars, cfg);
        const QStringList expected{ "a", "b", "c" };
        QCOMPARE(cfg.value(QStringLiteral("fallback")).toStringList(), expected);
    }

    void missingVarsAreSilent() {
        Config cfg;
        cfg.registerOption(QStringLiteral("opacity"), ConfigType::Float, 1.0);
        QHash<QString, QVariant> vars;
        QSignalSpy spy(&cfg, &Config::changed);
        ConfigGGlobalReader::readFromMap(vars, cfg);
        QCOMPARE(spy.count(), 0);
        QCOMPARE(cfg.value(QStringLiteral("opacity")).toDouble(), 1.0);
    }

    void boolCoercedFromInteger() {
        Config cfg;
        cfg.registerOption(QStringLiteral("frameless"), ConfigType::Bool, false);
        QHash<QString, QVariant> vars;
        vars.insert(QStringLiteral("dot_frameless"), 1);
        ConfigGGlobalReader::readFromMap(vars, cfg);
        QCOMPARE(cfg.value(QStringLiteral("frameless")).toBool(), true);
    }

    void boolRejectsNonNumericString() {
        // A non-numeric string is neither a bool nor an int, so coercion fails
        // and the malformed-value branch leaves the default intact.
        Config cfg;
        cfg.registerOption(QStringLiteral("frameless"), ConfigType::Bool, false);
        QHash<QString, QVariant> vars;
        vars.insert(QStringLiteral("dot_frameless"), QStringLiteral("hello"));
        ConfigGGlobalReader::readFromMap(vars, cfg);
        QCOMPARE(cfg.value(QStringLiteral("frameless")).toBool(), false);
    }

    void stringOptionAcceptsScalar() {
        Config cfg;
        cfg.registerOption(QStringLiteral("theme"), ConfigType::String, QStringLiteral("light"));
        QHash<QString, QVariant> vars;
        vars.insert(QStringLiteral("dot_theme"), QStringLiteral("dark"));
        ConfigGGlobalReader::readFromMap(vars, cfg);
        QCOMPARE(cfg.value(QStringLiteral("theme")).toString(), QStringLiteral("dark"));
    }

    void stringListFromScalarWrapsInList() {
        Config cfg;
        cfg.registerOption(QStringLiteral("fallback"), ConfigType::StringList, QStringList{});
        QHash<QString, QVariant> vars;
        vars.insert(QStringLiteral("dot_fallback"), QStringLiteral("solo"));
        ConfigGGlobalReader::readFromMap(vars, cfg);
        QCOMPARE(cfg.value(QStringLiteral("fallback")).toStringList(),
                 QStringList{ QStringLiteral("solo") });
    }

    void unconvertibleValuesAreIgnored() {
        // A map converts to neither a string nor a list, so both String and
        // StringList options must reject it and keep their defaults.
        Config cfg;
        cfg.registerOption(QStringLiteral("theme"), ConfigType::String, QStringLiteral("light"));
        cfg.registerOption(QStringLiteral("fallback"), ConfigType::StringList,
                           QStringList{ QStringLiteral("def") });
        QHash<QString, QVariant> vars;
        vars.insert(QStringLiteral("dot_theme"), QVariant(QVariantMap{ { "k", 1 } }));
        vars.insert(QStringLiteral("dot_fallback"), QVariant(QVariantMap{ { "k", 1 } }));
        ConfigGGlobalReader::readFromMap(vars, cfg);
        QCOMPARE(cfg.value(QStringLiteral("theme")).toString(), QStringLiteral("light"));
        QCOMPARE(cfg.value(QStringLiteral("fallback")).toStringList(),
                 QStringList{ QStringLiteral("def") });
    }

    void unrelatedPrefixDoesNotApply() {
        Config cfg;
        cfg.registerOption(QStringLiteral("padding"), ConfigType::Int, 0);
        const QHash<QString, QVariant> vars{ { QStringLiteral("previous_app_padding"), 9 } };
        ConfigGGlobalReader::readFromMap(vars, cfg);
        QCOMPARE(cfg.value(QStringLiteral("padding")).toInt(), 0);
    }
};

QTEST_GUILESS_MAIN(TestConfigGReader)
#include "test_config_g_reader.moc"
