#include <msgpack.hpp>
#include <QtTest>

#include "HighlightTable.h"

using namespace dot;

namespace {

msgpack::object_handle packAttrMap(const std::vector<std::pair<std::string, int>> &intKeys,
                                   const std::vector<std::pair<std::string, bool>> &boolKeys) {
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    pk.pack_map(static_cast<uint32_t>(intKeys.size() + boolKeys.size()));
    for(const auto &[k, v]: intKeys) {
        pk.pack(k);
        pk.pack(v);
    }
    for(const auto &[k, v]: boolKeys) {
        pk.pack(k);
        pk.pack(v);
    }
    msgpack::object_handle h;
    msgpack::unpack(h, buf.data(), buf.size());
    return h;
}

// Packs the ext_hlstate `info` array as [ { "hi_name": <name> }, ... ].
msgpack::object_handle packInfoNames(const std::vector<std::string> &names) {
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    pk.pack_array(static_cast<uint32_t>(names.size()));
    for(const auto &n: names) {
        pk.pack_map(1);
        pk.pack(std::string("hi_name"));
        pk.pack(n);
    }
    msgpack::object_handle h;
    msgpack::unpack(h, buf.data(), buf.size());
    return h;
}

} // namespace

class TestHighlightTable : public QObject {
    Q_OBJECT
private slots:
    void defaultColorsSet() {
        HighlightTable h;
        h.setDefaultColors(0xff0000, 0x00ff00, 0x0000ff);
        QCOMPARE(h.defaultFg(), QColor(0xff, 0x00, 0x00));
        QCOMPARE(h.defaultBg(), QColor(0x00, 0xff, 0x00));
        QCOMPARE(h.defaultSp(), QColor(0x00, 0x00, 0xff));
    }

    void defineAttrFgBgBoldItalic() {
        HighlightTable h;
        auto m = packAttrMap({ { "foreground", 0xff8800 }, { "background", 0x123456 } },
                             { { "bold", true }, { "italic", true } });
        h.defineAttr(7, m.get());
        HlAttr a = h.attr(7);
        QCOMPARE(a.fg, QColor(0xff, 0x88, 0x00));
        QCOMPARE(a.bg, QColor(0x12, 0x34, 0x56));
        QVERIFY(a.bold);
        QVERIFY(a.italic);
        QVERIFY(!a.underline);
    }

    void defineAttrUndercurlSp() {
        HighlightTable h;
        h.setDefaultColors(0xffffff, 0x000000, 0xff0000);
        auto m = packAttrMap({ { "special", 0x00ff00 } }, { { "undercurl", true } });
        h.defineAttr(3, m.get());
        HlAttr a = h.attr(3);
        QCOMPARE(a.sp, QColor(0x00, 0xff, 0x00));
        QVERIFY(a.undercurl);
    }

    void resolvedFallsBackToDefaults() {
        HighlightTable h;
        h.setDefaultColors(0xaaaaaa, 0x111111, 0xff0000);
        auto m = packAttrMap({}, { { "bold", true } });
        h.defineAttr(5, m.get());
        HlAttr a = h.resolved(5);
        QCOMPARE(a.fg, QColor(0xaa, 0xaa, 0xaa));
        QCOMPARE(a.bg, QColor(0x11, 0x11, 0x11));
        QVERIFY(a.bold);
        QCOMPARE(a.sp, QColor(0xff, 0x00, 0x00));

        h.setDefaultColors(-1, 0, -1);
        a = h.resolved(5);
        QVERIFY(!a.fg.isValid());
        QCOMPARE(a.bg, QColor(0, 0, 0));
        QVERIFY(!a.sp.isValid());
    }

    void resolvedReverseSwapsFgBg() {
        HighlightTable h;
        h.setDefaultColors(0xaaaaaa, 0x111111, 0xff0000);
        auto m = packAttrMap({ { "foreground", 0x222222 }, { "background", 0x888888 } },
                             { { "reverse", true } });
        h.defineAttr(6, m.get());
        HlAttr a = h.resolved(6);
        QCOMPARE(a.fg, QColor(0x88, 0x88, 0x88));
        QCOMPARE(a.bg, QColor(0x22, 0x22, 0x22));

        const auto inherited = packAttrMap({ { "blend", 35 } }, { { "reverse", true } });
        h.defineAttr(6, inherited.get());
        a = h.resolved(6);
        QCOMPARE(a.fg, h.defaultBg());
        QCOMPARE(a.bg, h.defaultFg());
        QCOMPARE(a.blend, 35);
    }

    void getIntHandlesNegativeAndNonIntValues() {
        // A negative blend exercises the NEGATIVE_INTEGER arm; a boolean under an
        // integer key exercises the "present but wrong type -> default" arm.
        HighlightTable h;
        auto m = packAttrMap({ { "blend", -5 } }, { { "background", true } });
        h.defineAttr(9, m.get());
        HlAttr a = h.attr(9);
        QCOMPARE(a.blend, -5);
        QVERIFY(!a.bg.isValid()); // background was not a usable integer
    }

    void setRoundedHighlightsRecomputesExistingAttrs() {
        // Defining the attr before any rounded set means it starts non-rounded;
        // a later setRoundedHighlights must walk the existing attrs and flip it.
        HighlightTable h;
        auto attrMap = packAttrMap({ { "background", 0x445566 } }, {});
        auto info = packInfoNames({ "Search" });
        h.defineAttr(4, attrMap.get(), &info.get());
        QVERIFY(!h.isRounded(4));
        h.setRoundedHighlights({ QStringLiteral("Search") });
        QVERIFY(h.isRounded(4));
        h.setHighlightGroup(QStringLiteral("NormalFloat"), 4);
        QCOMPARE(h.requiredAttrId(RequiredHighlight::NormalFloat), std::optional<int>(4));
        QVERIFY(h.resolved(4).isRounded);
        h.setRoundedHighlights({});
        QVERIFY(!h.resolved(4).isRounded);
        QCOMPARE(h.requiredAttrId(RequiredHighlight::NormalFloat), std::optional<int>(4));
    }

    void negativeColorsInvalidateDefaults() {
        QFETCH(int, background);
        QFETCH(int, foreground);
        QFETCH(int, special);
        HighlightTable table;
        const auto attrs =
            packAttrMap({ { "background", -1 }, { "foreground", -1 }, { "special", -1 } }, {});
        table.setDefaultColors(0x123456, 0x654321, 0xabcdef);
        table.setDefaultColors(foreground, background, special);
        table.defineAttr(1, attrs.get());

        const HlAttr attr = table.attr(1);
        QVERIFY(!attr.fg.isValid());
        QVERIFY(!attr.bg.isValid());
        QVERIFY(!attr.sp.isValid());

        const HlAttr resolved = table.resolved(1);
        QCOMPARE(resolved.fg.isValid(), foreground >= 0);
        QCOMPARE(resolved.bg.isValid(), background >= 0);
        QCOMPARE(resolved.sp.isValid(), special >= 0);
        if(foreground >= 0) QCOMPARE(resolved.fg, QColor(0, 0, 0));
        if(background >= 0) QCOMPARE(resolved.bg, QColor(0, 0, 0));
        if(special >= 0) QCOMPARE(resolved.sp, QColor(0, 0, 0));
    }

    void negativeColorsInvalidateDefaults_data() {
        QTest::addColumn<int>("background");
        QTest::addColumn<int>("foreground");
        QTest::addColumn<int>("special");
        QTest::newRow("all") << -1 << -1 << -1;
        QTest::newRow("background") << -1 << 0 << 0;
        QTest::newRow("foreground") << 0 << -1 << 0;
        QTest::newRow("special") << 0 << 0 << -1;
    }

    void requiredAssociationsUseLatestEvent() {
        HighlightTable table;
        const auto attrs = packAttrMap({ { "background", 0x123456 } }, {});
        const auto info = packInfoNames({ "Pmenu", "Visual" });
        QSignalSpy changed(&table, &HighlightTable::changed);
        QVERIFY(!table.requiredAttrId(RequiredHighlight::Pmenu));
        QVERIFY(!table.contains(7));
        QVERIFY(table.contains(0));

        table.setHighlightGroup(QStringLiteral("Pmenu"), 0);
        QCOMPARE(table.requiredAttrId(RequiredHighlight::Pmenu), std::optional<int>(0));
        const auto count = changed.count();
        table.setHighlightGroup(QStringLiteral("Pmenu"), 0);
        table.setHighlightGroup(QStringLiteral("pmenu"), 8);
        table.setHighlightGroup(QStringLiteral("Unknown"), 9);
        QCOMPARE(changed.count(), count);

        table.defineAttr(7, attrs.get(), &info.get());
        QVERIFY(table.contains(7));
        QCOMPARE(table.requiredAttrId(RequiredHighlight::Pmenu), std::optional<int>(7));
        QCOMPARE(table.requiredAttrId(RequiredHighlight::Visual), std::optional<int>(7));
        QVERIFY(!table.requiredAttrId(RequiredHighlight::NormalFloat));
        table.setHighlightGroup(QStringLiteral("Pmenu"), 11);
        QCOMPARE(table.requiredAttrId(RequiredHighlight::Pmenu), std::optional<int>(11));
        table.defineAttr(12, attrs.get(), &info.get());
        QCOMPARE(table.requiredAttrId(RequiredHighlight::Pmenu), std::optional<int>(12));
        QTest::ignoreMessage(QtWarningMsg,
                             "Invalid hl_group_set: highlight id must be non-negative");
        table.setHighlightGroup(QStringLiteral("Pmenu"), -1);
        QCOMPARE(table.requiredAttrId(RequiredHighlight::Pmenu), std::optional<int>(12));
    }

    void requiredAssociationsUseBothMetadataNames() {
        HighlightTable table;
        msgpack::sbuffer buffer;
        msgpack::packer<msgpack::sbuffer> packer(&buffer);
        const auto attrs = packAttrMap({}, {});
        packer.pack_array(1);
        packer.pack_map(2);
        packer.pack("hi_name");
        packer.pack("NormalFloat");
        packer.pack("ui_name");
        packer.pack("Pmenu");
        const auto info = msgpack::unpack(buffer.data(), buffer.size());
        table.defineAttr(4, attrs.get(), &info.get());
        QCOMPARE(table.requiredAttrId(RequiredHighlight::NormalFloat), std::optional<int>(4));
        QCOMPARE(table.requiredAttrId(RequiredHighlight::Pmenu), std::optional<int>(4));
        QCOMPARE(table.attr(4).names, QStringList{ QStringLiteral("Pmenu") });
    }

    void resetClearsNamedAssociations() {
        HighlightTable table;
        const auto attrs = packAttrMap({}, {});
        const auto info = packInfoNames({ "NormalFloat", "Visual" });
        QVERIFY(!table.defaultFg().isValid());
        QVERIFY(!table.defaultBg().isValid());
        QVERIFY(!table.defaultSp().isValid());
        table.setDefaultColors(0x123456, 0x654321, 0xabcdef);
        table.setRoundedHighlights({ QStringLiteral("NormalFloat") });
        table.defineAttr(4, attrs.get(), &info.get());
        table.setHighlightGroup(QStringLiteral("Pmenu"), 4);
        QSignalSpy changed(&table, &HighlightTable::changed);

        table.clear();

        QCOMPARE(changed.count(), 1);
        QVERIFY(!table.defaultFg().isValid());
        QVERIFY(!table.defaultBg().isValid());
        QVERIFY(!table.defaultSp().isValid());
        QVERIFY(!table.contains(4));
        QVERIFY(!table.requiredAttrId(RequiredHighlight::NormalFloat));
        QVERIFY(!table.requiredAttrId(RequiredHighlight::Pmenu));
        QVERIFY(!table.requiredAttrId(RequiredHighlight::Visual));
        table.defineAttr(4, attrs.get(), &info.get());
        QVERIFY(table.isRounded(4));
    }

    void packedRgbColors() {
        QFETCH(int, rgb);
        QFETCH(QColor, expected);

        HighlightTable table;
        const auto attrs =
            packAttrMap({ { "background", rgb }, { "foreground", rgb }, { "special", rgb } }, {});
        table.setDefaultColors(rgb, rgb, rgb);
        table.defineAttr(1, attrs.get());

        const HlAttr attr = table.attr(1);
        QCOMPARE(table.defaultFg(), expected);
        QCOMPARE(table.defaultBg(), expected);
        QCOMPARE(table.defaultSp(), expected);
        QCOMPARE(attr.fg, expected);
        QCOMPARE(attr.bg, expected);
        QCOMPARE(attr.sp, expected);
    }

    void packedRgbColors_data() {
        QTest::addColumn<int>("rgb");
        QTest::addColumn<QColor>("expected");
        QTest::newRow("black") << 0x000000 << QColor(0, 0, 0);
        QTest::newRow("mixed") << 0x123456 << QColor(0x12, 0x34, 0x56);
        QTest::newRow("upper-byte-ignored") << 0x7f123456 << QColor(0x12, 0x34, 0x56);
        QTest::newRow("white") << 0xffffff << QColor(0xff, 0xff, 0xff);
    }
};

QTEST_GUILESS_MAIN(TestHighlightTable)
#include "test_highlight_table.moc"
