#include "ChatModel.h"
#include "ColorPalette.h"
#include "Config.h"
#include "ConfigGGlobalReader.h"
#include "CursorItem.h"
#include "GridItem.h"
#include "IntegrationHelpers.h"
#include "support/ColorHelpers.h"
#include "WindowChrome.h"

#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QtTest>

using namespace dot;
using namespace dot::test;

namespace {

template <typename Predicate>
bool waitUntil(Predicate &&predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while(!predicate()) {
        if(timer.elapsed() >= timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

QImage capture(QQuickItem *item) {
    const auto result = item->grabToImage();
    if(!result || !waitUntil([&] { return !result->image().isNull(); })) return {};
    return result->image();
}

struct UiColorHarness {
    NvimConnector conn;
    WindowChrome chrome;
    QQmlApplicationEngine engine;
    QQuickItem *input = nullptr;
    bool paletteReadyAtFirstVisibility = false;
    QQuickItem *panel = nullptr;
    bool visibilityObserved = false;
    QQuickWindow *window = nullptr;

    bool start(const QStringList &args = {}, bool waitForVisible = true) {
        if(!startTestNvim(conn, args)) return false;
        engine.rootContext()->setContextProperty(QStringLiteral("$connector"), &conn);
        engine.rootContext()->setContextProperty(QStringLiteral("$windowChrome"), &chrome);
        engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
        if(engine.rootObjects().isEmpty()) return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if(!window || window->isVisible()) return false;
        QObject::connect(window, &QWindow::visibleChanged, &conn, [this](bool visible) {
            if(visible && !visibilityObserved) {
                const auto *colors = conn.colorPalette();
                visibilityObserved = true;
                paletteReadyAtFirstVisibility = colors->chromeValid() && colors->paneValid() &&
                                                window->color() == colors->frame();
            }
        });
        input = window->findChild<QQuickItem *>(QStringLiteral("chatInput"));
        panel = window->findChild<QQuickItem *>(QStringLiteral("chatPanel"));
        if(!input || !panel) return false;
        if(!waitForVisible) return true;
        return QTest::qWaitForWindowExposed(window) && paletteReadyAtFirstVisibility;
    }

    bool openChat() {
        auto *toggle = window->findChild<QQuickItem *>(QStringLiteral("chatToggle"));
        if(!toggle) return false;
        const QPoint position =
            toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position);
        return waitUntil([&] { return panel->isVisible() && window->activeFocusItem() == input; });
    }

    bool defineColors() {
        conn.execLua(QStringLiteral(R"lua(
vim.api.nvim_set_hl(0, 'Normal', {fg = 0xb5e3c8, bg = 0x1c1f25})
vim.api.nvim_set_hl(0, 'NormalFloat', {fg = 0xeff3f8, bg = 0x23272e})
vim.api.nvim_set_hl(0, 'Pmenu', {fg = 0xabcdef, bg = 0x2f323a})
vim.api.nvim_set_hl(0, 'Visual', {fg = 0x123456, bg = 0x8b9dff})
vim.api.nvim_set_hl(0, 'DotPaletteCursor', {fg = 0x00ffff, bg = 0xff00ff})
vim.o.guicursor = 'a:block-DotPaletteCursor-blinkwait0-blinkon0-blinkoff0'
vim.api.nvim_buf_set_lines(0, 0, -1, false, {'the X cursor', 'semantic color bindings'})
vim.api.nvim_win_set_cursor(0, {1, 4})
vim.bo.modified = false
vim.cmd('redraw')
)lua"));
        return waitUntil([&] {
            const auto *colors = conn.colorPalette();
            return colors->primaryText() == QColor("#b5e3c8") &&
                   colors->paneBackground() == QColor("#1c1f25") &&
                   colors->cursorBackground() == QColor("#ff00ff") &&
                   colors->menuForeground() == QColor("#abcdef") &&
                   colors->overlayBackground() == QColor("#23272e") &&
                   colors->textSelectionForeground() == QColor("#123456");
        });
    }
};

} // namespace

class TestUiColorBindings : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        qRegisterMetaType<dot::Notification>();
        qRegisterMetaType<dot::ObjectHandlePtr>();
    }

    void productionHighlightsReachExistingConsumers() {
        QFETCH(QString, theme);
        QFETCH(QColor, frame);
        QFETCH(QColor, resting);
        UiColorHarness ui;
        QVERIFY(ui.start({ QStringLiteral("--cmd"), QStringLiteral("set background=") + theme }));
        QVERIFY(ui.openChat());
        QVERIFY(ui.defineColors());
        auto *colors = ui.conn.colorPalette();
        auto *cursor = ui.window->findChild<CursorItem *>();
        auto *grid = ui.window->findChild<GridItem *>();
        auto *inputRow = ui.window->findChild<QQuickItem *>(QStringLiteral("chatInputRow"));
        auto *model = ui.window->findChild<ChatModel *>();
        QVERIFY(cursor && grid && inputRow && model);
        QCOMPARE(colors->theme(), theme);
        QCOMPARE(ui.window->color(), frame);
        QCOMPARE(ui.panel->property("color").value<QColor>(), QColor("#1c1f25"));
        QCOMPARE(ui.panel->opacity(), qreal(1));
        QCOMPARE(ui.input->property("color").value<QColor>(), QColor("#b5e3c8"));
        QCOMPARE(inputRow->property("color").value<QColor>(), resting);
        QVERIFY(colors->resolutionErrors().isEmpty());
        QCOMPARE(colors->cursorForeground(), QColor("#00ffff"));
        QCOMPARE(colors->menuBackground(), QColor("#2f323a"));
        QCOMPARE(colors->overlayForeground(), QColor("#eff3f8"));
        QCOMPARE(colors->textSelectionBackground(), QColor("#8b9dff"));
        model->appendBlock(QStringLiteral("assistant"), QStringLiteral("primary text proof"));
        QTRY_VERIFY(findTextItem(ui.panel, QStringLiteral("primary text proof")));
        auto *message = findTextItem(ui.panel, QStringLiteral("primary text proof"));
        QCOMPARE(message->property("color").value<QColor>(), QColor("#b5e3c8"));

        QSignalSpy published(colors, &ColorPalette::paletteChanged);
        ui.conn.command(QStringLiteral("highlight Normal guifg=#000000 guibg=#ffffff"));
        QTRY_COMPARE(ui.input->property("color").value<QColor>(), QColor(0, 0, 0));
        QVERIFY(!published.isEmpty());
        QCOMPARE(ui.panel->property("color").value<QColor>(), QColor(Qt::white));
        QCOMPARE(ui.window->color(), frame);
        QCOMPARE(inputRow->property("color").value<QColor>(), resting);
        QCOMPARE(ui.window->findChild<QQuickItem *>(QStringLiteral("chatInput")), ui.input);
        QCOMPARE(ui.window->findChild<QQuickItem *>(QStringLiteral("chatPanel")), ui.panel);
        QCOMPARE(ui.window->findChild<CursorItem *>(), cursor);
        QCOMPARE(ui.window->findChild<GridItem *>(), grid);
        QCOMPARE(ui.window->activeFocusItem(), ui.input);
        for(const auto key: { Qt::Key_C, Qt::Key_O, Qt::Key_L, Qt::Key_O, Qt::Key_R })
            QTest::keyClick(ui.window, key);
        QCOMPARE(ui.input->property("text").toString(), QStringLiteral("color"));
        QCOMPARE(ui.input->opacity(), qreal(1));
        QCOMPARE(message->opacity(), qreal(1));
        const QImage image = capture(ui.panel);
        QVERIFY(!image.isNull());
        bool varied = false;
        const QRgb first = image.pixel(0, 0);
        for(int y = 0; y < image.height() && !varied; ++y) {
            for(int x = 0; x < image.width(); ++x) {
                if(image.pixel(x, y) != first) {
                    varied = true;
                    break;
                }
            }
        }
        QVERIFY(varied);
    }

    void productionHighlightsReachExistingConsumers_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QColor>("frame");
        QTest::addColumn<QColor>("resting");
        QTest::newRow("dark") << QStringLiteral("dark") << QColor("#11111b") << QColor("#313244");
        QTest::newRow("light") << QStringLiteral("light") << QColor("#dce0e8") << QColor("#ccd0da");
    }

    void exactProfilesFollowNeovimBackground() {
        QFETCH(QString, background);
        QFETCH(QStringList, expected);
        UiColorHarness ui;
        QVERIFY(
            ui.start({ QStringLiteral("--cmd"), QStringLiteral("set background=") + background }));
        auto *colors = ui.conn.colorPalette();
        QCOMPARE(colors->theme(), background);
        QCOMPARE(expected.size(), static_cast<qsizetype>(chromeRoles.size()));
        for(size_t i = 0; i < chromeRoles.size(); ++i) {
            QString property = QString::fromLatin1(chromeRoles.at(i)).mid(3);
            property[0] = property[0].toLower();
            QCOMPARE(colors->property(qPrintable(property)).value<QColor>(),
                     QColor(expected.at(static_cast<qsizetype>(i))));
        }
        QVERIFY(colors->chromeValid());
        QVERIFY(ui.paletteReadyAtFirstVisibility);
        QVERIFY(colors->resolutionErrors().isEmpty());
        const auto *paletteObject = colors;
        const QString other =
            background == QStringLiteral("dark") ? QStringLiteral("light") : QStringLiteral("dark");
        ui.conn.command(QStringLiteral("set background=") + other);
        QTRY_COMPARE(colors->theme(), other);
        QCOMPARE(colors->frame(),
                 other == QStringLiteral("light") ? QColor("#dce0e8") : QColor("#11111b"));
        QCOMPARE(ui.window->color(), colors->frame());
        ui.conn.command(QStringLiteral("set background=") + background);
        QTRY_COMPARE(colors->theme(), background);
        QCOMPARE(ui.conn.colorPalette(), paletteObject);
        for(size_t i = 0; i < chromeRoles.size(); ++i) {
            QString property = QString::fromLatin1(chromeRoles.at(i)).mid(3);
            property[0] = property[0].toLower();
            QCOMPARE(colors->property(qPrintable(property)).value<QColor>(),
                     QColor(expected.at(static_cast<qsizetype>(i))));
        }
    }

    void exactProfilesFollowNeovimBackground_data() {
        QTest::addColumn<QString>("background");
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

    void startupPreservesUserHighlights() {
        QFETCH(QString, event);
        QTemporaryDir directory;
        UiColorHarness ui;
        QVERIFY(directory.isValid());
        const QString initPath = directory.filePath(QStringLiteral("init.lua"));
        QFile init(initPath);
        QVERIFY(init.open(QIODevice::WriteOnly));
        const QString apply = QStringLiteral(R"lua(
local function apply()
  vim.api.nvim_set_hl(0, "DotUserAccent", { fg = 0x123456 })
  vim.api.nvim_set_hl(0, "DotAccent", { link = "DotUserAccent" })
  vim.api.nvim_set_hl(0, "DotFrame", { bg = 0x456789 })
end
)lua");
        const QString action =
            event.isEmpty()
                ? QStringLiteral("apply()")
                : QStringLiteral("vim.api.nvim_create_autocmd('%1', { callback = apply })")
                      .arg(event);
        const QByteArray code =
            (QStringLiteral("vim.o.background = 'light'\n") + apply + action).toUtf8();
        QCOMPARE(init.write(code), static_cast<qint64>(code.size()));
        init.close();
        QVERIFY(ui.start({ QStringLiteral("-u"), initPath }));
        auto *colors = ui.conn.colorPalette();
        QCOMPARE(colors->theme(), QStringLiteral("light"));
        QCOMPARE(colors->accent(), QColor("#123456"));
        QCOMPARE(colors->frame(), QColor("#456789"));
        QCOMPARE(colors->text(), QColor("#4c4f69"));
        QCOMPARE(ui.window->color(), QColor("#456789"));
        QVERIFY(ui.paletteReadyAtFirstVisibility);
        ui.conn.command(QStringLiteral("set background=dark"));
        QTRY_COMPARE(colors->theme(), QStringLiteral("dark"));
        QCOMPARE(colors->accent(), QColor("#123456"));
        QCOMPARE(colors->frame(), QColor("#456789"));
        QCOMPARE(colors->text(), QColor("#cdd6f4"));
    }

    void startupPreservesUserHighlights_data() {
        QTest::addColumn<QString>("event");
        QTest::newRow("init") << QString();
        QTest::newRow("ui-enter") << QStringLiteral("UIEnter");
        QTest::newRow("vim-enter") << QStringLiteral("VimEnter");
    }

    void directEditsAndLinkedTargetsUpdateExistingObjects() {
        UiColorHarness ui;
        QVERIFY(ui.start());
        QVERIFY(ui.openChat());
        auto *colors = ui.conn.colorPalette();
        auto *cursor = ui.window->findChild<CursorItem *>();
        auto *grid = ui.window->findChild<GridItem *>();
        auto *inputRow = ui.window->findChild<QQuickItem *>(QStringLiteral("chatInputRow"));
        QVERIFY(cursor && grid && inputRow);
        ui.conn.execLua(QStringLiteral(R"lua(
vim.api.nvim_set_hl(0, "DotAccent", { fg = 0x102030 })
vim.api.nvim_set_hl(0, "DotFrame", { bg = 0x304050 })
vim.api.nvim_set_hl(0, "DotResting", { bg = 0x506070 })
vim.cmd("redraw")
)lua"));
        QTRY_COMPARE(colors->accent(), QColor("#102030"));
        QCOMPARE(colors->frame(), QColor("#304050"));
        QCOMPARE(ui.window->color(), QColor("#304050"));
        QCOMPARE(inputRow->property("color").value<QColor>(), QColor("#506070"));
        ui.conn.execLua(QStringLiteral(R"lua(
vim.api.nvim_set_hl(0, "DotUserTarget", { bg = 0x345678, fg = 0x123456 })
vim.api.nvim_set_hl(0, "DotAccent", { link = "DotUserTarget" })
vim.api.nvim_set_hl(0, "DotResting", { link = "DotUserTarget" })
vim.cmd("redraw")
)lua"));
        QTRY_COMPARE(colors->accent(), QColor("#123456"));
        QCOMPARE(colors->resting(), QColor("#345678"));
        ui.conn.execLua(QStringLiteral(R"lua(
vim.api.nvim_set_hl(0, "DotUserTarget", { bg = 0x6789ab, fg = 0xabcdef, reverse = true })
vim.cmd("redraw")
)lua"));
        QTRY_COMPARE(colors->accent(), QColor("#6789ab"));
        QCOMPARE(colors->resting(), QColor("#abcdef"));
        QCOMPARE(inputRow->property("color").value<QColor>(), QColor("#abcdef"));
        ui.conn.execLua(QStringLiteral(R"lua(
vim.api.nvim_set_hl(0, "DotAccent", { bold = true })
vim.api.nvim_set_hl(0, "DotResting", { italic = true })
vim.api.nvim_set_hl(0, "Normal", { bg = 0x102030, fg = 0xaabbcc })
vim.cmd("redraw")
)lua"));
        QTRY_COMPARE(colors->accent(), QColor("#aabbcc"));
        QCOMPARE(colors->resting(), QColor("#102030"));
        QCOMPARE(inputRow->property("color").value<QColor>(), QColor("#102030"));
        ui.conn.command(QStringLiteral("highlight Normal guifg=#abcdef guibg=#123456"));
        QTRY_COMPARE(colors->accent(), QColor("#abcdef"));
        QCOMPARE(colors->resting(), QColor("#123456"));
        QCOMPARE(ui.window->findChild<QQuickItem *>(QStringLiteral("chatInput")), ui.input);
        QCOMPARE(ui.window->findChild<QQuickItem *>(QStringLiteral("chatPanel")), ui.panel);
        QCOMPARE(ui.window->findChild<CursorItem *>(), cursor);
        QCOMPARE(ui.window->findChild<GridItem *>(), grid);
        QCOMPARE(ui.window->activeFocusItem(), ui.input);
        QVERIFY(colors->resolutionErrors().isEmpty());
    }

    void colorschemeReloadRecreatesDefaultsAfterUserHandlers() {
        UiColorHarness ui;
        QVERIFY(ui.start({ QStringLiteral("--cmd"), QStringLiteral("set background=light") }));
        ui.conn.execLua(QStringLiteral(R"lua(
vim.api.nvim_create_autocmd("ColorScheme", {
  callback = function()
    vim.api.nvim_set_hl(0, "DotAccent", { fg = 0x123456 })
  end,
})
vim.api.nvim_set_hl(0, "DotDefaultFrame", { bg = 0x987654 })
vim.cmd("colorscheme default")
)lua"));
        auto *colors = ui.conn.colorPalette();
        QTRY_COMPARE(colors->accent(), QColor("#123456"));
        QTRY_COMPARE(colors->frame(), QColor("#dce0e8"));
        QVERIFY(colors->chromeValid());
        ui.conn.execLua(QStringLiteral(R"lua(
vim.api.nvim_set_hl(0, "DotAccent", { fg = 0xabcdef })
vim.cmd("colorscheme default")
)lua"));
        QTRY_COMPARE(colors->accent(), QColor("#123456"));
        QCOMPARE(colors->frame(), QColor("#dce0e8"));
        QCOMPARE(colors->text(), QColor("#4c4f69"));
        QVERIFY(colors->resolutionErrors().isEmpty());
    }

    void firstVisibilityWaitsForColorsAndRecovers() {
        QFETCH(bool, safetyTimer);
        QTemporaryDir directory;
        UiColorHarness ui;
        QVERIFY(directory.isValid());
        const QString initPath = directory.filePath(QStringLiteral("init.lua"));
        QFile init(initPath);
        QVERIFY(init.open(QIODevice::WriteOnly));
        const QByteArray code = R"lua(
local colors = require("dot.colors")
local refresh = colors.refresh
colors.refresh = function(generation)
  _G.dot_test_release_colors = function()
    colors.refresh = refresh
    return refresh(generation)
  end
  return true
end
)lua";
        QCOMPARE(init.write(code), static_cast<qint64>(code.size()));
        init.close();
        QVERIFY(ui.start({ QStringLiteral("-u"), initPath }, false));
        QVERIFY(
            waitUntil([&] { return ui.conn.attached() && ui.conn.colorPalette()->paneValid(); }));
        QVERIFY(!ui.conn.colorPalette()->chromeValid());
        QVERIFY(!ui.window->isVisible());
        if(safetyTimer) {
            ui.window->setProperty("_requestedCols",
                                   ui.window->property("_requestedCols").toInt() + 1);
            QVERIFY(waitUntil([&] { return ui.window->property("_showSafetyExpired").toBool(); }));
            QVERIFY(!ui.window->isVisible());
            const msgpack::object invalid;
            ui.conn.colorPalette()->stageChromeSnapshot(invalid);
            QTest::ignoreMessage(
                QtCriticalMsg,
                "ColorPalette: chrome: invalid snapshot: expected background and roles");
            QVERIFY(redrawEvent(ui.conn, "flush", [](auto &pk) { pk.pack_array(0); }));
            QVERIFY(!ui.window->isVisible());
        }
        ui.conn.execLua(QStringLiteral("assert(_G.dot_test_release_colors)()"));
        QVERIFY(QTest::qWaitForWindowExposed(ui.window));
        QVERIFY(ui.paletteReadyAtFirstVisibility);
        QVERIFY(ui.conn.colorPalette()->chromeValid());
        QVERIFY(ui.conn.colorPalette()->resolutionErrors().isEmpty());
    }

    void firstVisibilityWaitsForColorsAndRecovers_data() {
        QTest::addColumn<bool>("safetyTimer");
        QTest::newRow("normal") << false;
        QTest::newRow("safety-timer") << true;
    }

    void colorRefreshBatchesHighlightEditsAndBecomesIdle() {
        NvimConnector conn;
        QVERIFY(startTestNvim(conn));
        QVERIFY(conn.attachUi(80, 24));
        QVERIFY(waitUntil([&] { return conn.colorPalette()->chromeValid(); }));
        conn.execLua(QStringLiteral(R"lua(
local colors = require("dot.colors")
local refresh = colors.refresh
colors.refresh = function(...)
  vim.g.dot_test_color_requests = vim.g.dot_test_color_requests + 1
  return refresh(...)
end
vim.g.dot_test_color_requests = 0
vim.api.nvim_buf_set_lines(0, 0, -1, false, {"first line", "second line"})
vim.cmd("redraw")
)lua"));
        QVERIFY(evalSync(conn, QStringLiteral("g:dot_test_color_requests")));
        QTest::qWait(100);
        conn.execLua(QStringLiteral(R"lua(
vim.g.dot_test_color_requests = 0
for index = 1, 40 do
  vim.api.nvim_set_hl(0, "DotAccent", { fg = index })
end
vim.cmd("redraw")
)lua"));
        QTRY_COMPARE(conn.colorPalette()->accent(), QColor::fromRgb(40));
        QTest::qWait(100);
        const auto highlightRequests = evalSync(conn, QStringLiteral("g:dot_test_color_requests"));
        QVERIFY(highlightRequests);
        QCOMPARE(highlightRequests->toInt(), 1);
        conn.execLua(QStringLiteral(R"lua(
vim.g.dot_test_color_requests = 0
for index = 1, 12 do
  vim.api.nvim_win_set_cursor(0, {1, index % 4})
  vim.api.nvim_buf_set_lines(0, 1, 2, false, {"line " .. index})
  vim.cmd("redraw")
end
vim.g.dot_test_grid_edits_done = true
)lua"));
        const auto done = evalSync(conn, QStringLiteral("g:dot_test_grid_edits_done"));
        QVERIFY(done && done->toBool());
        QTest::qWait(100);
        const auto gridRequests = evalSync(conn, QStringLiteral("g:dot_test_color_requests"));
        QVERIFY(gridRequests);
        QCOMPARE(gridRequests->toInt(), 0);
    }

    void reinstallDiscardsStaleScheduledSetup() {
        NvimConnector conn;
        QFile script(QStringLiteral(":/dot_backend.lua"));
        QVERIFY(startTestNvim(conn));
        QVERIFY(conn.attachUi(80, 24));
        QVERIFY(waitUntil([&] { return conn.colorPalette()->chromeValid(); }));
        QVERIFY(script.open(QIODevice::ReadOnly));
        const QString code = QString::fromUtf8(script.readAll());
        conn.execLua(QStringLiteral(R"lua(
local bootstrap = assert(loadstring([====[%1]====]))
local client_id
for _, info in ipairs(vim.api.nvim_list_chans()) do
  if info.client and info.client.attributes.dot_backend then
    client_id = info.client.attributes.dot_backend
  end
end
assert(client_id)
vim.api.nvim_set_hl(0, "DotAccent", { fg = 0x123456 })
vim.o.background = "light"
local define = vim.api.nvim_set_hl
vim.g.dot_test_default_definitions = 0
vim.api.nvim_set_hl = function(namespace, name, attributes)
  if name:match("^DotDefault") then
    vim.g.dot_test_default_definitions = vim.g.dot_test_default_definitions + 1
  end
  return define(namespace, name, attributes)
end
bootstrap(client_id, %2, 1)
bootstrap(client_id, %2, 1)
vim.schedule(function()
  vim.api.nvim_set_hl = define
  vim.g.dot_test_reinstalled = true
end)
)lua")
                         .arg(code)
                         .arg(ImageModel::kMaxEncodedBytes));
        QVERIFY(waitUntil([&] {
            const auto done =
                evalSync(conn, QStringLiteral("get(g:, 'dot_test_reinstalled', v:false)"));
            return done && done->toBool();
        }));
        const auto definitions = evalSync(conn, QStringLiteral("g:dot_test_default_definitions"));
        QVERIFY(definitions);
        QCOMPARE(definitions->toInt(), static_cast<int>(chromeRoles.size()));
        QTRY_COMPARE(conn.colorPalette()->theme(), QStringLiteral("light"));
        QCOMPARE(conn.colorPalette()->accent(), QColor("#123456"));
        QCOMPARE(conn.colorPalette()->frame(), QColor("#dce0e8"));
        const auto autocmds =
            evalSync(conn, QStringLiteral("len(nvim_get_autocmds({'group': 'DotColors'}))"));
        QVERIFY(autocmds);
        QCOMPARE(autocmds->toInt(), 2);
    }

    void realSnapshotPreservesMissingFalseAndZero() {
        Config config;
        NvimConnector conn;
        config.registerOption(QStringLiteral("flag"), ConfigType::Bool, true);
        config.registerOption(QStringLiteral("number"), ConfigType::Int, 10);
        config.registerOption(QStringLiteral("omitted"), ConfigType::Int, 1);
        config.setFromGGlobal(QStringLiteral("omitted"), 12);
        QVERIFY(startTestNvim(
            conn, { QStringLiteral("--cmd"),
                    QStringLiteral("let g:dot_flag = v:false | let g:dot_number = 0") }));
        QVERIFY(conn.attachUi(80, 24));
        QVERIFY(waitForAttach(&conn));
        QSignalSpy changed(&config, &Config::changed);
        connect(&config, &Config::changed, &config, [&] {
            QCOMPARE(config.value(QStringLiteral("flag")).toBool(), false);
            QCOMPARE(config.value(QStringLiteral("number")).toInt(), 0);
            QCOMPARE(config.value(QStringLiteral("omitted")).toInt(), 1);
        });

        ConfigGGlobalReader::read(conn, config);

        QTRY_COMPARE(changed.count(), 3);
        QCOMPARE(changed.at(0).at(0).toString(), QStringLiteral("flag"));
        QCOMPARE(changed.at(1).at(0).toString(), QStringLiteral("number"));
        QCOMPARE(changed.at(2).at(0).toString(), QStringLiteral("omitted"));
    }

    void missingPublishedContentHidesWithoutDroppingFocus() {
        UiColorHarness ui;
        QVERIFY(ui.start());
        QVERIFY(ui.openChat());
        QVERIFY(ui.defineColors());
        auto *colors = ui.conn.colorPalette();
        auto *shell = ui.window->findChild<QQuickItem *>(QStringLiteral("shell"));
        QVERIFY(shell);
        const QColor oldForeground = ui.input->property("color").value<QColor>();
        QVERIFY(redrawEvent(ui.conn, "default_colors_set", [](auto &pk) {
            pk.pack_array(5);
            pk.pack(-1);
            pk.pack(0x123456);
            pk.pack(-1);
            pk.pack(0);
            pk.pack(0);
        }));
        QCOMPARE(ui.input->property("color").value<QColor>(), oldForeground);
        QCOMPARE(ui.panel->opacity(), qreal(1));
        QCOMPARE(shell->opacity(), qreal(1));
        QTest::ignoreMessage(QtCriticalMsg, "ColorPalette: pane: missing foreground");

        QVERIFY(redrawEvent(ui.conn, "flush", [](auto &pk) { pk.pack_array(0); }));

        QVERIFY(!colors->paneValid());
        QCOMPARE(ui.panel->opacity(), qreal(0));
        QCOMPARE(shell->opacity(), qreal(0));
        QCOMPARE(ui.window->activeFocusItem(), ui.input);
        QCOMPARE(colors->resolutionErrors(),
                 QStringList{ QStringLiteral("pane: missing foreground") });
        ui.conn.command(QStringLiteral("highlight Normal guifg=#b5e3c9 guibg=#1c1f26"));
        QTRY_COMPARE(ui.panel->opacity(), qreal(1));
        QCOMPARE(shell->opacity(), qreal(1));
        QCOMPARE(ui.input->property("color").value<QColor>(), QColor("#b5e3c9"));
        QCOMPARE(ui.window->activeFocusItem(), ui.input);
    }

    void restartDropsColorsAndReinitializesTheProvider() {
        bool sawInvalid = false;
        QTemporaryDir directory;
        UiColorHarness ui;
        QVERIFY(directory.isValid());
        const QString marker = directory.filePath(QStringLiteral("started"));
        const QString initPath = directory.filePath(QStringLiteral("init.lua"));
        QFile init(initPath);
        QVERIFY(init.open(QIODevice::WriteOnly));
        const QByteArray code = QStringLiteral(R"lua(
if vim.fn.filereadable([[%1]]) == 0 then
  vim.fn.writefile({'started'}, [[%1]])
  vim.o.background = 'light'
else
  vim.o.background = 'dark'
end
)lua")
                                    .arg(marker)
                                    .toUtf8();
        QCOMPARE(init.write(code), static_cast<qint64>(code.size()));
        init.close();
        QStringList args{ QStringLiteral("-u"), initPath };
        QVERIFY(ui.start(args));
        const auto apiLevel = evalSync(ui.conn, QStringLiteral("api_info().version.api_level"));
        QVERIFY(apiLevel);
        if(apiLevel->toInt() < 14) QSKIP("Neovim lacks the restart UI event");
        QTRY_COMPARE(ui.conn.colorPalette()->theme(), QStringLiteral("light"));
        QVERIFY(ui.openChat());
        QVERIFY(ui.defineColors());
        auto *colors = ui.conn.colorPalette();
        auto *cursor = ui.window->findChild<CursorItem *>();
        auto *grid = ui.window->findChild<GridItem *>();
        const auto oldPid = evalSync(ui.conn, QStringLiteral("getpid()"));
        QVERIFY(oldPid);
        connect(colors, &ColorPalette::paletteChanged, &ui.conn, [&] {
            if(!colors->paneValid()) {
                sawInvalid = true;
                QVERIFY(!colors->chromeValid());
                QVERIFY(!colors->frame().isValid());
                QVERIFY(colors->theme().isEmpty());
                QVERIFY(!colors->cursorValid());
                QVERIFY(!colors->menuValid());
                QVERIFY(!colors->overlayValid());
                QVERIFY(!colors->textSelectionValid());
            }
        });
        ui.conn.command(QStringLiteral("restart!"));
        QVERIFY(waitUntil([&] {
            return sawInvalid && ui.conn.attached() && colors->chromeValid() &&
                   colors->paneValid() && colors->paneBackground() != QColor("#1c1f25");
        }, 20000));
        QTRY_COMPARE(colors->theme(), QStringLiteral("dark"));
        const auto newPid = evalSync(ui.conn, QStringLiteral("getpid()"));
        QVERIFY(newPid);
        QVERIFY(newPid != oldPid);
        QCOMPARE(ui.window->color(), QColor("#11111b"));
        QCOMPARE(ui.window->findChild<QQuickItem *>(QStringLiteral("chatInput")), ui.input);
        QCOMPARE(ui.window->findChild<QQuickItem *>(QStringLiteral("chatPanel")), ui.panel);
        QCOMPARE(ui.window->findChild<CursorItem *>(), cursor);
        QCOMPARE(ui.window->findChild<GridItem *>(), grid);
        QCOMPARE(ui.conn.colorPalette(), colors);
        QCOMPARE(ui.window->activeFocusItem(), ui.input);
        QVERIFY(ui.panel->isVisible());
    }
};

QTEST_MAIN(TestUiColorBindings)
#include "test_ui_color_bindings.moc"
