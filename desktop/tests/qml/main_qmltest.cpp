#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtQuickTest>

#include "NvimConnector.h"

namespace {

class TestSetup : public QObject {
    Q_OBJECT
public:
    explicit TestSetup(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    void applicationAvailable() { QQuickWindow::setGraphicsApi(QSGRendererInterface::Software); }

    void qmlEngineAvailable(QQmlEngine *engine) {
        // Provide a real NvimConnector but do NOT call start() — its child
        // models are valid QObjects with sensible defaults for binding tests.
        auto *connector = new dot::NvimConnector(this);
        engine->rootContext()->setContextProperty(QStringLiteral("$connector"), connector);
        engine->rootContext()->setContextProperty(
            QStringLiteral("$expectedScaleFactor"),
            qEnvironmentVariable("QT_SCALE_FACTOR", "1").toDouble());
    }
};

} // namespace

QUICK_TEST_MAIN_WITH_SETUP(qml_tests, TestSetup)

#include "main_qmltest.moc"
