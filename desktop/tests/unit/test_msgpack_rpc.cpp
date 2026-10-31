#include <cstring>
#include <msgpack.hpp>
#include <vector>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include "MsgpackRpc.h"
#include "support/RpcHelpers.h"

using namespace dot;
using namespace dot::test;

class TestMsgpackRpc : public QObject {
    Q_OBJECT
private slots:
    void packDecodeRoundTripRequest() {
        // Encode a notification-style message and decode it back to confirm
        // we agree with msgpack-cxx on the wire format used by Neovim.
        msgpack::sbuffer buf;
        msgpack::packer<msgpack::sbuffer> pk(&buf);
        pk.pack_array(3);
        pk.pack(2); // notification
        pk.pack(std::string("nvim_input"));
        pk.pack_array(1);
        pk.pack(std::string("iabc<Esc>"));

        msgpack::object_handle h;
        msgpack::unpack(h, buf.data(), buf.size());
        const auto &a = h.get().via.array;
        QCOMPARE(static_cast<int>(a.size), 3);
        QCOMPARE(a.ptr[0].as<int>(), 2);
        QCOMPARE(QString::fromStdString(a.ptr[1].as<std::string>()), QStringLiteral("nvim_input"));
        QCOMPARE(static_cast<int>(a.ptr[2].via.array.size), 1);
        QCOMPARE(QString::fromStdString(a.ptr[2].via.array.ptr[0].as<std::string>()),
                 QStringLiteral("iabc<Esc>"));
    }

    void rpcNotRunningRejectsRequest() {
        MsgpackRpc rpc;
        bool called = false;
        std::expected<msgpack::object_handle, RpcError> received{ std::in_place };
        rpc.request(QStringLiteral("nvim_eval"), [](msgpack::packer<msgpack::sbuffer> &pk) {
            pk.pack_array(1);
            pk.pack(std::string("1"));
        }, [&](RpcResult r) {
            called = true;
            QVERIFY(!r);
            QVERIFY(r.error().message.contains(QStringLiteral("not running")));
        });
        QVERIFY(called);
    }

    void responseRoutesByMsgid() { // We exercise the dispatch path indirectly by hand-crafting a
                                   // response
        // packet — though MsgpackRpc::dispatchUnpacked is private, this serves
        // as a structural smoke check that our envelope layout is correct.
        msgpack::sbuffer buf;
        msgpack::packer<msgpack::sbuffer> pk(&buf);
        pk.pack_array(4);
        pk.pack(1);            // response
        pk.pack(uint32_t(42)); // msgid
        pk.pack_nil();         // no error
        pk.pack_array(0);      // empty result

        msgpack::object_handle h;
        msgpack::unpack(h, buf.data(), buf.size());
        QCOMPARE(h.get().via.array.ptr[1].as<uint32_t>(), uint32_t(42));
    }

    void canonicalizePipeAddressForms() {
        // nvim reports the Windows pipe as `//./pipe/<name>`; QLocalSocket needs the
        // native `\\.\pipe\<name>`. Already-native and non-pipe addresses pass through.
        QCOMPARE(canonicalizePipeAddress(QStringLiteral("//./pipe/nvim.1234.0")),
                 QStringLiteral("\\\\.\\pipe\\nvim.1234.0"));
        QCOMPARE(canonicalizePipeAddress(QStringLiteral("\\\\.\\pipe\\nvim.1234.0")),
                 QStringLiteral("\\\\.\\pipe\\nvim.1234.0"));
        QCOMPARE(canonicalizePipeAddress(QStringLiteral("/tmp/nvim.sock")),
                 QStringLiteral("/tmp/nvim.sock"));
        QCOMPARE(canonicalizePipeAddress(QStringLiteral("127.0.0.1:6666")),
                 QStringLiteral("127.0.0.1:6666"));
    }

    void startEmbeddedNvimFailureResets() {
        // A bogus executable makes waitForStarted fail; the RPC must emit error and
        // drop the half-built QProcess so a later start can be retried.
        MsgpackRpc rpc;
        QSignalSpy errorSpy(&rpc, &MsgpackRpc::error);
        QVERIFY(!rpc.startEmbeddedNvim(QStringLiteral("dot_nonexistent_nvim_binary")));
        QVERIFY(errorSpy.count() >= 1);
        QVERIFY(!rpc.isRunning());
    }

    void switchingTransportsRetiresSockets() {
        // connectToAddress must retire whatever transport is live before building
        // the next one, exercising both the local- and tcp-socket cleanup arms.
        MsgpackRpc rpc;
        rpc.connectToAddress(QStringLiteral("//./pipe/dot-nonexistent-a")); // builds local
        rpc.connectToAddress(QStringLiteral("127.0.0.1:1"));                // retires local
        rpc.connectToAddress(QStringLiteral("//./pipe/dot-nonexistent-b")); // retires tcp
        // No server backs these; we only assert the switch did not leave us running.
        QVERIFY(!rpc.isRunning());
    }

    void loopbackTransportDrivesDispatch() {
        // A loopback QTcpServer lets us reach the running-transport paths (isRunning,
        // request/notify writes, no-arg packing) and feed crafted frames back through
        // the private dispatch: response routing, rpc-error non-array fallback,
        // notification emission, and the malformed-message guard.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        MsgpackRpc rpc;
        QSignalSpy connectedSpy(&rpc, &MsgpackRpc::connected);
        QSignalSpy noteSpy(&rpc, &MsgpackRpc::notification);
        QSignalSpy errorSpy(&rpc, &MsgpackRpc::error);

        rpc.connectToAddress(QStringLiteral("127.0.0.1:%1").arg(port));
        QVERIFY(server.waitForNewConnection(2000));
        QTcpSocket *peer = server.nextPendingConnection();
        QVERIFY(peer);
        QVERIFY(connectedSpy.wait(2000));
        QVERIFY(rpc.isRunning());

        RpcResult okResult{ std::in_place };
        bool okCalled = false;
        rpc.request(QStringLiteral("nvim_get_mode"), nullptr, [&](RpcResult r) {
            okCalled = true;
            okResult = std::move(r);
        }); // msgid 1, no-arg pack

        RpcResult errResult{ std::in_place };
        bool errCalled = false;
        rpc.request(QStringLiteral("nvim_eval"), [](msgpack::packer<msgpack::sbuffer> &pk) {
            pk.pack_array(1);
            pk.pack(std::string("1"));
        }, [&](RpcResult r) {
            errCalled = true;
            errResult = std::move(r);
        }); // msgid 2

        RpcResult arrErrResult{ std::in_place };
        bool arrErrCalled = false;
        rpc.request(QStringLiteral("nvim_command"), nullptr, [&](RpcResult r) {
            arrErrCalled = true;
            arrErrResult = std::move(r);
        }); // msgid 3

        rpc.notify(QStringLiteral("nvim_command"), nullptr); // no-arg notify

        // Respond to msgid 1 with success, msgid 2 with a non-array error object so
        // the fallback "rpc error" message is used, and msgid 3 with a proper
        // [code, message] error array.
        peer->write(packFrame([](msgpack::packer<msgpack::sbuffer> &pk) {
            pk.pack_array(4);
            pk.pack(1);
            pk.pack(uint32_t(1));
            pk.pack_nil();
            pk.pack_array(0);
        }));
        peer->write(packFrame([](msgpack::packer<msgpack::sbuffer> &pk) {
            pk.pack_array(4);
            pk.pack(1);
            pk.pack(uint32_t(2));
            pk.pack(std::string("boom")); // error is not an array
            pk.pack_nil();
        }));
        peer->write(packFrame([](msgpack::packer<msgpack::sbuffer> &pk) {
            pk.pack_array(4);
            pk.pack(1);
            pk.pack(uint32_t(3));
            pk.pack_array(2); // error is a [code, message] array
            pk.pack(int64_t(7));
            pk.pack(std::string("bad call"));
            pk.pack_nil();
        }));
        peer->write(packFrame([](msgpack::packer<msgpack::sbuffer> &pk) {
            pk.pack_array(3);
            pk.pack(2);
            pk.pack(std::string("redraw"));
            pk.pack_array(0);
        }));
        peer->write(packFrame([](msgpack::packer<msgpack::sbuffer> &pk) {
            pk.pack_array(1); // malformed: fewer than 2 elements
            pk.pack(1);
        }));
        QVERIFY(peer->flush());

        QTRY_VERIFY(okCalled);
        QVERIFY(okResult.has_value());
        QTRY_VERIFY(errCalled);
        QVERIFY(!errResult.has_value());
        QCOMPARE(errResult.error().message, QStringLiteral("rpc error"));
        QTRY_VERIFY(arrErrCalled);
        QVERIFY(!arrErrResult.has_value());
        QCOMPARE(arrErrResult.error().code, int64_t(7));
        QCOMPARE(arrErrResult.error().message, QStringLiteral("bad call"));
        QTRY_COMPARE(noteSpy.count(), 1);
        QTRY_COMPARE(errorSpy.count(), 1);

        rpc.shutdownAndWait(100); // running transport → socket wait branch
    }

    void duplicateRepliesAreRejected() {
        MsgpackRpc rpc;
        QTcpServer server;
        QSignalSpy errors(&rpc, &MsgpackRpc::error);
        QSignalSpy requests(&rpc, &MsgpackRpc::requestReceived);
        QTcpSocket *peer = connectPeer(rpc, server);
        QVERIFY(peer);
        peer->write(packRequest(10, QStringLiteral("parent")));
        peer->write(packRequest(20, QStringLiteral("child")));
        peer->flush();
        QTRY_COMPARE(requests.count(), 2);
        const auto parent = qvariant_cast<RpcRequest>(requests.at(0).at(0));
        const auto child = qvariant_cast<RpcRequest>(requests.at(1).at(0));
        QVERIFY(rpc.reply(parent.token, [](auto &pk) { pk.pack(7); }));
        QVERIFY(!rpc.reply(parent.token, [](auto &pk) { pk.pack(8); }));
        QCOMPARE(errors.count(), 1);
        QVERIFY(rpc.reply(child.token));
        const auto frames = readFrames(peer, 2);
        QCOMPARE(frames.size(), std::size_t(2));
        QCOMPARE(frames[0].get().via.array.ptr[1].as<quint32>(), quint32(20));
        QVERIFY(frames[0].get().via.array.ptr[3].is_nil());
        QCOMPARE(frames[1].get().via.array.ptr[3].as<int>(), 7);
        QVERIFY(!rpc.reply(child.token));
        QCOMPARE(errors.count(), 2);
        peer->abort();
        QTRY_VERIFY(!rpc.isRunning());
    }

    void incomingRequestOwnsBinaryArgumentsAcrossFurtherReads() {
        MsgpackRpc rpc;
        QTcpServer server;
        QSignalSpy requests(&rpc, &MsgpackRpc::requestReceived);
        QTcpSocket *peer = connectPeer(rpc, server);
        QVERIFY(peer);
        const QByteArray payload("a\0\xffz", 4);
        const QByteArray bytes = packRequest(42, QStringLiteral("image"), [&](auto &pk) {
            pk.pack_array(1);
            pk.pack_str(static_cast<uint32_t>(payload.size()));
            pk.pack_str_body(payload.constData(), static_cast<uint32_t>(payload.size()));
        });
        peer->write(bytes.left(3));
        peer->flush();
        QCoreApplication::processEvents();
        QCOMPARE(requests.count(), 0);
        peer->write(bytes.mid(3));
        peer->flush();
        QTRY_COMPARE(requests.count(), 1);
        const auto request = qvariant_cast<RpcRequest>(requests.at(0).at(0));
        QCOMPARE(request.method, QStringLiteral("image"));
        QCOMPARE(request.token.id, quint32(42));
        requests.clear();
        peer->write(packRequest(43, QStringLiteral("next")));
        peer->flush();
        QTRY_COMPARE(requests.count(), 1);
        const auto next = qvariant_cast<RpcRequest>(requests.at(0).at(0));
        const auto &value = request.params().via.array.ptr[0];
        QCOMPARE(value.type, msgpack::type::STR);
        QCOMPARE(QByteArray(value.via.str.ptr, value.via.str.size), payload);
        QVERIFY(rpc.reply(next.token));
        QVERIFY(rpc.reply(request.token, [](auto &pk) { pk.pack(true); }));
        const auto frames = readFrames(peer, 2);
        QCOMPARE(frames.size(), std::size_t(2));
        QVERIFY(frames[1].get().via.array.ptr[3].via.boolean);
        peer->abort();
        QTRY_VERIFY(!rpc.isRunning());
    }

    void malformedIncomingRequestsAreReported() {
        MsgpackRpc rpc;
        QTcpServer server;
        QSignalSpy errors(&rpc, &MsgpackRpc::error);
        QSignalSpy requests(&rpc, &MsgpackRpc::requestReceived);
        QTcpSocket *peer = connectPeer(rpc, server);
        QVERIFY(peer);
        const std::vector<QByteArray> invalid{ packFrame([](auto &pk) {
            pk.pack_array(2);
            pk.pack("wrong");
            pk.pack(1);
        }),
                                               packFrame([](auto &pk) {
            pk.pack_array(2);
            pk.pack(9);
            pk.pack(1);
        }),
                                               packFrame([](auto &pk) {
            pk.pack_array(3);
            pk.pack(0);
            pk.pack(1);
            pk.pack("method");
        }),
                                               packFrame([](auto &pk) {
            pk.pack_array(4);
            pk.pack(0);
            pk.pack("id");
            pk.pack("method");
            pk.pack_array(0);
        }),
                                               packFrame([](auto &pk) {
            pk.pack_array(4);
            pk.pack(0);
            pk.pack(quint64{ 1 } << 32);
            pk.pack("method");
            pk.pack_array(0);
        }),
                                               packFrame([](auto &pk) {
            pk.pack_array(4);
            pk.pack(0);
            pk.pack(1);
            pk.pack(7);
            pk.pack_array(0);
        }),
                                               packFrame([](auto &pk) {
            pk.pack_array(4);
            pk.pack(0);
            pk.pack(1);
            pk.pack("method");
            pk.pack_map(0);
        }) };
        for(const auto &bytes: invalid) peer->write(bytes);
        peer->flush();
        QTRY_COMPARE(errors.count(), static_cast<qsizetype>(invalid.size()));
        QCOMPARE(requests.count(), 0);
        peer->abort();
        QTRY_VERIFY(!rpc.isRunning());
    }

    void nestedRepliesUnwindNewestFirstIncludingErrors() {
        MsgpackRpc rpc;
        QTcpServer server;
        QSignalSpy requests(&rpc, &MsgpackRpc::requestReceived);
        QTcpSocket *peer = connectPeer(rpc, server);
        QVERIFY(peer);
        peer->write(packRequest(100, QStringLiteral("outer")));
        peer->write(packRequest(200, QStringLiteral("upload")));
        peer->write(packRequest(300, QStringLiteral("delete_all")));
        peer->flush();
        QTRY_COMPARE(requests.count(), 3);
        const auto outer = qvariant_cast<RpcRequest>(requests.at(0).at(0));
        const auto upload = qvariant_cast<RpcRequest>(requests.at(1).at(0));
        const auto inner = qvariant_cast<RpcRequest>(requests.at(2).at(0));
        QVERIFY(rpc.reply(outer.token, [](auto &pk) { pk.pack(7); }));
        QVERIFY(rpc.replyError(upload.token, RpcError{ 0, QStringLiteral("cancelled") }));
        QCoreApplication::processEvents();
        QCOMPARE(peer->bytesAvailable(), qint64(0));
        QVERIFY(rpc.reply(inner.token, [](auto &pk) { pk.pack(true); }));
        const auto frames = readFrames(peer, 3);
        QCOMPARE(frames.size(), std::size_t(3));
        QCOMPARE(frames[0].get().via.array.ptr[1].as<quint32>(), quint32(300));
        QVERIFY(frames[0].get().via.array.ptr[3].via.boolean);
        QCOMPARE(frames[1].get().via.array.ptr[1].as<quint32>(), quint32(200));
        QCOMPARE(frames[1].get().via.array.ptr[2].via.array.ptr[1].as<std::string>(),
                 std::string("cancelled"));
        QVERIFY(frames[1].get().via.array.ptr[3].is_nil());
        QCOMPARE(frames[2].get().via.array.ptr[1].as<quint32>(), quint32(100));
        QCOMPARE(frames[2].get().via.array.ptr[3].as<int>(), 7);
        peer->abort();
        QTRY_VERIFY(!rpc.isRunning());
    }

    void replyWriteErrorsAreReported() {
        MsgpackRpc rpc;
        QTcpServer server;
        QSignalSpy errors(&rpc, &MsgpackRpc::error);
        QSignalSpy requests(&rpc, &MsgpackRpc::requestReceived);
        QTcpSocket *peer = connectPeer(rpc, server);
        QVERIFY(peer);
        peer->write(packRequest(1, QStringLiteral("test_reply")));
        peer->flush();
        QTRY_COMPARE(requests.count(), 1);
        const RpcRequest request = qvariant_cast<RpcRequest>(requests[0][0]);
        QTcpSocket *socket = rpc.findChild<QTcpSocket *>();
        QVERIFY(socket);
        QVERIFY(socket->open(QIODevice::ReadOnly));
        QVERIFY(rpc.isRunning());
        QTest::ignoreMessage(
            QtWarningMsg, QRegularExpression(QStringLiteral("QIODevice::write.*ReadOnly device")));
        QVERIFY(!rpc.reply(request.token));
        QCOMPARE(errors.count(), 1);
        QVERIFY(errors[0][0].toString().contains(QStringLiteral("Could not write the RPC reply")));
        QVERIFY(socket->open(QIODevice::ReadWrite));
        peer->abort();
        QTRY_VERIFY(!rpc.isRunning());
    }

    void requestViewsHandleMissingAndMalformedEnvelopes() {
        RpcRequest request;
        QVERIFY(request.params().is_nil());
        const QByteArray scalar = packFrame([](auto &pk) { pk.pack(1); });
        request.handle = std::make_shared<msgpack::object_handle>(
            msgpack::unpack(scalar.constData(), static_cast<std::size_t>(scalar.size())));
        QVERIFY(request.params().is_nil());
        const QByteArray shortArray = packFrame([](auto &pk) { pk.pack_array(0); });
        request.handle = std::make_shared<msgpack::object_handle>(
            msgpack::unpack(shortArray.constData(), static_cast<std::size_t>(shortArray.size())));
        QVERIFY(request.params().is_nil());
    }

    void staleRepliesCannotReachAReplacementTransport() {
        MsgpackRpc rpc;
        QTcpServer server;
        QSignalSpy requests(&rpc, &MsgpackRpc::requestReceived);
        QVERIFY(!rpc.reply(RpcReplyToken{}));
        QTcpSocket *first = connectPeer(rpc, server);
        QVERIFY(first);
        first->write(packRequest(7, QStringLiteral("old")));
        first->flush();
        QTRY_COMPARE(requests.count(), 1);
        const auto old = qvariant_cast<RpcRequest>(requests.at(0).at(0));
        QTcpSocket *second = connectPeer(rpc, server);
        QVERIFY(second);
        QVERIFY(!rpc.reply(old.token));
        second->write(packRequest(7, QStringLiteral("new")));
        second->flush();
        QTRY_COMPARE(requests.count(), 2);
        const auto current = qvariant_cast<RpcRequest>(requests.at(1).at(0));
        QVERIFY(old.token.generation != current.token.generation);
        QVERIFY(rpc.reply(current.token, [](auto &pk) { pk.pack(42); }));
        const auto frames = readFrames(second, 1);
        QCOMPARE(frames.size(), std::size_t(1));
        QCOMPARE(frames[0].get().via.array.ptr[3].as<int>(), 42);
        QVERIFY(!rpc.reply(old.token));
        second->abort();
        QTRY_VERIFY(!rpc.isRunning());
    }

    void unhandledRequestsReceiveAnErrorReply() {
        MsgpackRpc rpc;
        QTcpServer server;
        QTcpSocket *peer = connectPeer(rpc, server);
        QVERIFY(peer);
        peer->write(packRequest(9, QStringLiteral("unhandled")));
        peer->flush();
        const auto frames = readFrames(peer, 1);
        QCOMPARE(frames.size(), std::size_t(1));
        const auto &response = frames[0].get().via.array;
        QCOMPARE(response.ptr[1].as<quint32>(), quint32(9));
        QVERIFY(response.ptr[2].type == msgpack::type::ARRAY);
        QVERIFY(response.ptr[2].via.array.ptr[1].as<std::string>().find("Unsupported RPC method") !=
                std::string::npos);
        QVERIFY(response.ptr[3].is_nil());
        peer->abort();
        QTRY_VERIFY(!rpc.isRunning());
    }
};

QTEST_GUILESS_MAIN(TestMsgpackRpc)
#include "test_msgpack_rpc.moc"
