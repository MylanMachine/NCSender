#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>

namespace nc {

enum class TransferDirection { Send, Receive };

enum class TransferState {
    Idle,
    Opening,       // loading file / opening port
    WaitingForXon, // send: port open, waiting for the control's first XON
    Sending,
    Paused,        // send: control is holding us (XOFF or CTS low) or no progress
    Receiving,
    Finishing,     // send: all data handed to the port, letting it drain
    Done,
    Cancelled,
    Error
};

enum class TransferOutcome { Success, Cancelled, Failed };

struct TransferResult
{
    TransferDirection direction = TransferDirection::Send;
    TransferOutcome outcome = TransferOutcome::Failed;
    QString machine;      // panel / channel name, e.g. "Machine A"
    QString profileName;
    QString portName;
    QString filePath;     // file sent, or file saved by a receive
    qint64 fileSize = 0;  // size on disk
    qint64 bytesTotal = 0;       // bytes that were to be sent (after formatting); -1/0 for receive
    qint64 bytesTransferred = 0; // bytes actually sent or received
    QString message;      // error text, or a note on success
    QDateTime started;
    QDateTime finished;
    qint64 durationMs = 0;
};

QString toString(TransferDirection d);
QString toString(TransferState s);
QString toString(TransferOutcome o);

// Registers the types above for queued (cross-thread) signals.
void registerTransferMetaTypes();

} // namespace nc

Q_DECLARE_METATYPE(nc::TransferDirection)
Q_DECLARE_METATYPE(nc::TransferState)
Q_DECLARE_METATYPE(nc::TransferOutcome)
Q_DECLARE_METATYPE(nc::TransferResult)
