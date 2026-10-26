#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QScopeGuard>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "AppIcon.h"

static void compareIcon(const QImage &actual, int size, bool allowNativeRounding = false) {
    // The deployed Qt build cannot decode PNG reference images.
    const QImage expected = QIcon(QStringLiteral(DOT_ICON_PATH))
                                .pixmap(QSize(size, size))
                                .toImage()
                                .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QVERIFY(!expected.isNull());
    QCOMPARE(actual.size(), QSize(size, size));
    QCOMPARE(actual.pixelColor(0, 0).alpha(), 0);
    const QColor plate = actual.pixelColor(size / 2, size / 2);
    QCOMPARE(plate.alpha(), 255);
    QVERIFY(plate.green() > plate.red());
    QVERIFY(plate.red() > plate.blue());
    if(!allowNativeRounding) {
        QCOMPARE(actual, expected);
        return;
    }
    // Win32 and Qt round premultiplied antialiasing channels differently.
    for(int y = 0; y < size; ++y) {
        for(int x = 0; x < size; ++x) {
            const QRgb a = actual.pixel(x, y);
            const QRgb e = expected.pixel(x, y);
            QCOMPARE(qAlpha(a), qAlpha(e));
            if(qAlpha(e) == 0 || qAlpha(e) == 255) {
                QCOMPARE(a, e);
            } else {
                QVERIFY(qAbs(qRed(a) - qRed(e)) <= 1);
                QVERIFY(qAbs(qGreen(a) - qGreen(e)) <= 1);
                QVERIFY(qAbs(qBlue(a) - qBlue(e)) <= 1);
            }
        }
    }
}

static void frameSizes() {
    QTest::addColumn<int>("size");
    for(const int size: { 16, 32, 48, 64, 128, 256 }) {
        QTest::newRow(qPrintable(QString::number(size))) << size;
    }
}

class TestAppIcon : public QObject {
    Q_OBJECT
private slots:
#ifdef Q_OS_WIN
    void nativeFrames();
    void nativeFrames_data();
#endif
    void rasterisesEveryFrame();
    void rasterisesEveryFrame_data();
    void setsNonNullWindowIcon();
};

#ifdef Q_OS_WIN
void TestAppIcon::nativeFrames_data() { frameSizes(); }

void TestAppIcon::nativeFrames() {
    QFETCH(int, size);
    const QString executable = QStringLiteral(DOT_APP_EXE_PATH);
    const HMODULE module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(executable.utf16()), nullptr,
                                          LOAD_LIBRARY_AS_DATAFILE);
    QVERIFY2(module, qPrintable(executable));
    const auto releaseModule = qScopeGuard([module]() { FreeLibrary(module); });
    const auto icon =
        static_cast<HICON>(LoadImageW(module, L"IDI_ICON1", IMAGE_ICON, size, size, 0));
    QVERIFY2(icon, "The executable must embed the application icon");
    const auto releaseIcon = qScopeGuard([icon]() { DestroyIcon(icon); });
    const QImage actual =
        QImage::fromHICON(icon).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    compareIcon(actual, size, true);
}
#endif

void TestAppIcon::setsNonNullWindowIcon() {
    dot::setupApplicationIcon(*qGuiApp);
    const QIcon icon = QGuiApplication::windowIcon();
    QVERIFY(!icon.isNull());
}

void TestAppIcon::rasterisesEveryFrame_data() { frameSizes(); }

void TestAppIcon::rasterisesEveryFrame() {
    QFETCH(int, size);
    dot::setupApplicationIcon(*qGuiApp);
    const QIcon icon = QGuiApplication::windowIcon();
    const QImage actual = icon.pixmap(QSize(size, size))
                              .toImage()
                              .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    compareIcon(actual, size);
}

QTEST_MAIN(TestAppIcon)
#include "test_app_icon.moc"
