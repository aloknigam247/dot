#undef main

#include <cstdio>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

int dotTestMain(int argc, char *argv[]);

static void reportIdentityAndCleanCache() {
    if(!QStandardPaths::isTestModeEnabled()) {
        qFatal("The entrypoint driver must use test paths");
    }
    const QString cachePath =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("session.json"));
    std::fprintf(stdout, "application=%s\norganization=%s\n",
                 qPrintable(QCoreApplication::applicationName()),
                 qPrintable(QCoreApplication::organizationName()));
    if(QFile::exists(cachePath) && !QFile::remove(cachePath)) {
        qFatal("Could not remove the isolated test cache");
    }
}

int main(int argc, char *argv[]) {
    QStandardPaths::setTestModeEnabled(true);
    qAddPostRoutine(reportIdentityAndCleanCache);
    return dotTestMain(argc, argv);
}
