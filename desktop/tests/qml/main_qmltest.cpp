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
    dot::NvimConnector *m_connector = nullptr;

    static dot::CmdlineLine line(const QVariantList &chunks) {
        dot::CmdlineLine result;
        for(const auto &chunk: chunks) {
            const auto map = chunk.toMap();
            result.append({ map.value("attrId").toInt(), 0, map.value("text").toString().toUtf8() });
        }
        return result;
    }

public:
    explicit TestSetup(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE void defineHighlight(int id, const QVariantMap &attributes) {
        msgpack::sbuffer buffer;
        msgpack::packer<msgpack::sbuffer> pk(buffer);
        pk.pack_map(static_cast<uint32_t>(attributes.size()));
        for(auto it = attributes.begin(); it != attributes.end(); ++it) {
            pk.pack(it.key().toStdString());
            if(it.value().metaType().id() == QMetaType::Bool) pk.pack(it.value().toBool());
            else pk.pack(it.value().toInt());
        }
        m_connector->highlights()->defineAttr(id, msgpack::unpack(buffer.data(), buffer.size()).get());
    }

    Q_INVOKABLE void emitFlush() { emit m_connector->flush(); }
    Q_INVOKABLE void hideBlock() { m_connector->cmdline()->hideBlock(); }
    Q_INVOKABLE void hideCmdline(int level) { QVERIFY(m_connector->cmdline()->hide(level)); }
    Q_INVOKABLE void resetCmdline() { m_connector->cmdline()->reset(); }
    Q_INVOKABLE void setOverlayColors(const QColor &fg, const QColor &bg) {
        constexpr int overlay = 81;
        const auto attr = dot::test::colorAttrs(static_cast<int>(fg.rgb() & 0xffffffU),
                                               static_cast<int>(bg.rgb() & 0xffffffU));
        m_connector->highlights()->defineAttr(overlay, attr.get());
        m_connector->highlights()->setHighlightGroup(QStringLiteral("NormalFloat"), overlay);
        m_connector->colorPalette()->publishContent();
    }
    Q_INVOKABLE void setPosition(int level, int pos) { QVERIFY(m_connector->cmdline()->setPosition(level, pos)); }
    Q_INVOKABLE void setRoleResolved(const QString &role, bool resolved) {
        if(role == QStringLiteral("chrome")) {
            if(resolved) m_connector->colorPalette()->stageChromeSnapshot(distinctChromeSnapshot().get());
            else m_connector->colorPalette()->invalidateChrome(QStringLiteral("unresolved test palette"));
        } else if(role == QStringLiteral("cursor")) {
            m_connector->modeInfo()->setCurrentMode(QStringLiteral("normal"), resolved ? 0 : -1);
        } else {
            QCOMPARE(role, QStringLiteral("overlay"));
            m_connector->highlights()->setHighlightGroup(QStringLiteral("NormalFloat"), resolved ? 0 : 999);
        }
        m_connector->colorPalette()->publishContent();
    }
    Q_INVOKABLE void setSpecialCharacter(int level, const QString &character, bool shift) {
        QVERIFY(m_connector->cmdline()->setSpecialCharacter(level, character, shift));
    }
    Q_INVOKABLE void showBlock(const QVariantList &lines) {
        QList<dot::CmdlineLine> block;
        for(const auto &entry: lines) block.append(line(entry.toList()));
        m_connector->cmdline()->showBlock(block);
    }
    Q_INVOKABLE void showCmdline(int level, const QString &firstChar, const QString &prompt,
                                 const QVariantList &chunks, int bytePosition, int indent) {
        dot::CmdlineSession state;
        state.content = line(chunks);
        state.firstCharacter = firstChar;
        state.indent = indent;
        state.position = bytePosition;
        state.prompt = prompt;
        QVERIFY(m_connector->cmdline()->show(level, state));
    }

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
        m_connector = connector;
        dot::test::seedColorState(*connector);
        const auto chrome = distinctChromeSnapshot();
        connector->colorPalette()->stageChromeSnapshot(chrome.get());
        connector->colorPalette()->publishContent();
        engine->rootContext()->setContextProperty(QStringLiteral("$connector"), connector);
        engine->rootContext()->setContextProperty(QStringLiteral("$cmdlineFixture"), this);
        engine->rootContext()->setContextProperty(QStringLiteral("$keys"), this);
        engine->rootContext()->setContextProperty(
            QStringLiteral("$expectedScaleFactor"),
            qEnvironmentVariable("QT_SCALE_FACTOR", "1").toDouble());
    }
};

} // namespace

QUICK_TEST_MAIN_WITH_SETUP(qml_tests, TestSetup)

#include "main_qmltest.moc"
