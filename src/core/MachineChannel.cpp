#include "MachineChannel.h"

#include "TransferWorker.h"

#include <QThread>

namespace nc {

MachineChannel::MachineChannel(const QString& name, QObject* parent)
    : QObject(parent)
    , m_name(name)
{
    registerTransferMetaTypes();

    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("Serial %1").arg(name));

    m_worker = new TransferWorker(name);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    // Worker and channel live in different threads, so these are queued.
    connect(m_worker, &TransferWorker::stateChanged, this, &MachineChannel::stateChanged);
    connect(m_worker, &TransferWorker::progress, this, &MachineChannel::progress);
    connect(m_worker, &TransferWorker::dataReceived, this, &MachineChannel::dataReceived);
    connect(m_worker, &TransferWorker::runFinished, this, &MachineChannel::runFinished);
    connect(m_worker, &TransferWorker::finished, this, [this](const TransferResult& result) {
        m_busy = false;
        m_activePort.clear();
        emit finished(result);
    });

    m_thread->start();
}

MachineChannel::~MachineChannel()
{
    if (m_busy && m_thread->isRunning()) {
        // Make sure the port is released cleanly before the thread stops.
        TransferWorker* worker = m_worker;
        QMetaObject::invokeMethod(worker, [worker] { worker->cancel(); }, Qt::BlockingQueuedConnection);
    }
    m_thread->quit();
    if (!m_thread->wait(10000)) {
        // A badly behaved serial driver is stuck inside a system call.
        // Destroying a running QThread aborts the whole program, so as a last
        // resort stop the thread forcibly instead.
        m_thread->terminate();
        m_thread->wait();
    }
}

bool MachineChannel::startSend(const MachineProfile& profile, const SendRequest& request)
{
    if (m_busy)
        return false;
    m_busy = true;
    m_activePort = profile.portName;
    m_direction = TransferDirection::Send;
    TransferWorker* worker = m_worker;
    QMetaObject::invokeMethod(worker, [worker, profile, request] { worker->startSend(profile, request); },
                              Qt::QueuedConnection);
    return true;
}

bool MachineChannel::startReceive(const MachineProfile& profile)
{
    if (m_busy)
        return false;
    m_busy = true;
    m_activePort = profile.portName;
    m_direction = TransferDirection::Receive;
    TransferWorker* worker = m_worker;
    QMetaObject::invokeMethod(worker, [worker, profile] { worker->startReceive(profile); }, Qt::QueuedConnection);
    return true;
}

void MachineChannel::cancel()
{
    if (!m_busy)
        return;
    TransferWorker* worker = m_worker;
    QMetaObject::invokeMethod(worker, [worker] { worker->cancel(); }, Qt::QueuedConnection);
}

} // namespace nc
