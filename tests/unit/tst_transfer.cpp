// Transfer tests over the com0com virtual pairs (COM10<->COM11, COM12<->COM13).
// The far end of each pair is a QSerialPort opened right here in the test,
// playing the CNC control. Every test is skipped if the ports don't exist.

#include "AppPaths.h"
#include "MachineChannel.h"
#include "MachineProfile.h"
#include "ProgramFormatter.h"
#include "TransferLog.h"
#include "TransferTypes.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <functional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace nc;

#define QCOMPARE_ENUM(a, b) QCOMPARE(static_cast<int>(a), static_cast<int>(b))

namespace {

bool portExists(const QString& name)
{
    for (const QSerialPortInfo& i : QSerialPortInfo::availablePorts()) {
        if (i.portName().compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// The simulated control on the far end of a com0com pair.
class Control
{
public:
    explicit Control(const QString& port)
    {
        m_port.setPortName(port);
        m_port.setBaudRate(9600);
        m_port.setDataBits(QSerialPort::Data7);
        m_port.setParity(QSerialPort::EvenParity);
        m_port.setStopBits(QSerialPort::TwoStop);
        m_port.setFlowControl(QSerialPort::NoFlowControl);
        m_ok = m_port.open(QIODevice::ReadWrite);
        if (m_ok) {
            m_port.setDataTerminalReady(true);
            m_port.setRequestToSend(true);
            // Drain anything still in flight from an earlier test before listening.
            QTest::qWait(300);
            m_port.clear();
            QObject::connect(&m_port, &QSerialPort::readyRead, &m_port, [this] {
                m_data += m_port.readAll();
                if (m_onData)
                    m_onData(m_data);
            });
        }
    }
    bool ok() const { return m_ok; }
    QByteArray data() const { return m_data; }
    void clear() { m_data.clear(); }
    void send(const QByteArray& d)
    {
        m_port.write(d);
        m_port.flush();
    }
    QSerialPort& port() { return m_port; }
    // Called with everything received so far, as soon as more arrives, so a
    // test can react the way a control would without polling.
    void onData(std::function<void(const QByteArray&)> f) { m_onData = std::move(f); }

private:
    QSerialPort m_port;
    QByteArray m_data;
    std::function<void(const QByteArray&)> m_onData;
    bool m_ok = false;
};

MachineProfile profileFor(const QString& port, const QString& rxFolder)
{
    MachineProfile p;
    p.name = QStringLiteral("Test Mill");
    p.portName = port;
    p.baudRate = 9600;
    p.dataBits = 7;
    p.parity = Parity::Even;
    p.stopBits = StopBits::Two;
    p.flowControl = FlowControl::XonXoff;
    p.lineEnding = LineEnding::CRLF;
    p.receiveFolder = rxFolder;
    p.receiveStartTimeoutSec = 20;
    p.receiveIdleTimeoutSec = 2;
    return p;
}

TransferResult resultOf(const QSignalSpy& spy)
{
    return spy.isEmpty() ? TransferResult{} : spy.first().first().value<TransferResult>();
}

QByteArray largeProgram(int lines)
{
    QByteArray s("%\r\nO3001\r\n");
    for (int i = 1; i <= lines; ++i)
        s += "N" + QByteArray::number(i * 10) + " G01 X" + QByteArray::number(i) + ".000 Y-" + QByteArray::number(i) + ".500 F500\r\n";
    s += "M30\r\n%\r\n";
    return s;
}

} // namespace

class TestTransfer : public QObject
{
    Q_OBJECT

    QTemporaryDir m_data;

private slots:
    void initTestCase()
    {
        // Fallback saves land here instead of next to the exe.
        qputenv("NCSENDER_DATA_DIR", m_data.path().toLocal8Bit());
        registerTransferMetaTypes();
        if (!portExists(QStringLiteral("COM10")) || !portExists(QStringLiteral("COM11")) || !portExists(QStringLiteral("COM12"))
            || !portExists(QStringLiteral("COM13")))
            QSKIP("com0com pairs COM10<->COM11 and COM12<->COM13 are not installed");
    }

    // ---------------------------------------------------------------- send

    void send_bytesArriveExactly()
    {
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        const QByteArray program("O1001\nG00 X0 Y0\nM30\n");
        const QString file = dir.filePath(QStringLiteral("O1001.nc"));
        QVERIFY(writeFile(file, program));
        MachineProfile p = profileFor(QStringLiteral("COM10"), dir.path());
        p.leaderCount = 5;
        p.trailerCount = 5;

        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QVERIFY(ch.startSend(p, SendRequest{file}));
        QVERIFY(done.wait(15000));
        const TransferResult r = resultOf(done);
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Success);
        QTest::qWait(200);
        const QByteArray expected = ProgramFormatter::format(program, p);
        QCOMPARE(ctl.data(), expected);
        QCOMPARE(r.bytesTransferred, qint64(expected.size()));
        QCOMPARE(r.bytesTotal, qint64(expected.size()));
    }

    void send_xonFollowedByXoffKeepsWaiting()
    {
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        const QString file = dir.filePath(QStringLiteral("p.nc"));
        QVERIFY(writeFile(file, "%\nO1\nM30\n%\n"));
        MachineProfile p = profileFor(QStringLiteral("COM10"), dir.path());
        p.waitForXon = true;

        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QVERIFY(ch.startSend(p, SendRequest{file}));
        QTest::qWait(500);
        ctl.send(QByteArray("\x11\x13", 2)); // "ready" then immediately "wait"
        QTest::qWait(1500);
        QVERIFY2(ctl.data().isEmpty(), "sender started although the last control code was XOFF");
        QVERIFY(ch.isBusy());
        ctl.send(QByteArray(1, '\x11'));
        QVERIFY(done.wait(10000));
        QCOMPARE_ENUM(resultOf(done).outcome, TransferOutcome::Success);
        QTest::qWait(200);
        QVERIFY(ctl.data().contains("O1"));
    }

    void send_xoffPausesAndXonResumes()
    {
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        const QByteArray program = largeProgram(120); // ~4 KB, ~5 s at 9600
        const QString file = dir.filePath(QStringLiteral("big.nc"));
        QVERIFY(writeFile(file, program));
        const MachineProfile p = profileFor(QStringLiteral("COM10"), dir.path());

        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QSignalSpy states(&ch, &MachineChannel::stateChanged);
        QVERIFY(ch.startSend(p, SendRequest{file}));
        QTRY_VERIFY_WITH_TIMEOUT(ctl.data().size() > 500, 5000);
        ctl.send(QByteArray(1, '\x13'));
        QTest::qWait(300); // in-flight bytes settle
        const qsizetype atPause = ctl.data().size();
        QTest::qWait(2000);
        QCOMPARE(ctl.data().size(), atPause); // nothing while paused
        bool sawPaused = false;
        for (const auto& s : states)
            sawPaused = sawPaused || s.first().value<TransferState>() == TransferState::Paused;
        QVERIFY2(sawPaused, "status never showed Paused while the control held XOFF");
        QVERIFY(ch.isBusy());
        ctl.send(QByteArray(1, '\x11'));
        QVERIFY(done.wait(20000));
        QCOMPARE_ENUM(resultOf(done).outcome, TransferOutcome::Success);
        QTest::qWait(200);
        QCOMPARE(ctl.data(), ProgramFormatter::format(program, p));
    }

    void send_cancelMidTransferThenResend()
    {
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        const QString file = dir.filePath(QStringLiteral("big.nc"));
        QVERIFY(writeFile(file, largeProgram(200)));
        const MachineProfile p = profileFor(QStringLiteral("COM10"), dir.path());

        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QVERIFY(ch.startSend(p, SendRequest{file}));
        QTRY_VERIFY_WITH_TIMEOUT(ctl.data().size() > 300, 5000);
        ch.cancel();
        QVERIFY(done.wait(3000));
        const TransferResult r = resultOf(done);
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Cancelled);
        QVERIFY(r.bytesTransferred > 0 && r.bytesTransferred < r.bytesTotal);
        QTest::qWait(300);
        const qsizetype after = ctl.data().size();
        QTest::qWait(1000);
        QCOMPARE(ctl.data().size(), after); // really stopped

        done.clear();
        ctl.clear();
        const QString smallFile = dir.filePath(QStringLiteral("small.nc"));
        QVERIFY(writeFile(smallFile, "%\nO2\n%\n"));
        QVERIFY(ch.startSend(p, SendRequest{smallFile}));
        QVERIFY(done.wait(10000));
        QCOMPARE_ENUM(resultOf(done).outcome, TransferOutcome::Success);
    }

    // ---------------------------------------------------------------- drip feed

    void drip_eachCycleStartRunsTheProgramAgain()
    {
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        const QString file = dir.filePath(QStringLiteral("O5001.nc"));
        QVERIFY(writeFile(file, "%\nO5001\nG00 X0 Y0\nM30\n%\n"));
        MachineProfile p = profileFor(QStringLiteral("COM10"), dir.path());
        p.leaderCount = 4;
        p.trailerCount = 4; // never sent in drip feed
        MachineProfile noTrailer = p;
        noTrailer.trailerCount = 0;

        // The control reads the program and sends XOFF at the closing %, as a
        // Fanuc does at the end of a program in Remote mode.
        QByteArray expected = ProgramFormatter::format(readFile(file), noTrailer);
        bool held = false;
        ctl.onData([&](const QByteArray& all) {
            if (!held && all.endsWith(expected)) {
                held = true;
                ctl.send(QByteArray(1, '\x13'));
            }
        });

        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy runs(&ch, &MachineChannel::runFinished);
        QSignalSpy done(&ch, &MachineChannel::finished);
        QSignalSpy states(&ch, &MachineChannel::stateChanged);
        QVERIFY(ch.startSend(p, SendRequest{file, SendRequest::Mode::DripFeed}));
        QTest::qWait(1500);
        QVERIFY2(ctl.data().isEmpty(), "drip feed sent before the control asked for data");
        QVERIFY(ch.isBusy());

        ctl.send(QByteArray(1, '\x11')); // Cycle Start
        QVERIFY(runs.wait(15000));
        TransferResult r = runs.first().first().value<TransferResult>();
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Success);
        QCOMPARE(r.bytesTransferred, qint64(expected.size()));
        QVERIFY(r.message.startsWith(QLatin1String("Drip fed")));
        QCOMPARE(ctl.data(), expected);
        QVERIFY(done.isEmpty());
        QVERIFY(ch.isBusy()); // still armed
        QTRY_VERIFY_WITH_TIMEOUT(states.last().first().value<TransferState>() == TransferState::WaitingForXon, 2000);

        // An edit saved between runs goes out on the next Cycle Start, and
        // nothing is sent until then.
        QVERIFY(writeFile(file, "%\nO5001\nG00 X10 Y10\nM30\n%\n"));
        QTest::qWait(1000);
        QCOMPARE(ctl.data(), expected);
        ctl.clear();
        expected = ProgramFormatter::format(readFile(file), noTrailer);
        held = false;
        ctl.send(QByteArray(1, '\x11'));
        QVERIFY(runs.wait(15000));
        QCOMPARE(runs.size(), 2);
        QCOMPARE(ctl.data(), expected);

        ch.cancel();
        QVERIFY(done.wait(3000));
        QCOMPARE_ENUM(resultOf(done).outcome, TransferOutcome::Cancelled);
        QVERIFY(!ch.isBusy());
    }

    void drip_controlStoppingBeforeClosingPercentKeepsTheRunOpen()
    {
        // If the control stops reading after M30 without taking the closing
        // %, the run must not be treated as finished: dropping the rest could
        // lose the end of the program, and the next XON could then start a new
        // program while the machine still wants the old one.
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        const QString file = dir.filePath(QStringLiteral("p.nc"));
        QVERIFY(writeFile(file, "%\nO1\nM30\n%\n"));
        MachineProfile p = profileFor(QStringLiteral("COM10"), dir.path());
        p.charDelayMs = 100; // so the XOFF lands before the closing % goes out
        bool held = false;
        ctl.onData([&](const QByteArray& all) {
            if (!held && all.endsWith("M30\r\n")) {
                held = true;
                ctl.send(QByteArray(1, '\x13'));
            }
        });

        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy runs(&ch, &MachineChannel::runFinished);
        QVERIFY(ch.startSend(p, SendRequest{file, SendRequest::Mode::DripFeed}));
        QTest::qWait(500);
        ctl.send(QByteArray(1, '\x11'));
        QTRY_VERIFY_WITH_TIMEOUT(held, 10000);
        QTest::qWait(2500);
        QVERIFY2(runs.isEmpty(), "run finished while the closing % was still held back");
        QVERIFY(ctl.data().endsWith("M30\r\n"));

        ctl.send(QByteArray(1, '\x11'));
        QVERIFY(runs.wait(10000));
        QCOMPARE(ctl.data(), ProgramFormatter::format(readFile(file), p));
        ch.cancel();
    }

    void drip_stopsWithinTenCharactersOfXoffAndPacesAtLineRate()
    {
        // A Fanuc raises alarm 087 if more than ~10 characters arrive after
        // it sends XOFF. Hold the sender every 500 characters, once for longer
        // than any stall warning, and count what still arrives.
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        QByteArray program = "%\nO7002\n";
        for (int i = 0; program.size() < 3000; ++i)
            program += QStringLiteral("G01 X%1.123 Y%2.456 F500.\n").arg(i % 97).arg(i % 89).toLatin1();
        program += "M30\n%\n";
        const QString file = dir.filePath(QStringLiteral("O7002.nc"));
        QVERIFY(writeFile(file, program));
        const MachineProfile p = profileFor(QStringLiteral("COM10"), dir.path());
        MachineProfile noTrailer = p;
        noTrailer.trailerCount = 0;
        const QByteArray expected = ProgramFormatter::format(program, noTrailer);

        qsizetype xoffAt = -1;
        qsizetype nextHold = 500;
        int holds = 0;
        qsizetype worstOverrun = 0;
        ctl.onData([&](const QByteArray& all) {
            if (xoffAt >= 0) {
                worstOverrun = std::max(worstOverrun, all.size() - xoffAt);
            } else if (all.size() >= nextHold && !all.endsWith(expected)) {
                xoffAt = all.size();
#ifdef Q_OS_WIN
                // Straight onto the wire, as a control does. QSerialPort's
                // asynchronous write can take ~15 ms to start here, and what
                // arrives meanwhile is not the sender's overrun.
                ::TransmitCommChar(reinterpret_cast<HANDLE>(ctl.port().handle()), '\x13');
#else
                ctl.send(QByteArray(1, '\x13'));
#endif
                const int holdMs = holds++ == 1 ? 4000 : 400;
                QTimer::singleShot(holdMs, &ctl.port(), [&] {
                    xoffAt = -1;
                    nextHold = ctl.data().size() + 500;
                    ctl.send(QByteArray(1, '\x11'));
                });
            }
        });

        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy runs(&ch, &MachineChannel::runFinished);
        QSignalSpy done(&ch, &MachineChannel::finished);
        qint64 progressAhead = 0;
        QObject::connect(&ch, &MachineChannel::progress, &ch, [&](qint64 sent, qint64) {
            progressAhead = std::max(progressAhead, sent - qint64(ctl.data().size()));
        });
        QVERIFY(ch.startSend(p, SendRequest{file, SendRequest::Mode::DripFeed}));
        QTest::qWait(500);
        QElapsedTimer clock;
        clock.start();
        ctl.send(QByteArray(1, '\x11'));
        QVERIFY(runs.wait(60000));
        const qint64 elapsed = clock.elapsed();

        const TransferResult r = runs.first().first().value<TransferResult>();
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Success);
        QCOMPARE(r.bytesTransferred, qint64(expected.size()));
        QCOMPARE(ctl.data(), expected);
        QVERIFY(done.isEmpty()); // the long hold was not treated as an error
        QVERIFY(holds >= 5);
        QVERIFY2(worstOverrun <= 10, qPrintable(QStringLiteral("%1 characters after XOFF").arg(worstOverrun)));
        // Progress counts bytes that have left the port; this end reads them a
        // few characters late. Counting bytes handed to the driver would put
        // it hundreds ahead.
        QVERIFY2(progressAhead <= 10, qPrintable(QStringLiteral("progress ran %1 bytes ahead").arg(progressAhead)));
        // Never faster than the line: 11 bits per character at 9600 baud.
        const qint64 lineMs = qint64(expected.size()) * 11 * 1000 / 9600;
        QVERIFY2(elapsed >= lineMs, qPrintable(QStringLiteral("%1 ms < %2 ms").arg(elapsed).arg(lineMs)));

        ch.cancel();
        QVERIFY(done.wait(3000));
    }

    void drip_missingFileWhenRearmingStopsWithError()
    {
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        const QString file = dir.filePath(QStringLiteral("p.nc"));
        QVERIFY(writeFile(file, "%\nO1\nM30\n%\n"));
        const MachineProfile p = profileFor(QStringLiteral("COM10"), dir.path());

        // Delete the file as soon as the run has delivered all of it (the
        // source has closed it by then), before the worker re-arms.
        const QByteArray expected = ProgramFormatter::format(readFile(file), p);
        bool removed = false;
        ctl.onData([&](const QByteArray& all) {
            if (!removed && all.endsWith(expected))
                removed = QFile::remove(file);
        });

        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy runs(&ch, &MachineChannel::runFinished);
        QSignalSpy done(&ch, &MachineChannel::finished);
        QVERIFY(ch.startSend(p, SendRequest{file, SendRequest::Mode::DripFeed}));
        QTest::qWait(500);
        ctl.send(QByteArray(1, '\x11'));
        QVERIFY(runs.wait(10000));
        QVERIFY(removed);
        QVERIFY(done.wait(3000)); // re-arming can't read the file
        const TransferResult r = resultOf(done);
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Failed);
        QVERIFY2(r.message.contains(QLatin1String("not found")), qPrintable(r.message));
        QVERIFY(!ch.isBusy());
    }

    void send_portInUse()
    {
        QTemporaryDir dir;
        QSerialPort holder(QStringLiteral("COM10"));
        QVERIFY(holder.open(QIODevice::ReadWrite));
        const QString file = dir.filePath(QStringLiteral("p.nc"));
        QVERIFY(writeFile(file, "%\nO1\n%\n"));
        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QVERIFY(ch.startSend(profileFor(QStringLiteral("COM10"), dir.path()), SendRequest{file}));
        QVERIFY(done.wait(5000));
        const TransferResult r = resultOf(done);
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Failed);
        QVERIFY2(r.message.contains(QLatin1String("in use")), qPrintable(r.message));
    }

    // ---------------------------------------------------------------- receive

    void receive_noiseBeforeOpeningPercent()
    {
        // Regression: one noise byte used to make the opening % close the program.
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QVERIFY(ch.startReceive(profileFor(QStringLiteral("COM10"), dir.path())));
        QTest::qWait(400);
        // 0x01 and 0x1B stand in for line noise (they are neither leader nor program characters).
        static const char punch[] = "\x01\x1B\x00\x00\x12%\nO1001\nG00 X0\nM30\n%\x14\x00\x00";
        ctl.send(QByteArray(punch, sizeof(punch) - 1));
        QVERIFY(done.wait(10000));
        const TransferResult r = resultOf(done);
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Success);
        QCOMPARE(readFile(r.filePath), QByteArray("%\nO1001\nG00 X0\nM30\n%"));
        QVERIFY(QFileInfo(r.filePath).fileName().startsWith(QLatin1String("Test_Mill_O1001_")));
    }

    void receive_leaderOnlySavesNothing()
    {
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        MachineProfile p = profileFor(QStringLiteral("COM10"), dir.filePath(QStringLiteral("rx")));
        p.receiveStartTimeoutSec = 2;
        p.stripReceivedControlChars = false;
        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QVERIFY(ch.startReceive(p));
        QTest::qWait(300);
        ctl.send(QByteArray(30, '\0'));
        QVERIFY(done.wait(6000));
        const TransferResult r = resultOf(done);
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Failed);
        QVERIFY2(r.message.contains(QLatin1String("no program data")), qPrintable(r.message));
        QVERIFY(QDir(p.receiveFolder).entryList(QDir::Files).isEmpty());
    }

    void receive_cutOffProgramIsFlaggedNotSuccess()
    {
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QVERIFY(ch.startReceive(profileFor(QStringLiteral("COM10"), dir.path())));
        QTest::qWait(300);
        ctl.send("%\nO1\nG01 X10\n"); // punch stops, no closing %
        QVERIFY(done.wait(8000));
        const TransferResult r = resultOf(done);
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Failed);
        QVERIFY2(r.message.contains(QLatin1String("incomplete")), qPrintable(r.message));
        QVERIFY(r.filePath.contains(QLatin1String("_PARTIAL")));
        QCOMPARE(readFile(r.filePath), QByteArray("%\nO1\nG01 X10\n"));
    }

    void receive_programWithoutPercentEndsOnSilence()
    {
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QVERIFY(ch.startReceive(profileFor(QStringLiteral("COM10"), dir.path())));
        QTest::qWait(300);
        ctl.send("O1\nM30\n");
        QVERIFY(done.wait(8000));
        QCOMPARE_ENUM(resultOf(done).outcome, TransferOutcome::Success);
    }

    void receive_unusableFolderFallsBack()
    {
        // Regression: a failed save used to throw the received program away.
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        MachineProfile p = profileFor(QStringLiteral("COM10"), QStringLiteral("Q:/no/such/drive"));
        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QSignalSpy states(&ch, &MachineChannel::stateChanged);
        QVERIFY(ch.startReceive(p));
        QTest::qWait(300);
        QVERIFY2(done.isEmpty(), "receive refused to start because the folder is unreachable");
        bool warned = false;
        for (const auto& s : states)
            warned = warned || s.at(1).toString().contains(QLatin1String("not reachable"));
        QVERIFY2(warned, "operator was not told the folder is unreachable");
        ctl.send("%\nO7\nM30\n%\n");
        QVERIFY(done.wait(8000));
        const TransferResult r = resultOf(done);
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Success);
        QVERIFY2(r.filePath.startsWith(QDir(m_data.path()).filePath(QStringLiteral("received"))), qPrintable(r.filePath));
        QVERIFY2(r.message.contains(QLatin1String("could not be used")), qPrintable(r.message));
        QCOMPARE(readFile(r.filePath), QByteArray("%\nO7\nM30\n%"));
    }

    void receive_twoMachinesSameNameNeverOverwrite()
    {
        QTemporaryDir dir;
        Control a(QStringLiteral("COM11"));
        Control b(QStringLiteral("COM13"));
        QVERIFY(a.ok() && b.ok());
        const MachineProfile pa = profileFor(QStringLiteral("COM10"), dir.path());
        const MachineProfile pb = profileFor(QStringLiteral("COM12"), dir.path()); // same name, same folder
        MachineChannel ca(QStringLiteral("A")), cb(QStringLiteral("B"));
        QSignalSpy da(&ca, &MachineChannel::finished), db(&cb, &MachineChannel::finished);
        QVERIFY(ca.startReceive(pa));
        QVERIFY(cb.startReceive(pb));
        QTest::qWait(400);
        a.send("%\nO5\n(FROM A)\n%\n");
        b.send("%\nO5\n(FROM B)\n%\n");
        QVERIFY(da.wait(8000) || !da.isEmpty());
        QVERIFY(db.wait(8000) || !db.isEmpty());
        const QString fa = resultOf(da).filePath, fb = resultOf(db).filePath;
        QVERIFY(fa != fb);
        QVERIFY(readFile(fa).contains("FROM A"));
        QVERIFY(readFile(fb).contains("FROM B"));
    }

    void receive_cancelKeepsPartial()
    {
        QTemporaryDir dir;
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        MachineProfile p = profileFor(QStringLiteral("COM10"), dir.path());
        p.receiveIdleTimeoutSec = 0;
        MachineChannel ch(QStringLiteral("A"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QVERIFY(ch.startReceive(p));
        QTest::qWait(300);
        ctl.send("%\nO9\nG01 X1\n");
        QTest::qWait(800);
        ch.cancel();
        QVERIFY(done.wait(3000));
        const TransferResult r = resultOf(done);
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Cancelled);
        QVERIFY(r.filePath.contains(QLatin1String("_PARTIAL")));
        QCOMPARE(readFile(r.filePath), QByteArray("%\nO9\nG01 X1\n"));
    }

    // ---------------------------------------------------------------- both machines

    void parallel_sendAndReceiveDoNotInterfere()
    {
        QTemporaryDir dir;
        Control a(QStringLiteral("COM11"));
        Control b(QStringLiteral("COM13"));
        QVERIFY(a.ok() && b.ok());
        const QByteArray program = largeProgram(80);
        const QString file = dir.filePath(QStringLiteral("big.nc"));
        QVERIFY(writeFile(file, program));
        const MachineProfile pa = profileFor(QStringLiteral("COM10"), dir.path());
        const MachineProfile pb = profileFor(QStringLiteral("COM12"), dir.path());
        MachineChannel ca(QStringLiteral("A")), cb(QStringLiteral("B"));
        QSignalSpy da(&ca, &MachineChannel::finished), db(&cb, &MachineChannel::finished);
        QVERIFY(ca.startSend(pa, SendRequest{file}));
        QVERIFY(cb.startReceive(pb));
        QTest::qWait(300);
        // Machine A's control pauses it for 2 s; B's punch must not care.
        a.send(QByteArray(1, '\x13'));
        b.send("%\nO77\nM30\n%\n");
        QVERIFY(db.wait(8000));
        QCOMPARE_ENUM(resultOf(db).outcome, TransferOutcome::Success);
        QVERIFY2(ca.isBusy(), "A should still be paused while B finished");
        a.send(QByteArray(1, '\x11'));
        QVERIFY(da.wait(20000));
        QCOMPARE_ENUM(resultOf(da).outcome, TransferOutcome::Success);
        QTest::qWait(200);
        QCOMPARE(a.data(), ProgramFormatter::format(program, pa));
    }

    // ---------------------------------------------------------------- log

    void log_lockedFileKeepsEntriesUntilFree()
    {
#ifdef Q_OS_WIN
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("log.csv"));
        QVERIFY(writeFile(path, ""));
        TransferLog log(path);
        // Open with no sharing, the way Excel locks a CSV.
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GENERIC_READ, 0,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        QSignalSpy shown(&log, &TransferLog::entryAppended);
        TransferLogEntry e;
        e.timestamp = QDateTime::currentDateTime();
        e.machine = QStringLiteral("Machine A");
        e.result = QStringLiteral("Success");
        QString err;
        QVERIFY(!log.append(e, &err));
        QVERIFY(err.contains(QLatin1String("Excel")));
        QCOMPARE(shown.count(), 1); // still shown in the UI
        QCOMPARE(log.pendingCount(), 1);
        QVERIFY(!log.flushPending());
        CloseHandle(h);
        QVERIFY(log.flushPending());
        QCOMPARE(log.pendingCount(), 0);
        QCOMPARE(log.readRecent(5).size(), 1);
#else
        QSKIP("Windows only");
#endif
    }
};

QTEST_GUILESS_MAIN(TestTransfer)
#include "tst_transfer.moc"
