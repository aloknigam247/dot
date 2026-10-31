#pragma once

#include "MsgpackRpc.h"

#include <cstring>
#include <vector>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

namespace dot::test {

inline QByteArray packFrame(const PackFn &fn) {
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    fn(pk);
    return QByteArray(buf.data(), static_cast<qsizetype>(buf.size()));
}

inline QByteArray packRequest(quint32 id, const QString &method, const PackFn &packArgs = {}) {
    return packFrame([&](auto &pk) {
        pk.pack_array(4);
        pk.pack(0);
        pk.pack(id);
        pk.pack(method.toStdString());
        if(packArgs) packArgs(pk);
        else pk.pack_array(0);
    });
}

inline QTcpSocket *connectPeer(MsgpackRpc &rpc, QTcpServer &server) {
    QElapsedTimer timer;
    if(!server.isListening() && !server.listen(QHostAddress::LocalHost, 0)) return nullptr;
    rpc.connectToAddress(QStringLiteral("127.0.0.1:%1").arg(server.serverPort()));
    if(!server.hasPendingConnections() && !server.waitForNewConnection(3000)) return nullptr;
    QTcpSocket *peer = server.nextPendingConnection();
    timer.start();
    while(!rpc.isRunning() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return rpc.isRunning() ? peer : nullptr;
}

inline std::vector<msgpack::object_handle> readFrames(QTcpSocket *peer, std::size_t count) {
    msgpack::unpacker decoder;
    std::vector<msgpack::object_handle> frames;
    QElapsedTimer timer;
    frames.reserve(count);
    timer.start();
    while(frames.size() < count && timer.elapsed() < 3000) {
        msgpack::object_handle frame;
        const QByteArray bytes = peer->readAll();
        if(!bytes.isEmpty()) {
            decoder.reserve_buffer(static_cast<std::size_t>(bytes.size()));
            std::memcpy(decoder.buffer(), bytes.constData(),
                        static_cast<std::size_t>(bytes.size()));
            decoder.buffer_consumed(static_cast<std::size_t>(bytes.size()));
            while(decoder.next(frame)) frames.push_back(std::move(frame));
        }
        if(frames.size() < count) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return frames;
}

} // namespace dot::test
