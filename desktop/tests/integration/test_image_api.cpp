#include "ImageItem.h"
#include "IntegrationHelpers.h"
#include "MsgpackRpc.h"
#include "NvimConnector.h"
#include "support/ImageHelpers.h"
#include "support/ImageRenderHelpers.h"
#include "support/RpcHelpers.h"

#include <limits>
#include <QFile>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <QUuid>

namespace dot {
namespace {

using namespace dot::test;

template <typename Predicate>
bool waitUntil(Predicate &&predicate, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while(!predicate()) {
        if(timer.elapsed() >= timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

bool startImageNvim(NvimConnector &conn, const QStringList &arguments = {}) {
    QSignalSpy flushed(&conn, &NvimConnector::flush);
    if(!startTestNvim(conn, arguments) || !conn.attachUi(80, 24)) return false;
    if(!waitUntil([&] { return conn.attached() && !flushed.isEmpty(); })) return false;
    return waitUntil([&] {
        const auto entered = evalSync(conn, QStringLiteral("v:vim_did_enter"));
        return entered && entered->toBool();
    });
}

std::expected<void, QString> runLua(NvimConnector &conn, const QString &body) {
    std::optional<std::expected<void, QString>> result;
    QObject receiver;
    QObject::connect(&conn, &NvimConnector::customNotification, &receiver,
                     [&](const Notification &note) {
        if(note.method != QStringLiteral("dot_image_test_result")) return;
        const auto &params = note.params();
        if(params.type != msgpack::type::ARRAY || params.via.array.size != 2 ||
           params.via.array.ptr[0].type != msgpack::type::BOOLEAN ||
           params.via.array.ptr[1].type != msgpack::type::STR) {
            result = std::unexpected(QStringLiteral("Malformed image test result notification"));
            return;
        }
        if(params.via.array.ptr[0].via.boolean) {
            result.emplace();
        } else {
            const auto &error = params.via.array.ptr[1].via.str;
            result = std::unexpected(QString::fromUtf8(error.ptr, error.size));
        }
    });
    conn.execLua(QStringLiteral(R"lua(
local ok, err = xpcall(function()
%1
end, debug.traceback)
local channel
for _, info in ipairs(vim.api.nvim_list_chans()) do
  if info.client and info.client.attributes and info.client.attributes.dot_backend then
    channel = info.id
    break
  end
end
assert(channel, "Image test channel not found")
vim.rpcnotify(channel, "dot_image_test_result", ok, ok and "" or tostring(err))
)lua")
                     .arg(body));
    if(!waitUntil([&] { return result.has_value(); })) {
        return std::unexpected(QStringLiteral("Timed out waiting for the image Lua assertions"));
    }
    return std::move(*result);
}

QImage capture(QQuickItem *item) {
    const auto grab = item->grabToImage();
    if(!grab) return {};
    if(!waitUntil([&] { return !grab->image().isNull(); })) return {};
    return grab->image();
}

QString pngLiteral(QColor color = Qt::red) {
    return QString::fromLatin1(makePng(QSize(4, 4), QImage::Format_ARGB32, color).toBase64());
}

} // namespace

class TestImageApi : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        qRegisterMetaType<dot::Notification>();
        qRegisterMetaType<dot::ObjectHandlePtr>();
        NvimConnector conn;
        QVERIFY(startImageNvim(conn));
        const auto runtimeHasImages = evalSync(
            conn, QStringLiteral("len(nvim_get_runtime_file('lua/vim/ui/img.lua', v:false)) > 0"));
        QVERIFY(runtimeHasImages.has_value());
        m_supported = runtimeHasImages->toBool();
        QCOMPARE(conn.imagesAvailable(), m_supported);
        if(qEnvironmentVariableIntValue("DOT_REQUIRE_IMAGE_API") != 0) {
            QVERIFY2(m_supported, "This CI variant requires Neovim's experimental image API");
        }
    }

    void apiRoundTripPreservesOptionsBinaryPixelsAndErrors() {
        if(!m_supported) QSKIP("The installed Neovim has no image API");
        NvimConnector conn;
        QVERIFY(startImageNvim(conn));
        const auto result = runLua(conn, QStringLiteral(R"lua(
local img = vim.ui.img
assert(img == require("vim.ui.img"))
assert(img._supported())
local png = vim.base64.decode("%1")
local id = img.set(png, {col = 3, height = 4, row = 2, width = 6, zindex = -2147483648})
vim.g.image_test_id = id
assert(type(id) == "number" and id > 0)
local opts = img.get(id)
assert(opts.col == 3 and opts.row == 2 and opts.width == 6 and opts.height == 4)
assert(opts.zindex == -2147483648)
opts.row = 99
assert(img.get(id).row == 2)
assert(img.set(id, {row = 5, zindex = 2147483647}) == id)
assert(img.get(id).row == 5 and img.get(id).width == 6)
assert(img.get(id).zindex == 2147483647)
assert(img.get(4294967295) == nil, "Missing image IDs must return Lua nil")
assert(img.del(4294967295) == false)
local function rejected(fn) assert(not pcall(fn)) end
rejected(function() img.set("not a PNG", {}) end)
rejected(function() img.set(id, {row = 0}) end)
rejected(function() img.set(id, {width = -1}) end)
rejected(function() img.set(id, {col = 1.5}) end)
rejected(function() img.set(id, {zindex = math.huge}) end)
rejected(function() img.set(id, {row = 2147483648}) end)
rejected(function() img.set(id, {unknown = 1}) end)
rejected(function() img.set(id, {1, 2}) end)
rejected(function() img.set(id, {[1] = 2, row = 3}) end)
rejected(function() img.set(id, false) end)
rejected(function() img.set(false, {}) end)
rejected(function() img.get(0) end)
rejected(function() img.get(1.25) end)
rejected(function() img.del(4294967296) end)
rejected(function() img.set(4294967295, {}) end)
rejected(function() img.set(string.rep("x", %2 + 1), {}) end)
assert(img.get(id).row == 5)
assert(img.del(id))
assert(img.get(id) == nil and not img.del(id))
local natural = img.set(png)
assert(next(img.get(natural)) == nil)
assert(img.del(math.huge))
assert(not img.del(math.huge))
vim.g.image_test_id = img.set(png, {row = 3})
)lua")
                                             .arg(pngLiteral())
                                             .arg(ImageModel::kMaxEncodedBytes));
        QVERIFY2(result.has_value(), qPrintable(result ? QString{} : result.error()));
        const auto id = getVarSync(conn, QStringLiteral("image_test_id"));
        QVERIFY(id.has_value());
        const auto it = conn.images()->placements().constFind(id->toUInt());
        QVERIFY(it != conn.images()->placements().cend());
        QCOMPARE(it->pixels.size(), QSize(4, 4));
        QCOMPARE(it->pixels.pixelColor(0, 0), QColor(Qt::red));
    }

    void backendIsInstalledBeforeUserInit() {
        if(!m_supported) QSKIP("The installed Neovim has no image API");
        NvimConnector conn;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) +
                                                QStringLiteral(".lua"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray init = QStringLiteral(R"lua(
assert(vim.ui.img == require("vim.ui.img"))
assert(vim.ui.img._supported())
vim.g.image_init_id = vim.ui.img.set(vim.base64.decode("%1"), {col = 2, row = 3, width = 4})
vim.g.image_init_ok = true
)lua")
                                    .arg(pngLiteral())
                                    .toUtf8();
        QCOMPARE(file.write(init), qint64(init.size()));
        file.close();
        QVERIFY(startImageNvim(conn, { QStringLiteral("-u"), path }));
        const auto result =
            runLua(conn, QStringLiteral("assert(vim.g.image_init_ok and vim.g.image_init_id > 0)"));
        QVERIFY2(result.has_value(), qPrintable(result ? QString{} : result.error()));
        QCOMPARE(conn.images()->count(), qsizetype(1));
    }

    void coalescedNestedRepliesKeepTheUiAttached() {
        std::optional<RpcReplyToken> outer;
        int requests = 0;
        NvimConnector conn;
        QVERIFY(startImageNvim(conn));
        MsgpackRpc *rpc = conn.findChild<MsgpackRpc *>();
        QVERIFY(rpc);
        QProcess *process = rpc->findChild<QProcess *>();
        QVERIFY(process);
        QVERIFY(QObject::disconnect(rpc, &MsgpackRpc::requestReceived, &conn,
                                    &NvimConnector::onRequest));
        QObject::connect(rpc, &MsgpackRpc::requestReceived, &conn, [&](const RpcRequest &request) {
            ++requests;
            if(request.method == QStringLiteral("coalesced_outer")) {
                outer = request.token;
                const auto channel = request.params().via.array.ptr[0].as<quint64>();
                conn.execLua(QStringLiteral("vim.g.image_inner_done="
                                            "(vim.rpcrequest(%1,'coalesced_inner')=='inner done')")
                                 .arg(channel));
                return;
            }
            QCOMPARE(request.method, QStringLiteral("coalesced_inner"));
            QVERIFY(outer.has_value());
            const QByteArray replies = packFrame([&](auto &pk) {
                pk.pack_array(4);
                pk.pack(1);
                pk.pack(request.token.id);
                pk.pack_nil();
                pk.pack("inner done");
                pk.pack_array(4);
                pk.pack(1);
                pk.pack(outer->id);
                pk.pack_nil();
                pk.pack("outer done");
            });
            QCOMPARE(process->write(replies), qint64(replies.size()));
        });
        const auto result = runLua(conn, QStringLiteral(R"lua(
local channel, client_type
for _, info in ipairs(vim.api.nvim_list_chans()) do
  if info.client and info.client.attributes and info.client.attributes.dot_backend then
    channel = info.id
    client_type = info.client.type
  end
end
assert(channel)
assert(vim.rpcrequest(channel, "coalesced_outer", channel) == "outer done")
assert(vim.g.image_inner_done)
assert(client_type == "msgpack-rpc")
)lua"));
        QVERIFY2(result.has_value(), qPrintable(result ? QString{} : result.error()));
        QCOMPARE(requests, 2);
        QVERIFY(conn.attached());
        conn.command(QStringLiteral("qa!"));
        QVERIFY(waitUntil([&] { return !rpc->isRunning(); }));
    }

    void duplicatePendingAttachDoesNotSendAnotherRequest() {
        NvimConnector conn;
        QVERIFY(startTestNvim(conn));
        QVERIFY(conn.attachUi(80, 24));
        QVERIFY(conn.attachUi(109, 35));
        QCOMPARE(conn.m_sentUiSizes.size(), std::size_t(1));
        QVERIFY(conn.m_attachPending);
        QVERIFY(waitUntil([&] {
            return conn.attached() && conn.grid()->cols() == 80 && conn.grid()->rows() == 24;
        }));
        QVERIFY(!conn.m_attachPending);
        conn.tryResize(109, 35);
        QVERIFY(waitUntil([&] { return conn.grid()->cols() == 109 && conn.grid()->rows() == 35; }));
    }

    void gridScrollMovesAndClipsTheImage() {
        if(!m_supported) QSKIP("The installed Neovim has no image API");
        NvimConnector conn;
        QVERIFY(startImageNvim(conn));
        const auto result = runLua(conn, QStringLiteral(R"lua(
local lines={}
for i=1,100 do lines[i]=tostring(i) end
vim.api.nvim_buf_set_lines(0,0,-1,false,lines)
vim.api.nvim_win_set_cursor(0,{10,0})
vim.cmd("redraw!")
local id=vim.ui.img.set(vim.base64.decode("%1"),{col=2,row=2,width=4,height=4})
vim.cmd([[execute "normal! 4\<C-e>"]])
vim.cmd("redraw")
assert(vim.fn.winsaveview().topline == 5)
vim.g.image_scroll_id=id
)lua")
                                             .arg(pngLiteral()));
        QVERIFY2(result.has_value(), qPrintable(result ? QString{} : result.error()));
        const auto id = getVarSync(conn, QStringLiteral("image_scroll_id"));
        QVERIFY(id.has_value());
        const auto image = conn.images()->placements().constFind(id->toUInt());
        QVERIFY(image != conn.images()->placements().cend());
        QCOMPARE(image->position, QPointF(1, -3));
        QVERIFY(image->clip.has_value());
    }

    void imageCallsDoNotRepaintTheRealConnectedGrid() {
        if(!m_supported) QSKIP("The installed Neovim has no image API");
        NvimConnector conn;
        QQuickWindow window;
        CountingGrid grid;
        CountingImages images;
        grid.setParentItem(window.contentItem());
        grid.setConnector(&conn);
        grid.setSize(QSizeF(80 * grid.cellWidth(), 24 * grid.cellHeight()));
        window.resize(qCeil(grid.width()), qCeil(grid.height()));
        images.setGrid(&grid);
        images.setModel(conn.images());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(startImageNvim(conn));
        auto prepared = runLua(
            conn,
            QStringLiteral(
                "vim.opt.laststatus=0; vim.opt.ruler=false; vim.opt.showcmd=false; vim.cmd('redraw!')"));
        QVERIFY2(prepared.has_value(), qPrintable(prepared ? QString{} : prepared.error()));
        QTest::qWait(100);
        const int backgrounds = grid.backgrounds.load();
        const int foregrounds = grid.foregrounds.load();
        QVERIFY(backgrounds > 0 && foregrounds > 0);
        QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
        auto created = runLua(
            conn,
            QStringLiteral(
                "vim.g.image_live_id=vim.ui.img.set(vim.base64.decode('%1'), {col=3,row=3,width=6})")
                .arg(pngLiteral()));
        QVERIFY2(created.has_value(), qPrintable(created ? QString{} : created.error()));
        QVERIFY(waitUntil([&] { return !frames.isEmpty(); }));
        QCOMPARE(grid.backgrounds.load(), backgrounds);
        QCOMPARE(grid.foregrounds.load(), foregrounds);
        frames.clear();
        auto updated = runLua(
            conn, QStringLiteral("vim.ui.img.set(vim.g.image_live_id,{col=5,row=4,zindex=-1})"));
        QVERIFY2(updated.has_value(), qPrintable(updated ? QString{} : updated.error()));
        QVERIFY(waitUntil([&] { return !frames.isEmpty(); }));
        QCOMPARE(grid.backgrounds.load(), backgrounds);
        QCOMPARE(grid.foregrounds.load(), foregrounds);
        frames.clear();
        auto deleted = runLua(conn, QStringLiteral("assert(vim.ui.img.del(vim.g.image_live_id))"));
        QVERIFY2(deleted.has_value(), qPrintable(deleted ? QString{} : deleted.error()));
        QVERIFY(waitUntil([&] { return !frames.isEmpty(); }));
        QCOMPARE(grid.backgrounds.load(), backgrounds);
        QCOMPARE(grid.foregrounds.load(), foregrounds);
        auto changed = runLua(
            conn,
            QStringLiteral(
                "vim.api.nvim_buf_set_lines(0,0,-1,false,{'positive control'}); vim.cmd('redraw')"));
        QVERIFY2(changed.has_value(), qPrintable(changed ? QString{} : changed.error()));
        QVERIFY(waitUntil([&] {
            return grid.backgrounds.load() > backgrounds && grid.foregrounds.load() > foregrounds;
        }));
    }

    void missingImageApiDisablesImagesWithVisibleError() {
        NvimConnector conn;
        QVERIFY(startImageNvim(conn));
        const auto setup = runLua(conn, QStringLiteral(R"lua(
local lookup = vim.api.nvim_get_runtime_file
vim.api.nvim_get_runtime_file = function(pattern, all)
  if pattern == "lua/vim/ui/img.lua" then return {} end
  return lookup(pattern, all)
end
)lua"));
        QVERIFY2(setup.has_value(), qPrintable(setup ? QString{} : setup.error()));
        QTest::ignoreMessage(QtWarningMsg,
                             "dot: Neovim image API is unavailable; images are disabled.");
        conn.installDotBackend();
        QVERIFY(waitUntil([&] {
            const auto messages = evalSync(conn, QStringLiteral("execute('messages')"));
            return messages && messages->toString().contains(QStringLiteral(
                                   "Neovim image API is unavailable; images are disabled."));
        }));
        QVERIFY(!conn.imagesAvailable());
        QVERIFY(conn.attached());
        conn.input(QStringLiteral("iordinary editing<Esc>"));
        QVERIFY(waitUntil([&] {
            const auto line = evalSync(conn, QStringLiteral("getline(1)"));
            return line && line->toString() == QStringLiteral("ordinary editing");
        }));
    }

    void nestedCancellationRepliesWithoutDeadlockingNeovim() {
        if(!m_supported) QSKIP("The installed Neovim has no image API");
        bool nested = false;
        QSemaphore entered;
        QSemaphore release;
        NvimConnector conn;
        const auto unblock = qScopeGuard([&] { release.release(); });
        QVERIFY(startImageNvim(conn));
        // Keep the parent request pending while the nested cancellation runs.
        conn.images()->m_pool.start([&] {
            entered.release();
            release.acquire();
        });
        QVERIFY(entered.tryAcquire(1, 5000));
        MsgpackRpc *rpc = conn.findChild<MsgpackRpc *>();
        QVERIFY(rpc);
        const auto hook = QObject::connect(rpc, &MsgpackRpc::requestReceived, &conn,
                                           [&](const RpcRequest &request) {
            if(nested || request.method != QStringLiteral("dot_image")) return;
            const auto &params = request.params();
            if(params.type != msgpack::type::ARRAY || params.via.array.size != 3) return;
            if(params.via.array.ptr[1].type != msgpack::type::STR) return;
            nested = true;
            conn.execLua(QStringLiteral(
                "vim.g.image_nested_deleted=vim.ui.img.del(math.huge); vim.g.image_nested_done=true"));
        });
        const auto result = runLua(conn, QStringLiteral(R"lua(
local ok, err = pcall(vim.ui.img.set, vim.base64.decode("%1"), {})
assert(not ok and tostring(err):find("cancelled"))
assert(vim.g.image_nested_done and vim.g.image_nested_deleted)
)lua")
                                             .arg(pngLiteral()));
        QObject::disconnect(hook);
        QVERIFY2(result.has_value(), qPrintable(result ? QString{} : result.error()));
        QVERIFY(nested);
        QCOMPARE(conn.images()->count(), qsizetype(0));
        QVERIFY(conn.attached());
    }

    void productionShellRendersImagesAndKeepsKeyboardFocus() {
        if(!m_supported) QSKIP("The installed Neovim has no image API");
        NvimConnector conn;
        QQmlApplicationEngine engine;
        QVERIFY(startTestNvim(conn));
        engine.rootContext()->setContextProperty(QStringLiteral("$connector"), &conn);
        engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(waitForAttach(&conn));
        QVERIFY(waitUntil([&] { return conn.imagesAvailable(); }));
        const auto result = runLua(conn, QStringLiteral(R"lua(
vim.opt.guicursor="a:block-blinkwait0-blinkon0-blinkoff0"
vim.g.image_shell_id=vim.ui.img.set(vim.base64.decode("%1"),{col=5,row=5,width=8})
)lua")
                                             .arg(pngLiteral()));
        QVERIFY2(result.has_value(), qPrintable(result ? QString{} : result.error()));
        auto *layer = window->findChild<ImageItem *>(QStringLiteral("imageLayer"));
        QVERIFY(layer);
        const auto id = getVarSync(conn, QStringLiteral("image_shell_id"));
        QVERIFY(id.has_value());
        QQuickItem *imageItem =
            layer->findChild<QQuickItem *>(QStringLiteral("image_%1").arg(id->toUInt()));
        QVERIFY(imageItem);
        const QImage pixels = capture(window->contentItem());
        QVERIFY2(!pixels.isNull(), "The real QML shell produced no captured pixels");
        const QPointF center = imageItem->mapToItem(
            window->contentItem(), QPointF(imageItem->width() / 2, imageItem->height() / 2));
        const QPoint point(qRound(center.x()), qRound(center.y()));
        QVERIFY(pixels.rect().contains(point));
        QCOMPARE(pixels.pixelColor(point), QColor(Qt::red));
        QQuickItem *focus = window->activeFocusItem();
        QVERIFY(qobject_cast<GridItem *>(focus));
        QTest::keyClick(window, Qt::Key_I);
        for(char c: QByteArrayLiteral("imagefocus")) QTest::keyClick(window, c);
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(waitUntil([&] {
            const auto line = evalSync(conn, QStringLiteral("getline('.')"));
            return line && line->toString() == QStringLiteral("imagefocus");
        }));
        QCOMPARE(window->activeFocusItem(), focus);
        QTest::keyClick(window, Qt::Key_Colon);
        for(char c: QByteArrayLiteral("let g:image_focus_ok = 42")) QTest::keyClick(window, c);
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(waitUntil([&] {
            const auto value = getVarSync(conn, QStringLiteral("image_focus_ok"));
            return value && value->toInt() == 42;
        }));
        QCOMPARE(window->activeFocusItem(), focus);
    }

    void rawRpcRejectsMalformedOptionsAndAcceptsBinaryPayloads() {
        NvimConnector conn;
        QTcpServer server;
        MsgpackRpc *rpc = conn.findChild<MsgpackRpc *>();
        QVERIFY(rpc);
        QTcpSocket *peer = connectPeer(*rpc, server);
        QVERIFY(peer);
        conn.setImagesAvailable(true);
        peer->write(packRequest(0, QStringLiteral("unsupported_image_method")));
        peer->flush();
        const auto unsupported = readFrames(peer, 1);
        QCOMPARE(unsupported.size(), std::size_t(1));
        const auto &unsupportedError = unsupported[0].get().via.array.ptr[2];
        QVERIFY(unsupportedError.type == msgpack::type::ARRAY);
        QVERIFY(QString::fromStdString(unsupportedError.via.array.ptr[1].as<std::string>())
                    .contains(QStringLiteral("Unsupported RPC method")));
        struct Invalid {
            PackFn arguments;
            QString expected;
        };
        const std::vector<Invalid> invalid{ { [](auto &pk) { pk.pack_array(0); },
                                              QStringLiteral("Invalid image RPC") },
                                            { [](auto &pk) {
            pk.pack_array(1);
            pk.pack(true);
        }, QStringLiteral("Invalid image RPC") },
                                            { [](auto &pk) {
            pk.pack_array(1);
            pk.pack("unknown");
        }, QStringLiteral("Unknown image action") },
                                            { [](auto &pk) {
            pk.pack_array(1);
            pk.pack("set");
        }, QStringLiteral("set requires") },
                                            { [](auto &pk) {
            pk.pack_array(2);
            pk.pack("delete_all");
            pk.pack(1);
        }, QStringLiteral("takes no") },
                                            { [](auto &pk) {
            pk.pack_array(1);
            pk.pack("get");
        }, QStringLiteral("exactly one") },
                                            { [](auto &pk) {
            pk.pack_array(3);
            pk.pack("set");
            pk.pack(1);
            pk.pack_map(2);
            pk.pack("row");
            pk.pack(1);
            pk.pack("row");
            pk.pack(2);
        }, QStringLiteral("Duplicate") },
                                            { [](auto &pk) {
            pk.pack_array(3);
            pk.pack("set");
            pk.pack(1);
            pk.pack_map(1);
            pk.pack(1);
            pk.pack(2);
        }, QStringLiteral("names must be strings") },
                                            { [](auto &pk) {
            pk.pack_array(3);
            pk.pack("set");
            pk.pack(1);
            pk.pack_map(6);
            for(int i = 0; i < 6; ++i) {
                pk.pack(i);
                pk.pack(i);
            }
        }, QStringLiteral("unsupported fields") },
                                            { [](auto &pk) {
            pk.pack_array(3);
            pk.pack("set");
            pk.pack(1);
            pk.pack_map(1);
            pk.pack("row");
            pk.pack(std::numeric_limits<quint64>::max());
        }, QStringLiteral("integer between") },
                                            { [](auto &pk) {
            pk.pack_array(3);
            pk.pack("set");
            pk.pack(1);
            pk.pack_map(1);
            pk.pack("row");
            pk.pack_float(1.25f);
        }, QStringLiteral("integer between") } };
        quint32 sequence = 1;
        for(const auto &entry: invalid) {
            peer->write(packRequest(sequence++, QStringLiteral("dot_image"), entry.arguments));
            peer->flush();
            const auto frames = readFrames(peer, 1);
            QCOMPARE(frames.size(), std::size_t(1));
            const auto &error = frames[0].get().via.array.ptr[2];
            QVERIFY(error.type == msgpack::type::ARRAY);
            const QString message =
                QString::fromStdString(error.via.array.ptr[1].as<std::string>());
            QVERIFY2(message.contains(entry.expected), qPrintable(message));
        }
        const QByteArray png = makePng();
        peer->write(packRequest(sequence++, QStringLiteral("dot_image"), [&](auto &pk) {
            pk.pack_array(3);
            pk.pack("set");
            pk.pack_bin(static_cast<uint32_t>(png.size()));
            pk.pack_bin_body(png.constData(), static_cast<uint32_t>(png.size()));
            pk.pack_map(1);
            pk.pack("row");
            pk.pack_double(2.0);
        }));
        peer->flush();
        const auto frames = readFrames(peer, 1);
        QCOMPARE(frames.size(), std::size_t(1));
        QVERIFY(frames[0].get().via.array.ptr[2].is_nil());
        const quint32 id = frames[0].get().via.array.ptr[3].as<quint32>();
        QVERIFY(conn.images()->get(id).has_value());
        QCOMPARE(conn.images()->get(id)->row, std::optional<qint32>(2));
        const QByteArray oversized(ImageModel::kMaxEncodedBytes + 1, 'x');
        for(const bool binary: { false, true }) {
            peer->write(packRequest(sequence++, QStringLiteral("dot_image"), [&](auto &pk) {
                pk.pack_array(3);
                pk.pack("set");
                const auto size = static_cast<uint32_t>(oversized.size());
                if(binary) {
                    pk.pack_bin(size);
                    pk.pack_bin_body(oversized.constData(), size);
                } else {
                    pk.pack_str(size);
                    pk.pack_str_body(oversized.constData(), size);
                }
                pk.pack_map(0);
            }));
            peer->flush();
            const auto rejected = readFrames(peer, 1);
            QCOMPARE(rejected.size(), std::size_t(1));
            const auto &error = rejected[0].get().via.array.ptr[2];
            QVERIFY(error.type == msgpack::type::ARRAY);
            QVERIFY(QString::fromStdString(error.via.array.ptr[1].as<std::string>())
                        .contains(QStringLiteral("encoded image limit")));
            QCOMPARE(conn.images()->count(), qsizetype(1));
            QVERIFY(conn.images()->get(id).has_value());
        }
        conn.setImagesAvailable(false);
        peer->write(packRequest(sequence++, QStringLiteral("dot_image"), [id](auto &pk) {
            pk.pack_array(2);
            pk.pack("get");
            pk.pack(id);
        }));
        peer->flush();
        const auto unavailable = readFrames(peer, 1);
        QCOMPARE(unavailable.size(), std::size_t(1));
        QVERIFY(!unavailable[0].get().via.array.ptr[2].is_nil());
        peer->abort();
        QVERIFY(waitUntil([&] { return !rpc->isRunning(); }));
    }

    void redrawBarrierPrecedesAbsoluteImagePlacement() {
        if(!m_supported) QSKIP("The installed Neovim has no image API");
        NvimConnector conn;
        QVERIFY(startImageNvim(conn));
        const auto result = runLua(conn, QStringLiteral(R"lua(
local lines={}
for i=1,100 do lines[i]=tostring(i) end
vim.api.nvim_buf_set_lines(0,0,-1,false,lines)
vim.cmd("normal! gg")
vim.cmd("redraw!")
local id=vim.ui.img.set(vim.base64.decode("%1"),{col=4,row=8,width=4,height=4})
vim.cmd([[execute "normal! 3\<C-e>"]])
vim.ui.img.set(id,{row=8})
vim.g.image_order_id=id
)lua")
                                             .arg(pngLiteral()));
        QVERIFY2(result.has_value(), qPrintable(result ? QString{} : result.error()));
        const auto id = getVarSync(conn, QStringLiteral("image_order_id"));
        QVERIFY(id.has_value());
        const auto it = conn.images()->placements().constFind(id->toUInt());
        QVERIFY(it != conn.images()->placements().cend());
        QCOMPARE(it->position, QPointF(3, 7));
        QVERIFY(!it->clip.has_value());
    }

    void restartReinstallsTheBackendWithoutOldImages() {
        if(!m_supported) QSKIP("The installed Neovim has no image API");
        NvimConnector conn;
        QVERIFY(startImageNvim(conn));
        auto created = runLua(
            conn,
            QStringLiteral(
                "vim.ui.img.set(vim.base64.decode('%1'),{row=3});vim.g.image_old_pid=vim.fn.getpid()")
                .arg(pngLiteral()));
        QVERIFY2(created.has_value(), qPrintable(created ? QString{} : created.error()));
        const auto oldPid = getVarSync(conn, QStringLiteral("image_old_pid"));
        QVERIFY(oldPid.has_value());
        conn.command(QStringLiteral("restart!"));
        QVERIFY(waitUntil([&] {
            if(!conn.attached() || !conn.imagesAvailable()) return false;
            const auto pid = evalSync(conn, QStringLiteral("getpid()"), 1000);
            return pid && pid->toLongLong() != oldPid->toLongLong();
        }, 25000));
        QCOMPARE(conn.images()->count(), qsizetype(0));
        const auto result = runLua(conn, QStringLiteral(R"lua(
assert(vim.ui.img == require("vim.ui.img"))
assert(vim.ui.img._supported())
assert(vim.g.image_old_pid == nil)
vim.ui.img.set(vim.base64.decode("%1"), {row=4})
)lua")
                                             .arg(pngLiteral(Qt::green)));
        QVERIFY2(result.has_value(), qPrintable(result ? QString{} : result.error()));
        QCOMPARE(conn.images()->count(), qsizetype(1));
        QCOMPARE(conn.images()->placements().cbegin()->pixels.pixelColor(0, 0), QColor(Qt::green));
    }

    void supportMatchesTheAttachedDotUi() {
        std::optional<RpcResult> detached;
        NvimConnector conn;
        QVERIFY(startImageNvim(conn));
        const auto attached = runLua(conn, QStringLiteral("assert(vim.ui.img._supported())"));
        QVERIFY2(attached.has_value(), qPrintable(attached ? QString{} : attached.error()));
        MsgpackRpc *rpc = conn.findChild<MsgpackRpc *>();
        QVERIFY(rpc);
        rpc->request(QStringLiteral("nvim_ui_detach"), {},
                     [&](RpcResult result) { detached = std::move(result); });
        QVERIFY(waitUntil([&] { return detached.has_value(); }));
        QVERIFY(detached->has_value());
        QVERIFY(rpc->isRunning());
        const auto unsupported =
            runLua(conn, QStringLiteral("assert(not vim.ui.img._supported())"));
        QVERIFY2(unsupported.has_value(),
                 qPrintable(unsupported ? QString{} : unsupported.error()));
    }

    void textEditingWorksWhetherOrNotTheImageApiExists() {
        NvimConnector conn;
        QVERIFY(startImageNvim(conn));
        QCOMPARE(conn.imagesAvailable(), m_supported);
        const auto result = runLua(
            conn, QStringLiteral("vim.api.nvim_buf_set_lines(0,0,-1,false,{'ordinary editing'});"
                                 "assert(vim.api.nvim_get_current_line()=='ordinary editing')"));
        QVERIFY2(result.has_value(), qPrintable(result ? QString{} : result.error()));
        QCOMPARE(conn.images()->count(), qsizetype(0));
    }

private:
    bool m_supported = false;
};

} // namespace dot

QTEST_MAIN(dot::TestImageApi)
#include "test_image_api.moc"
