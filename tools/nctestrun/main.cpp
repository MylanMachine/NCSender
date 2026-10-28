// nctestrun - automated NCSender tests over com0com virtual serial port pairs.
//
// Needs two com0com pairs (see README "Testing without machines"):
//     COM10 <-> COM11   (app side = COM10, simulated control = COM11)
//     COM12 <-> COM13   (app side = COM12, simulated control = COM13)
// with baud-rate emulation on (EmuBR=yes) so data moves at real RS232 speed.
//
// nccli.exe drives the same transfer engine the GUI uses; nctestpeer.exe
// plays the CNC control on the other end of each pair. This program only
// starts those two tools, waits for them and checks what they report.
//
//   nctestrun                   run all tests
//   nctestrun --only T06,T10    run selected tests
//   nctestrun --list            list the tests
//
// Exit code: 0 = all passed, 1 = a test failed, 2 = setup problem.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QTextStream>
#include <QThread>

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace {

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}

// ------------------------------------------------------------------ sample programs

const char* const kSampleProgram =
    "%\r\n"
    "O1001 (NCSENDER SAMPLE - FACE AND DRILL)\r\n"
    "(TEST FILE ONLY - NOT FOR A REAL MACHINE)\r\n"
    "G21 G17 G40 G49 G80 G90\r\n"
    "T1 M06 (50MM FACE MILL)\r\n"
    "G54 G00 X-30. Y0. S1200 M03\r\n"
    "G43 H01 Z25. M08\r\n"
    "Z2.\r\n"
    "G01 Z0. F300.\r\n"
    "X130. F800.\r\n"
    "G00 Z25.\r\n"
    "T2 M06 (6.8 DRILL)\r\n"
    "G54 G00 X20. Y20. S1500 M03\r\n"
    "G43 H02 Z25. M08\r\n"
    "G81 G98 X20. Y20. Z-15. R2. F200.\r\n"
    "X60.\r\n"
    "X100.\r\n"
    "G80\r\n"
    "G00 Z100. M09\r\n"
    "G91 G28 Z0. M05\r\n"
    "G90\r\n"
    "M30\r\n"
    "%\r\n";

const char* const kNoPercentProgram =
    "O2002 (NCSENDER SAMPLE - NO PERCENT SIGNS IN FILE)\r\n"
    "G18 G21 G40 G99\r\n"
    "T0101\r\n"
    "G50 S2500\r\n"
    "G96 S180 M03\r\n"
    "G00 X52. Z2. M08\r\n"
    "G01 Z0. F0.2\r\n"
    "X-1.6 F0.12\r\n"
    "G00 X100. Z100. M09\r\n"
    "M30\r\n";

QByteArray makeLargeProgram(int number, int lines)
{
    QByteArray s;
    s += "%\r\n";
    s += QStringLiteral("O%1 (NCSENDER TEST PROGRAM %1)\r\n").arg(number, 4, 10, QLatin1Char('0')).toLatin1();
    s += "G21 G17 G40 G49 G80 G90\r\n";
    for (int i = 1; i <= lines; ++i) {
        const double x = std::fmod(i * 1.37 + number, 250.0);
        const double y = std::fmod(i * 0.73 * number, 180.0);
        s += QStringLiteral("N%1 G01 X%2 Y%3 F%4\r\n")
                 .arg(i * 10)
                 .arg(x, 0, 'f', 3)
                 .arg(y, 0, 'f', 3)
                 .arg(400 + number)
                 .toLatin1();
    }
    s += "M30\r\n%\r\n";
    return s;
}

// Same rule as nctestpeer: ignore NUL/DC codes, DEL and CR, trim the ends.
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

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

// ------------------------------------------------------------------ runner

struct Config
{
    QString binDir;
    QString outDir;
    QString appA = QStringLiteral("COM10");
    QString peerA = QStringLiteral("COM11");
    QString appB = QStringLiteral("COM12");
    QString peerB = QStringLiteral("COM13");
    int baud = 9600;
    int largeLines = 300;
};

struct Outcome
{
    bool ok = false;
    QString note;
};

using CliResult = QHash<QString, QString>;

class Runner
{
public:
    explicit Runner(const Config& c)
        : m_c(c)
        , m_nccli(QDir(c.binDir).filePath(QStringLiteral("nccli.exe")))
        , m_peer(QDir(c.binDir).filePath(QStringLiteral("nctestpeer.exe")))
    {
    }

    QString nccli() const { return m_nccli; }
    QString peerExe() const { return m_peer; }
    const Config& cfg() const { return m_c; }
    QString path(const QString& name) const { return QDir(m_c.outDir).filePath(name); }

    QString smallProgram, noPercentProgram, largeA, largeB, profiles;

    QStringList serialArgs() const
    {
        return {QStringLiteral("--baud"), QString::number(m_c.baud), QStringLiteral("--databits"), QStringLiteral("7"),
                QStringLiteral("--parity"), QStringLiteral("even"), QStringLiteral("--stopbits"), QStringLiteral("2")};
    }

    std::unique_ptr<QProcess> start(const QString& exe, const QStringList& args, const QString& name)
    {
        auto p = std::make_unique<QProcess>();
        p->setProgram(exe);
        p->setArguments(args);
        p->setStandardOutputFile(path(name + QStringLiteral(".out.txt")));
        p->setStandardErrorFile(path(name + QStringLiteral(".err.txt")));
        QFile cmds(path(QStringLiteral("commands.txt")));
        if (cmds.open(QIODevice::Append)) {
            QStringList quoted;
            for (const QString& a : args)
                quoted << (a.contains(QLatin1Char(' ')) ? QLatin1Char('"') + a + QLatin1Char('"') : a);
            cmds.write((name + QStringLiteral("> ") + QFileInfo(exe).fileName() + QLatin1Char(' ')
                        + quoted.join(QLatin1Char(' ')) + QStringLiteral("\r\n"))
                           .toLocal8Bit());
        }
        p->start();
        p->waitForStarted(10000);
        return p;
    }

    // Exit code, or -999 if it had to be killed, -998 if it crashed.
    static int wait(std::unique_ptr<QProcess>& p, int timeoutSec)
    {
        if (!p)
            return -997;
        int code;
        if (!p->waitForFinished(timeoutSec * 1000)) {
            p->kill();
            p->waitForFinished(3000);
            code = -999;
        } else {
            code = p->exitStatus() == QProcess::NormalExit ? p->exitCode() : -998;
        }
        p.reset();
        return code;
    }

    std::unique_ptr<QProcess> startPeer(const QString& name, QStringList args)
    {
        auto p = start(m_peer, args + serialArgs(), name);
        QThread::msleep(700); // let it open its port before the app starts sending
        return p;
    }

    std::unique_ptr<QProcess> startPunch(const QString& name, QStringList args)
    {
        return start(m_peer, QStringList{QStringLiteral("punch")} + args + serialArgs(), name);
    }

    std::unique_ptr<QProcess> startCli(const QString& name, const QStringList& args)
    {
        return start(m_nccli, QStringList{QStringLiteral("--profiles"), profiles} + args, name);
    }

    int runCli(const QString& name, const QStringList& args, int timeoutSec = 180)
    {
        auto p = startCli(name, args);
        return wait(p, timeoutSec);
    }

    QList<CliResult> cliResults(const QString& name) const
    {
        QList<CliResult> list;
        static const QRegularExpression field(QStringLiteral("(\\w+)=(\"([^\"]*)\"|\\S+)"));
        const QList<QByteArray> lines = readFile(path(name + QStringLiteral(".out.txt"))).split('\n');
        for (const QByteArray& raw : lines) {
            const QString line = QString::fromLocal8Bit(raw).trimmed();
            if (!line.startsWith(QLatin1String("RESULT ")))
                continue;
            CliResult r;
            auto it = field.globalMatch(line);
            while (it.hasNext()) {
                const auto m = it.next();
                r.insert(m.captured(1), m.capturedStart(3) >= 0 ? m.captured(3) : m.captured(2));
            }
            list.append(r);
        }
        return list;
    }

    CliResult cliResultFor(const QString& name, const QString& profile) const
    {
        for (const CliResult& r : cliResults(name)) {
            if (r.value(QStringLiteral("profile")) == profile)
                return r;
        }
        return {};
    }

    QString firstMessage(const QString& name) const
    {
        const auto r = cliResults(name);
        return r.isEmpty() ? QStringLiteral("(no RESULT line)") : r.first().value(QStringLiteral("message"));
    }

    QString peerSummary(const QString& name) const
    {
        QStringList keep;
        const QList<QByteArray> lines = readFile(path(name + QStringLiteral(".out.txt"))).split('\n');
        for (const QByteArray& raw : lines) {
            const QString line = QString::fromLocal8Bit(raw).trimmed();
            if (line.contains(QLatin1String("PEER RESULT")) || line.contains(QLatin1String("FAIL:")))
                keep << line;
        }
        return keep.join(QStringLiteral(" | "));
    }

    static QString newestFile(const QString& dir)
    {
        const QFileInfoList files = QDir(dir).entryInfoList(QDir::Files, QDir::Time);
        return files.isEmpty() ? QString() : files.first().absoluteFilePath();
    }

    static bool sameProgram(const QString& a, const QString& b)
    {
        return !a.isEmpty() && !b.isEmpty() && normalize(readFile(a)) == normalize(readFile(b));
    }

private:
    Config m_c;
    QString m_nccli;
    QString m_peer;
};

QString code(int c)
{
    return QString::number(c);
}

struct TestCase
{
    QString id;
    QString title;
    std::function<Outcome(Runner&)> run;
};

std::vector<TestCase> buildTests(const Config& c)
{
    std::vector<TestCase> t;
    const QString A = QStringLiteral("Virtual Test A");
    const QString B = QStringLiteral("Virtual Test B");
    auto send = [](const QString& profile, const QString& file) { return QStringList{QStringLiteral("--send"), profile + QLatin1Char('=') + file}; };
    auto set = [](const QString& kv) { return QStringList{QStringLiteral("--set"), kv}; };
    auto expectErr = [](const QString& text) { return QStringList{QStringLiteral("--expect-error"), text}; };

    t.push_back({QStringLiteral("T01"), QStringLiteral("Plain send, machine A (%1 -> %2)").arg(c.appA, c.peerA), [=](Runner& r) {
        auto peer = r.startPeer(QStringLiteral("T01-peer"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerA, QStringLiteral("--expect"), r.smallProgram});
        const int cli = r.runCli(QStringLiteral("T01-cli"), send(A, r.smallProgram));
        const int pc = Runner::wait(peer, 30);
        return Outcome{cli == 0 && pc == 0, QStringLiteral("cli=%1 peer=%2  %3").arg(code(cli), code(pc), r.peerSummary(QStringLiteral("T01-peer")))};
    }});

    t.push_back({QStringLiteral("T02"), QStringLiteral("Plain send, machine B (%1 -> %2)").arg(c.appB, c.peerB), [=](Runner& r) {
        auto peer = r.startPeer(QStringLiteral("T02-peer"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerB, QStringLiteral("--expect"), r.smallProgram});
        const int cli = r.runCli(QStringLiteral("T02-cli"), send(B, r.smallProgram));
        const int pc = Runner::wait(peer, 30);
        return Outcome{cli == 0 && pc == 0, QStringLiteral("cli=%1 peer=%2  %3").arg(code(cli), code(pc), r.peerSummary(QStringLiteral("T02-peer")))};
    }});

    t.push_back({QStringLiteral("T03"), QStringLiteral("Missing % added automatically"), [=](Runner& r) {
        const QString expected = r.path(QStringLiteral("O2002_expected.nc"));
        writeFile(expected, QByteArray("%\r\n") + kNoPercentProgram + "%\r\n");
        auto peer = r.startPeer(QStringLiteral("T03-peer"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerA, QStringLiteral("--expect"), expected});
        const int cli = r.runCli(QStringLiteral("T03-cli"), send(A, r.noPercentProgram));
        const int pc = Runner::wait(peer, 30);
        return Outcome{cli == 0 && pc == 0, QStringLiteral("cli=%1 peer=%2  %3").arg(code(cli), code(pc), r.peerSummary(QStringLiteral("T03-peer")))};
    }});

    t.push_back({QStringLiteral("T04"), QStringLiteral("Wait for XON: nothing is sent until the control sends XON"), [=](Runner& r) {
        auto peer = r.startPeer(QStringLiteral("T04-peer"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerA, QStringLiteral("--xon-delay"), QStringLiteral("2500"), QStringLiteral("--expect"), r.smallProgram});
        const int cli = r.runCli(QStringLiteral("T04-cli"), send(A, r.smallProgram) + set(QStringLiteral("waitForXon=true")));
        const int pc = Runner::wait(peer, 30);
        return Outcome{cli == 0 && pc == 0, QStringLiteral("cli=%1 peer=%2  %3").arg(code(cli), code(pc), r.peerSummary(QStringLiteral("T04-peer")))};
    }});

    t.push_back({QStringLiteral("T05"), QStringLiteral("Timeout waiting for XON is reported as an error"), [=](Runner& r) {
        const int cli = r.runCli(QStringLiteral("T05-cli"), send(A, r.smallProgram) + set(QStringLiteral("waitForXon=true"))
                                                             + set(QStringLiteral("xonTimeoutSec=3")) + expectErr(QStringLiteral("XON")));
        return Outcome{cli == 0, QStringLiteral("cli=%1  message: %2").arg(code(cli), r.firstMessage(QStringLiteral("T05-cli")))};
    }});

    t.push_back({QStringLiteral("T06"), QStringLiteral("XON/XOFF flow control: control pauses the sender repeatedly"), [=](Runner& r) {
        auto peer = r.startPeer(QStringLiteral("T06-peer"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerA, QStringLiteral("--pause-every"), QStringLiteral("1500"),
                                                             QStringLiteral("--pause-ms"), QStringLiteral("1500"), QStringLiteral("--expect"), r.largeA});
        const int cli = r.runCli(QStringLiteral("T06-cli"), send(A, r.largeA));
        const int pc = Runner::wait(peer, 120);
        return Outcome{cli == 0 && pc == 0, QStringLiteral("cli=%1 peer=%2  %3").arg(code(cli), code(pc), r.peerSummary(QStringLiteral("T06-peer")))};
    }});

    t.push_back({QStringLiteral("T07"), QStringLiteral("RTS/CTS flow control: control drops CTS to pause"), [=](Runner& r) {
        auto peer = r.startPeer(QStringLiteral("T07-peer"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerA, QStringLiteral("--pause-every"), QStringLiteral("1500"),
                                                             QStringLiteral("--pause-ms"), QStringLiteral("1500"), QStringLiteral("--pause-mode"), QStringLiteral("rts"),
                                                             QStringLiteral("--expect"), r.largeA});
        const int cli = r.runCli(QStringLiteral("T07-cli"), send(A, r.largeA) + set(QStringLiteral("flowControl=rtscts")));
        const int pc = Runner::wait(peer, 120);
        return Outcome{cli == 0 && pc == 0, QStringLiteral("cli=%1 peer=%2  %3").arg(code(cli), code(pc), r.peerSummary(QStringLiteral("T07-peer")))};
    }});

    t.push_back({QStringLiteral("T08"), QStringLiteral("Receive a punched program (leader, DC2/DC4, closing %)"), [=](Runner& r) {
        const QString rxDir = r.path(QStringLiteral("received_T08"));
        auto cliP = r.startCli(QStringLiteral("T08-cli"), {QStringLiteral("--receive"), A, QStringLiteral("--set"), QStringLiteral("receiveFolder=") + rxDir});
        QThread::msleep(800);
        auto peer = r.startPunch(QStringLiteral("T08-peer"), {QStringLiteral("--port"), c.peerA, QStringLiteral("--file"), r.smallProgram, QStringLiteral("--leader"),
                                                              QStringLiteral("20"), QStringLiteral("--dc2"), QStringLiteral("--start-delay"), QStringLiteral("500")});
        const int pc = Runner::wait(peer, 60);
        const int cli = Runner::wait(cliP, 60);
        const QString saved = Runner::newestFile(rxDir);
        const bool same = Runner::sameProgram(saved, r.smallProgram);
        return Outcome{cli == 0 && pc == 0 && same, QStringLiteral("cli=%1 peer=%2 saved=%3 contentMatches=%4")
                                                        .arg(code(cli), code(pc), QFileInfo(saved).fileName(), same ? QStringLiteral("yes") : QStringLiteral("no"))};
    }});

    t.push_back({QStringLiteral("T09"), QStringLiteral("Receive cut off before the closing %: flagged incomplete, data kept"), [=](Runner& r) {
        const QString rxDir = r.path(QStringLiteral("received_T09"));
        auto cliP = r.startCli(QStringLiteral("T09-cli"), {QStringLiteral("--receive"), A, QStringLiteral("--set"), QStringLiteral("receiveFolder=") + rxDir,
                                                           QStringLiteral("--set"), QStringLiteral("receiveIdleTimeoutSec=3"),
                                                           QStringLiteral("--expect-error"), QStringLiteral("No closing")});
        QThread::msleep(800);
        auto peer = r.startPunch(QStringLiteral("T09-peer"), {QStringLiteral("--port"), c.peerA, QStringLiteral("--file"), r.smallProgram, QStringLiteral("--truncate"),
                                                              QStringLiteral("200"), QStringLiteral("--start-delay"), QStringLiteral("500")});
        const int pc = Runner::wait(peer, 60);
        const int cli = Runner::wait(cliP, 60);
        const QString msg = r.firstMessage(QStringLiteral("T09-cli"));
        const QString saved = Runner::newestFile(rxDir);
        const bool ok = cli == 0 && pc == 0 && saved.contains(QLatin1String("_PARTIAL")) && msg.contains(QLatin1String("incomplete"));
        return Outcome{ok, QStringLiteral("cli=%1 peer=%2 saved=%3 message: %4").arg(code(cli), code(pc), QFileInfo(saved).fileName(), msg)};
    }});

    t.push_back({QStringLiteral("T10"), QStringLiteral("Two sends at the same time on A and B"), [=](Runner& r) {
        auto peerA = r.startPeer(QStringLiteral("T10-peerA"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerA, QStringLiteral("--expect"), r.largeA});
        auto peerB = r.startPeer(QStringLiteral("T10-peerB"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerB, QStringLiteral("--expect"), r.largeB});
        const int cli = r.runCli(QStringLiteral("T10-cli"), send(A, r.largeA) + send(B, r.largeB));
        const int pa = Runner::wait(peerA, 60);
        const int pb = Runner::wait(peerB, 60);
        const CliResult ra = r.cliResultFor(QStringLiteral("T10-cli"), A);
        const CliResult rb = r.cliResultFor(QStringLiteral("T10-cli"), B);
        QString note = QStringLiteral("cli=%1 peerA=%2 peerB=%3").arg(code(cli), code(pa), code(pb));
        bool overlapOk = false;
        if (!ra.isEmpty() && !rb.isEmpty()) {
            const qint64 sA = ra.value(QStringLiteral("startMs")).toLongLong(), eA = ra.value(QStringLiteral("endMs")).toLongLong();
            const qint64 sB = rb.value(QStringLiteral("startMs")).toLongLong(), eB = rb.value(QStringLiteral("endMs")).toLongLong();
            const qint64 overlap = qMin(eA, eB) - qMax(sA, sB);
            const qint64 shorter = qMin(eA - sA, eB - sB);
            note += QStringLiteral(" durations=%1ms/%2ms overlap=%3ms").arg(eA - sA).arg(eB - sB).arg(overlap);
            if (shorter < 1000)
                note += QStringLiteral(" (too fast to judge overlap - is EmuBR on?)");
            overlapOk = shorter >= 1000 && overlap >= shorter * 8 / 10;
        }
        return Outcome{cli == 0 && pa == 0 && pb == 0 && overlapOk, note};
    }});

    t.push_back({QStringLiteral("T11"), QStringLiteral("Send on A while receiving on B at the same time"), [=](Runner& r) {
        const QString rxDir = r.path(QStringLiteral("received_T11"));
        auto peerA = r.startPeer(QStringLiteral("T11-peerA"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerA, QStringLiteral("--expect"), r.largeA});
        auto cliP = r.startCli(QStringLiteral("T11-cli"), send(A, r.largeA) + QStringList{QStringLiteral("--receive"), B} + set(QStringLiteral("receiveFolder=") + rxDir));
        auto peerB = r.startPunch(QStringLiteral("T11-peerB"), {QStringLiteral("--port"), c.peerB, QStringLiteral("--file"), r.largeB, QStringLiteral("--start-delay"), QStringLiteral("1500")});
        const int pa = Runner::wait(peerA, 90);
        const int pb = Runner::wait(peerB, 90);
        const int cli = Runner::wait(cliP, 90);
        const bool same = Runner::sameProgram(Runner::newestFile(rxDir), r.largeB);
        return Outcome{cli == 0 && pa == 0 && pb == 0 && same, QStringLiteral("cli=%1 peerA=%2 peerB=%3 receivedMatches=%4")
                                                                   .arg(code(cli), code(pa), code(pb), same ? QStringLiteral("yes") : QStringLiteral("no"))};
    }});

    t.push_back({QStringLiteral("T12"), QStringLiteral("Two XON/XOFF-paced sends at once: pausing A never stalls B"), [=](Runner& r) {
        // A is paused by its control for long stretches; B must keep running
        // at full speed and finish well before A.
        auto peerA = r.startPeer(QStringLiteral("T12-peerA"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerA, QStringLiteral("--pause-every"), QStringLiteral("800"),
                                                               QStringLiteral("--pause-ms"), QStringLiteral("3000"), QStringLiteral("--expect"), r.largeA});
        auto peerB = r.startPeer(QStringLiteral("T12-peerB"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerB, QStringLiteral("--expect"), r.largeB});
        const int cli = r.runCli(QStringLiteral("T12-cli"), send(A, r.largeA) + send(B, r.largeB));
        const int pa = Runner::wait(peerA, 180);
        const int pb = Runner::wait(peerB, 180);
        const CliResult ra = r.cliResultFor(QStringLiteral("T12-cli"), A);
        const CliResult rb = r.cliResultFor(QStringLiteral("T12-cli"), B);
        QString note = QStringLiteral("cli=%1 peerA=%2 peerB=%3").arg(code(cli), code(pa), code(pb));
        bool independent = false;
        if (!ra.isEmpty() && !rb.isEmpty()) {
            const qint64 dA = ra.value(QStringLiteral("durationMs")).toLongLong();
            const qint64 dB = rb.value(QStringLiteral("durationMs")).toLongLong();
            note += QStringLiteral(" durA=%1ms durB=%2ms").arg(dA).arg(dB);
            independent = dB * 10 < dA * 7;
        }
        return Outcome{cli == 0 && pa == 0 && pb == 0 && independent, note};
    }});

    t.push_back({QStringLiteral("T13"), QStringLiteral("Port already in use is reported clearly"), [=](Runner& r) {
        // Hold port A open with a receive, then try to send on it from a second process.
        auto holder = r.startCli(QStringLiteral("T13-holder"), {QStringLiteral("--receive"), A, QStringLiteral("--set"), QStringLiteral("receiveStartTimeoutSec=8"),
                                                                QStringLiteral("--set"), QStringLiteral("receiveFolder=") + r.path(QStringLiteral("received_T13"))});
        QThread::msleep(1200);
        const int cli = r.runCli(QStringLiteral("T13-cli"), send(A, r.smallProgram) + expectErr(QStringLiteral("in use")));
        Runner::wait(holder, 20);
        return Outcome{cli == 0, QStringLiteral("cli=%1  message: %2").arg(code(cli), r.firstMessage(QStringLiteral("T13-cli")))};
    }});

    t.push_back({QStringLiteral("T14"), QStringLiteral("File not found is reported clearly"), [=](Runner& r) {
        const int cli = r.runCli(QStringLiteral("T14-cli"), send(A, r.path(QStringLiteral("does_not_exist.nc"))) + expectErr(QStringLiteral("not found")));
        return Outcome{cli == 0, QStringLiteral("cli=%1  message: %2").arg(code(cli), r.firstMessage(QStringLiteral("T14-cli")))};
    }});

    t.push_back({QStringLiteral("T15"), QStringLiteral("Cancel mid-transfer releases the port"), [=](Runner& r) {
        auto peer = r.startPeer(QStringLiteral("T15-peer"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerA, QStringLiteral("--idle-timeout"), QStringLiteral("3000")});
        r.runCli(QStringLiteral("T15-cli"), send(A, r.largeA) + QStringList{QStringLiteral("--timeout"), QStringLiteral("3")}, 60);
        Runner::wait(peer, 30);
        const auto first = r.cliResults(QStringLiteral("T15-cli"));
        // The port must be free again: a quick send must now work.
        auto peer2 = r.startPeer(QStringLiteral("T15-peer2"), {QStringLiteral("receive"), QStringLiteral("--port"), c.peerA, QStringLiteral("--expect"), r.smallProgram});
        const int cli2 = r.runCli(QStringLiteral("T15-cli2"), send(A, r.smallProgram));
        const int p2 = Runner::wait(peer2, 30);
        const QString outcome = first.isEmpty() ? QStringLiteral("(none)") : first.first().value(QStringLiteral("outcome"));
        const QString bytes = first.isEmpty() ? QString() : first.first().value(QStringLiteral("bytes")) + QLatin1Char('/') + first.first().value(QStringLiteral("total"));
        return Outcome{outcome == QLatin1String("Cancelled") && cli2 == 0 && p2 == 0,
                       QStringLiteral("first outcome=%1 bytes=%2; resend cli=%3 peer=%4").arg(outcome, bytes, code(cli2), code(p2))};
    }});

    return t;
}

void warnIfNoBaudEmulation(const Config& c)
{
    const QStringList candidates{QStringLiteral("C:/Program Files (x86)/com0com/setupc.exe"), QStringLiteral("C:/Program Files/com0com/setupc.exe")};
    for (const QString& setupc : candidates) {
        if (!QFileInfo::exists(setupc))
            continue;
        QProcess p;
        p.setWorkingDirectory(QFileInfo(setupc).absolutePath());
        p.start(setupc, {QStringLiteral("list")});
        if (!p.waitForFinished(15000))
            return;
        const QStringList lines = QString::fromLocal8Bit(p.readAllStandardOutput()).split(QLatin1Char('\n'));
        for (const QString& port : {c.appA, c.peerA, c.appB, c.peerB}) {
            const QRegularExpression re(QStringLiteral("PortName=%1(,|\\s|$)").arg(QRegularExpression::escape(port)), QRegularExpression::CaseInsensitiveOption);
            for (const QString& line : lines) {
                if (re.match(line).hasMatch() && !line.contains(QLatin1String("EmuBR=yes"), Qt::CaseInsensitive))
                    out() << "WARNING: " << port << " does not have EmuBR=yes. Flow-control and overlap tests will fail." << Qt::endl;
            }
        }
        return;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nctestrun"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Automated NCSender tests over com0com virtual serial port pairs."));
    parser.addHelpOption();
    const QCommandLineOption binO(QStringLiteral("bin-dir"), QStringLiteral("Folder with nccli.exe and nctestpeer.exe (default: this exe's folder)."), QStringLiteral("dir"));
    const QCommandLineOption outO(QStringLiteral("out-dir"), QStringLiteral("Where logs and received files go (default: <bin-dir>\\testrun-out)."), QStringLiteral("dir"));
    const QCommandLineOption appAO(QStringLiteral("app-port-a"), QStringLiteral("App side of pair A (default COM10)."), QStringLiteral("port"), QStringLiteral("COM10"));
    const QCommandLineOption peerAO(QStringLiteral("peer-port-a"), QStringLiteral("Control side of pair A (default COM11)."), QStringLiteral("port"), QStringLiteral("COM11"));
    const QCommandLineOption appBO(QStringLiteral("app-port-b"), QStringLiteral("App side of pair B (default COM12)."), QStringLiteral("port"), QStringLiteral("COM12"));
    const QCommandLineOption peerBO(QStringLiteral("peer-port-b"), QStringLiteral("Control side of pair B (default COM13)."), QStringLiteral("port"), QStringLiteral("COM13"));
    const QCommandLineOption baudO(QStringLiteral("baud"), QStringLiteral("Baud rate for all tests (default 9600)."), QStringLiteral("n"), QStringLiteral("9600"));
    const QCommandLineOption linesO(QStringLiteral("large-lines"), QStringLiteral("Lines in the large test programs (default 300)."), QStringLiteral("n"), QStringLiteral("300"));
    const QCommandLineOption onlyO(QStringLiteral("only"), QStringLiteral("Comma-separated test IDs to run, e.g. T06,T10."), QStringLiteral("ids"));
    const QCommandLineOption listO(QStringLiteral("list"), QStringLiteral("List the tests and exit."));
    parser.addOptions({binO, outO, appAO, peerAO, appBO, peerBO, baudO, linesO, onlyO, listO});
    parser.process(app);

    Config c;
    c.binDir = parser.isSet(binO) ? QDir(parser.value(binO)).absolutePath() : QCoreApplication::applicationDirPath();
    c.outDir = parser.isSet(outO) ? QDir(parser.value(outO)).absolutePath() : QDir(c.binDir).filePath(QStringLiteral("testrun-out"));
    c.appA = parser.value(appAO).toUpper();
    c.peerA = parser.value(peerAO).toUpper();
    c.appB = parser.value(appBO).toUpper();
    c.peerB = parser.value(peerBO).toUpper();
    c.baud = parser.value(baudO).toInt();
    c.largeLines = qMax(10, parser.value(linesO).toInt());

    std::vector<TestCase> tests = buildTests(c);
    if (parser.isSet(listO)) {
        for (const TestCase& t : tests)
            out() << t.id << "  " << t.title << Qt::endl;
        return 0;
    }
    QStringList only;
    for (const QString& id : parser.value(onlyO).split(QLatin1Char(','), Qt::SkipEmptyParts))
        only << id.trimmed().toUpper();

    Runner r(c);
    if (!QFileInfo::exists(r.nccli()) || !QFileInfo::exists(r.peerExe())) {
        out() << "Cannot find nccli.exe / nctestpeer.exe in " << QDir::toNativeSeparators(c.binDir)
              << ". Build the project first or pass --bin-dir." << Qt::endl;
        return 2;
    }

    QStringList present;
    for (const QSerialPortInfo& p : QSerialPortInfo::availablePorts())
        present << p.portName().toUpper();
    QStringList missing;
    for (const QString& p : {c.appA, c.peerA, c.appB, c.peerB}) {
        if (!present.contains(p))
            missing << p;
    }
    if (!missing.isEmpty()) {
        out() << "These ports do not exist: " << missing.join(QStringLiteral(", ")) << Qt::endl
              << "Create the com0com pairs first (README, 'Testing without machines')." << Qt::endl;
        return 2;
    }
    warnIfNoBaudEmulation(c);

    QDir(c.outDir).removeRecursively();
    if (!QDir().mkpath(c.outDir)) {
        out() << "Cannot create " << QDir::toNativeSeparators(c.outDir) << Qt::endl;
        return 2;
    }

    r.smallProgram = r.path(QStringLiteral("O1001.nc"));
    r.noPercentProgram = r.path(QStringLiteral("O2002_no_percent.nc"));
    r.largeA = r.path(QStringLiteral("O3001_large_A.nc"));
    r.largeB = r.path(QStringLiteral("O3002_large_B.nc"));
    writeFile(r.smallProgram, kSampleProgram);
    writeFile(r.noPercentProgram, kNoPercentProgram);
    writeFile(r.largeA, makeLargeProgram(3001, c.largeLines));
    writeFile(r.largeB, makeLargeProgram(3002, c.largeLines));

    auto profile = [&](const QString& name, const QString& port) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), name);
        o.insert(QStringLiteral("port"), port);
        o.insert(QStringLiteral("baudRate"), c.baud);
        o.insert(QStringLiteral("dataBits"), 7);
        o.insert(QStringLiteral("parity"), QStringLiteral("even"));
        o.insert(QStringLiteral("stopBits"), QStringLiteral("2"));
        o.insert(QStringLiteral("flowControl"), QStringLiteral("xonxoff"));
        o.insert(QStringLiteral("lineEnding"), QStringLiteral("CRLF"));
        o.insert(QStringLiteral("leaderChar"), 0);
        o.insert(QStringLiteral("leaderCount"), 10);
        o.insert(QStringLiteral("trailerChar"), 0);
        o.insert(QStringLiteral("trailerCount"), 10);
        o.insert(QStringLiteral("ensurePercent"), true);
        o.insert(QStringLiteral("charDelayMs"), 0);
        o.insert(QStringLiteral("waitForXon"), false);
        o.insert(QStringLiteral("xonTimeoutSec"), 30);
        o.insert(QStringLiteral("receiveFolder"), r.path(QStringLiteral("received")));
        o.insert(QStringLiteral("receiveExtension"), QStringLiteral("nc"));
        o.insert(QStringLiteral("receiveStartTimeoutSec"), 30);
        o.insert(QStringLiteral("receiveIdleTimeoutSec"), 5);
        o.insert(QStringLiteral("stripReceivedControlChars"), true);
        return o;
    };
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("profiles"), QJsonArray{profile(QStringLiteral("Virtual Test A"), c.appA), profile(QStringLiteral("Virtual Test B"), c.appB)});
    r.profiles = r.path(QStringLiteral("test-profiles.json"));
    writeFile(r.profiles, QJsonDocument(root).toJson());

    struct Row
    {
        QString id, title;
        bool ok;
        double seconds;
    };
    QList<Row> rows;
    for (const TestCase& t : tests) {
        if (!only.isEmpty() && !only.contains(t.id))
            continue;
        out() << Qt::endl << "=== " << t.id << "  " << t.title << Qt::endl;
        QElapsedTimer timer;
        timer.start();
        const Outcome o = t.run(r);
        const double secs = timer.elapsed() / 1000.0;
        out() << "    " << (o.ok ? "PASS  " : "FAIL  ") << o.note << Qt::endl;
        rows.append({t.id, t.title, o.ok, secs});
        QThread::msleep(500); // let ports close fully between tests
    }

    int failed = 0;
    out() << Qt::endl << "================ SUMMARY ================" << Qt::endl;
    for (const Row& row : rows) {
        out() << row.id << "  " << (row.ok ? "PASS" : "FAIL") << "  " << QString::number(row.seconds, 'f', 1).rightJustified(6)
              << " s  " << row.title << Qt::endl;
        if (!row.ok)
            ++failed;
    }
    out() << Qt::endl << "Logs and received files: " << QDir::toNativeSeparators(c.outDir) << Qt::endl;
    if (failed > 0) {
        out() << failed << " test(s) FAILED" << Qt::endl;
        return 1;
    }
    out() << "All " << rows.size() << " tests passed" << Qt::endl;
    return 0;
}
