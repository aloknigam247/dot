#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtQuickTest>

#include "NvimConnector.h"
#include "support/ColorHelpers.h"

namespace {

class TestSetup : public QObject {
    Q_OBJECT
public:
    explicit TestSetup(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    void applicationAvailable() { QQuickWindow::setGraphicsApi(QSGRendererInterface::Software); }

    void qmlEngineAvailable(QQmlEngine *engine) {
        // Isolated QML has no Neovim process to supply its required color snapshot.
        auto *connector = new dot::NvimConnector(this);
        dot::test::seedColorState(*connector);
        engine->rootContext()->setContextProperty(QStringLiteral("$connector"), connector);
        engine->rootContext()->setContextProperty(
            QStringLiteral("$expectedScaleFactor"),
            qEnvironmentVariable("QT_SCALE_FACTOR", "1").toDouble());
    }
};

} // namespace

QUICK_TEST_MAIN_WITH_SETUP(qml_tests, TestSetup)

#include "main_qmltest.moc"
