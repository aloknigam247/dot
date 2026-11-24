#include <QCoreApplication>
#include <QKeyEvent>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtQuickTest>

#include "NvimConnector.h"
#include "support/ColorHelpers.h"

namespace {

// Every role gets a unique color so a binding to the wrong role cannot pass.
msgpack::object_handle distinctChromeSnapshot() {
    return dot::test::chromeSnapshot("dark", [](msgpack::packer<msgpack::sbuffer> &packer) {
        packer.pack_map(static_cast<uint32_t>(dot::test::chromeRoles.size()));
        int index = 0;
        for(const char *role: dot::test::chromeRoles) {
            const int color = 0x102030 + index++ * 0x0a0b0c;
            packer.pack(role);
            const auto attributes = dot::test::chromeAttributes(color, color);
            packer.pack(attributes.get());
        }
    });
}

class TestSetup : public QObject {
    Q_OBJECT
public:
    explicit TestSetup(QObject *parent = nullptr) : QObject(parent) {}

    // QtTest cannot synthesize the release/press pair Qt delivers while a key is held.
    Q_INVOKABLE void repeatKey(QObject *window, int key) {
        for(const auto type: { QEvent::KeyRelease, QEvent::KeyPress }) {
            QKeyEvent event(type, key, Qt::NoModifier, QString(), true);
            QCoreApplication::sendEvent(window, &event);
        }
    }

public slots:
    void applicationAvailable() { QQuickWindow::setGraphicsApi(QSGRendererInterface::Software); }

    void qmlEngineAvailable(QQmlEngine *engine) {
        // Isolated QML has no Neovim process to supply its required color snapshot.
        auto *connector = new dot::NvimConnector(this);
        dot::test::seedColorState(*connector);
        const auto chrome = distinctChromeSnapshot();
        connector->colorPalette()->stageChromeSnapshot(chrome.get());
        connector->colorPalette()->publishContent();
        engine->rootContext()->setContextProperty(QStringLiteral("$connector"), connector);
        engine->rootContext()->setContextProperty(QStringLiteral("$keys"), this);
        engine->rootContext()->setContextProperty(
            QStringLiteral("$expectedScaleFactor"),
            qEnvironmentVariable("QT_SCALE_FACTOR", "1").toDouble());
    }
};

} // namespace

QUICK_TEST_MAIN_WITH_SETUP(qml_tests, TestSetup)

#include "main_qmltest.moc"
