#include "ColorPalette.h"
#include "Config.h"
#include "support/ColorHelpers.h"
#include "support/RpcHelpers.h"

#include <array>
#include <QtTest>

using namespace dot;
using namespace dot::test;

namespace {

struct Fixture {
    Config config;
    NvimConnector conn;
    ColorPalette &colors = *conn.colorPalette();
    HighlightTable &highlights = *conn.highlights();
    ModeInfo &mode = *conn.modeInfo();

    Fixture() {
        ColorPalette::registerConfig(config);
        colors.bindConfig(config);
    }

    void populate() {
        const auto attrs = colorAttrs(0xabcdef, 0x123456);
        const auto modes = cursorModes({ 4 });
        highlights.setDefaultColors(0xabcdef, 0x123456, 0x654321);
        for(int id = 1; id <= 4; ++id) highlights.defineAttr(id, attrs.get());
        highlights.setHighlightGroup(QStringLiteral("NormalFloat"), 3);
        highlights.setHighlightGroup(QStringLiteral("Pmenu"), 2);
        highlights.setHighlightGroup(QStringLiteral("Visual"), 1);
        mode.setModes(modes.get(), true);
        mode.setCurrentMode(QStringLiteral("normal"), 0);
    }
};

const auto fixedRoles = std::to_array<const char *>(
    { "accent", "changed", "error", "frame", "information", "mutedText", "raised", "resting",
      "selectedText", "selection", "success", "text", "warning" });

void expectErrors(const QStringList &errors) {
    for(const QString &error: errors)
        QTest::ignoreMessage(QtCriticalMsg, qPrintable(QStringLiteral("ColorPalette: ") + error));
}

} // namespace

class TestUiColors : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qRegisterMetaType<dot::Notification>();
        qInfo("ColorPalette object size: %zu bytes", sizeof(ColorPalette));
    }

    void malformedColorEventsAreDiagnosed() {
        QFETCH(QString, event);
        QFETCH(QString, error);
        QFETCH(QByteArray, payload);
        NvimConnector conn;
        seedColorState(conn);
        const auto arguments =
            msgpack::unpack(payload.constData(), static_cast<size_t>(payload.size()));
        QSignalSpy changed(conn.colorPalette(), &ColorPalette::paletteChanged);
        QTest::ignoreMessage(QtWarningMsg, qPrintable(error));

        QVERIFY(redrawEvent(conn, qPrintable(event), [&](auto &pk) { pk.pack(arguments.get()); }));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));

        QCOMPARE(changed.count(), 0);
        QCOMPARE(conn.colorPalette()->primaryText(), QColor(Qt::white));
        QVERIFY(conn.colorPalette()->resolutionErrors().isEmpty());
    }

    void malformedColorEventsAreDiagnosed_data() {
        QTest::addColumn<QString>("event");
        QTest::addColumn<QString>("error");
        QTest::addColumn<QByteArray>("payload");
        const auto row = [](const char *name, const char *event, const char *error,
                            const PackFn &arguments) {
            QTest::newRow(name) << QString::fromLatin1(event) << QString::fromLatin1(error)
                                << packFrame(arguments);
        };
        row("attribute-id", "hl_attr_define",
            "Invalid hl_attr_define: highlight id must be a non-negative integer", [](auto &pk) {
            pk.pack_array(4);
            pk.pack(-1);
            pk.pack_map(0);
            pk.pack_map(0);
            pk.pack_array(0);
        });
        row("attribute-shape", "hl_attr_define",
            "Invalid hl_attr_define: expected id, RGB map, cterm map, and info array",
            [](auto &pk) {
            pk.pack_array(3);
            pk.pack(0);
            pk.pack_map(0);
            pk.pack_map(0);
        });
        row("default-channel", "default_colors_set",
            "Invalid default_colors_set: RGB channels must be integers", [](auto &pk) {
            pk.pack_array(5);
            pk.pack(false);
            pk.pack(0);
            pk.pack(0);
            pk.pack(0);
            pk.pack(0);
        });
        row("default-overflow", "default_colors_set",
            "Invalid default_colors_set: RGB channels must be integers", [](auto &pk) {
            pk.pack_array(5);
            pk.pack(0x100000000ULL);
            pk.pack(0);
            pk.pack(0);
            pk.pack(0);
            pk.pack(0);
        });
        row("default-shape", "default_colors_set",
            "Invalid default_colors_set: expected five color channels",
            [](auto &pk) { pk.pack_array(0); });
        row("group-id", "hl_group_set",
            "Invalid hl_group_set: highlight id must be a non-negative integer", [](auto &pk) {
            pk.pack_array(2);
            pk.pack("Pmenu");
            pk.pack(-1);
        });
        row("group-shape", "hl_group_set", "Invalid hl_group_set: expected name and highlight id",
            [](auto &pk) {
            pk.pack_array(1);
            pk.pack("Pmenu");
        });
    }

    void exactProfileRoles() {
        QFETCH(QString, theme);
        QFETCH(QStringList, expected);
        Fixture fixture;
        fixture.config.setFromCli(QStringLiteral("theme"), theme);
        QCOMPARE(fixture.colors.theme(), theme);
        QCOMPARE(expected.size(), static_cast<qsizetype>(fixedRoles.size()));
        for(size_t i = 0; i < fixedRoles.size(); ++i) {
            const QColor color = fixture.colors.property(fixedRoles[i]).value<QColor>();
            QCOMPARE(color, QColor(expected[static_cast<qsizetype>(i)]));
            QCOMPARE(color.alpha(), 255);
        }
        QCOMPARE(fixture.colors.selectedText(),
                 theme == QStringLiteral("dark") ? QColor("#1e1e2e") : QColor("#eff1f5"));
    }

    void exactProfileRoles_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("dark") << QStringLiteral("dark")
                              << QStringList{ "#cba6f7", "#89b4fa", "#f38ba8", "#11111b", "#94e2d5",
                                              "#bac2de", "#45475a", "#313244", "#1e1e2e", "#cba6f7",
                                              "#a6e3a1", "#cdd6f4", "#f9e2af" };
        QTest::newRow("light") << QStringLiteral("light")
                               << QStringList{ "#8839ef", "#1e66f5", "#d20f39", "#dce0e8",
                                               "#179299", "#5c5f77", "#bcc0cc", "#ccd0da",
                                               "#eff1f5", "#8839ef", "#40a02b", "#4c4f69",
                                               "#df8e1d" };
    }

    void fixedChromeSurvivesContentThemeChange() {
        QFETCH(QString, theme);
        QVariantMap before;
        Fixture fixture;
        fixture.populate();
        fixture.config.setFromCli(QStringLiteral("theme"), theme);
        fixture.colors.publishContent();
        for(const char *role: fixedRoles)
            before.insert(QString::fromLatin1(role), fixture.colors.property(role));
        QSignalSpy published(&fixture.colors, &ColorPalette::paletteChanged);

        fixture.highlights.setDefaultColors(0, 0x654321, -1);
        QCOMPARE(published.count(), 0);
        QCOMPARE(fixture.colors.primaryText(), QColor("#abcdef"));
        fixture.colors.publishContent();

        QCOMPARE(published.count(), 1);
        QCOMPARE(fixture.colors.primaryText(), QColor(0, 0, 0));
        QCOMPARE(fixture.colors.paneBackground(), QColor("#654321"));
        for(const char *role: fixedRoles)
            QCOMPARE(fixture.colors.property(role), before.value(QString::fromLatin1(role)));
        fixture.colors.publishContent();
        QCOMPARE(published.count(), 1);
    }

    void fixedChromeSurvivesContentThemeChange_data() {
        QTest::addColumn<QString>("theme");
        QTest::newRow("dark") << QStringLiteral("dark");
        QTest::newRow("light") << QStringLiteral("light");
    }

    void invalidOverridePreservesLastValue() {
        Fixture fixture;
        fixture.config.setFromGGlobal(QStringLiteral("theme"), QStringLiteral("light"));
        const QColor frame = fixture.colors.frame();
        QSignalSpy changed(&fixture.colors, &ColorPalette::paletteChanged);
        QTest::ignoreMessage(QtWarningMsg,
                             "ColorPalette: invalid theme invalid (expected dark or light)");

        fixture.config.setFromGGlobal(QStringLiteral("theme"), QStringLiteral("invalid"));

        QCOMPARE(fixture.colors.theme(), QStringLiteral("light"));
        QCOMPARE(fixture.colors.frame(), frame);
        QCOMPARE(changed.count(), 0);
        fixture.config.setFromCli(QStringLiteral("theme"), QStringLiteral("dark"));
        QCOMPARE(fixture.colors.theme(), QStringLiteral("dark"));
        QCOMPARE(changed.count(), 1);
        fixture.config.setFromGGlobal(QStringLiteral("theme"), QStringLiteral("light"));
        QCOMPARE(fixture.colors.theme(), QStringLiteral("dark"));
        QCOMPARE(changed.count(), 1);
    }

    void rebindingDisconnectsTheOldConfig() {
        Config other;
        Fixture fixture;
        ColorPalette::registerConfig(other);
        other.setFromCli(QStringLiteral("theme"), QStringLiteral("light"));
        fixture.colors.bindConfig(other);
        fixture.config.setFromCli(QStringLiteral("theme"), QStringLiteral("light"));
        fixture.config.setFromCli(QStringLiteral("theme"), QStringLiteral("dark"));
        QCOMPARE(fixture.colors.theme(), QStringLiteral("light"));
        other.setFromCli(QStringLiteral("theme"), QStringLiteral("dark"));
        QCOMPARE(fixture.colors.theme(), QStringLiteral("dark"));
    }

    void cursorFallbackReversesContentPair() {
        Fixture fixture;
        fixture.populate();
        const auto modes = cursorModes({ 0 });
        fixture.mode.setModes(modes.get(), true);
        fixture.colors.publishContent();
        QVERIFY(fixture.colors.cursorValid());
        QCOMPARE(fixture.colors.cursorBackground(), fixture.colors.primaryText());
        QCOMPARE(fixture.colors.cursorForeground(), fixture.colors.paneBackground());
        fixture.highlights.setDefaultColors(0, 0, 0);
        fixture.colors.publishContent();
        QVERIFY(fixture.colors.cursorValid());
        QCOMPARE(fixture.colors.cursorBackground(), QColor(0, 0, 0));
        QCOMPARE(fixture.colors.cursorForeground(), QColor(0, 0, 0));
    }

    void cursorFollowsTheActiveModeAndReverseAttribute() {
        Fixture fixture;
        fixture.populate();
        const auto attrs = colorAttrs(0, 0x654321, true);
        const auto modes = cursorModes({ 4, 5 });
        fixture.highlights.defineAttr(5, attrs.get());
        fixture.mode.setModes(modes.get(), true);
        fixture.colors.publishContent();
        const QColor oldBackground = fixture.colors.cursorBackground();
        fixture.mode.setCurrentMode(QStringLiteral("insert"), 1);
        QCOMPARE(fixture.colors.cursorBackground(), oldBackground);

        fixture.colors.publishContent();

        QCOMPARE(fixture.colors.cursorBackground(), QColor(0, 0, 0));
        QCOMPARE(fixture.colors.cursorForeground(), QColor("#654321"));
    }

    void inheritedChannelsRemainNeovimOwned() {
        Fixture fixture;
        fixture.populate();
        const auto attrs = colorAttrs(-1, 0);
        fixture.highlights.defineAttr(2, attrs.get());
        fixture.colors.publishContent();
        QVERIFY(fixture.colors.menuValid());
        QCOMPARE(fixture.colors.menuBackground(), QColor(0, 0, 0));
        QCOMPARE(fixture.colors.menuForeground(), fixture.colors.primaryText());
        QCOMPARE(fixture.colors.menuForeground(), QColor("#abcdef"));
    }

    void missingNamedRoleReportsError() {
        QFETCH(QString, name);
        QFETCH(QString, property);
        QFETCH(QString, error);
        Fixture fixture;
        const auto modes = cursorModes({ 0 });
        fixture.highlights.setDefaultColors(0xabcdef, 0x123456, 0);
        fixture.mode.setModes(modes.get(), true);
        fixture.mode.setCurrentMode(QStringLiteral("normal"), 0);
        for(const QString &group:
            { QStringLiteral("NormalFloat"), QStringLiteral("Pmenu"), QStringLiteral("Visual") }) {
            if(group != name) fixture.highlights.setHighlightGroup(group, 0);
        }
        expectErrors({ error });
        fixture.colors.publishContent();
        QCOMPARE(fixture.colors.resolutionErrors(), QStringList{ error });
        QVERIFY(!fixture.colors.property(qPrintable(property + "Valid")).toBool());
        QSignalSpy changed(&fixture.colors, &ColorPalette::paletteChanged);
        fixture.colors.publishContent();
        QCOMPARE(changed.count(), 0);

        fixture.highlights.setHighlightGroup(name, 0);
        fixture.colors.publishContent();

        QVERIFY(fixture.colors.property(qPrintable(property + "Valid")).toBool());
        QVERIFY(fixture.colors.resolutionErrors().isEmpty());
        QCOMPARE(changed.count(), 1);
    }

    void missingNamedRoleReportsError_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("property");
        QTest::addColumn<QString>("error");
        QTest::newRow("menu") << QStringLiteral("Pmenu") << QStringLiteral("menu")
                              << QStringLiteral("menu: missing Pmenu association");
        QTest::newRow("overlay") << QStringLiteral("NormalFloat") << QStringLiteral("overlay")
                                 << QStringLiteral("overlay: missing NormalFloat association");
        QTest::newRow("selection") << QStringLiteral("Visual") << QStringLiteral("textSelection")
                                   << QStringLiteral("text selection: missing Visual association");
    }

    void missingResolvedChannelReportsError() {
        QFETCH(bool, foreground);
        QFETCH(int, id);
        QFETCH(QString, property);
        QFETCH(QString, role);
        Fixture fixture;
        const QString channel =
            foreground ? QStringLiteral("foreground") : QStringLiteral("background");
        QStringList errors{ QStringLiteral("pane: missing ") + channel };
        fixture.populate();
        fixture.colors.publishContent();
        const auto attrs = colorAttrs(foreground ? -1 : 0xabcdef, foreground ? 0x123456 : -1);
        fixture.highlights.setDefaultColors(foreground ? -1 : 0xabcdef, foreground ? 0x123456 : -1,
                                            0);
        if(id != 0) {
            fixture.highlights.defineAttr(id, attrs.get());
            errors.append(role + QStringLiteral(": missing ") + channel);
        }
        expectErrors(errors);

        fixture.colors.publishContent();

        QCOMPARE(fixture.colors.resolutionErrors(), errors);
        QVERIFY(!fixture.colors.property(qPrintable(property + "Valid")).toBool());
        if(property == QStringLiteral("pane")) {
            QVERIFY(!fixture.colors.paneBackground().isValid());
            QVERIFY(!fixture.colors.primaryText().isValid());
        } else {
            QVERIFY(!fixture.colors.property(qPrintable(property + "Background"))
                         .value<QColor>()
                         .isValid());
            QVERIFY(!fixture.colors.property(qPrintable(property + "Foreground"))
                         .value<QColor>()
                         .isValid());
        }
    }

    void missingResolvedChannelReportsError_data() {
        QTest::addColumn<bool>("foreground");
        QTest::addColumn<int>("id");
        QTest::addColumn<QString>("property");
        QTest::addColumn<QString>("role");
        for(const bool foreground: { false, true }) {
            const QByteArray suffix = foreground ? "-foreground" : "-background";
            QTest::newRow(("cursor" + suffix).constData())
                << foreground << 4 << QStringLiteral("cursor") << QStringLiteral("cursor");
            QTest::newRow(("menu" + suffix).constData())
                << foreground << 2 << QStringLiteral("menu") << QStringLiteral("menu");
            QTest::newRow(("overlay" + suffix).constData())
                << foreground << 3 << QStringLiteral("overlay") << QStringLiteral("overlay");
            QTest::newRow(("pane" + suffix).constData())
                << foreground << 0 << QStringLiteral("pane") << QStringLiteral("pane");
            QTest::newRow(("selection" + suffix).constData())
                << foreground << 1 << QStringLiteral("textSelection")
                << QStringLiteral("text selection");
        }
    }

    void missingCursorStateReportsTheSpecificError() {
        QFETCH(int, state);
        QFETCH(QString, error);
        Fixture fixture;
        fixture.populate();
        fixture.colors.publishContent();
        if(state == 0) {
            fixture.mode.reset();
        } else if(state == 1) {
            msgpack::sbuffer buffer;
            msgpack::packer<msgpack::sbuffer> packer(&buffer);
            packer.pack_array(1);
            packer.pack_map(0);
            const auto modes = msgpack::unpack(buffer.data(), buffer.size());
            fixture.mode.setModes(modes.get(), true);
        } else {
            const auto modes = cursorModes({ 99 });
            fixture.mode.setModes(modes.get(), true);
        }
        expectErrors({ error });
        fixture.colors.publishContent();
        QVERIFY(!fixture.colors.cursorValid());
        QCOMPARE(fixture.colors.resolutionErrors(), QStringList{ error });
    }

    void missingCursorStateReportsTheSpecificError_data() {
        QTest::addColumn<int>("state");
        QTest::addColumn<QString>("error");
        QTest::newRow("attribute") << 2 << QStringLiteral("cursor: missing highlight attribute");
        QTest::newRow("id") << 1 << QStringLiteral("cursor: missing highlight id");
        QTest::newRow("mode") << 0 << QStringLiteral("cursor: missing current mode");
    }

    void undefinedNamedAttributeDoesNotInheritDefaults() {
        Fixture fixture;
        fixture.populate();
        fixture.highlights.setHighlightGroup(QStringLiteral("NormalFloat"), 99);
        fixture.highlights.setHighlightGroup(QStringLiteral("Pmenu"), 98);
        fixture.highlights.setHighlightGroup(QStringLiteral("Visual"), 97);
        const QStringList errors{ QStringLiteral("menu: missing highlight attribute"),
                                  QStringLiteral("overlay: missing highlight attribute"),
                                  QStringLiteral("text selection: missing highlight attribute") };
        expectErrors(errors);
        fixture.colors.publishContent();
        QCOMPARE(fixture.colors.resolutionErrors(), errors);
        QVERIFY(!fixture.colors.menuValid());
        QVERIFY(!fixture.colors.overlayValid());
        QVERIFY(!fixture.colors.textSelectionValid());
        QVERIFY(fixture.colors.paneValid());
        QVERIFY(fixture.colors.cursorValid());
    }

    void resetInvalidatesRequiredRoles() {
        Fixture fixture;
        fixture.populate();
        fixture.colors.publishContent();
        const QColor frame = fixture.colors.frame();
        const QStringList errors{ QStringLiteral("pane: missing foreground"),
                                  QStringLiteral("pane: missing background"),
                                  QStringLiteral("cursor: missing current mode"),
                                  QStringLiteral("menu: missing Pmenu association"),
                                  QStringLiteral("overlay: missing NormalFloat association"),
                                  QStringLiteral("text selection: missing Visual association") };
        QSignalSpy changed(&fixture.colors, &ColorPalette::paletteChanged);
        fixture.highlights.clear();
        fixture.mode.reset();
        QCOMPARE(changed.count(), 0);
        expectErrors(errors);
        fixture.colors.resetContent();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(fixture.colors.resolutionErrors(), errors);
        QVERIFY(!fixture.colors.cursorValid());
        QVERIFY(!fixture.colors.menuValid());
        QVERIFY(!fixture.colors.overlayValid());
        QVERIFY(!fixture.colors.paneValid());
        QVERIFY(!fixture.colors.textSelectionValid());
        QCOMPARE(fixture.colors.frame(), frame);
        fixture.colors.publishContent();
        QCOMPARE(changed.count(), 1);
        fixture.populate();
        fixture.colors.publishContent();
        QVERIFY(fixture.colors.resolutionErrors().isEmpty());
        QCOMPARE(changed.count(), 2);
    }

    void redrawPublishesBeforeFlushWithoutTransientValues() {
        NvimConnector conn;
        seedColorState(conn);
        auto *colors = conn.colorPalette();
        QSignalSpy changed(colors, &ColorPalette::paletteChanged);
        QStringList order;
        connect(colors, &ColorPalette::paletteChanged, colors, [&] {
            order.append(QStringLiteral("palette"));
            QCOMPARE(colors->primaryText(), QColor("#abcdef"));
            QCOMPARE(colors->paneBackground(), QColor("#123456"));
            QCOMPARE(colors->menuForeground(), QColor("#654321"));
            QCOMPARE(colors->menuBackground(), QColor(0, 0, 0));
        });
        connect(&conn, &NvimConnector::flush, colors, [&] {
            order.append(QStringLiteral("flush"));
            QCOMPARE(changed.count(), 1);
        });
        QVERIFY(redrawEvent(conn, "default_colors_set", [](auto &pk) {
            pk.pack_array(5);
            pk.pack(0xabcdef);
            pk.pack(0x123456);
            pk.pack(-1);
            pk.pack(0);
            pk.pack(0);
        }));
        QVERIFY(redrawEvent(conn, "hl_attr_define", [](auto &pk) {
            const auto attrs = colorAttrs(0x654321, 0);
            pk.pack_array(4);
            pk.pack(7);
            pk.pack(attrs.get());
            pk.pack_map(0);
            pk.pack_array(0);
        }));
        QVERIFY(redrawEvent(conn, "hl_group_set", [](auto &pk) {
            pk.pack_array(2);
            pk.pack("Pmenu");
            pk.pack(7);
        }));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(colors->primaryText(), QColor(Qt::white));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QCOMPARE(order, QStringList({ QStringLiteral("palette"), QStringLiteral("flush") }));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QCOMPARE(changed.count(), 1);
    }
};

QTEST_GUILESS_MAIN(TestUiColors)
#include "test_ui_colors.moc"
