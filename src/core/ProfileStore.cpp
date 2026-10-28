#include "ProfileStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

namespace nc {

ProfileStore::ProfileStore(const QString& filePath)
    : m_path(filePath)
    , m_profiles(defaultProfiles())
{
}

bool ProfileStore::load(QString* error)
{
    m_loadedFromFile = false;
    QFile f(m_path);
    if (!f.exists()) {
        m_profiles = defaultProfiles();
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        m_profiles = defaultProfiles();
        if (error)
            *error = QStringLiteral("Cannot open %1: %2").arg(QDir::toNativeSeparators(m_path), f.errorString());
        return false;
    }

    QByteArray data = f.readAll();
    if (data.startsWith("\xEF\xBB\xBF"))
        data.remove(0, 3); // UTF-8 BOM written by some editors / PowerShell

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    const bool parsed = pe.error == QJsonParseError::NoError && doc.isObject();
    const bool hasList = parsed && doc.object().value(QStringLiteral("profiles")).isArray();
    if (!hasList) {
        m_profiles = defaultProfiles();
        // Keep the unreadable file safe: later saves overwrite .bak, but this
        // copy is only ever written once per bad file.
        const QString keep = m_path + QStringLiteral(".unreadable-")
                             + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
        QFile::copy(m_path, keep);
        if (error) {
            const QString why = parsed ? QStringLiteral("it has no \"profiles\" list")
                                       : QStringLiteral("%1 at character %2").arg(pe.errorString()).arg(pe.offset);
            *error = QStringLiteral("%1 could not be read (%2). The built-in example profiles are being used. "
                                    "A copy of the unreadable file was kept as %3.")
                         .arg(QDir::toNativeSeparators(m_path), why, QFileInfo(keep).fileName());
        }
        return false;
    }

    QList<MachineProfile> list;
    const QJsonArray arr = doc.object().value(QStringLiteral("profiles")).toArray();
    for (const QJsonValue& v : arr) {
        if (v.isObject())
            list.append(MachineProfile::fromJson(v.toObject()));
    }
    m_profiles = list;
    m_loadedFromFile = true;
    return true;
}

bool ProfileStore::save(QString* error)
{
    QJsonArray arr;
    for (const MachineProfile& p : m_profiles)
        arr.append(p.toJson());
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("profiles"), arr);

    QDir().mkpath(QFileInfo(m_path).absolutePath());
    if (QFile::exists(m_path)) {
        const QString bak = m_path + QStringLiteral(".bak");
        QFile::remove(bak);
        QFile::copy(m_path, bak);
    }

    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1: %2").arg(QDir::toNativeSeparators(m_path), f.errorString());
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error)
            *error = QStringLiteral("Cannot save %1: %2").arg(QDir::toNativeSeparators(m_path), f.errorString());
        return false;
    }
    m_loadedFromFile = true;
    return true;
}

std::optional<MachineProfile> ProfileStore::find(const QString& name) const
{
    for (const MachineProfile& p : m_profiles) {
        if (p.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0)
            return p;
    }
    return std::nullopt;
}

QList<MachineProfile> ProfileStore::defaultProfiles()
{
    // The shop's two Fanuc machines, both at 9600 baud. Data format is the
    // standard Fanuc ISO setting (7 data bits, even parity, 2 stop bits,
    // XON/XOFF); the COM ports depend on which adapter each cable is in.
    // Check both against the controls' I/O channel settings and fix them in
    // Profiles > Edit Profiles if they differ.
    MachineProfile robodrill;
    robodrill.name = QStringLiteral("Robodrill");
    robodrill.portName = QStringLiteral("COM3");
    robodrill.baudRate = 9600;
    robodrill.dataBits = 7;
    robodrill.parity = Parity::Even;
    robodrill.stopBits = StopBits::Two;
    robodrill.flowControl = FlowControl::XonXoff;

    MachineProfile hawk = robodrill;
    hawk.name = QStringLiteral("Hawk 150 Lathe (Fanuc 21i-T)");
    hawk.portName = QStringLiteral("COM4");

    // Profiles for testing with com0com virtual port pairs (see README).
    MachineProfile testA = robodrill;
    testA.name = QStringLiteral("Virtual Test A");
    testA.portName = QStringLiteral("COM10");
    testA.leaderCount = 10;
    testA.trailerCount = 10;
    testA.receiveIdleTimeoutSec = 5;

    MachineProfile testB = testA;
    testB.name = QStringLiteral("Virtual Test B");
    testB.portName = QStringLiteral("COM12");

    return {robodrill, hawk, testA, testB};
}

} // namespace nc
