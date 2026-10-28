#include <QRegularExpression>
#include <QtTest>

#include "Version.h"
#include "VersionString.h"

// Stringify the CMake-injected PROJECT_VERSION token so the header value can be
// checked against an independently supplied expectation (not a sibling macro
// from the same substitution, which would be tautological).
#define DOT_STR2(x) #x
#define DOT_STR(x)  DOT_STR2(x)

class TestVersion : public QObject {
    Q_OBJECT
private slots:
    void macroIsSemver() {
        const QString v = QString::fromLatin1(DOT_VERSION);
        QVERIFY2(!v.isEmpty(), "DOT_VERSION must not be empty");
        QRegularExpression re(QStringLiteral("^\\d+\\.\\d+\\.\\d+$"));
        QVERIFY2(re.match(v).hasMatch(),
                 qPrintable(QStringLiteral("DOT_VERSION '%1' is not X.Y.Z").arg(v)));
    }

    void macroMatchesProjectVersion() {
        QCOMPARE(QString::fromLatin1(DOT_VERSION),
                 QString::fromLatin1(DOT_STR(DOT_EXPECTED_VERSION)));
    }

    void cliStringIsExact() {
        QCOMPARE(dot::versionString(), QStringLiteral("dot ") + QString::fromLatin1(DOT_VERSION));
    }
};

QTEST_GUILESS_MAIN(TestVersion)
#include "test_version.moc"
