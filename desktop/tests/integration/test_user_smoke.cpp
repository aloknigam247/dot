// User-POV smoke harness: loads the real Main.qml in a QQuickWindow with the
// production NvimConnector, then injects key events through Qt's focus chain
// (NOT directly via NvimConnector::input) so the test exercises the exact
// path a user's keystroke takes — including focus management and grid layout.
//
// Specifically reproduces the bug Alok hit: after pressing ':', the very next
// keystroke was lost because focus was invalidated on a grid layout event. The
// test drives ':<command><CR>' through the focus chain and asserts nvim
// actually received it, so a focus regression fails hard.

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QtTest>
#include <QUuid>

#include "IntegrationHelpers.h"
#include "NvimConnector.h"
#include "WindowChrome.h"

using namespace dot;
using namespace dot::test;

namespace {

bool hasMultipleColors(const QImage &image) {
    if(image.isNull() || image.width() <= 0 || image.height() <= 0) return false;
    const QRgb first = image.pixel(0, 0);
    for(int y = 0; y < image.height(); ++y) {
        for(int x = 0; x < image.width(); ++x) {
            if(image.pixel(x, y) != first) return true;
        }
    }
    return false;
}

QQuickWindow *loadMainQml(QQmlApplicationEngine &engine, NvimConnector *conn,
                          WindowChrome *chrome = nullptr) {
    engine.rootContext()->setContextProperty(QStringLiteral("$connector"), conn);
    if(chrome) {
        engine.rootContext()->setContextProperty(QStringLiteral("$windowChrome"), chrome);
    }
    engine.loadFromModule(QStringLiteral("Dot"), QStringLiteral("Main"));
    if(engine.rootObjects().isEmpty()) return nullptr;
    return qobject_cast<QQuickWindow *>(engine.rootObjects().first());
}

QString missingExecutable() {
    const QString fileName = QStringLiteral("dot_missing_%1.exe")
                                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    return QDir::toNativeSeparators(QDir::temp().absoluteFilePath(fileName));
}

QString nativeStartError(const QString &executable) {
    QProcess probe;
    probe.start(executable, { QStringLiteral("--embed"), QStringLiteral("--clean") });
    if(probe.waitForStarted(5000)) {
        probe.kill();
        probe.waitForFinished();
        return {};
    }
    return probe.errorString();
}

// Polls a predicate up to timeoutMs, processing events between checks. Returns
// true on first success, false on timeout. Used in lieu of QSignalSpy::wait
// for predicates that depend on accumulated nvim state across multiple
// flush()es (e.g. cmdline content updating).
template <typename F>
bool waitUntil(F &&predicate, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while(!predicate()) {
        if(t.elapsed() >= timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return true;
}

} // namespace

class TestUserSmoke : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Tests run under QT_QPA_PLATFORM=minimal (the only headless platform
        // vcpkg's Qt deploys here). Synthetic key events still route through
        // QWindowSystemInterface so QTest::keyClick targeting QQuickWindow
        // works. Force the QML software renderer so QQuickWindow::grabWindow()
        // produces real pixels without a GPU surface.
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        qRegisterMetaType<dot::Notification>("dot::Notification");
        qRegisterMetaType<dot::ObjectHandlePtr>("dot::ObjectHandlePtr");
    }

    // Core reproduction of the "keystroke lost after ':'" focus bug. Drives a
    // full ':let ...<CR>' through Qt's focus chain (QTest::keyClick targets the
    // window's activeFocusItem), then asserts nvim actually applied the command.
    // A focus regression after the colon makes the trailing keys vanish and the
    // variable never gets set — a hard failure here, not a soft focus check.
    void keypressAfterColonReachesNvim() {
        NvimConnector conn;
        QSignalSpy flushSpy(&conn, &NvimConnector::flush);
        QVERIFY(startTestNvim(conn));

        QQmlApplicationEngine engine;
        QQuickWindow *window = loadMainQml(engine, &conn);
        QVERIFY2(window, "Main.qml failed to load");
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(waitForAttach(&conn));
        QVERIFY(waitUntil([&] { return !flushSpy.isEmpty(); }, 5000));

        QVERIFY2(window->activeFocusItem() != nullptr,
                 "No active focus item after attach — Shell didn't force focus");

        QTest::keyClick(window, Qt::Key_Colon);
        for(char c: QByteArrayLiteral("let g:smoke_after_colon = 7")) {
            QTest::keyClick(window, c);
        }
        QTest::keyClick(window, Qt::Key_Return);

        const bool applied = waitUntil([&] {
            const auto v = evalSync(conn, QStringLiteral("get(g:, 'smoke_after_colon', 0)"), 500);
            return v && v->toInt() == 7;
        }, 5000);
        QVERIFY2(applied,
                 "keystrokes after ':' never reached nvim — focus was lost after the colon");
        QVERIFY2(window->activeFocusItem() != nullptr, "Focus lost after command entry");
    }

    void launchFailureAfterQmlShowsInlineDetails() {
        WindowChrome chrome;
        NvimConnector conn;
        QQmlApplicationEngine engine;
        QSignalSpy activationSpy(&chrome, &WindowChrome::activated);
        QSignalSpy attachedSpy(&conn, &NvimConnector::attachComplete);
        QSignalSpy resizeSpy(conn.resizeCoalescer(), &ResizeCoalescer::resizeRequested);
        QSignalSpy startupErrorSpy(&conn, &NvimConnector::startupErrorChanged);
        const QString missing = missingExecutable();
        const QString nativeError = nativeStartError(missing);
        const QString expected =
            QStringLiteral("Failed to start Neovim.\nExecutable: %1\nError: %2")
                .arg(missing, nativeError);

        QVERIFY(!QFileInfo::exists(missing));
        QVERIFY(!nativeError.isEmpty());

        QQuickWindow *window = loadMainQml(engine, &conn, &chrome);
        QVERIFY(window);
        QVERIFY(!window->isVisible());
        chrome.activateOnShow(window);

        QVERIFY(!conn.start(missing, { QStringLiteral("--clean") }));
        QCOMPARE(conn.startupError(), expected);
        QCOMPARE(startupErrorSpy.count(), 1);
        QVERIFY(waitUntil([&] { return window->isVisible(); }, 5000));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCOMPARE(activationSpy.count(), 1);

        QVERIFY(waitUntil([&] {
            return window->findChild<QQuickItem *>(QStringLiteral("startupErrorText")) != nullptr;
        }, 5000));
        auto *details = window->findChild<QQuickItem *>(QStringLiteral("startupErrorText"));
        auto *shell = window->findChild<QQuickItem *>(QStringLiteral("shell"));
        QVERIFY(details);
        QVERIFY(shell);
        QVERIFY(details->isVisible());
        QCOMPARE(details->property("text").toString(), expected);
        QVERIFY(!shell->isVisible());
        QVERIFY(!shell->isEnabled());
        QCOMPARE(attachedSpy.count(), 0);
        QCOMPARE(resizeSpy.count(), 0);

        QVERIFY(window->close());
        QVERIFY(!window->isVisible());

        QVERIFY(startTestNvim(conn));
        QCOMPARE(conn.startupError(), QString{});
        QCOMPARE(startupErrorSpy.count(), 2);
    }

    void launchFailureBeforeQmlShowsInlineDetails() {
        WindowChrome chrome;
        NvimConnector conn;
        QQmlApplicationEngine engine;
        QSignalSpy activationSpy(&chrome, &WindowChrome::activated);
        QSignalSpy attachedSpy(&conn, &NvimConnector::attachComplete);
        QSignalSpy resizeSpy(conn.resizeCoalescer(), &ResizeCoalescer::resizeRequested);
        const QString missing = missingExecutable();
        const QString nativeError = nativeStartError(missing);
        const QString expected =
            QStringLiteral("Failed to start Neovim.\nExecutable: %1\nError: %2")
                .arg(missing, nativeError);

        QVERIFY(!QFileInfo::exists(missing));
        QVERIFY(!nativeError.isEmpty());
        QVERIFY(!conn.start(missing, { QStringLiteral("--clean") }));
        QCOMPARE(conn.startupError(), expected);

        QQuickWindow *window = loadMainQml(engine, &conn, &chrome);
        QVERIFY(window);
        QVERIFY(!window->isVisible());
        chrome.activateOnShow(window);
        QVERIFY(waitUntil([&] { return window->isVisible(); }, 5000));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCOMPARE(activationSpy.count(), 1);

        QVERIFY(waitUntil([&] {
            return window->findChild<QQuickItem *>(QStringLiteral("startupErrorText")) != nullptr;
        }, 5000));
        auto *chatToggle = window->findChild<QQuickItem *>(QStringLiteral("chatToggle"));
        auto *details = window->findChild<QQuickItem *>(QStringLiteral("startupErrorText"));
        auto *shell = window->findChild<QQuickItem *>(QStringLiteral("shell"));
        QVERIFY(chatToggle);
        QVERIFY(details);
        QVERIFY(shell);
        QVERIFY(details->isVisible());
        QCOMPARE(details->property("text").toString(), expected);
        QVERIFY(!chatToggle->isVisible());
        QVERIFY(!chatToggle->isEnabled());
        QVERIFY(!shell->isVisible());
        QVERIFY(!shell->isEnabled());

        QVERIFY(!conn.attachUi(80, 24));
        conn.tryResize(90, 30);
        conn.requestResize(100, 40);
        QTest::qWait(50);
        QCOMPARE(attachedSpy.count(), 0);
        QCOMPARE(resizeSpy.count(), 0);

        window->resize(480, 240);
        QTest::qWait(1200);
        QCOMPARE(activationSpy.count(), 1);
        QCOMPARE(details->property("text").toString(), expected);
        QVERIFY(details->isVisible());

        const QSharedPointer<QQuickItemGrabResult> grab = window->contentItem()->grabToImage();
        QSignalSpy grabSpy(grab.data(), &QQuickItemGrabResult::ready);
        if(grab->image().isNull()) QVERIFY(grabSpy.wait(5000));
        QVERIFY(hasMultipleColors(grab->image()));

        QVERIFY(window->close());
        QVERIFY(!window->isVisible());
    }
};

QTEST_MAIN(TestUserSmoke)
#include "test_user_smoke.moc"
