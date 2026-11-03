#include <msgpack.hpp>
#include <QtTest>

#include "ModeInfo.h"

using namespace dot;

namespace {

struct TestMode {
    int attrId = 0;
    int blinkOff = 0;
    int blinkOn = 0;
    int blinkWait = 0;
    const char *name = "";
    int percentage = 100;
    const char *shape = "block";
    const char *shortName = "";
};

msgpack::object_handle modeTable(std::initializer_list<TestMode> modes) {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> packer(&buffer);
    packer.pack_array(static_cast<uint32_t>(modes.size()));
    for(const TestMode &mode: modes) {
        packer.pack_map(8);
        packer.pack("attr_id");
        packer.pack(mode.attrId);
        packer.pack("blinkoff");
        packer.pack(mode.blinkOff);
        packer.pack("blinkon");
        packer.pack(mode.blinkOn);
        packer.pack("blinkwait");
        packer.pack(mode.blinkWait);
        packer.pack("cell_percentage");
        packer.pack(mode.percentage);
        packer.pack("cursor_shape");
        packer.pack(mode.shape);
        packer.pack("name");
        packer.pack(mode.name);
        packer.pack("short_name");
        packer.pack(mode.shortName);
    }
    return msgpack::unpack(buffer.data(), buffer.size());
}

} // namespace

class TestModeInfo : public QObject {
    Q_OBJECT
private slots:
    // One mode whose fields deliberately exercise the type-mismatch arms of the
    // map readers: a non-string name, a negative integer, and an integer key
    // carrying a string value (which must fall back to the default).
    void setModesParsesFieldsAndTypeMismatches() {
        msgpack::sbuffer buf;
        msgpack::packer<msgpack::sbuffer> pk(&buf);
        pk.pack_array(1);
        pk.pack_map(5);
        pk.pack(std::string("name"));
        pk.pack(42); // non-string name -> empty, triggers the setCurrentMode fallback
        pk.pack(std::string("cursor_shape"));
        pk.pack(std::string("vertical"));
        pk.pack(std::string("cell_percentage"));
        pk.pack(-1); // negative integer arm
        pk.pack(std::string("blinkon"));
        pk.pack(std::string("nope")); // wrong type -> default (0)
        pk.pack(std::string("blinkwait"));
        pk.pack(250);

        msgpack::object_handle h;
        msgpack::unpack(h, buf.data(), buf.size());

        ModeInfo mi;
        QSignalSpy spy(&mi, &ModeInfo::currentChanged);
        mi.setModes(h.get(), true);
        mi.setCurrentMode(QStringLiteral("insert"), 0);

        QVERIFY(mi.cursorStyleEnabled());
        QCOMPARE(mi.currentName(), QStringLiteral("insert")); // fell back to the passed name
        QCOMPARE(mi.cursorShapeInt(), static_cast<int>(CursorShape::Vertical));
        QCOMPARE(mi.cellPercentage(), -1);
        QCOMPARE(mi.blinkOn(), 0);
        QCOMPARE(mi.blinkWait(), 250);
        QVERIFY(spy.count() >= 1);
        QVERIFY(mi.currentModeValid());
        QVERIFY(!mi.currentHighlightValid());
    }

    void setCurrentModeOutOfRangeUsesNameOnly() {
        ModeInfo mi;
        // No modes defined: any index is out of range, so the descriptor resets
        // to defaults and only the name is applied.
        mi.setCurrentMode(QStringLiteral("cmdline"), 99);
        QCOMPARE(mi.currentName(), QStringLiteral("cmdline"));
        QCOMPARE(mi.cellPercentage(), 100); // default descriptor
        QCOMPARE(mi.cursorShapeInt(), static_cast<int>(CursorShape::Block));
        QVERIFY(!mi.currentModeValid());
        QVERIFY(!mi.currentHighlightValid());
    }

    void setModesRejectsNonArray() {
        msgpack::sbuffer buf;
        msgpack::packer<msgpack::sbuffer> pk(&buf);
        pk.pack(7); // not an array

        msgpack::object_handle h;
        msgpack::unpack(h, buf.data(), buf.size());

        ModeInfo mi;
        const auto initial = modeTable({ { .percentage = 20, .shape = "vertical" } });
        mi.setModes(initial.get(), true);
        mi.setCurrentMode(QStringLiteral("normal"), 0);
        QSignalSpy spy(&mi, &ModeInfo::currentChanged);
        QTest::ignoreMessage(QtWarningMsg, "Invalid mode_info_set: mode_info must be an array");
        mi.setModes(h.get(), false);
        QCOMPARE(mi.currentName(), QStringLiteral("normal"));
        QCOMPARE(mi.cursorShapeInt(), static_cast<int>(CursorShape::Block));
        QCOMPARE(mi.cellPercentage(), 100);
        QVERIFY(!mi.cursorStyleEnabled());
        QCOMPARE(spy.count(), 1);
    }

    void duplicateTableDoesNotNotify() {
        ModeInfo mode;
        const auto table =
            modeTable({ { .blinkWait = 250, .percentage = 20, .shape = "vertical" } });
        mode.setModes(table.get(), true);
        mode.setCurrentMode(QStringLiteral("insert"), 0);
        QSignalSpy spy(&mode, &ModeInfo::currentChanged);

        mode.setModes(table.get(), true);

        QCOMPARE(spy.count(), 0);
        QCOMPARE(mode.blinkWait(), 250);
        QCOMPARE(mode.cellPercentage(), 20);
    }

    void highlightValidityRequiresAValidInteger() {
        QFETCH(QVariant, value);
        QFETCH(bool, valid);
        ModeInfo mode;
        msgpack::sbuffer buffer;
        msgpack::packer<msgpack::sbuffer> packer(&buffer);
        QVERIFY(!mode.currentModeValid());
        QVERIFY(!mode.currentHighlightValid());
        packer.pack_array(1);
        packer.pack_map(value.isValid() ? 1 : 0);
        if(value.isValid()) {
            packer.pack("attr_id");
            if(value.typeId() == QMetaType::QString) packer.pack(value.toString().toStdString());
            else if(value.typeId() == QMetaType::ULongLong) packer.pack(value.toULongLong());
            else packer.pack(value.toInt());
        }
        const auto table = msgpack::unpack(buffer.data(), buffer.size());
        mode.setModes(table.get(), true);
        QVERIFY(!mode.currentModeValid());
        QVERIFY(!mode.currentHighlightValid());
        mode.setCurrentMode(QStringLiteral("normal"), 0);
        QVERIFY(mode.currentModeValid());
        QCOMPARE(mode.currentHighlightValid(), valid);
        if(valid) QCOMPARE(mode.attrId(), value.toInt());
        mode.reset();
        QVERIFY(!mode.currentModeValid());
        QVERIFY(!mode.currentHighlightValid());
    }

    void highlightValidityRequiresAValidInteger_data() {
        QTest::addColumn<QVariant>("value");
        QTest::addColumn<bool>("valid");
        QTest::newRow("missing") << QVariant() << false;
        QTest::newRow("negative") << QVariant(-1) << false;
        QTest::newRow("overflow") << QVariant::fromValue<qulonglong>(0x100000000ULL) << false;
        QTest::newRow("positive") << QVariant(17) << true;
        QTest::newRow("string") << QVariant(QStringLiteral("0")) << false;
        QTest::newRow("zero") << QVariant(0) << true;
    }

    void modeValidityFollowsTableReplacement() {
        ModeInfo mode;
        const auto empty = modeTable({});
        const auto table = modeTable({ {} });
        mode.setCurrentMode(QStringLiteral("normal"), 0);
        QVERIFY(!mode.currentModeValid());
        QSignalSpy changed(&mode, &ModeInfo::currentChanged);
        mode.setModes(table.get(), false);
        QCOMPARE(changed.count(), 1);
        QVERIFY(mode.currentModeValid());
        QVERIFY(mode.currentHighlightValid());
        QCOMPARE(mode.attrId(), 0);
        mode.setModes(empty.get(), false);
        QCOMPARE(changed.count(), 2);
        QVERIFY(!mode.currentModeValid());
        QVERIFY(!mode.currentHighlightValid());
    }

    void missingNamesUseTheLastModeEventName() {
        ModeInfo mode;
        const auto initial =
            modeTable({ { .name = "descriptor-label", .percentage = 20, .shape = "vertical" } });
        const auto replacement = modeTable({ { .percentage = 40, .shape = "vertical" } });
        mode.setModes(initial.get(), true);
        mode.setCurrentMode(QStringLiteral("insert"), 0);
        QCOMPARE(mode.currentName(), QStringLiteral("descriptor-label"));

        mode.setModes(replacement.get(), true);

        QCOMPARE(mode.currentName(), QStringLiteral("insert"));
        QCOMPARE(mode.cellPercentage(), 40);
    }

    void modeTableTracksTheCurrentIndex() {
        ModeInfo mode;
        const auto initial = modeTable(
            { { .name = "normal" }, { .name = "insert", .percentage = 20, .shape = "vertical" } });
        const auto replacement =
            modeTable({ { .name = "normal", .percentage = 80, .shape = "horizontal" },
                        { .name = "insert", .percentage = 40, .shape = "vertical" } });
        mode.setModes(initial.get(), true);
        mode.setCurrentMode(QStringLiteral("insert"), 1);

        mode.setModes(replacement.get(), true);

        QCOMPARE(mode.currentName(), QStringLiteral("insert"));
        QCOMPARE(mode.cellPercentage(), 40);
        QCOMPARE(mode.cursorShapeInt(), static_cast<int>(CursorShape::Vertical));
    }

    void removedModeFallsBack() {
        ModeInfo mode;
        const auto initial = modeTable({ { .percentage = 20, .shape = "vertical" } });
        const auto empty = modeTable({});
        mode.setModes(initial.get(), true);
        mode.setCurrentMode(QStringLiteral("insert"), 0);
        QSignalSpy spy(&mode, &ModeInfo::currentChanged);

        mode.setModes(empty.get(), true);

        QCOMPARE(mode.currentName(), QStringLiteral("insert"));
        QCOMPARE(mode.cellPercentage(), 100);
        QCOMPARE(mode.cursorShapeInt(), static_cast<int>(CursorShape::Block));
        QCOMPARE(spy.count(), 1);
    }

    void resetDiscardsTheCurrentIdentity() {
        ModeInfo mode;
        const auto table = modeTable({ { .percentage = 20, .shape = "vertical" } });
        mode.setModes(table.get(), true);
        mode.setCurrentMode(QStringLiteral("insert"), 0);
        mode.reset();
        QVERIFY(!mode.cursorStyleEnabled());

        mode.setModes(table.get(), true);

        QVERIFY(mode.currentName().isEmpty());
        QCOMPARE(mode.cellPercentage(), 100);
        QCOMPARE(mode.cursorShapeInt(), static_cast<int>(CursorShape::Block));
        mode.setCurrentMode(QStringLiteral("normal"), -1);
        QCOMPARE(mode.currentName(), QStringLiteral("normal"));
        QCOMPARE(mode.cellPercentage(), 100);
    }

    void styleAndBlinkChangesNotify() {
        ModeInfo mode;
        const auto initial = modeTable({ { .percentage = 20, .shape = "vertical" } });
        const auto blinking = modeTable({ { .attrId = 5,
                                            .blinkOff = 30,
                                            .blinkOn = 40,
                                            .blinkWait = 50,
                                            .percentage = 20,
                                            .shape = "vertical",
                                            .shortName = "i" } });
        mode.setModes(initial.get(), true);
        mode.setCurrentMode(QStringLiteral("insert"), 0);
        QSignalSpy spy(&mode, &ModeInfo::currentChanged);

        mode.setModes(initial.get(), false);
        QCOMPARE(spy.count(), 1);
        QVERIFY(!mode.cursorStyleEnabled());
        QCOMPARE(mode.cellPercentage(), 20);
        mode.setModes(blinking.get(), false);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(mode.attrId(), 5);
        QCOMPARE(mode.blinkOff(), 30);
        QCOMPARE(mode.blinkOn(), 40);
        QCOMPARE(mode.blinkWait(), 50);
    }

    void tableReplacementRefreshesCurrentMode() {
        ModeInfo mode;
        const auto initial =
            modeTable({ { .name = "normal", .percentage = 20, .shape = "vertical" } });
        const auto replacement =
            modeTable({ { .name = "normal", .percentage = 80, .shape = "horizontal" } });
        mode.setModes(initial.get(), true);
        mode.setCurrentMode(QStringLiteral("normal"), 0);
        QSignalSpy spy(&mode, &ModeInfo::currentChanged);

        mode.setModes(replacement.get(), true);

        QCOMPARE(mode.cellPercentage(), 80);
        QCOMPARE(mode.cursorShapeInt(), static_cast<int>(CursorShape::Horizontal));
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_GUILESS_MAIN(TestModeInfo)
#include "test_mode_info.moc"
