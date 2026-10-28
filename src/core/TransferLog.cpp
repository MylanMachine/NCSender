#include "TransferLog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>

namespace nc {

namespace {

const char* const kHeader =
    "Timestamp,Machine,Direction,Profile,Port,File,FileSize,BytesTransferred,Result,DurationSec,Detail";
const QString kTimeFormat = QStringLiteral("yyyy-MM-dd HH:mm:ss");

QString csvField(QString s)
{
    s.replace(QLatin1String("\r\n"), QLatin1String(" "));
    s.replace(QLatin1Char('\r'), QLatin1Char(' '));
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"'))) {
        s.replace(QLatin1String("\""), QLatin1String("\"\""));
        s = QLatin1Char('"') + s + QLatin1Char('"');
    }
    return s;
}

QStringList parseCsvLine(const QString& line)
{
    QStringList fields;
    QString cur;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    cur.append(QLatin1Char('"'));
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cur.append(c);
            }
        } else if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == QLatin1Char(',')) {
            fields.append(cur);
            cur.clear();
        } else {
            cur.append(c);
        }
    }
    fields.append(cur);
    return fields;
}

} // namespace

TransferLogEntry TransferLogEntry::fromResult(const TransferResult& r)
{
    TransferLogEntry e;
    e.timestamp = r.finished.isValid() ? r.finished : QDateTime::currentDateTime();
    e.machine = r.machine;
    e.direction = toString(r.direction);
    e.profile = r.profileName;
    e.port = r.portName;
    e.file = QDir::toNativeSeparators(r.filePath);
    e.fileSize = r.fileSize;
    e.bytes = r.bytesTransferred;
    e.result = toString(r.outcome);
    e.durationSec = r.durationMs / 1000.0;
    e.detail = r.message;
    return e;
}

TransferLog::TransferLog(const QString& path, QObject* parent)
    : QObject(parent)
    , m_path(path)
{
    qRegisterMetaType<nc::TransferLogEntry>();
}

bool TransferLog::append(const TransferLogEntry& e, QString* error)
{
    bool written;
    {
        QMutexLocker lock(&m_mutex);
        m_pending.append(e);
        written = writePendingLocked(error);
    }
    // Shown in the UI even if the file is locked; it stays queued and is
    // written by a later append() or flushPending().
    emit entryAppended(e);
    return written;
}

bool TransferLog::flushPending(QString* error)
{
    QMutexLocker lock(&m_mutex);
    return m_pending.isEmpty() || writePendingLocked(error);
}

int TransferLog::pendingCount() const
{
    QMutexLocker lock(&m_mutex);
    return int(m_pending.size());
}

bool TransferLog::writePendingLocked(QString* error)
{
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QFile f(m_path);
    const bool needHeader = !f.exists() || f.size() == 0;
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error)
            *error = QStringLiteral("Cannot write the transfer log %1 (%2). Is it open in Excel? %3 entr%4 waiting "
                                    "and will be written when the file is free.")
                         .arg(QDir::toNativeSeparators(m_path), f.errorString())
                         .arg(m_pending.size())
                         .arg(m_pending.size() == 1 ? QStringLiteral("y is") : QStringLiteral("ies are"));
        return false;
    }
    QByteArray out;
    if (needHeader)
        out += QByteArray(kHeader) + "\r\n";
    for (const TransferLogEntry& e : std::as_const(m_pending)) {
        const QStringList fields{
            e.timestamp.toString(kTimeFormat),
            csvField(e.machine),
            csvField(e.direction),
            csvField(e.profile),
            csvField(e.port),
            csvField(e.file),
            QString::number(e.fileSize),
            QString::number(e.bytes),
            csvField(e.result),
            QString::number(e.durationSec, 'f', 1),
            csvField(e.detail),
        };
        out += fields.join(QLatin1Char(',')).toUtf8() + "\r\n";
    }
    if (f.write(out) != out.size()) {
        if (error)
            *error = QStringLiteral("Error writing the transfer log: %1").arg(f.errorString());
        return false;
    }
    m_pending.clear();
    return true;
}

QList<TransferLogEntry> TransferLog::readRecent(int maxEntries) const
{
    QMutexLocker lock(&m_mutex);
    QList<TransferLogEntry> entries;
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return entries;

    constexpr qint64 kTail = 512 * 1024;
    const bool skipFirst = f.size() > kTail;
    if (skipFirst)
        f.seek(f.size() - kTail);
    QList<QByteArray> lines = f.readAll().split('\n');
    if (skipFirst && !lines.isEmpty())
        lines.removeFirst(); // probably a partial line

    for (int i = lines.size() - 1; i >= 0 && entries.size() < maxEntries; --i) {
        const QString line = QString::fromUtf8(lines.at(i)).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("Timestamp,")))
            continue;
        const QStringList c = parseCsvLine(line);
        if (c.size() < 11)
            continue;
        TransferLogEntry e;
        e.timestamp = QDateTime::fromString(c.at(0), kTimeFormat);
        e.machine = c.at(1);
        e.direction = c.at(2);
        e.profile = c.at(3);
        e.port = c.at(4);
        e.file = c.at(5);
        e.fileSize = c.at(6).toLongLong();
        e.bytes = c.at(7).toLongLong();
        e.result = c.at(8);
        e.durationSec = c.at(9).toDouble();
        e.detail = c.mid(10).join(QLatin1Char(','));
        entries.append(e);
    }
    return entries;
}

} // namespace nc
