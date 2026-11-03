#include "ChatModel.h"
#include "ColorPalette.h"
#include "Config.h"
#include "ConfigCliReader.h"
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
    Config config;
    NvimConnector conn;
    WindowChrome chrome;
    QQmlApplicationEngine engine;
    QQuickItem *input = nullptr;
    QQuickItem *panel = nullptr;
    QQuickWindow *window = nullptr;

    bool start(QStringList args = {}) {
        ColorPalette::registerConfig(config);
        ConfigCliReader::extract(args, config);
        conn.colorPalette()->bindConfig(config);
        QObject::connect(&conn, &NvimConnector::attachComplete, &config,
                         [this] { ConfigGGlobalReader::read(conn, config); });
        if(!startTestNvim(conn, args)) return false;
        engine.rootContext()->setContextProperty(QStringLiteral("$connector"), &conn);
        engine.rootContext()->setContextProperty(QStringLiteral("$windowChrome"), &chrome);
        engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
        if(engine.rootObjects().isEmpty()) return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if(!window || !QTest::qWaitForWindowExposed(window)) return false;
        input = window->findChild<QQuickItem *>(QStringLiteral("chatInput"));
        panel = window->findChild<QQuickItem *>(QStringLiteral("chatPanel"));
        return input && panel &&
               waitUntil([&] { return conn.attached() && conn.colorPalette()->paneValid(); });
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
        QVERIFY(ui.start({ QStringLiteral("--dot-theme=") + theme }));
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

    void profileConfigUsesProductionPrecedence() {
        QFETCH(QString, cli);
        QFETCH(QString, global);
        QFETCH(QString, expected);
        UiColorHarness ui;
        QStringList arguments{
            QStringLiteral("--cmd"),
            QStringLiteral("let g:dot_theme = '%1'").arg(global),
        };
        if(!cli.isEmpty()) arguments.append(QStringLiteral("--dot-theme=") + cli);
        QVERIFY(ui.start(arguments));
        QTRY_COMPARE(ui.config.value(QStringLiteral("theme")).toString(), expected);
        QCOMPARE(ui.conn.colorPalette()->theme(), expected);
        QCOMPARE(ui.window->color(),
                 expected == QStringLiteral("light") ? QColor("#dce0e8") : QColor("#11111b"));

        ui.conn.command(QStringLiteral("unlet g:dot_theme"));
        ConfigGGlobalReader::read(ui.conn, ui.config);
        const QString after = cli.isEmpty() ? QStringLiteral("dark") : cli;
        QTRY_COMPARE(ui.config.value(QStringLiteral("theme")).toString(), after);
        QCOMPARE(ui.conn.colorPalette()->theme(), after);
    }

    void profileConfigUsesProductionPrecedence_data() {
        QTest::addColumn<QString>("cli");
        QTest::addColumn<QString>("global");
        QTest::addColumn<QString>("expected");
        QTest::newRow("cli") << QStringLiteral("dark") << QStringLiteral("light")
                             << QStringLiteral("dark");
        QTest::newRow("global") << QString() << QStringLiteral("light") << QStringLiteral("light");
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

    void restartDropsColorsAndMissingGlobals() {
        QFETCH(bool, cliOverride);
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
  vim.g.dot_theme = 'light'
end
)lua")
                                    .arg(marker)
                                    .toUtf8();
        QCOMPARE(init.write(code), static_cast<qint64>(code.size()));
        init.close();
        QStringList args{ QStringLiteral("-u"), initPath };
        if(cliOverride) args.append(QStringLiteral("--dot-theme=light"));
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
        connect(colors, &ColorPalette::paletteChanged, &ui.config, [&] {
            if(!colors->paneValid()) {
                sawInvalid = true;
                QVERIFY(!colors->cursorValid());
                QVERIFY(!colors->menuValid());
                QVERIFY(!colors->overlayValid());
                QVERIFY(!colors->textSelectionValid());
            }
        });
        ui.conn.command(QStringLiteral("restart!"));
        QVERIFY(waitUntil([&] {
            return sawInvalid && ui.conn.attached() && colors->paneValid() &&
                   colors->paneBackground() != QColor("#1c1f25");
        }, 20000));
        const QString expected = cliOverride ? QStringLiteral("light") : QStringLiteral("dark");
        QTRY_COMPARE(colors->theme(), expected);
        const auto newPid = evalSync(ui.conn, QStringLiteral("getpid()"));
        QVERIFY(newPid);
        QVERIFY(newPid != oldPid);
        QCOMPARE(ui.window->color(), cliOverride ? QColor("#dce0e8") : QColor("#11111b"));
        QCOMPARE(ui.window->findChild<QQuickItem *>(QStringLiteral("chatInput")), ui.input);
        QCOMPARE(ui.window->findChild<QQuickItem *>(QStringLiteral("chatPanel")), ui.panel);
        QCOMPARE(ui.window->findChild<CursorItem *>(), cursor);
        QCOMPARE(ui.window->findChild<GridItem *>(), grid);
        QCOMPARE(ui.conn.colorPalette(), colors);
        QCOMPARE(ui.window->activeFocusItem(), ui.input);
        QVERIFY(ui.panel->isVisible());
    }

    void restartDropsColorsAndMissingGlobals_data() {
        QTest::addColumn<bool>("cliOverride");
        QTest::newRow("cli-survives") << true;
        QTest::newRow("missing-global") << false;
    }
};

QTEST_MAIN(TestUiColorBindings)
#include "test_ui_color_bindings.moc"
