#pragma once

#include "DataSource.h"
#include "MachineProfile.h"
#include "TransferTypes.h"

#include <QObject>

class QThread;

namespace nc {

class TransferWorker;

// One independent serial channel (one machine). Owns a dedicated thread with
// its own TransferWorker and QSerialPort. Two channels share nothing, so one
// machine's transfer can never block or slow the other.
//
// Use it from the UI thread. Every call is queued to the worker, and every
// signal arrives back on the UI thread.
class MachineChannel : public QObject
{
    Q_OBJECT

public:
    explicit MachineChannel(const QString& name, QObject* parent = nullptr);
    ~MachineChannel() override;

    QString name() const { return m_name; }
    bool isBusy() const { return m_busy; }
    QString activePortName() const { return m_busy ? m_activePort : QString(); }
    TransferDirection activeDirection() const { return m_direction; }

    // Return false if a transfer is already running on this channel. A drip
    // feed send keeps running (one run per Cycle Start) until cancel().
    bool startSend(const MachineProfile& profile, const SendRequest& request);
    bool startReceive(const MachineProfile& profile);
    void cancel();

signals:
    void stateChanged(nc::TransferState state, const QString& message);
    void progress(qint64 done, qint64 total);
    void dataReceived(const QByteArray& chunk);
    // Drip feed: one run completed; the channel stays busy, armed for the next.
    void runFinished(const nc::TransferResult& result);
    void finished(const nc::TransferResult& result);

private:
    QString m_name;
    QThread* m_thread = nullptr;
    TransferWorker* m_worker = nullptr;
    bool m_busy = false;
    QString m_activePort;
    TransferDirection m_direction = TransferDirection::Send;
};

} // namespace nc
