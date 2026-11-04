#include "ColorPalette.h"
#include "support/ColorHelpers.h"
#include "support/RpcHelpers.h"

#include <array>
#include <QtTest>

using namespace dot;
using namespace dot::test;

namespace {

struct Fixture {
    NvimConnector conn;
    ColorPalette &colors = *conn.colorPalette();
    HighlightTable &highlights = *conn.highlights();
    ModeInfo &mode = *conn.modeInfo();

    Fixture() {
        const auto snapshot = chromeSnapshot();
        colors.stageChromeSnapshot(snapshot.get());
    }

    void populate() {
        const auto attrs = colorAttrs(0xabcdef, 0x123456);
        const auto modes = cursorModes({ 4 });
        const auto snapshot = chromeSnapshot();
        colors.stageChromeSnapshot(snapshot.get());
        highlights.setDefaultColors(0xabcdef, 0x123456, 0x654321);
        for(int id = 1; id <= 4; ++id) highlights.defineAttr(id, attrs.get());
        highlights.setHighlightGroup(QStringLiteral("NormalFloat"), 3);
        highlights.setHighlightGroup(QStringLiteral("Pmenu"), 2);
        highlights.setHighlightGroup(QStringLiteral("Visual"), 1);
        mode.setModes(modes.get(), true);
        mode.setCurrentMode(QStringLiteral("normal"), 0);
    }
};

const auto chromeProperties = std::to_array<const char *>(
    { "accent", "changed", "error", "frame", "information", "mutedText", "raised", "resting",
      "selectedText", "selection", "success", "text", "warning" });

void expectErrors(const QStringList &errors) {
    for(const QString &error: errors)
        QTest::ignoreMessage(QtCriticalMsg, qPrintable(QStringLiteral("ColorPalette: ") + error));
}

bool highlightAndFlush(NvimConnector &conn) {
    return redrawEvent(conn, "hl_attr_define", [](auto &pk) {
        pk.pack_array(4);
        pk.pack(8);
        pk.pack_map(0);
        pk.pack_map(0);
        pk.pack_array(0);
    }) && redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); });
}

bool replyTo(QTcpSocket *peer, const msgpack::object &request, const PackFn &error = {},
             const PackFn &result = {}) {
    const QByteArray reply = packFrame([&](auto &pk) {
        pk.pack_array(4);
        pk.pack(1);
        pk.pack(request.via.array.ptr[1]);
        if(error) error(pk);
        else pk.pack_nil();
        if(result) result(pk);
        else pk.pack(true);
    });
    return peer->write(reply) == reply.size() && peer->flush();
}

} // namespace

class TestUiColors : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qRegisterMetaType<dot::Notification>();
        qInfo("ColorPalette object size: %zu bytes", sizeof(ColorPalette));
    }

    void defaultColorsDoNotPartiallyRefreshChrome() {
        NvimConnector conn;
        QTcpServer server;
        auto *peer = connectPeer(*conn.findChild<MsgpackRpc *>(), server);
        QVERIFY(peer);
        seedColorState(conn);
        const auto snapshotFor = [](int accent) {
            return chromeSnapshot("dark", [accent](auto &pk) {
                pk.pack_map(static_cast<uint32_t>(chromeRoles.size()));
                for(const char *role: chromeRoles) {
                    const auto attr =
                        chromeAttributes(QByteArray(role) == "DotAccent" ? accent : -1, -1);
                    pk.pack(role);
                    pk.pack(attr.get());
                }
            });
        };
        const auto initial = snapshotFor(0x010203);
        QVERIFY(colorNotification(conn, 0, initial.get()));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QCOMPARE(conn.colorPalette()->text(), QColor(Qt::white));
        QVERIFY(redrawEvent(conn, "default_colors_set", [](auto &pk) {
            pk.pack_array(5);
            pk.pack(0xaabbcc);
            pk.pack(0x102030);
            pk.pack(-1);
            pk.pack(0);
            pk.pack(0);
        }));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QCOMPARE(conn.colorPalette()->primaryText(), QColor("#aabbcc"));
        QCOMPARE(conn.colorPalette()->text(), QColor(Qt::white));
        QCOMPARE(conn.colorPalette()->frame(), QColor(Qt::black));
        QCOMPARE(conn.colorPalette()->accent(), QColor("#010203"));
        const auto requests = readFrames(peer, 1);
        QCOMPARE(requests.size(), size_t(1));
        const auto updated = snapshotFor(0x040506);
        QVERIFY(colorNotification(conn, 0, updated.get()));
        QCOMPARE(conn.colorPalette()->text(), QColor(Qt::white));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QCOMPARE(conn.colorPalette()->text(), QColor("#aabbcc"));
        QCOMPARE(conn.colorPalette()->frame(), QColor("#102030"));
        QCOMPARE(conn.colorPalette()->accent(), QColor("#040506"));
        QVERIFY(replyTo(peer, requests.front().get()));
    }

    void delayedSnapshotPublishesOnlyAtItsOwnFlush() {
        NvimConnector conn;
        QTcpServer server;
        auto *rpc = conn.findChild<MsgpackRpc *>();
        QVERIFY(rpc);
        auto *peer = connectPeer(*rpc, server);
        QVERIFY(peer);
        seedColorState(conn);
        const auto initial = chromeSnapshot();
        QVERIFY(colorNotification(conn, 0, initial.get()));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QSignalSpy changed(conn.colorPalette(), &ColorPalette::paletteChanged);
        QVERIFY(highlightAndFlush(conn));
        const auto requests = readFrames(peer, 1);
        QCOMPARE(requests.size(), size_t(1));
        const auto &request = requests.front().get();
        QCOMPARE(request.via.array.ptr[2].as<std::string>(), "nvim_exec_lua");
        QCOMPARE(request.via.array.ptr[3].via.array.ptr[0].as<std::string>(),
                 "return require('dot.colors').refresh(...)");
        QCOMPARE(conn.colorPalette()->theme(), QStringLiteral("dark"));
        QCOMPARE(changed.count(), 0);
        const auto updated = chromeSnapshot("light");
        QVERIFY(colorNotification(conn, 0, updated.get()));
        QCOMPARE(conn.colorPalette()->theme(), QStringLiteral("dark"));
        QCOMPARE(changed.count(), 0);
        QVERIFY(redrawEvent(conn, "default_colors_set", [](auto &pk) {
            pk.pack_array(5);
            pk.pack(0xffffff);
            pk.pack(0);
            pk.pack(-1);
            pk.pack(0);
            pk.pack(0);
        }));
        QVERIFY(redrawEvent(conn, "hl_attr_define", [](auto &pk) {
            pk.pack_array(4);
            pk.pack(9);
            pk.pack_map(0);
            pk.pack_map(0);
            pk.pack_array(0);
        }));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QCOMPARE(conn.colorPalette()->theme(), QStringLiteral("light"));
        QCOMPARE(changed.count(), 1);
        QVERIFY(replyTo(peer, request));
        QTest::qWait(30);
        QCOMPARE(peer->bytesAvailable(), qint64(0));
    }

    void dirtyDuringRefreshReplaysOnceAndThenBecomesIdle() {
        NvimConnector conn;
        QTcpServer server;
        auto *peer = connectPeer(*conn.findChild<MsgpackRpc *>(), server);
        QVERIFY(peer);
        seedColorState(conn);
        const auto initial = chromeSnapshot();
        QVERIFY(colorNotification(conn, 0, initial.get()));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QVERIFY(highlightAndFlush(conn));
        const auto first = readFrames(peer, 1);
        QCOMPARE(first.size(), size_t(1));
        for(int i = 0; i < 6; ++i) QVERIFY(highlightAndFlush(conn));
        QTest::qWait(30);
        QCOMPARE(peer->bytesAvailable(), qint64(0));
        QVERIFY(replyTo(peer, first.front().get()));
        const auto replay = readFrames(peer, 1);
        QCOMPARE(replay.size(), size_t(1));
        QVERIFY(replyTo(peer, replay.front().get()));
        for(int i = 0; i < 6; ++i) {
            QVERIFY(redrawEvent(conn, "grid_cursor_goto", [](auto &pk) {
                pk.pack_array(3);
                pk.pack(1);
                pk.pack(0);
                pk.pack(0);
            }));
            QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        }
        QTest::qWait(30);
        QCOMPARE(peer->bytesAvailable(), qint64(0));
    }

    void failedRefreshInvalidatesChromeAndCanRecover() {
        QFETCH(bool, rpcError);
        NvimConnector conn;
        QTcpServer server;
        auto *peer = connectPeer(*conn.findChild<MsgpackRpc *>(), server);
        QVERIFY(peer);
        seedColorState(conn);
        const auto initial = chromeSnapshot();
        QVERIFY(colorNotification(conn, 0, initial.get()));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QVERIFY(highlightAndFlush(conn));
        const auto request = readFrames(peer, 1);
        QCOMPARE(request.size(), size_t(1));
        if(rpcError) {
            expectErrors({ QStringLiteral("chrome: refresh failed: color failure") });
            QVERIFY(replyTo(peer, request.front().get(), [](auto &pk) {
                pk.pack_array(2);
                pk.pack(0);
                pk.pack("color failure");
            }));
        } else {
            expectErrors({ QStringLiteral("chrome: invalid refresh response") });
            QVERIFY(replyTo(peer, request.front().get(), {}, [](auto &pk) { pk.pack(false); }));
        }
        QTRY_VERIFY(!conn.colorPalette()->chromeValid());
        QVERIFY(!conn.colorPalette()->frame().isValid());
        QVERIFY(conn.colorPalette()->paneValid());
        QVERIFY(highlightAndFlush(conn));
        const auto retry = readFrames(peer, 1);
        QCOMPARE(retry.size(), size_t(1));
        QVERIFY(colorNotification(conn, 0, initial.get()));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QVERIFY(replyTo(peer, retry.front().get()));
        QTRY_VERIFY(conn.colorPalette()->chromeValid());
        QVERIFY(conn.colorPalette()->resolutionErrors().isEmpty());
    }

    void failedRefreshInvalidatesChromeAndCanRecover_data() {
        QTest::addColumn<bool>("rpcError");
        QTest::newRow("invalid-result") << false;
        QTest::newRow("rpc-error") << true;
    }

    void staleRepliesAndSnapshotsCannotAffectTheReplacementSession() {
        NvimConnector conn;
        QTcpServer server;
        auto *peer = connectPeer(*conn.findChild<MsgpackRpc *>(), server);
        QVERIFY(peer);
        seedColorState(conn);
        const auto dark = chromeSnapshot();
        const auto light = chromeSnapshot("light");
        QVERIFY(colorNotification(conn, 0, dark.get()));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QVERIFY(highlightAndFlush(conn));
        const auto oldRequest = readFrames(peer, 1);
        QCOMPARE(oldRequest.size(), size_t(1));
        QVERIFY(QMetaObject::invokeMethod(&conn, "onRpcDisconnected", Qt::DirectConnection));
        QVERIFY(colorNotification(conn, 0, light.get()));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QCOMPARE(conn.colorPalette()->theme(), QStringLiteral("dark"));
        QVERIFY(colorNotification(conn, 1, light.get()));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QVERIFY(highlightAndFlush(conn));
        const auto current = readFrames(peer, 1);
        QCOMPARE(current.size(), size_t(1));
        QCOMPARE(conn.colorPalette()->theme(), QStringLiteral("light"));
        QVERIFY(replyTo(peer, oldRequest.front().get()));
        QTest::qWait(30);
        QVERIFY(highlightAndFlush(conn));
        QTest::qWait(30);
        QCOMPARE(peer->bytesAvailable(), qint64(0));
        QVERIFY(replyTo(peer, current.front().get()));
        const auto replay = readFrames(peer, 1);
        QCOMPARE(replay.size(), size_t(1));
        QVERIFY(replyTo(peer, replay.front().get()));
        QTest::qWait(30);
        QCOMPARE(peer->bytesAvailable(), qint64(0));
    }

    void malformedChromeSnapshotsInvalidateAtomically() {
        QFETCH(QByteArray, payload);
        QFETCH(QString, error);
        Fixture fixture;
        fixture.populate();
        fixture.colors.publishContent();
        const auto snapshot =
            msgpack::unpack(payload.constData(), static_cast<size_t>(payload.size()));
        QSignalSpy changed(&fixture.colors, &ColorPalette::paletteChanged);
        fixture.colors.stageChromeSnapshot(snapshot.get());
        QVERIFY(fixture.colors.chromeValid());
        QCOMPARE(changed.count(), 0);
        expectErrors({ error });
        fixture.colors.publishContent();
        QCOMPARE(changed.count(), 1);
        QVERIFY(!fixture.colors.chromeValid());
        QVERIFY(fixture.colors.theme().isEmpty());
        for(const char *role: chromeProperties)
            QVERIFY(!fixture.colors.property(role).value<QColor>().isValid());
        QCOMPARE(fixture.colors.resolutionErrors(), QStringList{ error });
        fixture.colors.stageChromeSnapshot(snapshot.get());
        fixture.colors.publishContent();
        QCOMPARE(changed.count(), 1);
        fixture.populate();
        fixture.colors.publishContent();
        QVERIFY(fixture.colors.chromeValid());
        QVERIFY(fixture.colors.resolutionErrors().isEmpty());
        QCOMPARE(changed.count(), 2);
    }

    void malformedChromeSnapshotsInvalidateAtomically_data() {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<QString>("error");
        const auto row = [](const char *name, const char *error, const PackFn &payload) {
            QTest::newRow(name) << packFrame(payload)
                                << QStringLiteral("chrome: invalid snapshot: ") +
                                       QString::fromLatin1(error);
        };
        const auto roleRow = [&](const char *name, const char *error, const PackFn &fields) {
            row(name, error, [&](auto &pk) {
                const auto snapshot = chromeSnapshot("dark", [&](auto &roles) {
                    roles.pack_map(1);
                    roles.pack("DotAccent");
                    fields(roles);
                });
                pk.pack(snapshot.get());
            });
        };
        row("root-nil", "expected background and roles", [](auto &pk) { pk.pack_nil(); });
        row("root-size", "expected background and roles", [](auto &pk) { pk.pack_map(0); });
        row("root-key", "field names must be strings", [](auto &pk) {
            pk.pack_map(2);
            pk.pack(0);
            pk.pack_nil();
            pk.pack("roles");
            pk.pack_map(0);
        });
        row("root-duplicate", "unexpected or duplicate field background", [](auto &pk) {
            pk.pack_map(2);
            for(int i = 0; i < 2; ++i) {
                pk.pack("background");
                pk.pack("dark");
            }
        });
        row("root-unknown", "unexpected or duplicate field unknown", [](auto &pk) {
            pk.pack_map(2);
            pk.pack("background");
            pk.pack("dark");
            pk.pack("unknown");
            pk.pack_map(0);
        });
        row("background-type", "background must be dark or light", [](auto &pk) {
            pk.pack_map(2);
            pk.pack("background");
            pk.pack(false);
            pk.pack("roles");
            pk.pack_map(0);
        });
        row("background-value", "background must be dark or light", [](auto &pk) {
            const auto snapshot = chromeSnapshot("invalid");
            pk.pack(snapshot.get());
        });
        row("roles-type", "roles must be a map", [](auto &pk) {
            const auto snapshot = chromeSnapshot("dark", [](auto &roles) { roles.pack_array(0); });
            pk.pack(snapshot.get());
        });
        row("role-key", "role names must be strings", [](auto &pk) {
            const auto snapshot = chromeSnapshot("dark", [](auto &roles) {
                roles.pack_map(1);
                roles.pack(0);
                roles.pack_map(0);
            });
            pk.pack(snapshot.get());
        });
        row("role-unknown", "unknown role Unknown", [](auto &pk) {
            const auto snapshot = chromeSnapshot("dark", [](auto &roles) {
                roles.pack_map(1);
                roles.pack("Unknown");
                roles.pack_map(0);
            });
            pk.pack(snapshot.get());
        });
        row("role-duplicate", "duplicate role DotAccent", [](auto &pk) {
            const auto snapshot = chromeSnapshot("dark", [](auto &roles) {
                const auto attr = chromeAttributes();
                roles.pack_map(2);
                for(int i = 0; i < 2; ++i) {
                    roles.pack("DotAccent");
                    roles.pack(attr.get());
                }
            });
            pk.pack(snapshot.get());
        });
        roleRow("attribute-type", "DotAccent requires bg, exists, fg, and reverse",
                [](auto &pk) { pk.pack_nil(); });
        roleRow("attribute-size", "DotAccent requires bg, exists, fg, and reverse",
                [](auto &pk) { pk.pack_map(0); });
        roleRow("attribute-key", "DotAccent field names must be strings", [](auto &pk) {
            pk.pack_map(4);
            for(int i = 0; i < 4; ++i) {
                pk.pack(i);
                pk.pack_nil();
            }
        });
        roleRow("attribute-duplicate", "DotAccent duplicate field bg", [](auto &pk) {
            pk.pack_map(4);
            for(int i = 0; i < 4; ++i) {
                pk.pack("bg");
                pk.pack_nil();
            }
        });
        roleRow("attribute-unknown", "DotAccent unknown field unknown", [](auto &pk) {
            pk.pack_map(4);
            pk.pack("unknown");
            pk.pack_nil();
            for(const char *key: { "bg", "fg", "reverse" }) {
                pk.pack(key);
                pk.pack_nil();
            }
        });
        const auto fieldRow = [&](const char *name, const char *error, const char *field,
                                  const PackFn &value) {
            roleRow(name, error, [&](auto &pk) {
                pk.pack_map(4);
                for(const char *key: { "bg", "exists", "fg", "reverse" }) {
                    pk.pack(key);
                    if(QByteArray(key) == field) value(pk);
                    else if(QByteArray(key) == "exists") pk.pack(true);
                    else if(QByteArray(key) == "reverse") pk.pack(false);
                    else pk.pack_nil();
                }
            });
        };
        fieldRow("bg-type", "DotAccent bg must be an RGB color or nil", "bg",
                 [](auto &pk) { pk.pack(false); });
        fieldRow("exists-type", "DotAccent exists must be a boolean", "exists",
                 [](auto &pk) { pk.pack(0); });
        fieldRow("fg-negative", "DotAccent fg must be an RGB color or nil", "fg",
                 [](auto &pk) { pk.pack(-1); });
        fieldRow("fg-overflow", "DotAccent fg must be an RGB color or nil", "fg",
                 [](auto &pk) { pk.pack(0x1000000); });
        fieldRow("fg-type", "DotAccent fg must be an RGB color or nil", "fg",
                 [](auto &pk) { pk.pack(1.5); });
        fieldRow("reverse-type", "DotAccent reverse must be a boolean", "reverse",
                 [](auto &pk) { pk.pack_nil(); });
    }

    void missingChromeChannelCannotUseAnInventedDefault() {
        QFETCH(bool, foreground);
        Fixture fixture;
        fixture.populate();
        const QString group = foreground ? QStringLiteral("DotAccent") : QStringLiteral("DotFrame");
        const QString channel =
            foreground ? QStringLiteral("foreground") : QStringLiteral("background");
        const auto snapshot = chromeSnapshot("dark", [&](auto &pk) {
            pk.pack_map(static_cast<uint32_t>(chromeRoles.size()));
            for(const char *role: chromeRoles) {
                const bool missing = QString::fromLatin1(role) == group;
                const auto attr = chromeAttributes(missing ? -1 : 0, missing ? -1 : 0);
                pk.pack(role);
                pk.pack(attr.get());
            }
        });
        fixture.colors.stageChromeSnapshot(snapshot.get());
        fixture.highlights.setDefaultColors(foreground ? -1 : 0, foreground ? 0 : -1, 0);
        const QStringList errors{
            QStringLiteral("pane: missing ") + channel,
            QStringLiteral("chrome: %1 missing %2").arg(group, channel),
        };
        expectErrors(errors);
        fixture.colors.publishContent();
        QVERIFY(!fixture.colors.chromeValid());
        QCOMPARE(fixture.colors.resolutionErrors(), errors);
    }

    void missingChromeChannelCannotUseAnInventedDefault_data() {
        QTest::addColumn<bool>("foreground");
        QTest::newRow("background") << false;
        QTest::newRow("foreground") << true;
    }

    void malformedColorNotificationIsDiagnosed() {
        QFETCH(QByteArray, payload);
        NvimConnector conn;
        seedColorState(conn);
        const Notification note{
            QStringLiteral("dot_colors"),
            std::make_shared<msgpack::object_handle>(
                msgpack::unpack(payload.constData(), static_cast<size_t>(payload.size()))),
        };
        QSignalSpy changed(conn.colorPalette(), &ColorPalette::paletteChanged);
        QTest::ignoreMessage(QtCriticalMsg, "Invalid dot_colors: expected generation and snapshot");
        QVERIFY(QMetaObject::invokeMethod(&conn, "onNotification", Qt::DirectConnection,
                                          Q_ARG(dot::Notification, note)));
        QVERIFY(redrawEvent(conn, "flush", [](auto &pk) { pk.pack_array(0); }));
        QCOMPARE(changed.count(), 0);
        QVERIFY(conn.colorPalette()->chromeValid());
    }

    void malformedColorNotificationIsDiagnosed_data() {
        QTest::addColumn<QByteArray>("payload");
        const auto row = [](const char *name, const PackFn &params) {
            QTest::newRow(name) << packFrame([&](auto &pk) {
                pk.pack_array(3);
                pk.pack(2);
                pk.pack("dot_colors");
                params(pk);
            });
        };
        row("generation-negative", [](auto &pk) {
            pk.pack_array(2);
            pk.pack(-1);
            pk.pack_nil();
        });
        row("generation-type", [](auto &pk) {
            pk.pack_array(2);
            pk.pack(false);
            pk.pack_nil();
        });
        row("params-size", [](auto &pk) { pk.pack_array(0); });
        row("params-type", [](auto &pk) { pk.pack_nil(); });
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

    void exactReceivedRoles() {
        QFETCH(bool, reverse);
        QFETCH(QString, theme);
        Fixture fixture;
        fixture.populate();
        const auto snapshot = chromeSnapshot(qPrintable(theme), [&](auto &pk) {
            pk.pack_map(static_cast<uint32_t>(chromeRoles.size()));
            for(size_t i = 0; i < chromeRoles.size(); ++i) {
                const auto attr = chromeAttributes(0x112200 + static_cast<int>(i),
                                                   0x334400 + static_cast<int>(i), true, reverse);
                pk.pack(chromeRoles.at(i));
                pk.pack(attr.get());
            }
        });
        fixture.colors.stageChromeSnapshot(snapshot.get());
        QVERIFY(!fixture.colors.chromeValid());
        fixture.colors.publishContent();
        QVERIFY(fixture.colors.chromeValid());
        QCOMPARE(fixture.colors.theme(), theme);
        for(size_t i = 0; i < chromeProperties.size(); ++i) {
            const QByteArray role(chromeProperties.at(i));
            const bool surface =
                role == "frame" || role == "raised" || role == "resting" || role == "selection";
            const QRgb base = surface != reverse ? 0x334400 : 0x112200;
            const QColor color = fixture.colors.property(chromeProperties.at(i)).value<QColor>();
            QCOMPARE(color, QColor::fromRgb(base + static_cast<QRgb>(i)));
            QCOMPARE(color.alpha(), 255);
        }
    }

    void exactReceivedRoles_data() {
        QTest::addColumn<bool>("reverse");
        QTest::addColumn<QString>("theme");
        QTest::newRow("dark") << false << QStringLiteral("dark");
        QTest::newRow("dark-reverse") << true << QStringLiteral("dark");
        QTest::newRow("light") << false << QStringLiteral("light");
        QTest::newRow("light-reverse") << true << QStringLiteral("light");
    }

    void explicitChromeSurvivesDefaultColorChange() {
        QFETCH(QString, theme);
        QVariantMap before;
        Fixture fixture;
        fixture.populate();
        const auto snapshot = chromeSnapshot(qPrintable(theme));
        fixture.colors.stageChromeSnapshot(snapshot.get());
        fixture.colors.publishContent();
        for(const char *role: chromeProperties)
            before.insert(QString::fromLatin1(role), fixture.colors.property(role));
        QSignalSpy published(&fixture.colors, &ColorPalette::paletteChanged);

        fixture.highlights.setDefaultColors(0, 0x654321, -1);
        QCOMPARE(published.count(), 0);
        QCOMPARE(fixture.colors.primaryText(), QColor("#abcdef"));
        fixture.colors.publishContent();

        QCOMPARE(published.count(), 1);
        QCOMPARE(fixture.colors.primaryText(), QColor(0, 0, 0));
        QCOMPARE(fixture.colors.paneBackground(), QColor("#654321"));
        for(const char *role: chromeProperties)
            QCOMPARE(fixture.colors.property(role), before.value(QString::fromLatin1(role)));
        fixture.colors.publishContent();
        QCOMPARE(published.count(), 1);
    }

    void explicitChromeSurvivesDefaultColorChange_data() {
        QTest::addColumn<QString>("theme");
        QTest::newRow("dark") << QStringLiteral("dark");
        QTest::newRow("light") << QStringLiteral("light");
    }

    void incompleteSnapshotInvalidatesOnlyTheMissingRole() {
        QFETCH(bool, omitted);
        Fixture fixture;
        fixture.populate();
        fixture.colors.publishContent();
        const auto snapshot = chromeSnapshot("light", [&](auto &pk) {
            pk.pack_map(static_cast<uint32_t>(chromeRoles.size()) - (omitted ? 1 : 0));
            for(const char *role: chromeRoles) {
                const bool missing = QByteArray(role) == "DotAccent";
                if(missing && omitted) continue;
                const auto attr = chromeAttributes(0, 0, !missing);
                pk.pack(role);
                pk.pack(attr.get());
            }
        });
        const QColor frame = fixture.colors.frame();
        fixture.colors.stageChromeSnapshot(snapshot.get());
        QVERIFY(fixture.colors.chromeValid());
        QCOMPARE(fixture.colors.theme(), QStringLiteral("dark"));
        QCOMPARE(fixture.colors.frame(), frame);
        expectErrors({ QStringLiteral("chrome: missing DotAccent") });
        fixture.colors.publishContent();
        QVERIFY(!fixture.colors.chromeValid());
        QVERIFY(!fixture.colors.accent().isValid());
        QCOMPARE(fixture.colors.frame(), QColor(0, 0, 0));
        QCOMPARE(fixture.colors.theme(), QStringLiteral("light"));
        QCOMPARE(fixture.colors.resolutionErrors(),
                 QStringList{ QStringLiteral("chrome: missing DotAccent") });
    }

    void incompleteSnapshotInvalidatesOnlyTheMissingRole_data() {
        QTest::addColumn<bool>("omitted");
        QTest::newRow("nonexistent") << false;
        QTest::newRow("omitted") << true;
    }

    void inheritedChromeUsesOnlyNeovimDefaults() {
        QFETCH(bool, reverse);
        Fixture fixture;
        fixture.populate();
        const auto snapshot = chromeSnapshot("dark", [&](auto &pk) {
            const auto attr = chromeAttributes(-1, -1, true, reverse);
            pk.pack_map(static_cast<uint32_t>(chromeRoles.size()));
            for(const char *role: chromeRoles) {
                pk.pack(role);
                pk.pack(attr.get());
            }
        });
        fixture.colors.stageChromeSnapshot(snapshot.get());
        fixture.colors.publishContent();
        QVERIFY(fixture.colors.chromeValid());
        QCOMPARE(fixture.colors.accent(), reverse ? QColor("#123456") : QColor("#abcdef"));
        QCOMPARE(fixture.colors.frame(), reverse ? QColor("#abcdef") : QColor("#123456"));
        fixture.highlights.setDefaultColors(0, 0, 0);
        fixture.colors.publishContent();
        QCOMPARE(fixture.colors.accent(), reverse ? QColor("#123456") : QColor("#abcdef"));
        QCOMPARE(fixture.colors.frame(), reverse ? QColor("#abcdef") : QColor("#123456"));
        fixture.colors.stageChromeSnapshot(snapshot.get());
        fixture.colors.publishContent();
        QCOMPARE(fixture.colors.accent(), QColor(0, 0, 0));
        QCOMPARE(fixture.colors.frame(), QColor(0, 0, 0));
    }

    void inheritedChromeUsesOnlyNeovimDefaults_data() {
        QTest::addColumn<bool>("reverse");
        QTest::newRow("normal") << false;
        QTest::newRow("reverse") << true;
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
        const QStringList errors{ QStringLiteral("pane: missing foreground"),
                                  QStringLiteral("pane: missing background"),
                                  QStringLiteral("cursor: missing current mode"),
                                  QStringLiteral("menu: missing Pmenu association"),
                                  QStringLiteral("overlay: missing NormalFloat association"),
                                  QStringLiteral("text selection: missing Visual association") };
        QSignalSpy changed(&fixture.colors, &ColorPalette::paletteChanged);
        const auto pending = chromeSnapshot("light");
        fixture.colors.stageChromeSnapshot(pending.get());
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
        QVERIFY(!fixture.colors.chromeValid());
        QVERIFY(fixture.colors.theme().isEmpty());
        for(const char *role: chromeProperties)
            QVERIFY(!fixture.colors.property(role).value<QColor>().isValid());
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
