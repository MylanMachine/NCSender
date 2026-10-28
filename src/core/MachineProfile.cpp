#include "MachineProfile.h"

#include "AppPaths.h"

#include <QDir>
#include <QJsonValue>

namespace nc {

QJsonObject MachineProfile::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("port"), portName);
    o.insert(QStringLiteral("baudRate"), baudRate);
    o.insert(QStringLiteral("dataBits"), dataBits);
    o.insert(QStringLiteral("parity"), toString(parity));
    o.insert(QStringLiteral("stopBits"), toString(stopBits));
    o.insert(QStringLiteral("flowControl"), toString(flowControl));
    o.insert(QStringLiteral("lineEnding"), toString(lineEnding));
    o.insert(QStringLiteral("leaderChar"), leaderByte);
    o.insert(QStringLiteral("leaderCount"), leaderCount);
    o.insert(QStringLiteral("trailerChar"), trailerByte);
    o.insert(QStringLiteral("trailerCount"), trailerCount);
    o.insert(QStringLiteral("ensurePercent"), ensurePercent);
    o.insert(QStringLiteral("charDelayMs"), charDelayMs);
    o.insert(QStringLiteral("waitForXon"), waitForXon);
    o.insert(QStringLiteral("xonTimeoutSec"), xonTimeoutSec);
    o.insert(QStringLiteral("programFolder"), programFolder);
    o.insert(QStringLiteral("receiveFolder"), receiveFolder);
    o.insert(QStringLiteral("receiveExtension"), receiveExtension);
    o.insert(QStringLiteral("receiveStartTimeoutSec"), receiveStartTimeoutSec);
    o.insert(QStringLiteral("receiveIdleTimeoutSec"), receiveIdleTimeoutSec);
    o.insert(QStringLiteral("stripReceivedControlChars"), stripReceivedControlChars);
    return o;
}

MachineProfile MachineProfile::fromJson(const QJsonObject& o)
{
    MachineProfile p;

    auto str = [&](const char* key, const QString& def) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isString())
            return v.toString();
        if (v.isDouble())
            return QString::number(v.toDouble());
        return def;
    };
    auto num = [&](const char* key, int def) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isDouble())
            return v.toInt(def);
        if (v.isString()) {
            bool ok = false;
            const int n = v.toString().toInt(&ok);
            return ok ? n : def;
        }
        return def;
    };
    auto flag = [&](const char* key, bool def) {
        const QJsonValue v = o.value(QLatin1String(key));
        return v.isBool() ? v.toBool() : def;
    };

    p.name = str("name", p.name).trimmed();
    p.portName = str("port", p.portName).trimmed();
    p.baudRate = num("baudRate", p.baudRate);
    p.dataBits = num("dataBits", p.dataBits);
    p.parity = parityFromString(str("parity", toString(p.parity)), p.parity);
    p.stopBits = stopBitsFromString(str("stopBits", toString(p.stopBits)), p.stopBits);
    p.flowControl = flowControlFromString(str("flowControl", toString(p.flowControl)), p.flowControl);
    p.lineEnding = lineEndingFromString(str("lineEnding", toString(p.lineEnding)), p.lineEnding);
    p.leaderByte = num("leaderChar", p.leaderByte);
    p.leaderCount = num("leaderCount", p.leaderCount);
    p.trailerByte = num("trailerChar", p.trailerByte);
    p.trailerCount = num("trailerCount", p.trailerCount);
    p.ensurePercent = flag("ensurePercent", p.ensurePercent);
    p.charDelayMs = num("charDelayMs", p.charDelayMs);
    p.waitForXon = flag("waitForXon", p.waitForXon);
    p.xonTimeoutSec = num("xonTimeoutSec", p.xonTimeoutSec);
    p.programFolder = str("programFolder", p.programFolder);
    p.receiveFolder = str("receiveFolder", p.receiveFolder);
    p.receiveExtension = str("receiveExtension", p.receiveExtension);
    p.receiveStartTimeoutSec = num("receiveStartTimeoutSec", p.receiveStartTimeoutSec);
    p.receiveIdleTimeoutSec = num("receiveIdleTimeoutSec", p.receiveIdleTimeoutSec);
    p.stripReceivedControlChars = flag("stripReceivedControlChars", p.stripReceivedControlChars);
    return p;
}

QString MachineProfile::validate() const
{
    const QString who = name.trimmed().isEmpty() ? QStringLiteral("A profile") : QStringLiteral("Profile '%1'").arg(name);

    if (name.trimmed().isEmpty())
        return QStringLiteral("Profile name cannot be empty.");
    if (portName.trimmed().isEmpty())
        return QStringLiteral("%1 has no COM port.").arg(who);
    if (baudRate < 50 || baudRate > 4000000)
        return QStringLiteral("%1 has an invalid baud rate (%2).").arg(who).arg(baudRate);
    if (dataBits < 5 || dataBits > 8)
        return QStringLiteral("%1: data bits must be 5, 6, 7 or 8.").arg(who);
    if (stopBits == StopBits::OneAndHalf && dataBits != 5)
        return QStringLiteral("%1: 1.5 stop bits can only be used with 5 data bits.").arg(who);
    if (stopBits == StopBits::Two && dataBits == 5)
        return QStringLiteral("%1: 2 stop bits cannot be used with 5 data bits (use 1.5).").arg(who);
    if (leaderByte < 0 || leaderByte > 255 || trailerByte < 0 || trailerByte > 255)
        return QStringLiteral("%1: leader/trailer character must be 0-255.").arg(who);
    if (leaderCount < 0 || leaderCount > 10000 || trailerCount < 0 || trailerCount > 10000)
        return QStringLiteral("%1: leader/trailer count must be 0-10000.").arg(who);
    if (charDelayMs < 0 || charDelayMs > 10000)
        return QStringLiteral("%1: character delay must be 0-10000 ms.").arg(who);
    if (xonTimeoutSec < 0 || receiveStartTimeoutSec < 0 || receiveIdleTimeoutSec < 0)
        return QStringLiteral("%1: timeouts cannot be negative.").arg(who);
    if (xonTimeoutSec > 86400 || receiveStartTimeoutSec > 86400 || receiveIdleTimeoutSec > 86400)
        return QStringLiteral("%1: timeouts can be at most 86400 s (24 hours).").arg(who);
    static const QString badChars = QStringLiteral("/\\:*?\"<>|");
    for (const QChar c : receiveExtension) {
        if (badChars.contains(c))
            return QStringLiteral("%1: the receive file extension contains an invalid character.").arg(who);
    }
    return {};
}

QString MachineProfile::summary() const
{
    QString flow;
    switch (flowControl) {
    case FlowControl::None: flow = QStringLiteral("no flow control"); break;
    case FlowControl::XonXoff: flow = QStringLiteral("XON/XOFF"); break;
    case FlowControl::RtsCts: flow = QStringLiteral("RTS/CTS"); break;
    }
    QString s = QStringLiteral("%1   %2 %3-%4-%5   %6   %7")
                    .arg(portName)
                    .arg(baudRate)
                    .arg(dataBits)
                    .arg(parityLetter(parity))
                    .arg(toString(stopBits), flow, displayName(lineEnding));
    if (waitForXon)
        s += QStringLiteral("   waits for XON");
    if (charDelayMs > 0)
        s += QStringLiteral("   %1 ms/char").arg(charDelayMs);
    return s;
}

QString MachineProfile::effectiveReceiveFolder() const
{
    // Relative folders are taken relative to the app's data folder, never
    // to whatever the current working directory happens to be.
    const QDir base(AppPaths::dataDir());
    if (!receiveFolder.trimmed().isEmpty())
        return QDir::cleanPath(base.absoluteFilePath(receiveFolder.trimmed()));
    if (!programFolder.trimmed().isEmpty())
        return QDir::cleanPath(QDir(base.absoluteFilePath(programFolder.trimmed())).filePath(QStringLiteral("received")));
    return base.filePath(QStringLiteral("received"));
}

} // namespace nc
