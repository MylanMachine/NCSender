#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace nc::AppPaths {

QString dataDir()
{
    static const QString dir = [] {
        // Automated tests point this at a temporary folder so they never
        // touch the real profiles, settings or log.
        const QString overrideDir = qEnvironmentVariable("NCSENDER_DATA_DIR");
        if (!overrideDir.isEmpty()) {
            QDir().mkpath(overrideDir);
            return QDir(overrideDir).absolutePath();
        }
        const QString exeDir = QCoreApplication::applicationDirPath();
        QTemporaryFile probe(QDir(exeDir).filePath(QStringLiteral(".ncsender_write_test_XXXXXX")));
        if (probe.open())
            return exeDir;

        QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (fallback.isEmpty())
            fallback = QDir::homePath() + QStringLiteral("/NCSender");
        QDir().mkpath(fallback);
        return fallback;
    }();
    return dir;
}

QString profilesFile()
{
    return QDir(dataDir()).filePath(QStringLiteral("profiles.json"));
}

QString settingsFile()
{
    return QDir(dataDir()).filePath(QStringLiteral("ncsender.ini"));
}

QString logFile()
{
    return QDir(dataDir()).filePath(QStringLiteral("transfer_log.csv"));
}

} // namespace nc::AppPaths
