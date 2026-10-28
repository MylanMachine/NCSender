#pragma once

#include "TransferTypes.h"

#include <QDateTime>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

namespace nc {

struct TransferLogEntry
{
    QDateTime timestamp;
    QString machine;
    QString direction;
    QString profile;
    QString port;
    QString file;
    qint64 fileSize = 0;
    qint64 bytes = 0;
    QString result;
    double durationSec = 0.0;
    QString detail;

    static TransferLogEntry fromResult(const TransferResult& r);
};

// Append-only CSV log, one line per transfer, readable in Excel:
//   Timestamp,Machine,Direction,Profile,Port,File,FileSize,BytesTransferred,Result,DurationSec,Detail
// append() can be called from any thread.
class TransferLog : public QObject
{
    Q_OBJECT

public:
    explicit TransferLog(const QString& path, QObject* parent = nullptr);

    QString path() const { return m_path; }

    // Returns false if the file could not be written (for example it is open
    // in Excel). The entry is then kept in memory and written by the next
    // append() or flushPending(); entryAppended is emitted either way.
    bool append(const TransferLogEntry& entry, QString* error = nullptr);
    bool flushPending(QString* error = nullptr);
    int pendingCount() const;

    // Most recent entries in the file, newest first.
    QList<TransferLogEntry> readRecent(int maxEntries) const;

signals:
    void entryAppended(const nc::TransferLogEntry& entry);

private:
    bool writePendingLocked(QString* error);

    QString m_path;
    mutable QMutex m_mutex;
    QList<TransferLogEntry> m_pending;
};

} // namespace nc

Q_DECLARE_METATYPE(nc::TransferLogEntry)
