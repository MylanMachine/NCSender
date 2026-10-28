#pragma once

#include "DataSource.h"
#include "MachineProfile.h"
#include "TransferTypes.h"

#include <QElapsedTimer>
#include <QObject>
#include <QSerialPort>
#include <memory>

class QTimer;

namespace nc {

// Owns one QSerialPort and runs one transfer at a time. It lives on its own
// QThread (see MachineChannel) and is entirely event driven. It never blocks,
// so a cancel request or a port error is handled within milliseconds, and
// nothing it does can stall the UI or the other machine's worker.
//
// Send pipeline:
//   DataSource --pull small chunks--> QSerialPort --driver--> wire
// Only a few hundred bytes are queued in the port at any moment. That means:
//   - XON/XOFF and RTS/CTS are enforced by the Windows serial driver, which
//     stops within a character of XOFF / CTS dropping
//   - progress reflects what has actually left the PC
//   - cancel only has to discard a small buffer
//
// Drip feed (SendRequest::Mode::DripFeed) stays armed until cancelled: each
// time the control sends XON (Cycle Start in Remote mode) the program is
// streamed again, re-read from disk. The port stays open between runs, and
// nothing queued is ever purged except on cancel or error, so the end of a
// program can't be lost from a USB adapter's buffer.
class TransferWorker : public QObject
{
    Q_OBJECT

public:
    explicit TransferWorker(const QString& channelName, QObject* parent = nullptr);
    ~TransferWorker() override;

    // Call these in the worker's thread only (MachineChannel queues them).
    void startSend(const nc::MachineProfile& profile, const nc::SendRequest& request);
    void startReceive(const nc::MachineProfile& profile);
    void cancel();

signals:
    void stateChanged(nc::TransferState state, const QString& message);
    void progress(qint64 done, qint64 total); // total = -1 when unknown (receive)
    void dataReceived(const QByteArray& chunk);
    // Drip feed only: one run completed and the worker is armed for the next.
    void runFinished(const nc::TransferResult& result);
    void finished(const nc::TransferResult& result);

private:
    enum class Mode { Idle, Send, Receive };

    void recreatePort();
    bool beginTransfer(const MachineProfile& profile, TransferDirection direction);
    bool openPort(FlowControl flow, QString* error);
    void closePort();
    void stopTimers();
    void setState(TransferState state, const QString& message = QString());
    void emitProgress(bool force);
    void finish(TransferOutcome outcome, const QString& message);

    // send
    void beginSending();
    void pump();
    bool writeChunk(const QByteArray& chunk);
    void checkSendComplete();
    void onDrainTick();
    void completeRun();
    void armNextRun();
    bool setXonXoff(bool on);
    void onXonTimeout();
    void onStatusPoll();
    QString sendingMessage() const;

    struct PortStatus
    {
        bool ok = true;       // false: port gone (unplugged) or not open
        qint64 outQueue = 0;  // bytes still in the driver's transmit queue
        bool xoffHold = false;
        bool ctsHold = false;
    };
    PortStatus queryPortStatus() const;

    // receive
    void handleReceivedBytes(const QByteArray& data);
    void onReceiveTimeout();
    void finishReceive(TransferOutcome outcome, const QString& message);
    QString saveReceived(bool partial, QString* error, QString* note);
    static QString writeUniqueFile(const QString& folder, const QString& base, const QString& ext,
                                   const QByteArray& data, QString* why);

    // port events
    void onReadyRead();
    void onBytesWritten(qint64 bytes);
    void onPortError(QSerialPort::SerialPortError error);
    void handlePendingError();

    QString m_channelName;
    QSerialPort* m_port = nullptr;
    QTimer* m_pumpTimer = nullptr;
    QTimer* m_xonTimer = nullptr;
    QTimer* m_rxTimer = nullptr;
    QTimer* m_statusTimer = nullptr;
    QTimer* m_drainTimer = nullptr;

    Mode m_mode = Mode::Idle;
    MachineProfile m_profile;
    TransferResult m_result;
    TransferState m_state = TransferState::Idle;
    QString m_stateMessage;
    QElapsedTimer m_clock;
    QElapsedTimer m_progressClock;
    QElapsedTimer m_lastActivity;
    bool m_suppressPortErrors = false; // while opening/configuring/closing
    bool m_errorQueued = false;
    QString m_pendingError;

    // send state
    std::unique_ptr<DataSource> m_source;
    SendRequest m_request;
    bool m_dripFeed = false;
    bool m_waitingForXon = false;
    bool m_sending = false;
    bool m_draining = false;
    bool m_drainGuardStarted = false;
    QElapsedTimer m_drainGuard;
    qint64 m_queued = 0;  // bytes handed to QSerialPort
    qint64 m_written = 0; // bytes the driver reported as written

    // receive state
    QByteArray m_rx;
    qint64 m_rxRawBytes = 0;
    bool m_rxStarted = false;
    bool m_rxOpenedWithPercent = false;
    bool m_rxComplete = false;
};

} // namespace nc
