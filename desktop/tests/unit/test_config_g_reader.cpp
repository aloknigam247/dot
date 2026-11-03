#include <QSignalSpy>
#include <QtTest>

#include "Config.h"
#include "ConfigGGlobalReader.h"
#include "NvimConnector.h"
#include "support/RpcHelpers.h"

using namespace dot;
using namespace dot::test;

namespace {

void sendSnapshot(QTcpSocket *peer, quint32 id, const QString &value) {
    peer->write(packFrame([&](auto &pk) {
        pk.pack_array(4);
        pk.pack(1);
        pk.pack(id);
        pk.pack_nil();
        pk.pack_array(1);
        pk.pack_array(2);
        pk.pack(true);
        pk.pack(value.toStdString());
    }));
    peer->flush();
}

bool finishReplies(NvimConnector &conn, QTcpSocket *peer) {
    const auto complete = std::make_shared<bool>(false);
    conn.readGGlobals({ QStringLiteral("barrier") },
                      [complete](NvimConnector::GGlobalSnapshotResult result) {
        *complete = result.has_value();
    });
    const auto requests = readFrames(peer, 1);
    if(requests.size() != 1) return false;
    sendSnapshot(peer, requests[0].get().via.array.ptr[1].as<quint32>(), QStringLiteral("done"));
    QElapsedTimer timer;
    timer.start();
    while(!*complete && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return *complete;
}

} // namespace

class TestConfigGReader : public QObject {
    Q_OBJECT
private slots:
    void missingSnapshotValuesRemovePreviousGlobals() {
        Config cfg;
        cfg.registerOption(QStringLiteral("alpha"), ConfigType::Int, 1);
        cfg.registerOption(QStringLiteral("beta"), ConfigType::Int, 2);
        ConfigGGlobalReader::readFromMap(
            { { QStringLiteral("dot_alpha"), 11 }, { QStringLiteral("dot_beta"), 12 } }, cfg);
        QSignalSpy changed(&cfg, &Config::changed);
        connect(&cfg, &Config::changed, &cfg, [&] {
            QCOMPARE(cfg.value(QStringLiteral("alpha")).toInt(), 1);
            QCOMPARE(cfg.value(QStringLiteral("beta")).toInt(), 22);
        });

        ConfigGGlobalReader::readFromMap({ { QStringLiteral("dot_beta"), 22 } }, cfg);

        QCOMPARE(changed.count(), 2);
        QCOMPARE(changed.at(0).at(0).toString(), QStringLiteral("alpha"));
        QCOMPARE(changed.at(1).at(0).toString(), QStringLiteral("beta"));
    }

    void invalidSnapshotValueIsDiagnosedAndCleared() {
        Config cfg;
        cfg.registerOption(QStringLiteral("size"), ConfigType::Int, 1);
        cfg.setFromGGlobal(QStringLiteral("size"), 5);
        QTest::ignoreMessage(QtWarningMsg, "ConfigGGlobalReader: dot_size has an invalid type");

        ConfigGGlobalReader::readFromMap(
            { { QStringLiteral("dot_size"), QStringLiteral("invalid") } }, cfg);

        QCOMPARE(cfg.value(QStringLiteral("size")).toInt(), 1);
    }

    void newestSuccessfulSnapshotWins() {
        Config cfg;
        NvimConnector conn;
        QTcpServer server;
        auto *rpc = conn.findChild<MsgpackRpc *>();
        QVERIFY(rpc);
        auto *peer = connectPeer(*rpc, server);
        QVERIFY(peer);
        cfg.registerOption(QStringLiteral("theme"), ConfigType::String, QStringLiteral("dark"));
        ConfigGGlobalReader::read(conn, cfg);
        ConfigGGlobalReader::read(conn, cfg);
        const auto requests = readFrames(peer, 2);
        QCOMPARE(requests.size(), size_t(2));
        const auto firstId = requests[0].get().via.array.ptr[1].as<quint32>();
        const auto secondId = requests[1].get().via.array.ptr[1].as<quint32>();
        QSignalSpy changed(&cfg, &Config::changed);

        sendSnapshot(peer, secondId, QStringLiteral("light"));
        QTRY_COMPARE(cfg.value(QStringLiteral("theme")).toString(), QStringLiteral("light"));
        sendSnapshot(peer, firstId, QStringLiteral("old"));
        QVERIFY(finishReplies(conn, peer));

        QCOMPARE(cfg.value(QStringLiteral("theme")).toString(), QStringLiteral("light"));
        QCOMPARE(changed.count(), 1);
    }

    void mapReplacementInvalidatesPendingRead() {
        Config cfg;
        NvimConnector conn;
        QTcpServer server;
        auto *rpc = conn.findChild<MsgpackRpc *>();
        QVERIFY(rpc);
        auto *peer = connectPeer(*rpc, server);
        QVERIFY(peer);
        cfg.registerOption(QStringLiteral("theme"), ConfigType::String, QStringLiteral("dark"));
        ConfigGGlobalReader::read(conn, cfg);
        const auto requests = readFrames(peer, 1);
        QCOMPARE(requests.size(), size_t(1));
        ConfigGGlobalReader::readFromMap(
            { { QStringLiteral("dot_theme"), QStringLiteral("light") } }, cfg);
        sendSnapshot(peer, requests[0].get().via.array.ptr[1].as<quint32>(), QStringLiteral("old"));
        QVERIFY(finishReplies(conn, peer));

        QCOMPARE(cfg.value(QStringLiteral("theme")).toString(), QStringLiteral("light"));
    }

    void failedSnapshotPreservesPreviousGlobals() {
        Config cfg;
        NvimConnector conn;
        cfg.registerOption(QStringLiteral("theme"), ConfigType::String, QStringLiteral("dark"));
        cfg.setFromGGlobal(QStringLiteral("theme"), QStringLiteral("light"));
        QSignalSpy changed(&cfg, &Config::changed);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^ConfigGGlobalReader: .*"));

        ConfigGGlobalReader::read(conn, cfg);

        QCOMPARE(cfg.value(QStringLiteral("theme")).toString(), QStringLiteral("light"));
        QCOMPARE(changed.count(), 0);
    }

    void malformedSnapshotPreservesPreviousGlobals() {
        QFETCH(int, shape);
        Config cfg;
        NvimConnector conn;
        QTcpServer server;
        auto *rpc = conn.findChild<MsgpackRpc *>();
        QVERIFY(rpc);
        auto *peer = connectPeer(*rpc, server);
        QVERIFY(peer);
        cfg.registerOption(QStringLiteral("theme"), ConfigType::String, QStringLiteral("dark"));
        cfg.setFromGGlobal(QStringLiteral("theme"), QStringLiteral("light"));
        ConfigGGlobalReader::read(conn, cfg);
        const auto requests = readFrames(peer, 1);
        QCOMPARE(requests.size(), size_t(1));
        QSignalSpy changed(&cfg, &Config::changed);
        QTest::ignoreMessage(QtWarningMsg,
                             "ConfigGGlobalReader: Invalid g-global snapshot response");
        peer->write(packFrame([&](auto &pk) {
            pk.pack_array(4);
            pk.pack(1);
            pk.pack(requests[0].get().via.array.ptr[1].as<quint32>());
            pk.pack_nil();
            if(shape == 0) {
                pk.pack_nil();
                return;
            }
            pk.pack_array(1);
            if(shape == 1) {
                pk.pack_nil();
                return;
            }
            pk.pack_array(1);
            if(shape == 2) pk.pack(1);
            else pk.pack(true);
        }));
        peer->flush();
        QVERIFY(finishReplies(conn, peer));

        QCOMPARE(cfg.value(QStringLiteral("theme")).toString(), QStringLiteral("light"));
        QCOMPARE(changed.count(), 0);
    }

    void malformedSnapshotPreservesPreviousGlobals_data() {
        QTest::addColumn<int>("shape");
        QTest::newRow("missing-values") << 0;
        QTest::newRow("missing-entry") << 1;
        QTest::newRow("invalid-marker") << 2;
        QTest::newRow("missing-value") << 3;
    }

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
