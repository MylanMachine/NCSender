// Unit tests for nccore: everything that can be checked without a second
// serial device on the other end of the cable.

#include "DataSource.h"
#include "MachineChannel.h"
#include "MachineProfile.h"
#include "ProfileStore.h"
#include "ProgramFormatter.h"
#include "SerialSettings.h"
#include "TransferLog.h"
#include "TransferTypes.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace nc;

#define QCOMPARE_ENUM(a, b) QCOMPARE(static_cast<int>(a), static_cast<int>(b))

namespace {

MachineProfile plainProfile()
{
    MachineProfile p;
    p.name = QStringLiteral("Test");
    p.lineEnding = LineEnding::LF;
    p.ensurePercent = false;
    p.leaderCount = 0;
    p.trailerCount = 0;
    return p;
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

} // namespace

class TestCore : public QObject
{
    Q_OBJECT

private slots:
    // ---------------------------------------------------------------- SerialSettings
    void settings_roundTrip()
    {
        for (Parity v : allParities())
            QCOMPARE_ENUM(parityFromString(toString(v)), v);
        for (StopBits v : allStopBits())
            QCOMPARE_ENUM(stopBitsFromString(toString(v)), v);
        for (FlowControl v : allFlowControls())
            QCOMPARE_ENUM(flowControlFromString(toString(v)), v);
        for (LineEnding v : allLineEndings())
            QCOMPARE_ENUM(lineEndingFromString(toString(v)), v);
    }

    void settings_aliasesAndFallbacks()
    {
        QCOMPARE_ENUM(flowControlFromString(QStringLiteral("XON/XOFF")), FlowControl::XonXoff);
        QCOMPARE_ENUM(flowControlFromString(QStringLiteral("hardware")), FlowControl::RtsCts);
        QCOMPARE_ENUM(flowControlFromString(QStringLiteral("RTS/CTS")), FlowControl::RtsCts);
        QCOMPARE_ENUM(lineEndingFromString(QStringLiteral("cr lf")), LineEnding::CRLF);
        QCOMPARE_ENUM(parityFromString(QStringLiteral("E")), Parity::Even);
        QCOMPARE_ENUM(parityFromString(QStringLiteral("garbage"), Parity::Odd), Parity::Odd);
        QCOMPARE_ENUM(stopBitsFromString(QStringLiteral("3"), StopBits::One), StopBits::One);
    }

    void settings_bitsPerCharacter()
    {
        QCOMPARE(bitsPerCharacter(7, Parity::Even, StopBits::Two), 11.0);
        QCOMPARE(bitsPerCharacter(8, Parity::None, StopBits::One), 10.0);
        QCOMPARE(bitsPerCharacter(5, Parity::None, StopBits::OneAndHalf), 7.5);
    }

    // ---------------------------------------------------------------- ProgramFormatter
    void split_lineEndings()
    {
        QCOMPARE(ProgramFormatter::splitLines("a\r\nb\nc\rd"), (QList<QByteArray>{"a", "b", "c", "d"}));
        QCOMPARE(ProgramFormatter::splitLines("a\nb\n"), (QList<QByteArray>{"a", "b"}));
        QCOMPARE(ProgramFormatter::splitLines("a\n\nb"), (QList<QByteArray>{"a", "", "b"}));
        QCOMPARE(ProgramFormatter::splitLines(""), QList<QByteArray>{});
        QCOMPARE(ProgramFormatter::splitLines("\r\n"), (QList<QByteArray>{""}));
    }

    void format_convertsLineEndings()
    {
        MachineProfile p = plainProfile();
        p.lineEnding = LineEnding::CRLF;
        QCOMPARE(ProgramFormatter::format("A\nB\rC\r\n", p), QByteArray("A\r\nB\r\nC\r\n"));
        p.lineEnding = LineEnding::CR;
        QCOMPARE(ProgramFormatter::format("A\r\nB", p), QByteArray("A\rB\r"));
        p.lineEnding = LineEnding::LF;
        QCOMPARE(ProgramFormatter::format("A\r\nB", p), QByteArray("A\nB\n"));
    }

    void format_addsPercentWhenMissing()
    {
        MachineProfile p = plainProfile();
        p.ensurePercent = true;
        QCOMPARE(ProgramFormatter::format("O1\nM30\n", p), QByteArray("%\nO1\nM30\n%\n"));
    }

    void format_keepsExistingPercent()
    {
        MachineProfile p = plainProfile();
        p.ensurePercent = true;
        QCOMPARE(ProgramFormatter::format("%\nO1\nM30\n%\n", p), QByteArray("%\nO1\nM30\n%\n"));
    }

    void format_addsOnlyMissingSide()
    {
        MachineProfile p = plainProfile();
        p.ensurePercent = true;
        QCOMPARE(ProgramFormatter::format("%\nO1\nM30\n", p), QByteArray("%\nO1\nM30\n%\n"));
        QCOMPARE(ProgramFormatter::format("O1\nM30\n%\n", p), QByteArray("%\nO1\nM30\n%\n"));
    }

    void format_blankLinesAroundProgram()
    {
        MachineProfile p = plainProfile();
        p.ensurePercent = true;
        // Opening % goes before the first content line, closing % after the last.
        QCOMPARE(ProgramFormatter::format("\n\nO1\n\n", p), QByteArray("\n\n%\nO1\n%\n\n"));
    }

    void format_lonePercentGetsClosing()
    {
        MachineProfile p = plainProfile();
        p.ensurePercent = true;
        QCOMPARE(ProgramFormatter::format("%\n", p), QByteArray("%\n%\n"));
    }

    void format_emptyAndWhitespaceOnly()
    {
        MachineProfile p = plainProfile();
        p.ensurePercent = true;
        QCOMPARE(ProgramFormatter::format("", p), QByteArray("%\n%\n"));
        QCOMPARE(ProgramFormatter::format("  \n\t\n", p), QByteArray("%\n%\n"));
    }

    void format_headerCommentAbovePercentIsLeftAlone()
    {
        // Regression: a % used to be inserted before the comment, so the
        // control saw "% (comment) %" and stopped before the program.
        MachineProfile p = plainProfile();
        p.ensurePercent = true;
        QCOMPARE(ProgramFormatter::format("(POSTED 1/2)\n%\nO1234\nM30\n%\n", p),
                 QByteArray("(POSTED 1/2)\n%\nO1234\nM30\n%\n"));
        QCOMPARE(ProgramFormatter::format("(POSTED)\n%\nO1234\nM30\n", p), QByteArray("(POSTED)\n%\nO1234\nM30\n%\n"));
    }

    void format_onlyClosingPercent()
    {
        MachineProfile p = plainProfile();
        p.ensurePercent = true;
        QCOMPARE(ProgramFormatter::format("(HDR)\nO1\nM30\n%\n", p), QByteArray("%\n(HDR)\nO1\nM30\n%\n"));
    }

    void format_trailingCommentAfterClosingPercent()
    {
        MachineProfile p = plainProfile();
        p.ensurePercent = true;
        QCOMPARE(ProgramFormatter::format("%\nO1\n%\n(END)\n", p), QByteArray("%\nO1\n%\n(END)\n"));
    }

    void format_crcrlfIsOneLineBreak()
    {
        MachineProfile p = plainProfile();
        QCOMPARE(ProgramFormatter::format("A\r\r\nB\r\r\n", p), QByteArray("A\nB\n"));
        QCOMPARE(ProgramFormatter::stats("A\r\r\nB\r\r\n").lines, 2);
    }

    void format_ctrlZFollowedByNewline()
    {
        MachineProfile p = plainProfile();
        QCOMPARE(ProgramFormatter::format("O1\r\n\x1A\r\n", p), QByteArray("O1\n\n"));
    }

    void content_detection()
    {
        QVERIFY(!ProgramFormatter::hasProgramContent(""));
        QVERIFY(!ProgramFormatter::hasProgramContent(" \r\n\t\r\n"));
        QVERIFY(!ProgramFormatter::hasProgramContent("\xEF\xBB\xBF\r\n\x1A"));
        QVERIFY(ProgramFormatter::hasProgramContent("\r\n%\r\n"));
    }

    void source_rejectsBlankFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("blank.nc"));
        QVERIFY(writeFile(path, "  \r\n\r\n\x1A"));
        QString err;
        QVERIFY(!createDataSource(SendRequest{path}, plainProfile(), &err));
        QVERIFY2(err.contains(QLatin1String("no program")), qPrintable(err));
    }

    void profile_timeoutUpperLimit()
    {
        MachineProfile p;
        p.receiveIdleTimeoutSec = 86400;
        QVERIFY(p.validate().isEmpty());
        p.receiveIdleTimeoutSec = 86401;
        QVERIFY(!p.validate().isEmpty());
        p.receiveIdleTimeoutSec = 10;
        p.xonTimeoutSec = 3000000; // would overflow sec*1000 in a timer
        QVERIFY(!p.validate().isEmpty());
    }

    void store_missingProfilesListIsAnError()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("profiles.json"));
        QVERIFY(writeFile(path, "{\"version\":1}"));
        ProfileStore s(path);
        QString err;
        QVERIFY(!s.load(&err));
        QVERIFY(err.contains(QLatin1String("profiles")));
        // The unreadable file is kept under its own name.
        const QStringList kept = QDir(dir.path()).entryList({QStringLiteral("profiles.json.unreadable-*")});
        QCOMPARE(kept.size(), 1);
    }

    void format_noPercentWhenDisabled()
    {
        MachineProfile p = plainProfile();
        QCOMPARE(ProgramFormatter::format("O1\n", p), QByteArray("O1\n"));
    }

    void format_stripsBomAndCtrlZ()
    {
        MachineProfile p = plainProfile();
        QCOMPARE(ProgramFormatter::format("\xEF\xBB\xBFO1\n\x1A\x1A", p), QByteArray("O1\n"));
    }

    void format_leaderAndTrailer()
    {
        MachineProfile p = plainProfile();
        p.leaderCount = 3;
        p.leaderByte = 0x00;
        p.trailerCount = 2;
        p.trailerByte = 0x20;
        QCOMPARE(ProgramFormatter::format("X\n", p), QByteArray("\0\0\0X\n  ", 7));
    }

    void stats_countsLines()
    {
        QCOMPARE(ProgramFormatter::stats("a\nb\nc\n").lines, 3);
        QCOMPARE(ProgramFormatter::stats("a\nb\nc").lines, 3);
        QCOMPARE(ProgramFormatter::stats("").lines, 0);
        QCOMPARE(ProgramFormatter::stats("abc").bytes, qint64(3));
    }

    // ---------------------------------------------------------------- MachineProfile
    void profile_jsonRoundTrip()
    {
        MachineProfile p;
        p.name = QStringLiteral("Mill, \"3\"");
        p.portName = QStringLiteral("COM7");
        p.baudRate = 4800;
        p.dataBits = 8;
        p.parity = Parity::None;
        p.stopBits = StopBits::One;
        p.flowControl = FlowControl::RtsCts;
        p.lineEnding = LineEnding::CR;
        p.leaderByte = 0x7f;
        p.leaderCount = 50;
        p.trailerByte = 0x20;
        p.trailerCount = 5;
        p.ensurePercent = false;
        p.charDelayMs = 7;
        p.waitForXon = true;
        p.xonTimeoutSec = 0;
        p.programFolder = QStringLiteral("D:\\Progs");
        p.receiveFolder = QStringLiteral("D:\\In");
        p.receiveExtension = QStringLiteral("txt");
        p.receiveStartTimeoutSec = 120;
        p.receiveIdleTimeoutSec = 0;
        p.stripReceivedControlChars = false;

        const MachineProfile q = MachineProfile::fromJson(p.toJson());
        QCOMPARE(q.toJson(), p.toJson());
    }

    void profile_fromJsonDefaultsAndBadTypes()
    {
        const MachineProfile def;
        QJsonObject o;
        o.insert(QStringLiteral("name"), QStringLiteral("  Lathe  "));
        o.insert(QStringLiteral("baudRate"), QStringLiteral("19200")); // number as string
        o.insert(QStringLiteral("dataBits"), true);                    // wrong type -> default
        o.insert(QStringLiteral("parity"), QStringLiteral("bogus"));    // bad value -> default
        o.insert(QStringLiteral("stopBits"), 1);                       // number instead of string
        const MachineProfile p = MachineProfile::fromJson(o);
        QCOMPARE(p.name, QStringLiteral("Lathe"));
        QCOMPARE(p.baudRate, 19200);
        QCOMPARE(p.dataBits, def.dataBits);
        QCOMPARE_ENUM(p.parity, def.parity);
        QCOMPARE_ENUM(p.stopBits, StopBits::One);
        QCOMPARE(p.portName, def.portName);
    }

    void profile_validate_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<bool>("valid");
        QTest::newRow("default") << "default" << true;
        QTest::newRow("empty name") << "name" << false;
        QTest::newRow("empty port") << "port" << false;
        QTest::newRow("baud 0") << "baud" << false;
        QTest::newRow("data bits 9") << "bits" << false;
        QTest::newRow("1.5 stop with 7 bits") << "stop15" << false;
        QTest::newRow("2 stop with 5 bits") << "stop2five" << false;
        QTest::newRow("leader 300") << "leader" << false;
        QTest::newRow("negative timeout") << "timeout" << false;
        QTest::newRow("ext with slash") << "ext" << false;
        QTest::newRow("char delay huge") << "delay" << false;
    }

    void profile_validate()
    {
        QFETCH(QString, field);
        QFETCH(bool, valid);
        MachineProfile p;
        if (field == QLatin1String("name")) p.name = QStringLiteral("   ");
        if (field == QLatin1String("port")) p.portName.clear();
        if (field == QLatin1String("baud")) p.baudRate = 0;
        if (field == QLatin1String("bits")) p.dataBits = 9;
        if (field == QLatin1String("stop15")) { p.dataBits = 7; p.stopBits = StopBits::OneAndHalf; }
        if (field == QLatin1String("stop2five")) { p.dataBits = 5; p.stopBits = StopBits::Two; }
        if (field == QLatin1String("leader")) p.leaderByte = 300;
        if (field == QLatin1String("timeout")) p.receiveIdleTimeoutSec = -1;
        if (field == QLatin1String("ext")) p.receiveExtension = QStringLiteral("n/c");
        if (field == QLatin1String("delay")) p.charDelayMs = 999999;
        QCOMPARE(p.validate().isEmpty(), valid);
    }

    void profile_receiveFolder()
    {
        MachineProfile p;
        p.receiveFolder = QStringLiteral("C:/In/");
        QCOMPARE(p.effectiveReceiveFolder(), QStringLiteral("C:/In"));
        p.receiveFolder.clear();
        p.programFolder = QStringLiteral("C:/Progs");
        QCOMPARE(p.effectiveReceiveFolder(), QStringLiteral("C:/Progs/received"));
        p.programFolder.clear();
        QVERIFY(p.effectiveReceiveFolder().endsWith(QLatin1String("/received")));
    }

    // ---------------------------------------------------------------- ProfileStore
    void store_missingFileUsesDefaults()
    {
        QTemporaryDir dir;
        ProfileStore s(dir.filePath(QStringLiteral("profiles.json")));
        QString err;
        QVERIFY(s.load(&err));
        QVERIFY(!s.loadedFromFile());
        QCOMPARE(s.profiles().size(), ProfileStore::defaultProfiles().size());
    }

    void store_saveLoadRoundTripAndBackup()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("profiles.json"));
        ProfileStore s(path);
        MachineProfile a;
        a.name = QStringLiteral("A");
        MachineProfile b;
        b.name = QStringLiteral("B");
        b.portName = QStringLiteral("COM9");
        s.setProfiles({a, b});
        QVERIFY(s.save());
        QVERIFY(!QFile::exists(path + QStringLiteral(".bak")));
        QVERIFY(s.save());
        QVERIFY(QFile::exists(path + QStringLiteral(".bak")));

        ProfileStore t(path);
        QVERIFY(t.load());
        QVERIFY(t.loadedFromFile());
        QCOMPARE(t.profiles().size(), 2);
        QCOMPARE(t.profiles().at(1).portName, QStringLiteral("COM9"));
        QVERIFY(t.find(QStringLiteral("  b ")).has_value());
        QVERIFY(!t.find(QStringLiteral("C")).has_value());
    }

    void store_bomAndCorruptFiles()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("profiles.json"));
        QVERIFY(writeFile(path, "\xEF\xBB\xBF{\"version\":1,\"profiles\":[{\"name\":\"X\",\"port\":\"COM5\"}]}"));
        ProfileStore s(path);
        QVERIFY(s.load());
        QCOMPARE(s.profiles().size(), 1);
        QCOMPARE(s.profiles().first().portName, QStringLiteral("COM5"));

        QVERIFY(writeFile(path, "{ not json"));
        ProfileStore bad(path);
        QString err;
        QVERIFY(!bad.load(&err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(bad.profiles().size(), ProfileStore::defaultProfiles().size());
    }

    // ---------------------------------------------------------------- TransferLog
    void log_roundTripWithAwkwardText()
    {
        QTemporaryDir dir;
        TransferLog log(dir.filePath(QStringLiteral("log.csv")));
        TransferLogEntry e;
        e.timestamp = QDateTime(QDate(2026, 10, 18), QTime(9, 30, 5));
        e.machine = QStringLiteral("Machine A");
        e.direction = QStringLiteral("Send");
        e.profile = QStringLiteral("Mill, \"big\"");
        e.port = QStringLiteral("COM3");
        e.file = QStringLiteral("C:\\a,b\\O1.nc");
        e.fileSize = 1234;
        e.bytes = 1200;
        e.result = QStringLiteral("Failed");
        e.durationSec = 2.5;
        e.detail = QStringLiteral("Line one\nline \"two\", with comma");
        QSignalSpy spy(&log, &TransferLog::entryAppended);
        QVERIFY(log.append(e));
        QCOMPARE(spy.count(), 1);

        const auto back = log.readRecent(10);
        QCOMPARE(back.size(), 1);
        const TransferLogEntry& r = back.first();
        QCOMPARE(r.timestamp, e.timestamp);
        QCOMPARE(r.profile, e.profile);
        QCOMPARE(r.file, e.file);
        QCOMPARE(r.fileSize, e.fileSize);
        QCOMPARE(r.bytes, e.bytes);
        QCOMPARE(r.result, e.result);
        QCOMPARE(r.detail, QStringLiteral("Line one line \"two\", with comma")); // newline flattened
    }

    void log_newestFirstAndLimit()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("log.csv"));
        TransferLog log(path);
        for (int i = 0; i < 5; ++i) {
            TransferLogEntry e;
            e.timestamp = QDateTime::currentDateTime();
            e.machine = QStringLiteral("M%1").arg(i);
            QVERIFY(log.append(e));
        }
        const auto recent = log.readRecent(3);
        QCOMPARE(recent.size(), 3);
        QCOMPARE(recent.at(0).machine, QStringLiteral("M4"));
        QCOMPARE(recent.at(2).machine, QStringLiteral("M2"));
        QCOMPARE(readFile(path).count("Timestamp,"), 1); // header written once
    }

    void log_unwritableReportsError()
    {
        QTemporaryDir dir;
        TransferLog log(dir.path()); // a folder, not a file
        QString err;
        QVERIFY(!log.append(TransferLogEntry{}, &err));
        QVERIFY(!err.isEmpty());
    }

    // ---------------------------------------------------------------- DataSource
    void source_errors()
    {
        QTemporaryDir dir;
        const MachineProfile p = plainProfile();
        QString err;
        QVERIFY(!createDataSource(SendRequest{QString()}, p, &err));
        QVERIFY(err.contains(QLatin1String("No program")));
        QVERIFY(!createDataSource(SendRequest{dir.filePath(QStringLiteral("nope.nc"))}, p, &err));
        QVERIFY(err.contains(QLatin1String("File not found")));
        QVERIFY(!createDataSource(SendRequest{dir.path()}, p, &err));
        QVERIFY(err.contains(QLatin1String("Not a file")));
        const QString empty = dir.filePath(QStringLiteral("empty.nc"));
        QVERIFY(writeFile(empty, QByteArray()));
        QVERIFY(!createDataSource(SendRequest{empty}, p, &err));
        QVERIFY(err.contains(QLatin1String("empty")));
    }

    void source_readsFormattedInChunks()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.nc"));
        QVERIFY(writeFile(path, "AB\r\nCD\r\n"));
        QString err;
        qint64 size = 0;
        auto src = createDataSource(SendRequest{path}, plainProfile(), &err, &size);
        QVERIFY2(src, qPrintable(err));
        QCOMPARE(size, qint64(8));
        QCOMPARE(src->totalSize(), qint64(6)); // LF line endings
        QCOMPARE(src->read(4), QByteArray("AB\nC"));
        QVERIFY(!src->atEnd());
        QCOMPARE(src->read(100), QByteArray("D\n"));
        QVERIFY(src->atEnd());
        QCOMPARE(src->read(10), QByteArray());
    }

    // ---------------------------------------------------------------- drip feed source
    void stream_lineReaderMatchesSplitLines_data()
    {
        QTest::addColumn<QByteArray>("raw");
        QTest::newRow("crlf") << QByteArray("A\r\nB\r\nC\r\n");
        QTest::newRow("lf no final") << QByteArray("A\nB\nC");
        QTest::newRow("lone cr") << QByteArray("A\rB\rC\r");
        QTest::newRow("cr cr lf") << QByteArray("A\r\r\nB\r\r\nC");
        QTest::newRow("cr cr then text") << QByteArray("A\r\rB\r\r");
        QTest::newRow("mixed blanks") << QByteArray("\r\n\nA\r\r\n\r\nB\n\n");
        QTest::newRow("bom ctrl-z") << QByteArray("\xEF\xBB\xBF%\r\nO1\r\x1A\nM30\r\n%\r\n\x1A");
        QTest::newRow("only cr") << QByteArray("\r");
        QTest::newRow("trailing cr cr") << QByteArray("X\r\r");
    }

    void stream_lineReaderMatchesSplitLines()
    {
        QFETCH(QByteArray, raw);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.nc"));
        QVERIFY(writeFile(path, raw));
        const QList<QByteArray> expected = ProgramFormatter::splitLines(ProgramFormatter::cleaned(raw));
        // Tiny blocks put every line break (and every CR CR LF) across a block boundary.
        for (const qint64 block : {1, 2, 3, 4, 7, 65536}) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            StreamingFileSource::LineReader reader(&f, block);
            QList<QByteArray> got;
            QByteArray line;
            while (reader.next(&line))
                got.append(line);
            QVERIFY(!reader.failed());
            QCOMPARE(got, expected);
        }
    }

    void stream_matchesFormatterWithoutTrailer_data()
    {
        QTest::addColumn<QByteArray>("raw");
        QTest::newRow("full program") << QByteArray("%\r\nO1001\r\nG00 X0\r\nM30\r\n%\r\n");
        QTest::newRow("no percent") << QByteArray("O1001\nG00 X0\nM30\n");
        QTest::newRow("opening only") << QByteArray("(HEADER)\r\n%\r\nO1\r\nM30\r\n\r\n");
        QTest::newRow("closing only") << QByteArray("\r\nO1\r\nM30\r\n%");
        QTest::newRow("cr endings, bom, ctrl-z") << QByteArray("\xEF\xBB\xBFO1\rG1X1\rM30\r\x1A");
        QTest::newRow("blank lines around") << QByteArray("\n\n%\nO1\nM30\n%\n\n\n");
        QByteArray big("%\r\nO3001\r\n");
        for (int i = 1; i <= 20000; ++i)
            big += "N" + QByteArray::number(i) + " G01 X" + QByteArray::number(i) + ".5 F500\r\n";
        big += "M30\r\n%\r\n";
        QTest::newRow("bigger than one block") << big;
    }

    void stream_matchesFormatterWithoutTrailer()
    {
        QFETCH(QByteArray, raw);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.nc"));
        QVERIFY(writeFile(path, raw));

        for (const LineEnding eol : {LineEnding::CRLF, LineEnding::LF, LineEnding::CR}) {
            for (const bool percent : {true, false}) {
                MachineProfile p = plainProfile();
                p.lineEnding = eol;
                p.ensurePercent = percent;
                p.leaderCount = 3;
                p.trailerCount = 5; // never sent in drip feed
                MachineProfile noTrailer = p;
                noTrailer.trailerCount = 0;
                const QByteArray expected = ProgramFormatter::format(raw, noTrailer);

                QString err;
                auto src = createDataSource(SendRequest{path, SendRequest::Mode::DripFeed}, p, &err);
                QVERIFY2(src, qPrintable(err));
                QCOMPARE(src->totalSize(), qint64(expected.size()));
                QByteArray got;
                while (!src->atEnd()) {
                    const QByteArray chunk = src->read(128);
                    QVERIFY(!chunk.isEmpty());
                    got += chunk;
                    QCOMPARE(src->position(), qint64(got.size()));
                }
                QVERIFY(src->error().isEmpty());
                QCOMPARE(got, expected);
            }
        }
    }

    void format_lineEndOffsetsMatchFormattedOutput()
    {
        const QList<QByteArray> programs = {
            "%\r\nO1\r\nG0X1\r\nM30\r\n%\r\n", "O1\nG0X1\nM30\n", "(HDR)\n%\nO1\nM30\n\n", "\nO1\rM30\r%",
            "\xEF\xBB\xBFO1\r\r\nM30\x1A"};
        for (const QByteArray& raw : programs) {
            for (const LineEnding eol : {LineEnding::CRLF, LineEnding::LF}) {
                for (const bool percent : {true, false}) {
                    MachineProfile p = plainProfile();
                    p.lineEnding = eol;
                    p.ensurePercent = percent;
                    p.leaderCount = 2;
                    const QList<QByteArray> lines = ProgramFormatter::splitLines(ProgramFormatter::cleaned(raw));
                    ProgramFormatter::PercentPlanner planner;
                    QList<qint64> lengths;
                    for (const QByteArray& l : lines) {
                        planner.addLine(l);
                        lengths.append(l.size());
                    }
                    const QList<qint64> ends = ProgramFormatter::lineEndOffsets(lengths, planner.plan(), p);
                    const QByteArray out = ProgramFormatter::format(raw, p);
                    QCOMPARE(ends.size(), lines.size());
                    for (int i = 0; i < lines.size(); ++i)
                        QVERIFY2(out.left(ends.at(i)).endsWith(lines.at(i) + lineEndingBytes(eol)), raw.constData());
                }
            }
        }
    }

    void stream_endTailIsTheClosingPercentLine()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.nc"));
        MachineProfile p = plainProfile();
        p.lineEnding = LineEnding::CRLF;
        p.ensurePercent = true;
        p.leaderCount = 10;
        QString err;

        QVERIFY(writeFile(path, "%\nO1\nM30\n%\n"));
        auto src = createDataSource(SendRequest{path, SendRequest::Mode::DripFeed}, p, &err);
        QVERIFY2(src, qPrintable(err));
        QCOMPARE(src->endTailBytes(), qint64(3)); // "%\r\n"

        QVERIFY(writeFile(path, "O1\nM30\n")); // the closing % is added
        src = createDataSource(SendRequest{path, SendRequest::Mode::DripFeed}, p, &err);
        QVERIFY2(src, qPrintable(err));
        QCOMPARE(src->endTailBytes(), qint64(3));

        QVERIFY(writeFile(path, "%\nO1\nM30\n%\n\n")); // blank line after the %
        src = createDataSource(SendRequest{path, SendRequest::Mode::DripFeed}, p, &err);
        QVERIFY2(src, qPrintable(err));
        QCOMPARE(src->endTailBytes(), qint64(5));
    }

    void stream_rejectsBlankFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("blank.nc"));
        QVERIFY(writeFile(path, "  \r\n\r\n\t\r\n"));
        QString err;
        QVERIFY(!createDataSource(SendRequest{path, SendRequest::Mode::DripFeed}, plainProfile(), &err));
        QVERIFY(err.contains(QLatin1String("no program")));
    }

    // ---------------------------------------------------------------- MachineChannel
    void channel_missingPortFailsCleanly()
    {
        MachineChannel ch(QStringLiteral("T"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        MachineProfile p;
        p.portName = QStringLiteral("COM199");
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("p.nc"));
        QVERIFY(writeFile(file, "%\nO1\n%\n"));
        QVERIFY(ch.startSend(p, SendRequest{file}));
        QVERIFY(ch.isBusy());
        QVERIFY(!ch.startSend(p, SendRequest{file})); // second start refused while busy
        QVERIFY(done.wait(5000));
        QCOMPARE(done.count(), 1);
        const auto r = done.first().first().value<TransferResult>();
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Failed);
        QVERIFY2(r.message.contains(QLatin1String("not found")), qPrintable(r.message));
        QVERIFY(!ch.isBusy());

        done.clear();
        p.receiveFolder = dir.filePath(QStringLiteral("rx"));
        QVERIFY(ch.startReceive(p));
        QVERIFY(done.wait(5000));
        QCOMPARE_ENUM(done.first().first().value<TransferResult>().outcome, TransferOutcome::Failed);
        QVERIFY(!ch.isBusy());
    }

    void channel_missingFileFailsBeforeOpeningPort()
    {
        MachineChannel ch(QStringLiteral("T"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        MachineProfile p;
        p.portName = QStringLiteral("COM199");
        QVERIFY(ch.startSend(p, SendRequest{QStringLiteral("C:/definitely/not/here.nc")}));
        QVERIFY(done.wait(5000));
        const auto r = done.first().first().value<TransferResult>();
        QVERIFY2(r.message.startsWith(QLatin1String("File not found")), qPrintable(r.message));
    }

    void channel_cancelWhenIdleIsHarmless()
    {
        MachineChannel ch(QStringLiteral("T"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        ch.cancel();
        QTest::qWait(200);
        QCOMPARE(done.count(), 0);
    }

    // The tests below need a real or virtual port. They use COM10 from the
    // com0com setup and are skipped if it is not there.
    void channel_cancelWaitingReceive()
    {
        if (!portExists(QStringLiteral("COM10")))
            QSKIP("COM10 not present");
        MachineChannel ch(QStringLiteral("T"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QTemporaryDir dir;
        MachineProfile p;
        p.portName = QStringLiteral("COM10");
        p.receiveFolder = dir.filePath(QStringLiteral("rx"));
        QVERIFY(ch.startReceive(p));
        QTest::qWait(500);
        QCOMPARE(done.count(), 0); // still listening
        ch.cancel();
        QVERIFY(done.wait(3000));
        const auto r = done.first().first().value<TransferResult>();
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Cancelled);
        QVERIFY(QDir(p.receiveFolder).entryList(QDir::Files).isEmpty()); // nothing received, nothing saved

        // Port must be free again.
        done.clear();
        QVERIFY(ch.startReceive(p));
        QTest::qWait(300);
        ch.cancel();
        QVERIFY(done.wait(3000));
        QCOMPARE_ENUM(done.first().first().value<TransferResult>().outcome, TransferOutcome::Cancelled);
    }

    void channel_destroyWhileBusyDoesNotHang()
    {
        if (!portExists(QStringLiteral("COM10")))
            QSKIP("COM10 not present");
        QTemporaryDir dir;
        MachineProfile p;
        p.portName = QStringLiteral("COM10");
        p.receiveFolder = dir.filePath(QStringLiteral("rx"));
        QElapsedTimer t;
        {
            auto* ch = new MachineChannel(QStringLiteral("T"));
            QVERIFY(ch->startReceive(p));
            QTest::qWait(300);
            t.start();
            delete ch;
        }
        QVERIFY2(t.elapsed() < 3000, "destroying a busy channel took too long");
        // And the port was released.
        MachineChannel ch2(QStringLiteral("T2"));
        QSignalSpy done(&ch2, &MachineChannel::finished);
        QVERIFY(ch2.startReceive(p));
        QTest::qWait(300);
        QCOMPARE(done.count(), 0);
        ch2.cancel();
        QVERIFY(done.wait(3000));
    }

    void channel_xonTimeout()
    {
        if (!portExists(QStringLiteral("COM10")))
            QSKIP("COM10 not present");
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("p.nc"));
        QVERIFY(writeFile(file, "%\nO1\n%\n"));
        MachineChannel ch(QStringLiteral("T"));
        QSignalSpy done(&ch, &MachineChannel::finished);
        QSignalSpy states(&ch, &MachineChannel::stateChanged);
        MachineProfile p;
        p.portName = QStringLiteral("COM10");
        p.waitForXon = true;
        p.xonTimeoutSec = 1;
        QVERIFY(ch.startSend(p, SendRequest{file}));
        QVERIFY(done.wait(4000));
        const auto r = done.first().first().value<TransferResult>();
        QCOMPARE_ENUM(r.outcome, TransferOutcome::Failed);
        QVERIFY(r.message.contains(QLatin1String("XON")));
        QCOMPARE(r.bytesTransferred, qint64(0));
        bool sawWaiting = false;
        for (const auto& s : states)
            sawWaiting = sawWaiting || s.first().value<TransferState>() == TransferState::WaitingForXon;
        QVERIFY(sawWaiting);
    }

private:
    static bool portExists(const QString& name);
};

#include <QSerialPortInfo>

bool TestCore::portExists(const QString& name)
{
    for (const QSerialPortInfo& i : QSerialPortInfo::availablePorts()) {
        if (i.portName().compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QTEST_GUILESS_MAIN(TestCore)
#include "tst_core.moc"
