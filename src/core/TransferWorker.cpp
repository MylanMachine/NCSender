#include "TransferWorker.h"

#include "AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace nc {

namespace {

constexpr qint64 kChunkSize = 128;  // bytes per write() at full speed
constexpr qint64 kHighWater = 256;  // max bytes waiting inside QSerialPort
constexpr int kStatusPollMs = 250;
constexpr int kStallWarnMs = 3000;
constexpr int kDrainPollMs = 50;
constexpr qint64 kMaxReceiveBytes = 64LL * 1024 * 1024;

QSerialPort::Parity toQt(Parity p)
{
    switch (p) {
    case Parity::None: return QSerialPort::NoParity;
    case Parity::Even: return QSerialPort::EvenParity;
    case Parity::Odd: return QSerialPort::OddParity;
    case Parity::Mark: return QSerialPort::MarkParity;
    case Parity::Space: return QSerialPort::SpaceParity;
    }
    return QSerialPort::EvenParity;
}

QSerialPort::StopBits toQt(StopBits s)
{
    switch (s) {
    case StopBits::One: return QSerialPort::OneStop;
    case StopBits::OneAndHalf: return QSerialPort::OneAndHalfStop;
    case StopBits::Two: return QSerialPort::TwoStop;
    }
    return QSerialPort::TwoStop;
}

QSerialPort::FlowControl toQt(FlowControl f)
{
    switch (f) {
    case FlowControl::None: return QSerialPort::NoFlowControl;
    case FlowControl::XonXoff: return QSerialPort::SoftwareControl;
    case FlowControl::RtsCts: return QSerialPort::HardwareControl;
    }
    return QSerialPort::NoFlowControl;
}

QSerialPort::DataBits toQtDataBits(int bits)
{
    switch (bits) {
    case 5: return QSerialPort::Data5;
    case 6: return QSerialPort::Data6;
    case 7: return QSerialPort::Data7;
    default: return QSerialPort::Data8;
    }
}

QString describeOpenError(QSerialPort::SerialPortError e, const QString& port, const QString& qtText)
{
    switch (e) {
    case QSerialPort::PermissionError:
        return QStringLiteral("%1 is already in use by another program (or access was denied). Close the other "
                              "program, and make sure the other machine panel is not set to the same port.")
            .arg(port);
    case QSerialPort::DeviceNotFoundError:
        return QStringLiteral("%1 was not found. Check that the USB-serial adapter is plugged in, then click "
                              "Refresh Ports.")
            .arg(port);
    case QSerialPort::OpenError:
        return QStringLiteral("%1 is already open.").arg(port);
    case QSerialPort::UnsupportedOperationError:
        return QStringLiteral("%1 rejected the port settings in this profile (%2).").arg(port, qtText);
    default:
        return QStringLiteral("Could not open %1: %2").arg(port, qtText);
    }
}

// NUL tape leader and the DC1-DC4 device-control codes a control may emit.
bool isControlCode(char c)
{
    return c == '\0' || (c >= 0x11 && c <= 0x14);
}

// Characters allowed before the program actually starts (tape leader/feed).
bool isLeaderChar(char c)
{
    return isControlCode(c) || c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\x7f';
}

// Plain printable ASCII: the only bytes that can start a received program.
bool isPrintable(char c)
{
    const uchar u = static_cast<uchar>(c);
    return u >= 0x21 && u <= 0x7e;
}

QString sanitizeForFileName(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'))
            out.append(c);
        else if (!out.endsWith(QLatin1Char('_')))
            out.append(QLatin1Char('_'));
    }
    while (out.endsWith(QLatin1Char('_')))
        out.chop(1);
    while (out.startsWith(QLatin1Char('_')))
        out.remove(0, 1);
    return out.isEmpty() ? QStringLiteral("program") : out;
}

QString formatBytes(qint64 n)
{
    return QLocale::system().toString(n);
}

QString formatSeconds(qint64 ms)
{
    return QString::number(ms / 1000.0, 'f', 1);
}

} // namespace

TransferWorker::TransferWorker(const QString& channelName, QObject* parent)
    : QObject(parent)
    , m_channelName(channelName)
{
    recreatePort();

    m_pumpTimer = new QTimer(this);
    m_pumpTimer->setSingleShot(true);
    m_pumpTimer->setTimerType(Qt::PreciseTimer);

    m_xonTimer = new QTimer(this);
    m_xonTimer->setSingleShot(true);

    m_rxTimer = new QTimer(this);
    m_rxTimer->setSingleShot(true);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusPollMs);

    m_drainTimer = new QTimer(this);
    m_drainTimer->setSingleShot(true);

    connect(m_pumpTimer, &QTimer::timeout, this, &TransferWorker::pump);
    connect(m_xonTimer, &QTimer::timeout, this, &TransferWorker::onXonTimeout);
    connect(m_rxTimer, &QTimer::timeout, this, &TransferWorker::onReceiveTimeout);
    connect(m_statusTimer, &QTimer::timeout, this, &TransferWorker::onStatusPoll);
    connect(m_drainTimer, &QTimer::timeout, this, &TransferWorker::onDrainTick);
}

TransferWorker::~TransferWorker()
{
    if (m_port->isOpen()) {
        m_suppressPortErrors = true;
        m_port->clear();
        m_port->close();
    }
}

// ---------------------------------------------------------------- common

void TransferWorker::recreatePort()
{
    // Every transfer gets a brand-new QSerialPort. When a transfer is
    // cancelled mid-write, Qt's Windows backend can keep the aborted write
    // marked as in progress, and the next write on that object never starts
    // (found by the cancel-then-send test). A fresh object can't inherit that.
    if (m_port) {
        m_port->disconnect(this);
        if (m_port->isOpen()) {
            m_suppressPortErrors = true;
            m_port->close();
            m_suppressPortErrors = false;
        }
        m_port->deleteLater();
    }
    m_port = new QSerialPort(this);
    connect(m_port, &QSerialPort::readyRead, this, &TransferWorker::onReadyRead);
    connect(m_port, &QSerialPort::bytesWritten, this, &TransferWorker::onBytesWritten);
    connect(m_port, &QSerialPort::errorOccurred, this, &TransferWorker::onPortError);
}

bool TransferWorker::beginTransfer(const MachineProfile& profile, TransferDirection direction)
{
    if (m_mode != Mode::Idle) {
        TransferResult r;
        r.direction = direction;
        r.outcome = TransferOutcome::Failed;
        r.machine = m_channelName;
        r.profileName = profile.name;
        r.portName = profile.portName;
        r.started = r.finished = QDateTime::currentDateTime();
        r.message = QStringLiteral("This machine is already busy with another transfer.");
        emit finished(r);
        return false;
    }

    recreatePort();
    m_profile = profile;
    m_result = TransferResult();
    m_result.direction = direction;
    m_result.machine = m_channelName;
    m_result.profileName = profile.name;
    m_result.portName = profile.portName;
    m_result.started = QDateTime::currentDateTime();

    m_mode = direction == TransferDirection::Send ? Mode::Send : Mode::Receive;
    m_state = TransferState::Idle;
    m_stateMessage.clear();
    m_errorQueued = false;
    m_pendingError.clear();

    m_source.reset();
    m_dripFeed = false;
    m_waitingForXon = false;
    m_sending = false;
    m_draining = false;
    m_drainGuardStarted = false;
    m_queued = 0;
    m_written = 0;

    m_rx.clear();
    m_rxRawBytes = 0;
    m_rxStarted = false;
    m_rxOpenedWithPercent = false;
    m_rxComplete = false;

    m_clock.start();
    m_progressClock.start();
    m_lastActivity.start();
    return true;
}

bool TransferWorker::openPort(FlowControl flow, QString* error)
{
    // Settings are applied all together when the port opens.
    m_port->setPortName(m_profile.portName);
    m_port->setBaudRate(m_profile.baudRate);
    m_port->setDataBits(toQtDataBits(m_profile.dataBits));
    m_port->setParity(toQt(m_profile.parity));
    m_port->setStopBits(toQt(m_profile.stopBits));
    m_port->setFlowControl(toQt(flow));

    m_suppressPortErrors = true;
    const bool ok = m_port->open(QIODevice::ReadWrite);
    if (!ok) {
        *error = describeOpenError(m_port->error(), m_profile.portName, m_port->errorString());
        m_port->clearError();
        m_suppressPortErrors = false;
        return false;
    }

    // Many controls only talk when they see DSR (our DTR) and, unless the
    // driver is managing it for RTS/CTS, CTS (our RTS).
    m_port->setDataTerminalReady(true);
    if (flow != FlowControl::RtsCts)
        m_port->setRequestToSend(true);
    m_port->clear(); // discard anything left over from before we opened
    m_port->clearError();
    m_suppressPortErrors = false;
    return true;
}

void TransferWorker::closePort()
{
    if (!m_port->isOpen())
        return;
    m_suppressPortErrors = true;
    m_port->close();
    m_port->clearError();
    m_suppressPortErrors = false;
}

void TransferWorker::stopTimers()
{
    m_pumpTimer->stop();
    m_xonTimer->stop();
    m_rxTimer->stop();
    m_statusTimer->stop();
    m_drainTimer->stop();
}

void TransferWorker::setState(TransferState state, const QString& message)
{
    if (state == m_state && message == m_stateMessage)
        return;
    m_state = state;
    m_stateMessage = message;
    emit stateChanged(state, message);
}

void TransferWorker::emitProgress(bool force)
{
    if (!force && m_progressClock.elapsed() < 50)
        return;
    m_progressClock.restart();
    if (m_result.direction == TransferDirection::Send)
        emit progress(m_written, m_result.bytesTotal);
    else
        emit progress(m_rx.size(), -1);
}

void TransferWorker::finish(TransferOutcome outcome, const QString& message)
{
    if (m_mode == Mode::Idle)
        return;

    stopTimers();
    if (outcome != TransferOutcome::Success && m_port->isOpen()) {
        m_suppressPortErrors = true;
        m_port->clear(QSerialPort::Output); // throw away anything still queued
        m_suppressPortErrors = false;
    }
    closePort();

    const bool isSend = m_result.direction == TransferDirection::Send;
    m_result.outcome = outcome;
    m_result.bytesTransferred = isSend ? m_written : m_rx.size();
    m_result.finished = QDateTime::currentDateTime();
    m_result.durationMs = m_clock.elapsed();

    QString text = message;
    if (outcome == TransferOutcome::Success && text.isEmpty() && isSend) {
        text = QStringLiteral("%1 %2 bytes in %3 s")
                   .arg(m_dripFeed ? QStringLiteral("Drip fed") : QStringLiteral("Sent"), formatBytes(m_written),
                        formatSeconds(m_result.durationMs));
    }
    m_result.message = text;

    emitProgress(true);
    m_mode = Mode::Idle;
    m_sending = false;
    m_waitingForXon = false;
    m_source.reset();

    switch (outcome) {
    case TransferOutcome::Success: setState(TransferState::Done, text); break;
    case TransferOutcome::Cancelled: setState(TransferState::Cancelled, text); break;
    case TransferOutcome::Failed: setState(TransferState::Error, text); break;
    }
    emit finished(m_result);
}

void TransferWorker::cancel()
{
    if (m_mode == Mode::Receive)
        finishReceive(TransferOutcome::Cancelled, QStringLiteral("Cancelled by user."));
    else if (m_mode == Mode::Send)
        finish(TransferOutcome::Cancelled, QStringLiteral("Cancelled by user."));
}

// ---------------------------------------------------------------- send

void TransferWorker::startSend(const MachineProfile& profile, const SendRequest& request)
{
    if (!beginTransfer(profile, TransferDirection::Send))
        return;

    m_result.filePath = request.filePath;
    // Drip feed starts when the control asks for data: Cycle Start in Remote
    // mode makes it send XON. The machine may sit idle for any length of time
    // before that, so there is no timeout.
    m_request = request;
    m_dripFeed = request.mode == SendRequest::Mode::DripFeed;
    if (m_dripFeed) {
        m_profile.waitForXon = true;
        m_profile.xonTimeoutSec = 0;
    }
    setState(TransferState::Opening, QStringLiteral("Loading %1").arg(QFileInfo(request.filePath).fileName()));

    QString error;
    qint64 fileSize = 0;
    m_source = createDataSource(request, m_profile, &error, &fileSize);
    m_result.fileSize = fileSize;
    if (!m_source) {
        finish(TransferOutcome::Failed, error);
        return;
    }
    m_result.bytesTotal = m_source->totalSize();

    // When waiting for the first XON we must see that byte ourselves, but the
    // Windows driver swallows XON/XOFF when it is doing software flow control.
    // So wait with flow control off, then switch it on (see beginSending).
    FlowControl initialFlow = m_profile.flowControl;
    if (m_profile.waitForXon && initialFlow == FlowControl::XonXoff)
        initialFlow = FlowControl::None;

    setState(TransferState::Opening, QStringLiteral("Opening %1").arg(m_profile.portName));
    if (!openPort(initialFlow, &error)) {
        finish(TransferOutcome::Failed, error);
        return;
    }
    m_statusTimer->start();
    emitProgress(true);

    if (m_dripFeed) {
        m_waitingForXon = true;
        setState(TransferState::WaitingForXon,
                 QStringLiteral("Put the machine in Remote (DNC) mode and press Cycle Start."));
    } else if (m_profile.waitForXon) {
        m_waitingForXon = true;
        setState(TransferState::WaitingForXon,
                 QStringLiteral("Port open. Waiting for XON - start the program input (READ) at the control."));
        if (m_profile.xonTimeoutSec > 0)
            m_xonTimer->start(m_profile.xonTimeoutSec * 1000);
    } else {
        beginSending();
    }
}

bool TransferWorker::setXonXoff(bool on)
{
    m_suppressPortErrors = true;
    const bool ok = m_port->setFlowControl(on ? QSerialPort::SoftwareControl : QSerialPort::NoFlowControl);
    m_suppressPortErrors = false;
    if (!ok) {
        finish(TransferOutcome::Failed, QStringLiteral("Could not %1 XON/XOFF flow control on %2: %3")
                                            .arg(on ? QStringLiteral("enable") : QStringLiteral("disable"),
                                                 m_profile.portName, m_port->errorString()));
    }
    return ok;
}

void TransferWorker::beginSending()
{
    m_waitingForXon = false;
    m_xonTimer->stop();

    if (m_profile.waitForXon && m_profile.flowControl == FlowControl::XonXoff && !setXonXoff(true))
        return;

    if (m_dripFeed) {
        // A run is timed from Cycle Start, not from when it was armed.
        m_result.started = QDateTime::currentDateTime();
        m_clock.restart();
    }
    m_sending = true;
    m_lastActivity.restart();
    setState(TransferState::Sending, sendingMessage());
    pump();
}

QString TransferWorker::sendingMessage() const
{
    return QStringLiteral("%1 %2 to %3")
        .arg(m_dripFeed ? QStringLiteral("Drip feeding") : QStringLiteral("Sending"),
             m_source ? m_source->description() : QString(), m_profile.portName);
}

bool TransferWorker::writeChunk(const QByteArray& chunk)
{
    const qint64 n = m_port->write(chunk);
    if (m_mode != Mode::Send || m_errorQueued)
        return false; // an error was raised while writing
    if (n != chunk.size()) {
        finish(TransferOutcome::Failed,
               QStringLiteral("Write to %1 failed: %2").arg(m_profile.portName, m_port->errorString()));
        return false;
    }
    m_queued += n;
    return true;
}

void TransferWorker::pump()
{
    if (m_mode != Mode::Send || !m_sending || m_draining || m_errorQueued || !m_source)
        return;

    if (m_profile.charDelayMs > 0) {
        // One character at a time, each after the previous one has been
        // accepted by the driver, then the configured pause.
        if (m_port->bytesToWrite() == 0 && !m_source->atEnd()) {
            const QByteArray one = m_source->read(1);
            if (one.isEmpty() && !m_source->error().isEmpty()) {
                finish(TransferOutcome::Failed, m_source->error());
                return;
            }
            if (!one.isEmpty() && !writeChunk(one))
                return;
        }
        if (!m_source->atEnd()) {
            m_pumpTimer->start(m_profile.charDelayMs);
            return;
        }
    } else {
        while (m_port->bytesToWrite() < kHighWater && !m_source->atEnd()) {
            const QByteArray chunk = m_source->read(kChunkSize);
            if (chunk.isEmpty()) {
                if (const QString err = m_source->error(); !err.isEmpty()) {
                    finish(TransferOutcome::Failed, err);
                    return;
                }
                // Source has nothing ready yet.
                m_pumpTimer->start(20);
                return;
            }
            if (!writeChunk(chunk))
                return;
        }
    }
    checkSendComplete();
}

void TransferWorker::onBytesWritten(qint64 bytes)
{
    if (m_mode != Mode::Send || !m_sending)
        return;
    m_written += bytes;
    m_lastActivity.restart();
    if (m_state == TransferState::Paused)
        setState(m_draining ? TransferState::Finishing : TransferState::Sending, sendingMessage());
    emitProgress(false);

    if (m_profile.charDelayMs > 0)
        checkSendComplete(); // the pump timer drives the next character
    else
        pump();
}

void TransferWorker::checkSendComplete()
{
    if (m_mode != Mode::Send || !m_sending || m_draining || !m_source)
        return;
    if (!m_source->atEnd() || m_port->bytesToWrite() > 0 || m_written < m_queued)
        return;

    m_draining = true;
    emitProgress(true);
    setState(TransferState::Finishing, QStringLiteral("All data handed to the port, waiting for it to finish transmitting"));
    m_drainTimer->start(kDrainPollMs);
}

void TransferWorker::onDrainTick()
{
    if (m_mode != Mode::Send || !m_draining || m_errorQueued)
        return;

    // Wait until the driver's output queue is empty and the control is not
    // holding us (XOFF/CTS). Then wait out a guard time long enough for a
    // USB adapter's own hardware buffer (up to ~256 characters) to clock
    // out before the port is closed. Any hold restarts the guard.
    const PortStatus st = queryPortStatus();
    if (!st.ok) {
        finish(TransferOutcome::Failed,
               QStringLiteral("Lost connection to %1 while the last of the program was being sent - the adapter was "
                              "unplugged or stopped responding. The control may not have received the end of the program.")
                   .arg(m_profile.portName));
        return;
    }
    // In drip feed a hold once everything has gone is expected: the control
    // sends XOFF when it reads the closing % and keeps it until the next run.
    const bool held = (st.xoffHold || st.ctsHold) && !m_dripFeed;
    if (st.outQueue > 0 || held) {
        m_drainGuardStarted = false;
        m_drainTimer->start(kDrainPollMs);
        return;
    }
    if (!m_drainGuardStarted) {
        m_drainGuardStarted = true;
        m_drainGuard.start();
    }
    const double bits = bitsPerCharacter(m_profile.dataBits, m_profile.parity, m_profile.stopBits);
    const qint64 guardMs = std::clamp<qint64>(qint64(256.0 * bits * 1000.0 / qMax(1, m_profile.baudRate)) + 100, 100, 30000);
    if (m_drainGuard.elapsed() < guardMs) {
        m_drainTimer->start(kDrainPollMs);
        return;
    }
    completeRun();
}

void TransferWorker::completeRun()
{
    if (!m_dripFeed) {
        finish(TransferOutcome::Success, QString());
        return;
    }

    m_drainTimer->stop();
    m_pumpTimer->stop();
    m_result.outcome = TransferOutcome::Success;
    m_result.bytesTransferred = m_written;
    m_result.finished = QDateTime::currentDateTime();
    m_result.durationMs = m_clock.elapsed();
    m_result.message = QStringLiteral("Drip fed %1 bytes in %2 s")
                           .arg(formatBytes(m_written), formatSeconds(m_result.durationMs));
    emitProgress(true);
    setState(TransferState::Done, m_result.message);
    emit runFinished(m_result);
    armNextRun();
}

void TransferWorker::armNextRun()
{
    TransferResult next;
    next.direction = TransferDirection::Send;
    next.machine = m_result.machine;
    next.profileName = m_result.profileName;
    next.portName = m_result.portName;
    next.filePath = m_result.filePath;
    next.started = QDateTime::currentDateTime();
    m_result = next;

    m_sending = false;
    m_draining = false;
    m_drainGuardStarted = false;
    m_queued = 0;
    m_written = 0;
    m_clock.restart();

    // Re-read the file, so an edit saved between runs is picked up.
    QString error;
    qint64 fileSize = 0;
    m_source = createDataSource(m_request, m_profile, &error, &fileSize);
    m_result.fileSize = fileSize;
    if (!m_source) {
        finish(TransferOutcome::Failed, error);
        return;
    }
    m_result.bytesTotal = m_source->totalSize();

    // Flow control off again so the next XON reaches us (see startSend).
    if (m_profile.flowControl == FlowControl::XonXoff && !setXonXoff(false))
        return;
    m_waitingForXon = true;
    emitProgress(true);
    setState(TransferState::WaitingForXon,
             QStringLiteral("Run complete. For the next part, press Cycle Start on the machine."));
}

TransferWorker::PortStatus TransferWorker::queryPortStatus() const
{
    PortStatus s;
#ifdef Q_OS_WIN
    if (!m_port->isOpen()) {
        s.ok = false;
        return s;
    }
    COMSTAT st{};
    DWORD errors = 0;
    if (!::ClearCommError(m_port->handle(), &errors, &st)) {
        s.ok = false;
        return s;
    }
    s.outQueue = st.cbOutQue;
    s.xoffHold = st.fXoffHold != 0;
    s.ctsHold = st.fCtsHold != 0;
#endif
    return s;
}

void TransferWorker::onXonTimeout()
{
    if (m_mode != Mode::Send || !m_waitingForXon)
        return;
    finish(TransferOutcome::Failed,
           QStringLiteral("Timed out after %1 s waiting for XON from the control. Make sure the control is in "
                          "program input (READ) mode, the cable is connected, and the baud/parity settings match.")
               .arg(m_profile.xonTimeoutSec));
}

void TransferWorker::onStatusPoll()
{
    if (m_mode != Mode::Send || !m_sending)
        return;

    const PortStatus st = queryPortStatus();
    const bool xoffHold = st.ok && st.xoffHold;
    const bool ctsHold = st.ok && st.ctsHold;

    if ((xoffHold || ctsHold) && m_dripFeed) {
        // Normal while the machine works through its buffer, but also what a
        // Reset at the machine looks like. Resuming after a Reset would start
        // the machine part way through the program.
        setState(TransferState::Paused,
                 QStringLiteral("Machine is working through its buffer (control is holding data). If you pressed "
                                "Reset on the machine, click Restart before pressing Cycle Start again."));
    } else if (xoffHold) {
        setState(TransferState::Paused, QStringLiteral("Paused by the control (XOFF received), waiting for XON"));
    } else if (ctsHold) {
        setState(TransferState::Paused, QStringLiteral("Paused, waiting for the control to raise CTS"));
    } else if (m_lastActivity.elapsed() > kStallWarnMs && m_port->bytesToWrite() > 0) {
        setState(TransferState::Paused, QStringLiteral("No progress for %1 s - the control may be holding the transfer")
                                            .arg(m_lastActivity.elapsed() / 1000));
    } else if (m_state == TransferState::Paused) {
        if (m_draining)
            setState(TransferState::Finishing, QStringLiteral("All data handed to the port, waiting for it to finish transmitting"));
        else
            setState(TransferState::Sending, sendingMessage());
    }
}

// ---------------------------------------------------------------- receive

void TransferWorker::startReceive(const MachineProfile& profile)
{
    if (!beginTransfer(profile, TransferDirection::Receive))
        return;

    m_result.bytesTotal = -1;
    const QString folder = profile.effectiveReceiveFolder();
    m_result.filePath = folder;
    // An unreachable folder (offline share, missing drive) does not stop the
    // receive: the program is saved to a fallback folder instead (see
    // saveReceived). Just tell the operator up front.
    const bool folderOk = QDir().mkpath(folder);

    setState(TransferState::Opening, QStringLiteral("Opening %1").arg(profile.portName));
    QString error;
    if (!openPort(profile.flowControl, &error)) {
        finish(TransferOutcome::Failed, error);
        return;
    }

    QString listening =
        QStringLiteral("Listening on %1 - start the punch / program output at the control").arg(profile.portName);
    if (!folderOk) {
        listening += QStringLiteral(". Note: the receive folder %1 is not reachable, so the program will be saved in "
                                    "NCSender's own 'received' folder.")
                         .arg(QDir::toNativeSeparators(folder));
    }
    setState(TransferState::Receiving, listening);
    if (profile.receiveStartTimeoutSec > 0)
        m_rxTimer->start(profile.receiveStartTimeoutSec * 1000);
    emitProgress(true);
}

void TransferWorker::handleReceivedBytes(const QByteArray& data)
{
    if (m_mode != Mode::Receive || m_rxComplete || data.isEmpty())
        return;

    m_rxRawBytes += data.size();
    const bool strip = m_profile.stripReceivedControlChars;

    // End of program: the first meaningful character is normally the opening
    // %, and the next % closes the program. If the program does not open
    // with %, the first % closes it.
    //
    // Before the program starts, only a printable character can start it.
    // Leader characters are kept or dropped per the strip setting, and line
    // noise (a framing-error byte when the cable is plugged in or the control
    // powers up) is always dropped. Otherwise a single noise byte would turn
    // the opening % into the closing one.
    QByteArray accepted;
    accepted.reserve(data.size());
    bool closing = false;
    for (const char c : data) {
        if (!m_rxStarted) {
            if (isLeaderChar(c)) {
                if (!strip)
                    accepted.append(c);
                continue;
            }
            if (!isPrintable(c))
                continue;
            m_rxStarted = true;
            m_rxOpenedWithPercent = c == '%';
            accepted.append(c);
            continue; // an opening % does not end the program
        }
        if (strip && isControlCode(c))
            continue;
        accepted.append(c);
        if (c == '%') {
            closing = true;
            break; // ignore anything after the closing %
        }
    }

    if (!accepted.isEmpty()) {
        if (m_rx.size() + accepted.size() > kMaxReceiveBytes) {
            finishReceive(TransferOutcome::Failed,
                          QStringLiteral("Received more than 64 MB without a closing % - stopped."));
            return;
        }
        m_rx.append(accepted);
        emit dataReceived(accepted);
    }
    m_lastActivity.restart();
    emitProgress(false);

    if (closing) {
        finishReceive(TransferOutcome::Success, QString());
        return;
    }

    // Once the program has started, the start timeout is replaced by the
    // idle timeout. Leader or noise bytes alone do not change the timers.
    if (m_rxStarted) {
        if (m_profile.receiveIdleTimeoutSec > 0)
            m_rxTimer->start(m_profile.receiveIdleTimeoutSec * 1000);
        else
            m_rxTimer->stop();
        setState(TransferState::Receiving, QStringLiteral("Receiving from the control"));
    }
}

void TransferWorker::onReceiveTimeout()
{
    if (m_mode != Mode::Receive)
        return;
    if (m_rxRawBytes == 0) {
        finishReceive(TransferOutcome::Failed,
                      QStringLiteral("No data received within %1 s. Check the cable and that the control's punch "
                                     "output was started.")
                          .arg(m_profile.receiveStartTimeoutSec));
    } else if (!m_rxStarted) {
        finishReceive(TransferOutcome::Failed,
                      QStringLiteral("Only leader, control or noise characters were received within %1 s, no "
                                     "program data. Check baud rate and parity.")
                          .arg(m_profile.receiveStartTimeoutSec));
    } else if (m_rxOpenedWithPercent) {
        // It opened with % but never closed: the punch was cut off.
        finishReceive(TransferOutcome::Failed,
                      QStringLiteral("No closing % received - stopped after %1 s of silence. The program is "
                                     "probably incomplete (punch stopped or cable pulled).")
                          .arg(m_profile.receiveIdleTimeoutSec));
    } else {
        // Program without % markers: silence is the only way it can end.
        finishReceive(TransferOutcome::Success,
                      QStringLiteral("No closing % received - stopped after %1 s of silence.")
                          .arg(m_profile.receiveIdleTimeoutSec));
    }
}

void TransferWorker::finishReceive(TransferOutcome outcome, const QString& message)
{
    if (m_mode != Mode::Receive)
        return;
    m_rxComplete = true;
    stopTimers();
    closePort(); // stop anything more arriving while we save

    QString text = message;
    // Only save once real program data has started; leader or noise alone is
    // not worth a file.
    if (m_rxStarted && !m_rx.isEmpty()) {
        QString error;
        QString note;
        const bool partial = outcome != TransferOutcome::Success;
        const QString path = saveReceived(partial, &error, &note);
        if (path.isEmpty()) {
            outcome = TransferOutcome::Failed;
            text = (text.isEmpty() ? QString() : text + QLatin1Char(' '))
                + QStringLiteral("Could not save the received program anywhere: %1").arg(error);
        } else {
            m_result.filePath = path;
            m_result.fileSize = m_rx.size();
            const QString name = QDir::toNativeSeparators(note.isEmpty() ? QFileInfo(path).fileName() : path);
            if (partial) {
                text += QStringLiteral(" Partial data (%1 bytes) saved as %2.").arg(formatBytes(m_rx.size()), name);
            } else {
                const QString saved = QStringLiteral("Received %1 bytes, saved as %2").arg(formatBytes(m_rx.size()), name);
                text = text.isEmpty() ? saved : saved + QStringLiteral(". ") + text;
            }
            if (!note.isEmpty())
                text += QLatin1Char(' ') + note;
        }
    }
    finish(outcome, text.trimmed());
}

QString TransferWorker::saveReceived(bool partial, QString* error, QString* note)
{
    QString base = sanitizeForFileName(m_profile.name).left(40);

    // Include the program number (O1234 or <NAME>) when one is found near the top.
    static const QRegularExpression progRe(QStringLiteral("(?:^|[\\r\\n%])\\s*(O\\d{1,5}|<[^>\\r\\n]{1,32}>)"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = progRe.match(QString::fromLatin1(m_rx.left(4096)));
    if (m.hasMatch())
        base += QLatin1Char('_') + sanitizeForFileName(m.captured(1).toUpper());

    base += QLatin1Char('_') + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    if (partial)
        base += QStringLiteral("_PARTIAL");

    QString ext = m_profile.receiveExtension.trimmed();
    while (ext.startsWith(QLatin1Char('.')))
        ext.remove(0, 1);
    if (ext.isEmpty())
        ext = QStringLiteral("nc");

    // Never lose a received program: if the profile's folder can't be used
    // (offline share, disk full, no permission), fall back to the app's own
    // folder, then the temp folder, and say so.
    const QString primary = m_profile.effectiveReceiveFolder();
    const QStringList folders{primary, QDir(AppPaths::dataDir()).filePath(QStringLiteral("received")),
                              QDir(QDir::tempPath()).filePath(QStringLiteral("NCSender_received"))};
    QStringList problems;
    for (const QString& folder : folders) {
        QString why;
        const QString path = writeUniqueFile(folder, base, ext, m_rx, &why);
        if (!path.isEmpty()) {
            if (folder != primary) {
                *note = QStringLiteral("The receive folder %1 could not be used (%2), so it was saved to %3 instead.")
                            .arg(QDir::toNativeSeparators(primary), problems.value(0),
                                 QDir::toNativeSeparators(folder));
            }
            return path;
        }
        problems << why;
    }
    *error = problems.join(QStringLiteral("; "));
    return {};
}

QString TransferWorker::writeUniqueFile(const QString& folder, const QString& base, const QString& ext,
                                        const QByteArray& data, QString* why)
{
    if (!QDir().mkpath(folder)) {
        *why = QStringLiteral("cannot create folder %1").arg(QDir::toNativeSeparators(folder));
        return {};
    }
    const QDir dir(folder);
    for (int n = 0; n < 1000; ++n) {
        const QString path = dir.filePath(n == 0 ? QStringLiteral("%1.%2").arg(base, ext)
                                                 : QStringLiteral("%1_%2.%3").arg(base).arg(n).arg(ext));
        // Claim the name atomically so two machines finishing the same
        // program in the same second can never overwrite each other.
        QFile claim(path);
        if (!claim.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            if (QFileInfo::exists(path))
                continue; // taken, try the next number
            *why = claim.errorString();
            return {};
        }
        claim.close();

        QSaveFile f(path);
        if (f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit())
            return path;
        *why = f.errorString();
        QFile::remove(path);
        return {};
    }
    *why = QStringLiteral("too many files with the same name");
    return {};
}

// ---------------------------------------------------------------- port events

void TransferWorker::onReadyRead()
{
    const QByteArray data = m_port->readAll();
    if (data.isEmpty())
        return;

    if (m_mode == Mode::Send) {
        // Start on XON, unless a later XOFF in the same chunk says "wait".
        if (m_waitingForXon && data.lastIndexOf(kXon) > data.lastIndexOf(kXoff)) {
            setState(TransferState::Sending, QStringLiteral("XON received, starting"));
            beginSending();
        }
        // Anything else the control sends during a send is ignored.
        return;
    }
    if (m_mode == Mode::Receive)
        handleReceivedBytes(data);
}

void TransferWorker::onPortError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || error == QSerialPort::TimeoutError)
        return;
    if (m_suppressPortErrors || m_mode == Mode::Idle || m_errorQueued)
        return;

    const QString port = m_profile.portName;
    switch (error) {
    case QSerialPort::ResourceError:
    case QSerialPort::PermissionError: // Windows reports some USB unplugs as "access denied"
        m_pendingError = QStringLiteral("Lost connection to %1 - the serial adapter was unplugged or stopped "
                                        "responding.")
                             .arg(port);
        break;
    case QSerialPort::WriteError:
        m_pendingError = QStringLiteral("Write error on %1: %2").arg(port, m_port->errorString());
        break;
    case QSerialPort::ReadError:
        m_pendingError = QStringLiteral("Read error on %1: %2").arg(port, m_port->errorString());
        break;
    default:
        m_pendingError = QStringLiteral("Serial error on %1: %2").arg(port, m_port->errorString());
        break;
    }

    // This can be raised from inside QSerialPort::write()/read(). Closing the
    // port from in there is unsafe, so stop pumping now and tear down on the
    // next event-loop pass.
    m_errorQueued = true;
    m_sending = false;
    QMetaObject::invokeMethod(this, &TransferWorker::handlePendingError, Qt::QueuedConnection);
}

void TransferWorker::handlePendingError()
{
    if (!m_errorQueued)
        return;
    const QString msg = m_pendingError;
    if (m_mode == Mode::Receive)
        finishReceive(TransferOutcome::Failed, msg);
    else
        finish(TransferOutcome::Failed, msg);
    m_errorQueued = false;
}

} // namespace nc
