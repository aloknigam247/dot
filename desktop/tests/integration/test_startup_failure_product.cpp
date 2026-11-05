#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <QUuid>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#endif

namespace {

#ifdef Q_OS_WIN

using Microsoft::WRL::ComPtr;

constexpr UINT kPrintWindowRenderFullContent = 0x00000002;
constexpr int kProcessTimeoutMs = 30000;
constexpr int kUiTimeoutMs = 15000;

struct WindowSearch {
    DWORD processId = 0;
    HWND window = nullptr;
};

BOOL CALLBACK findDotWindowCallback(HWND window, LPARAM data) {
    DWORD processId = 0;
    wchar_t className[256]{};
    wchar_t title[256]{};
    auto *search = reinterpret_cast<WindowSearch *>(data);
    GetWindowThreadProcessId(window, &processId);
    if(processId != search->processId || !IsWindowVisible(window)) return TRUE;

    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if(QString::fromWCharArray(title) != QStringLiteral("dot")) return TRUE;
    if(!QString::fromWCharArray(className).startsWith(QStringLiteral("Qt"))) return TRUE;

    search->window = window;
    return FALSE;
}

HWND findDotWindow(qint64 processId) {
    WindowSearch search{ static_cast<DWORD>(processId), nullptr };
    EnumWindows(&findDotWindowCallback, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

template <typename Predicate>
bool waitUntil(Predicate &&predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while(!predicate()) {
        if(timer.elapsed() >= timeoutMs) return false;
        QTest::qWait(50);
    }
    return true;
}

QProcessEnvironment productEnvironment(const QString &scratchPath, bool isolatePath) {
    const QStringList removed{
        QStringLiteral("DOT_BOOT_PROFILE"),
        QStringLiteral("DOT_BOOT_PROFILE_FILE"),
        QStringLiteral("DOT_PROFILE_PAINT"),
        QStringLiteral("DOT_TEST_FRAME"),
        QStringLiteral("DOT_TEST_OUTPUT"),
        QStringLiteral("QML2_IMPORT_PATH"),
        QStringLiteral("QML_DISABLE_DISK_CACHE"),
        QStringLiteral("QML_IMPORT_PATH"),
        QStringLiteral("QT_PLUGIN_PATH"),
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
        QStringLiteral("QT_QUICK_BACKEND"),
    };
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for(const QString &name: removed) environment.remove(name);

    environment.insert(QStringLiteral("APPDATA"), scratchPath);
    environment.insert(QStringLiteral("LOCALAPPDATA"), scratchPath);
    if(isolatePath) environment.insert(QStringLiteral("PATH"), scratchPath);
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("windows"));
    environment.insert(QStringLiteral("XDG_CACHE_HOME"), scratchPath);
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), scratchPath);
    environment.insert(QStringLiteral("XDG_DATA_HOME"), scratchPath);
    environment.insert(QStringLiteral("XDG_STATE_HOME"), scratchPath);
    return environment;
}

QString nativeStartError(const QProcessEnvironment &environment, const QString &workingDirectory) {
    QProcess probe;
    const QString missing =
        QDir(workingDirectory)
            .filePath(QStringLiteral("dot_missing_%1.exe")
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    probe.setProcessEnvironment(environment);
    probe.setWorkingDirectory(workingDirectory);
    probe.start(missing, {});
    if(probe.waitForStarted(5000)) {
        probe.kill();
        probe.waitForFinished();
        return {};
    }
    return probe.errorString();
}

QString processDiagnostics(QProcess &process) {
    return QStringLiteral("state=%1 exitStatus=%2 exitCode=%3 stderr=%4 stdout=%5")
        .arg(static_cast<int>(process.state()))
        .arg(static_cast<int>(process.exitStatus()))
        .arg(process.exitCode())
        .arg(QString::fromLocal8Bit(process.readAllStandardError()))
        .arg(QString::fromLocal8Bit(process.readAllStandardOutput()));
}

bool accessibilityTreeContains(HWND window, const QString &expectedName) {
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElement> match;
    ComPtr<IUIAutomationElement> root;
    VARIANT value{};
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if(FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
    const auto uninitialize = qScopeGuard([initialized] {
        if(SUCCEEDED(initialized)) CoUninitialize();
    });

    if(FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(automation.GetAddressOf())))) {
        return false;
    }

    if(FAILED(automation->ElementFromHandle(window, root.GetAddressOf()))) return false;

    value.vt = VT_BSTR;
    value.bstrVal = SysAllocStringLen(reinterpret_cast<const OLECHAR *>(expectedName.utf16()),
                                      static_cast<UINT>(expectedName.size()));
    if(!value.bstrVal) return false;
    const auto clearValue = qScopeGuard([&value] { VariantClear(&value); });

    if(FAILED(automation->CreatePropertyCondition(UIA_NamePropertyId, value,
                                                  condition.GetAddressOf()))) {
        return false;
    }

    if(FAILED(root->FindFirst(TreeScope_Subtree, condition.Get(), match.GetAddressOf()))) {
        return false;
    }
    return match != nullptr;
}

bool renderedWindowHasMultipleColors(HWND window) {
    BITMAPINFO bitmapInfo{};
    void *bits = nullptr;
    RECT rect{};
    if(!GetWindowRect(window, &rect)) return false;

    const int height = rect.bottom - rect.top;
    const int width = rect.right - rect.left;
    if(width <= 0 || height <= 0) return false;

    HDC windowDc = GetWindowDC(window);
    if(!windowDc) return false;
    HDC memoryDc = CreateCompatibleDC(windowDc);
    if(!memoryDc) {
        ReleaseDC(window, windowDc);
        return false;
    }

    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HBITMAP bitmap = CreateDIBSection(windowDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if(!bitmap || !bits) {
        if(bitmap) DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(window, windowDc);
        return false;
    }

    HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    const auto cleanup = qScopeGuard([&] {
        SelectObject(memoryDc, previous);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(window, windowDc);
    });
    if(!PrintWindow(window, memoryDc, kPrintWindowRenderFullContent)) return false;

    const auto *pixels = static_cast<const quint32 *>(bits);
    const qsizetype pixelCount = static_cast<qsizetype>(width) * static_cast<qsizetype>(height);
    const quint32 first = pixels[0];
    for(qsizetype i = 1; i < pixelCount; ++i) {
        if(pixels[i] != first) return true;
    }
    return false;
}

#endif

} // namespace

class TestStartupFailureProduct : public QObject {
    Q_OBJECT

private:
#ifdef Q_OS_WIN
    void verifyMissingExecutable(bool keepStdinOpen);
#endif

private slots:
    void initTestCase() {
        QVERIFY2(QFileInfo::exists(QStringLiteral(DOT_APP_EXE_PATH)),
                 "The deployed dot executable must exist");
    }

    void missingExecutableKeepsProductWindowOpen() {
#ifdef Q_OS_WIN
        verifyMissingExecutable(false);
#else
        QSKIP("Windows product test");
#endif
    }

    void missingExecutableWithOpenStdinClosesNormally() {
#ifdef Q_OS_WIN
        verifyMissingExecutable(true);
#else
        QSKIP("Windows product test");
#endif
    }

    void successfulProductExitsAfterQa() {
#ifdef Q_OS_WIN
        QStringList arguments{
            QStringLiteral("--clean"),
            QStringLiteral("-c"),
            QStringLiteral("qa!"),
        };
        QProcess process;
        const QString templatePath = QDir::temp().absoluteFilePath(
            QStringLiteral("dot_startup_success_%1_XXXXXX")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QTemporaryDir scratch(templatePath);

        QVERIFY(scratch.isValid());
        QProcessEnvironment environment = productEnvironment(scratch.path(), false);
        const QStringList searchPaths = environment.value(QStringLiteral("PATH"))
                                            .split(QDir::listSeparator(), Qt::SkipEmptyParts);
        const QString nvim = QStandardPaths::findExecutable(QStringLiteral("nvim"), searchPaths);
        QVERIFY2(!nvim.isEmpty(), "A working nvim is required for the success control");

        process.setArguments(arguments);
        process.setProcessEnvironment(environment);
        process.setProgram(QStringLiteral(DOT_APP_EXE_PATH));
        process.setWorkingDirectory(scratch.path());
        const auto cleanup = qScopeGuard([&] {
            if(process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished();
            }
        });

        process.start();
        QVERIFY2(process.waitForStarted(5000), qPrintable(processDiagnostics(process)));
        QVERIFY2(process.waitForFinished(kProcessTimeoutMs),
                 qPrintable(processDiagnostics(process)));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
#else
        QSKIP("Windows product test");
#endif
    }
};

#ifdef Q_OS_WIN

void TestStartupFailureProduct::verifyMissingExecutable(bool keepStdinOpen) {
    QStringList arguments{ QStringLiteral("--clean") };
    QProcess process;
    HWND window = nullptr;
    const QString templatePath =
        QDir::temp().absoluteFilePath(QStringLiteral("dot_startup_failure_%1_XXXXXX")
                                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QTemporaryDir scratch(templatePath);

    QVERIFY(scratch.isValid());
    if(keepStdinOpen) arguments.append(QStringLiteral("-"));

    QProcessEnvironment environment = productEnvironment(scratch.path(), true);
    const QString nativeError = nativeStartError(environment, scratch.path());
    const QString expected =
        QStringLiteral("Failed to start Neovim.\nExecutable: nvim\nError: %1").arg(nativeError);
    const QString appDirectory = QFileInfo(QStringLiteral(DOT_APP_EXE_PATH)).absolutePath();
    const QString windowsDirectory = qEnvironmentVariable("WINDIR");

    QVERIFY(!nativeError.isEmpty());
    QVERIFY(!QFileInfo(QDir(appDirectory).filePath(QStringLiteral("nvim.exe"))).exists());
    QVERIFY(!QFileInfo(QDir(scratch.path()).filePath(QStringLiteral("nvim.exe"))).exists());
    QVERIFY(!QFileInfo(QDir(windowsDirectory).filePath(QStringLiteral("nvim.exe"))).exists());
    QVERIFY(
        !QFileInfo(QDir(windowsDirectory).filePath(QStringLiteral("System32/nvim.exe"))).exists());

    process.setArguments(arguments);
    process.setProcessEnvironment(environment);
    process.setProgram(QStringLiteral(DOT_APP_EXE_PATH));
    process.setWorkingDirectory(scratch.path());
    const auto cleanup = qScopeGuard([&] {
        if(process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
        }
    });

    process.start();
    QVERIFY2(process.waitForStarted(5000), qPrintable(processDiagnostics(process)));
    const bool foundWindow = waitUntil([&] {
        if(process.state() == QProcess::NotRunning) return false;
        window = findDotWindow(process.processId());
        return window != nullptr;
    }, kProcessTimeoutMs);
    QVERIFY2(foundWindow, qPrintable(processDiagnostics(process)));
    QVERIFY(process.state() != QProcess::NotRunning);
    if(keepStdinOpen) QVERIFY(process.isWritable());

    QVERIFY2(waitUntil([&] { return accessibilityTreeContains(window, expected); }, kUiTimeoutMs),
             qPrintable(expected));
    QVERIFY(renderedWindowHasMultipleColors(window));

    QVERIFY(PostMessageW(window, WM_CLOSE, 0, 0));
    QVERIFY2(process.waitForFinished(kUiTimeoutMs), qPrintable(processDiagnostics(process)));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
}

#endif

QTEST_GUILESS_MAIN(TestStartupFailureProduct)
#include "test_startup_failure_product.moc"
