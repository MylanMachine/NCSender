// nctestpeer - pretends to be a CNC control on the other end of a serial link.
//
// Modes
//   receive  Act like a control in program-input mode. Optionally send XON to
//            start, pause the sender every N bytes with XOFF (or by dropping
//            RTS), and check what arrives. Ends at the closing % or after
//            silence. Exit 0 = PASS, 1 = FAIL.
//   punch    Act like a control punching a program out: leader, optional DC2,
//            program text, optional DC4, trailer. Honors XOFF from the PC.
//   echo     Loopback: everything received is sent straight back.
//
// Examples (app on COM10, peer on the other end of the pair, COM11)
//   nctestpeer receive --port COM11 --expect O1001.nc
//   nctestpeer receive --port COM11 --xon-delay 2000 --expect O1001.nc
//   nctestpeer receive --port COM11 --pause-every 1000 --pause-ms 1500 --expect big.nc
//   nctestpeer punch   --port COM11 --file O1001.nc --leader 20 --dc2
//   nctestpeer echo    --port COM11

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTextStream>
#include <QTimer>

namespace {

constexpr char kXon = 0x11;
constexpr char kDc2 = 0x12;
constexpr char kXoff = 0x13;
constexpr char kDc4 = 0x14;

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}

void logLine(const QString& msg)
{
    out() << "[" << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) << "] " << msg << Qt::endl;
}

bool isLeaderChar(char c)
{
    return c == '\0' || (c >= 0x11 && c <= 0x14) || c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\x7f';
}

// For comparing what was sent with what was meant to be sent: drop NUL/DC
// codes, DEL and CR (line endings may legitimately differ), trim the ends.
QByteArray normalize(const QByteArray& in)
{
    QByteArray o;
    o.reserve(in.size());
    for (const char c : in) {
        if (c == '\0' || (c >= 0x11 && c <= 0x14) || c == '\x7f' || c == '\r')
            continue;
        o.append(c);
    }
    return o.trimmed();
}

QByteArray toLineEnding(const QByteArray& in, const QString& mode)
{
    if (mode == QLatin1String("keep"))
        return in;
    const QByteArray eol = mode == QLatin1String("crlf") ? QByteArrayLiteral("\r\n")
                         : mode == QLatin1String("cr")   ? QByteArrayLiteral("\r")
                                                         : QByteArrayLiteral("\n");
    QByteArray o;
    o.reserve(in.size() + in.size() / 20);
    for (qsizetype i = 0; i < in.size(); ++i) {
        const char c = in.at(i);
        if (c == '\r') {
            if (i + 1 < in.size() && in.at(i + 1) == '\n')
                ++i;
            o.append(eol);
        } else if (c == '\n') {
            o.append(eol);
        } else {
            o.append(c);
        }
    }
    return o;
}

struct Options
{
    QString mode;
    QString port;
    qint32 baud = 9600;
    QSerialPort::DataBits dataBits = QSerialPort::Data7;
    QSerialPort::Parity parity = QSerialPort::EvenParity;
    QSerialPort::StopBits stopBits = QSerialPort::TwoStop;

    // receive
    bool xonStart = false;
    int xonDelayMs = -1;
    int pauseEvery = 0;
    int pauseMs = 1000;
    QString pauseMode = QStringLiteral("xoff");
    int graceMs = 150; // bytes in this window after a pause starts are "in flight"
    int tolerance = 0; // bytes allowed after the grace window
    int idleTimeoutMs = 5000;
    int maxWaitMs = 120000;
    QString expectFile;
    QString outFile;

    // punch
    QString file;
    int startDelayMs = 1000;
    int leader = 20;
    bool dc2 = false;
    int charDelayMs = 0;
    int truncate = -1;
    QString eol = QStringLiteral("lf");
};

bool configurePort(QSerialPort& port, const Options& o, QSerialPort::FlowControl flow)
{
    port.setPortName(o.port);
    port.setBaudRate(o.baud);
    port.setDataBits(o.dataBits);
    port.setParity(o.parity);
    port.setStopBits(o.stopBits);
    port.setFlowControl(flow);
    if (!port.open(QIODevice::ReadWrite)) {
        logLine(QStringLiteral("ERROR: cannot open %1: %2").arg(o.port, port.errorString()));
        return false;
    }
    port.setDataTerminalReady(true);
    if (flow != QSerialPort::HardwareControl)
        port.setRequestToSend(true);
    port.clear();
    return true;
}

// ------------------------------------------------------------------ receive

class ReceivePeer
{
public:
    explicit ReceivePeer(const Options& o) : m_o(o) {}

    bool start()
    {
        if (!configurePort(m_port, m_o, QSerialPort::NoFlowControl))
            return false;
        QObject::connect(&m_port, &QSerialPort::readyRead, &m_port, [this] { onRead(); });
        QObject::connect(&m_port, &QSerialPort::errorOccurred, &m_port, [this](QSerialPort::SerialPortError e) {
            if (e == QSerialPort::ResourceError) {
                m_fails << QStringLiteral("port error: %1").arg(m_port.errorString());
                finish();
            }
        });

        m_idle.setSingleShot(true);
        QObject::connect(&m_idle, &QTimer::timeout, &m_port, [this] {
            logLine(QStringLiteral("no data for %1 ms - ending").arg(m_o.idleTimeoutMs));
            finish();
        });
        QTimer::singleShot(m_o.maxWaitMs, &m_port, [this] {
            if (!m_done) {
                m_fails << QStringLiteral("gave up after %1 ms (max wait)").arg(m_o.maxWaitMs);
                finish();
            }
        });

        logLine(QStringLiteral("receive: listening on %1 at %2 baud").arg(m_o.port).arg(m_o.baud));
        if (m_o.xonStart) {
            sendXon();
        } else if (m_o.xonDelayMs >= 0) {
            m_expectXon = true;
            logLine(QStringLiteral("will send XON in %1 ms; nothing may arrive before that").arg(m_o.xonDelayMs));
            QTimer::singleShot(m_o.xonDelayMs, &m_port, [this] { sendXon(); });
        }
        return true;
    }

private:
    void sendXon()
    {
        m_port.write(QByteArray(1, kXon));
        m_xonSent = true;
        logLine(QStringLiteral("sent XON"));
    }

    void onRead()
    {
        const QByteArray d = m_port.readAll();
        if (d.isEmpty() || m_done)
            return;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_firstMs == 0) {
            m_firstMs = nowMs;
            logLine(QStringLiteral("first data received"));
        }
        m_lastMs = nowMs;

        // Bytes that show up right after the pause starts were already on the
        // wire or in our own receive buffer, or arrived before our XOFF/RTS
        // change physically went out (process scheduling adds tens of ms).
        // Only bytes arriving after the grace window count against the sender.
        if (m_paused) {
            if (m_pauseClock.elapsed() <= m_o.graceMs)
                m_inFlight += d.size();
            else
                m_duringPause += d.size();
        }
        if (m_expectXon && !m_xonSent)
            m_beforeXon += d.size();

        for (const char c : d) {
            m_data.append(c);
            if (m_closing)
                continue;
            if (!m_started) {
                if (isLeaderChar(c))
                    continue;
                m_started = true;
                continue;
            }
            if (c == '%') {
                m_closing = true;
                logLine(QStringLiteral("closing % received"));
                QTimer::singleShot(400, &m_port, [this] { finish(); }); // collect trailer
            }
        }

        if (!m_paused && !m_closing && m_o.pauseEvery > 0) {
            m_sincePause += d.size();
            if (m_sincePause >= m_o.pauseEvery)
                startPause();
        }
        if (!m_paused)
            m_idle.start(m_o.idleTimeoutMs);
    }

    void startPause()
    {
        m_paused = true;
        m_duringPause = 0;
        m_inFlight = 0;
        m_pauseClock.start();
        ++m_pauses;
        m_idle.stop();
        if (m_o.pauseMode == QLatin1String("rts"))
            m_port.setRequestToSend(false);
        else
            m_port.write(QByteArray(1, kXoff));
        logLine(QStringLiteral("pause #%1 (%2) after %3 bytes").arg(m_pauses).arg(m_o.pauseMode).arg(m_data.size()));
        QTimer::singleShot(m_o.pauseMs, &m_port, [this] { endPause(); });
    }

    void endPause()
    {
        if (m_done)
            return;
        m_maxOverrun = qMax(m_maxOverrun, m_duringPause);
        m_maxInFlight = qMax(m_maxInFlight, m_inFlight);
        logLine(QStringLiteral("resume #%1: %2 bytes in the first %3 ms (in flight), %4 bytes after that")
                    .arg(m_pauses)
                    .arg(m_inFlight)
                    .arg(m_o.graceMs)
                    .arg(m_duringPause));
        m_paused = false;
        m_sincePause = 0;
        if (m_o.pauseMode == QLatin1String("rts"))
            m_port.setRequestToSend(true);
        else
            m_port.write(QByteArray(1, kXon));
        m_idle.start(m_o.idleTimeoutMs);
    }

    void finish()
    {
        if (m_done)
            return;
        m_done = true;
        if (m_paused) {
            m_maxOverrun = qMax(m_maxOverrun, m_duringPause);
            m_maxInFlight = qMax(m_maxInFlight, m_inFlight);
        }
        m_port.close();

        if (!m_o.outFile.isEmpty()) {
            QFile f(m_o.outFile);
            if (f.open(QIODevice::WriteOnly))
                f.write(m_data);
        }

        if (m_data.isEmpty())
            m_fails << QStringLiteral("no data received");
        if (m_expectXon && m_beforeXon > 0)
            m_fails << QStringLiteral("%1 bytes arrived BEFORE XON was sent - sender did not wait for XON").arg(m_beforeXon);
        if (m_pauses > 0 && m_maxOverrun > m_o.tolerance)
            m_fails << QStringLiteral("up to %1 bytes arrived more than %2 ms into a pause (tolerance %3) - sender did not honor %4")
                           .arg(m_maxOverrun)
                           .arg(m_o.graceMs)
                           .arg(m_o.tolerance)
                           .arg(m_o.pauseMode == QLatin1String("rts") ? QStringLiteral("CTS") : QStringLiteral("XOFF"));
        if (!m_o.expectFile.isEmpty()) {
            QFile f(m_o.expectFile);
            if (!f.open(QIODevice::ReadOnly)) {
                m_fails << QStringLiteral("cannot read expected file %1").arg(m_o.expectFile);
            } else {
                const QByteArray want = normalize(f.readAll());
                const QByteArray got = normalize(m_data);
                if (want != got) {
                    qsizetype i = 0;
                    while (i < want.size() && i < got.size() && want.at(i) == got.at(i))
                        ++i;
                    m_fails << QStringLiteral("content mismatch at character %1 (expected %2 chars, got %3). Expected: '%4' Got: '%5'")
                                   .arg(i)
                                   .arg(want.size())
                                   .arg(got.size())
                                   .arg(QString::fromLatin1(want.mid(i, 30)).replace(QLatin1Char('\n'), QLatin1String("\\n")),
                                        QString::fromLatin1(got.mid(i, 30)).replace(QLatin1Char('\n'), QLatin1String("\\n")));
                }
            }
        }

        for (const QString& f : m_fails)
            logLine(QStringLiteral("FAIL: ") + f);
        out() << "PEER RESULT: " << (m_fails.isEmpty() ? "PASS" : "FAIL") << " bytes=" << m_data.size()
              << " firstMs=" << m_firstMs << " lastMs=" << m_lastMs << " pauses=" << m_pauses
              << " maxInFlight=" << m_maxInFlight << " maxOverrun=" << m_maxOverrun << " beforeXon=" << m_beforeXon
              << " closingPercent=" << (m_closing ? "yes" : "no") << Qt::endl;
        QCoreApplication::exit(m_fails.isEmpty() ? 0 : 1);
    }

    Options m_o;
    QSerialPort m_port;
    QTimer m_idle;
    QByteArray m_data;
    QStringList m_fails;
    bool m_done = false;
    bool m_started = false;
    bool m_closing = false;
    bool m_expectXon = false;
    bool m_xonSent = false;
    bool m_paused = false;
    int m_pauses = 0;
    qint64 m_sincePause = 0;
    qint64 m_duringPause = 0;
    qint64 m_maxOverrun = 0;
    qint64 m_inFlight = 0;
    qint64 m_maxInFlight = 0;
    QElapsedTimer m_pauseClock;
    qint64 m_beforeXon = 0;
    qint64 m_firstMs = 0;
    qint64 m_lastMs = 0;
};

// ------------------------------------------------------------------ punch

class PunchPeer
{
public:
    explicit PunchPeer(const Options& o) : m_o(o) {}

    bool start()
    {
        QFile f(m_o.file);
        if (!f.open(QIODevice::ReadOnly)) {
            logLine(QStringLiteral("ERROR: cannot read %1").arg(m_o.file));
            return false;
        }
        QByteArray program = toLineEnding(f.readAll(), m_o.eol);
        const bool truncated = m_o.truncate >= 0 && m_o.truncate < program.size();
        if (truncated)
            program.truncate(m_o.truncate);

        m_payload = QByteArray(m_o.leader, '\0');
        if (m_o.dc2)
            m_payload.append(kDc2);
        m_payload.append(program);
        m_programEnd = m_payload.size();
        if (!truncated) {
            if (m_o.dc2)
                m_payload.append(kDc4);
            m_payload.append(QByteArray(m_o.leader, '\0'));
        }

        // Honor XOFF from the PC, like a real control punching out.
        if (!configurePort(m_port, m_o, QSerialPort::SoftwareControl))
            return false;
        QObject::connect(&m_port, &QSerialPort::bytesWritten, &m_port, [this](qint64 n) { onWritten(n); });
        QObject::connect(&m_port, &QSerialPort::errorOccurred, &m_port, [this](QSerialPort::SerialPortError e) {
            if (e == QSerialPort::ResourceError) {
                logLine(QStringLiteral("ERROR: %1").arg(m_port.errorString()));
                QCoreApplication::exit(1);
            }
        });
        m_charTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&m_charTimer, &QTimer::timeout, &m_port, [this] { writeNextChar(); });

        logLine(QStringLiteral("punch: %1 bytes to %2 in %3 ms%4")
                .arg(m_payload.size())
                .arg(m_o.port)
                .arg(m_o.startDelayMs)
                .arg(truncated ? QStringLiteral(" (truncated, no closing %)") : QString()));
        QTimer::singleShot(m_o.startDelayMs, &m_port, [this] {
            logLine(QStringLiteral("punch started"));
            // The trailer goes out as a separate write so we can tell when
            // the program itself has been fully sent (see onWritten).
            if (m_o.charDelayMs > 0)
                m_charTimer.start(m_o.charDelayMs);
            else
                m_port.write(m_payload.left(m_programEnd));
        });
        QTimer::singleShot(m_o.maxWaitMs, &m_port, [this] {
            logLine(QStringLiteral("FAIL: punch did not finish within %1 ms").arg(m_o.maxWaitMs));
            QCoreApplication::exit(1);
        });
        return true;
    }

private:
    void writeNextChar()
    {
        if (m_pos >= m_payload.size()) {
            m_charTimer.stop();
            return;
        }
        if (m_port.bytesToWrite() == 0)
            m_port.write(m_payload.mid(m_pos++, 1));
    }

    void onWritten(qint64 n)
    {
        m_written += n;
        if (m_finished)
            return;
        if (m_written >= m_payload.size()) {
            m_finished = true;
            logLine(QStringLiteral("PUNCH DONE bytes=%1").arg(m_written));
            QTimer::singleShot(500, &m_port, [] { QCoreApplication::exit(0); });
            return;
        }
        if (m_written >= m_programEnd && !m_trailerStarted) {
            m_trailerStarted = true;
            if (m_o.charDelayMs == 0)
                m_port.write(m_payload.mid(m_programEnd));
            // A receiver may close its port as soon as it sees the closing %,
            // which is correct. A real UART would still clock the trailer out
            // into nothing, but a com0com pair holds it forever, so don't wait
            // for it.
            QTimer::singleShot(2000, &m_port, [this] {
                if (m_finished)
                    return;
                m_finished = true;
                logLine(QStringLiteral("PUNCH DONE bytes=%1 (program complete; receiver closed before the trailer)")
                            .arg(m_written));
                QCoreApplication::exit(0);
            });
        }
    }

    Options m_o;
    QSerialPort m_port;
    QTimer m_charTimer;
    QByteArray m_payload;
    qsizetype m_programEnd = 0;
    qsizetype m_pos = 0;
    qint64 m_written = 0;
    bool m_trailerStarted = false;
    bool m_finished = false;
};

// ------------------------------------------------------------------ echo

class EchoPeer
{
public:
    explicit EchoPeer(const Options& o) : m_o(o) {}

    bool start()
    {
        if (!configurePort(m_port, m_o, QSerialPort::NoFlowControl))
            return false;
        QObject::connect(&m_port, &QSerialPort::readyRead, &m_port, [this] {
            const QByteArray d = m_port.readAll();
            m_total += d.size();
            m_port.write(d);
        });
        QObject::connect(&m_report, &QTimer::timeout, &m_port, [this] {
            if (m_total != m_reported) {
                logLine(QStringLiteral("echoed %1 bytes").arg(m_total));
                m_reported = m_total;
            }
        });
        m_report.start(1000);
        if (m_o.maxWaitMs > 0)
            QTimer::singleShot(m_o.maxWaitMs, &m_port, [] { QCoreApplication::exit(0); });
        logLine(QStringLiteral("echo: looping back everything on %1 (Ctrl+C to stop)").arg(m_o.port));
        return true;
    }

private:
    Options m_o;
    QSerialPort m_port;
    QTimer m_report;
    qint64 m_total = 0;
    qint64 m_reported = 0;
};

QSerialPort::Parity parseParity(const QString& s, bool* ok)
{
    *ok = true;
    const QString t = s.toLower();
    if (t == QLatin1String("none") || t == QLatin1String("n")) return QSerialPort::NoParity;
    if (t == QLatin1String("even") || t == QLatin1String("e")) return QSerialPort::EvenParity;
    if (t == QLatin1String("odd") || t == QLatin1String("o")) return QSerialPort::OddParity;
    if (t == QLatin1String("mark") || t == QLatin1String("m")) return QSerialPort::MarkParity;
    if (t == QLatin1String("space") || t == QLatin1String("s")) return QSerialPort::SpaceParity;
    *ok = false;
    return QSerialPort::NoParity;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nctestpeer"));

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("Simulated CNC control for testing NCSender over virtual or real serial ports."));
    p.addHelpOption();
    p.addPositionalArgument(QStringLiteral("mode"), QStringLiteral("receive | punch | echo | list"));

    const QCommandLineOption portO(QStringLiteral("port"), QStringLiteral("Serial port, e.g. COM11."), QStringLiteral("name"));
    const QCommandLineOption baudO(QStringLiteral("baud"), QStringLiteral("Baud rate (default 9600)."), QStringLiteral("n"), QStringLiteral("9600"));
    const QCommandLineOption dataO(QStringLiteral("databits"), QStringLiteral("5-8 (default 7)."), QStringLiteral("n"), QStringLiteral("7"));
    const QCommandLineOption parityO(QStringLiteral("parity"), QStringLiteral("none|even|odd|mark|space (default even)."), QStringLiteral("p"), QStringLiteral("even"));
    const QCommandLineOption stopO(QStringLiteral("stopbits"), QStringLiteral("1|1.5|2 (default 2)."), QStringLiteral("n"), QStringLiteral("2"));

    const QCommandLineOption xonStartO(QStringLiteral("xon-start"), QStringLiteral("receive: send XON immediately."));
    const QCommandLineOption xonDelayO(QStringLiteral("xon-delay"), QStringLiteral("receive: send XON after this delay; FAIL if anything arrives first."), QStringLiteral("ms"));
    const QCommandLineOption pauseEveryO(QStringLiteral("pause-every"), QStringLiteral("receive: pause the sender after every N bytes."), QStringLiteral("n"));
    const QCommandLineOption pauseMsO(QStringLiteral("pause-ms"), QStringLiteral("receive: pause length (default 1000)."), QStringLiteral("ms"), QStringLiteral("1000"));
    const QCommandLineOption pauseModeO(QStringLiteral("pause-mode"), QStringLiteral("receive: xoff (default) or rts."), QStringLiteral("mode"), QStringLiteral("xoff"));
    const QCommandLineOption graceO(QStringLiteral("grace-ms"), QStringLiteral("receive: bytes arriving this soon after a pause starts count as in flight (default 150)."), QStringLiteral("ms"), QStringLiteral("150"));
    const QCommandLineOption tolO(QStringLiteral("tolerance"), QStringLiteral("receive: bytes allowed to arrive after the grace window of a pause (default 0)."), QStringLiteral("n"), QStringLiteral("0"));
    const QCommandLineOption idleO(QStringLiteral("idle-timeout"), QStringLiteral("receive: end after this much silence (default 5000)."), QStringLiteral("ms"), QStringLiteral("5000"));
    const QCommandLineOption maxO(QStringLiteral("max-wait"), QStringLiteral("Give up after this long (default 120000; echo: 0 = forever)."), QStringLiteral("ms"));
    const QCommandLineOption expectO(QStringLiteral("expect"), QStringLiteral("receive: FAIL unless the data matches this file (ignoring CR, NUL, DC codes)."), QStringLiteral("file"));
    const QCommandLineOption outO(QStringLiteral("out"), QStringLiteral("receive: save the raw received bytes here."), QStringLiteral("file"));

    const QCommandLineOption fileO(QStringLiteral("file"), QStringLiteral("punch: program file to send."), QStringLiteral("file"));
    const QCommandLineOption startDelayO(QStringLiteral("start-delay"), QStringLiteral("punch: wait before sending (default 1000)."), QStringLiteral("ms"), QStringLiteral("1000"));
    const QCommandLineOption leaderO(QStringLiteral("leader"), QStringLiteral("punch: NUL leader/trailer length (default 20)."), QStringLiteral("n"), QStringLiteral("20"));
    const QCommandLineOption dc2O(QStringLiteral("dc2"), QStringLiteral("punch: wrap the program in DC2 ... DC4 like a Fanuc punch."));
    const QCommandLineOption charDelayO(QStringLiteral("char-delay"), QStringLiteral("punch: delay between characters."), QStringLiteral("ms"), QStringLiteral("0"));
    const QCommandLineOption truncO(QStringLiteral("truncate"), QStringLiteral("punch: send only the first N program bytes (no closing %)."), QStringLiteral("n"));
    const QCommandLineOption eolO(QStringLiteral("eol"), QStringLiteral("punch: keep|lf|crlf|cr (default lf, like Fanuc ISO)."), QStringLiteral("mode"), QStringLiteral("lf"));

    p.addOptions({portO, baudO, dataO, parityO, stopO, xonStartO, xonDelayO, pauseEveryO, pauseMsO, pauseModeO, graceO, tolO,
                  idleO, maxO, expectO, outO, fileO, startDelayO, leaderO, dc2O, charDelayO, truncO, eolO});
    p.process(app);

    const QStringList pos = p.positionalArguments();
    if (pos.isEmpty())
        p.showHelp(2);

    Options o;
    o.mode = pos.first().toLower();
    if (o.mode == QLatin1String("list")) {
        for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts())
            out() << info.portName() << "\t" << info.description() << Qt::endl;
        return 0;
    }

    o.port = p.value(portO);
    if (o.port.isEmpty()) {
        out() << "--port is required" << Qt::endl;
        return 2;
    }
    o.baud = p.value(baudO).toInt();
    switch (p.value(dataO).toInt()) {
    case 5: o.dataBits = QSerialPort::Data5; break;
    case 6: o.dataBits = QSerialPort::Data6; break;
    case 7: o.dataBits = QSerialPort::Data7; break;
    default: o.dataBits = QSerialPort::Data8; break;
    }
    bool ok = false;
    o.parity = parseParity(p.value(parityO), &ok);
    if (!ok) {
        out() << "bad --parity" << Qt::endl;
        return 2;
    }
    const QString stop = p.value(stopO);
    o.stopBits = stop == QLatin1String("1") ? QSerialPort::OneStop
               : stop == QLatin1String("1.5") ? QSerialPort::OneAndHalfStop
                                              : QSerialPort::TwoStop;

    o.xonStart = p.isSet(xonStartO);
    if (p.isSet(xonDelayO))
        o.xonDelayMs = p.value(xonDelayO).toInt();
    if (p.isSet(pauseEveryO))
        o.pauseEvery = p.value(pauseEveryO).toInt();
    o.pauseMs = p.value(pauseMsO).toInt();
    o.pauseMode = p.value(pauseModeO).toLower();
    o.graceMs = qMax(0, p.value(graceO).toInt());
    o.tolerance = p.value(tolO).toInt();
    o.idleTimeoutMs = p.value(idleO).toInt();
    o.maxWaitMs = p.isSet(maxO) ? p.value(maxO).toInt() : (o.mode == QLatin1String("echo") ? 0 : 120000);
    o.expectFile = p.value(expectO);
    o.outFile = p.value(outO);
    o.file = p.value(fileO);
    o.startDelayMs = p.value(startDelayO).toInt();
    o.leader = qMax(0, p.value(leaderO).toInt());
    o.dc2 = p.isSet(dc2O);
    o.charDelayMs = p.value(charDelayO).toInt();
    if (p.isSet(truncO))
        o.truncate = p.value(truncO).toInt();
    o.eol = p.value(eolO).toLower();

    if (o.mode == QLatin1String("receive")) {
        ReceivePeer peer(o);
        if (!peer.start())
            return 1;
        return app.exec();
    }
    if (o.mode == QLatin1String("punch")) {
        if (o.file.isEmpty()) {
            out() << "punch needs --file" << Qt::endl;
            return 2;
        }
        PunchPeer peer(o);
        if (!peer.start())
            return 1;
        return app.exec();
    }
    if (o.mode == QLatin1String("echo")) {
        EchoPeer peer(o);
        if (!peer.start())
            return 1;
        return app.exec();
    }
    out() << "unknown mode '" << o.mode << "' (receive | punch | echo | list)" << Qt::endl;
    return 2;
}
