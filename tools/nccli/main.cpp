// nccli - run NCSender transfers from the command line.
//
//   nccli --send "Virtual Test A=C:\progs\O1001.nc" --send "Virtual Test B=C:\progs\O2002.nc"
//   nccli --receive "Virtual Test A" --set receiveIdleTimeoutSec=3
//   nccli --list-ports
//
// Every job runs on its own MachineChannel (own thread + own port), all
// started at the same moment. One line per job is printed when it finishes:
//   RESULT job=1 kind=send profile="..." outcome=Success bytes=.. total=.. durationMs=.. startMs=.. endMs=.. file="..." message="..."
// Exit code 0 = every job succeeded (or, with --expect-error, every job
// failed with the expected text); 1 = otherwise; 2 = bad arguments.

#include "AppPaths.h"
#include "MachineChannel.h"
#include "ProfileStore.h"
#include "TransferLog.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QSerialPortInfo>
#include <QTextStream>
#include <QTimer>

#include <memory>
#include <vector>

using namespace nc;

namespace {

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}

QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
}

QString now()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
}

QString quoted(QString s)
{
    s.replace(QLatin1Char('"'), QLatin1Char('\''));
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    s.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return QLatin1Char('"') + s + QLatin1Char('"');
}

bool applyOverride(MachineProfile& p, const QString& kv, QString* error)
{
    const int eq = kv.indexOf(QLatin1Char('='));
    if (eq <= 0) {
        *error = QStringLiteral("--set expects key=value, got '%1'").arg(kv);
        return false;
    }
    const QString key = kv.left(eq).trimmed();
    const QString value = kv.mid(eq + 1);
    QJsonObject o = p.toJson();
    if (!o.contains(key)) {
        *error = QStringLiteral("Unknown profile key '%1'. Valid keys: %2").arg(key, o.keys().join(QStringLiteral(", ")));
        return false;
    }
    const QJsonValue current = o.value(key);
    if (current.isBool()) {
        const QString v = value.trimmed().toLower();
        o.insert(key, v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("yes") || v == QLatin1String("on"));
    } else if (current.isDouble()) {
        bool ok = false;
        const int n = value.trimmed().toInt(&ok);
        if (!ok) {
            *error = QStringLiteral("--set %1 needs a number, got '%2'").arg(key, value);
            return false;
        }
        o.insert(key, n);
    } else {
        o.insert(key, value);
    }
    p = MachineProfile::fromJson(o);
    return true;
}

struct Job
{
    int id = 0;
    TransferDirection kind = TransferDirection::Send;
    MachineProfile profile;
    QString file;
    MachineChannel* channel = nullptr;
    bool done = false;
    TransferResult result;
    int lastPercent = -1;
};

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nccli"));
    QCoreApplication::setOrganizationName(QStringLiteral("NCSender"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Headless NCSender transfers using the same transfer engine as the GUI. For testing and scripting."));
    parser.addHelpOption();
    const QCommandLineOption profilesOpt(QStringLiteral("profiles"),
                                         QStringLiteral("Profiles JSON file (default: profiles.json next to the exe)."),
                                         QStringLiteral("file"));
    const QCommandLineOption sendOpt(QStringLiteral("send"),
                                     QStringLiteral("Send a file: \"Profile Name=C:\\path\\program.nc\". Repeatable."),
                                     QStringLiteral("profile=file"));
    const QCommandLineOption recvOpt(QStringLiteral("receive"), QStringLiteral("Receive using a profile. Repeatable."),
                                     QStringLiteral("profile"));
    const QCommandLineOption setOpt(QStringLiteral("set"),
                                    QStringLiteral("Override a profile setting for every job, e.g. waitForXon=true. "
                                                   "Repeatable. Keys are the names used in profiles.json."),
                                    QStringLiteral("key=value"));
    const QCommandLineOption logOpt(QStringLiteral("log"), QStringLiteral("Append results to this transfer log CSV."),
                                    QStringLiteral("file"));
    const QCommandLineOption expectErrOpt(
        QStringLiteral("expect-error"),
        QStringLiteral("Exit 0 only if every job FAILS with a message containing this text (case-insensitive)."),
        QStringLiteral("text"));
    const QCommandLineOption timeoutOpt(QStringLiteral("timeout"),
                                        QStringLiteral("Cancel all jobs after this many seconds (default 600)."),
                                        QStringLiteral("seconds"), QStringLiteral("600"));
    const QCommandLineOption quietOpt(QStringLiteral("quiet"), QStringLiteral("Only print RESULT lines."));
    const QCommandLineOption listPortsOpt(QStringLiteral("list-ports"), QStringLiteral("List serial ports and exit."));
    parser.addOptions({profilesOpt, sendOpt, recvOpt, setOpt, logOpt, expectErrOpt, timeoutOpt, quietOpt, listPortsOpt});
    parser.process(app);

    if (parser.isSet(listPortsOpt)) {
        for (const QSerialPortInfo& p : QSerialPortInfo::availablePorts())
            out() << p.portName() << "\t" << p.description() << "\t" << p.manufacturer() << Qt::endl;
        return 0;
    }

    const QString profilesPath = parser.isSet(profilesOpt) ? parser.value(profilesOpt) : AppPaths::profilesFile();
    if (!QFileInfo::exists(profilesPath)) {
        err() << "Profiles file not found: " << QDir::toNativeSeparators(profilesPath) << Qt::endl;
        return 2;
    }
    ProfileStore store(profilesPath);
    QString error;
    if (!store.load(&error)) {
        err() << error << Qt::endl;
        return 2;
    }

    auto findProfile = [&](const QString& name, MachineProfile* p) {
        const auto found = store.find(name);
        if (!found) {
            QStringList names;
            for (const MachineProfile& x : store.profiles())
                names << x.name;
            err() << "No profile named '" << name << "'. Profiles: " << names.join(QStringLiteral(", ")) << Qt::endl;
            return false;
        }
        *p = *found;
        return true;
    };

    std::vector<std::unique_ptr<Job>> jobs;
    for (const QString& spec : parser.values(sendOpt)) {
        const int eq = spec.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            err() << "--send expects \"Profile Name=file path\", got: " << spec << Qt::endl;
            return 2;
        }
        auto job = std::make_unique<Job>();
        job->kind = TransferDirection::Send;
        job->file = spec.mid(eq + 1).trimmed();
        if (!findProfile(spec.left(eq), &job->profile))
            return 2;
        jobs.push_back(std::move(job));
    }
    for (const QString& name : parser.values(recvOpt)) {
        auto job = std::make_unique<Job>();
        job->kind = TransferDirection::Receive;
        if (!findProfile(name, &job->profile))
            return 2;
        jobs.push_back(std::move(job));
    }
    if (jobs.empty()) {
        err() << "Nothing to do. Use --send and/or --receive (see --help)." << Qt::endl;
        return 2;
    }

    for (auto& job : jobs) {
        for (const QString& kv : parser.values(setOpt)) {
            if (!applyOverride(job->profile, kv, &error)) {
                err() << error << Qt::endl;
                return 2;
            }
        }
    }

    std::unique_ptr<TransferLog> log;
    if (parser.isSet(logOpt))
        log = std::make_unique<TransferLog>(parser.value(logOpt));

    const bool quiet = parser.isSet(quietOpt);
    const QString expectError = parser.value(expectErrOpt);
    int remaining = int(jobs.size());

    auto finishAll = [&]() {
        bool ok = true;
        for (const auto& job : jobs) {
            if (!expectError.isEmpty()) {
                ok = ok && job->result.outcome == TransferOutcome::Failed
                     && job->result.message.contains(expectError, Qt::CaseInsensitive);
            } else {
                ok = ok && job->result.outcome == TransferOutcome::Success;
            }
        }
        out().flush();
        QCoreApplication::exit(ok ? 0 : 1);
    };

    int nextId = 1;
    for (auto& jobPtr : jobs) {
        Job* job = jobPtr.get();
        job->id = nextId++;
        const QString label = QStringLiteral("job%1 %2 %3").arg(job->id).arg(job->profile.name, job->profile.portName);
        job->channel = new MachineChannel(QStringLiteral("job%1").arg(job->id), &app);

        QObject::connect(job->channel, &MachineChannel::stateChanged, &app,
                         [quiet, label](TransferState s, const QString& msg) {
                             if (!quiet)
                                 out() << "[" << now() << "] [" << label << "] " << toString(s) << ": " << msg << Qt::endl;
                         });
        QObject::connect(job->channel, &MachineChannel::progress, &app, [quiet, label, job](qint64 done, qint64 total) {
            if (quiet || total <= 0)
                return;
            const int pct = int(done * 100 / total);
            if (pct / 25 != job->lastPercent / 25 || pct == 100) {
                if (pct != job->lastPercent)
                    out() << "[" << now() << "] [" << label << "] " << pct << "% (" << done << "/" << total << ")" << Qt::endl;
                job->lastPercent = pct;
            }
        });
        QObject::connect(job->channel, &MachineChannel::finished, &app,
                         [&, job](const TransferResult& r) {
                             if (job->done)
                                 return;
                             job->done = true;
                             job->result = r;
                             out() << "RESULT job=" << job->id
                                   << " kind=" << (r.direction == TransferDirection::Send ? "send" : "receive")
                                   << " profile=" << quoted(r.profileName) << " port=" << r.portName
                                   << " outcome=" << toString(r.outcome) << " bytes=" << r.bytesTransferred
                                   << " total=" << r.bytesTotal << " durationMs=" << r.durationMs
                                   << " startMs=" << r.started.toMSecsSinceEpoch()
                                   << " endMs=" << r.finished.toMSecsSinceEpoch()
                                   << " file=" << quoted(QDir::toNativeSeparators(r.filePath))
                                   << " message=" << quoted(r.message) << Qt::endl;
                             if (log)
                                 log->append(TransferLogEntry::fromResult(r));
                             if (--remaining == 0)
                                 finishAll();
                         });
    }

    // Start every job in the same event-loop pass so they really run together.
    QTimer::singleShot(0, &app, [&] {
        for (auto& job : jobs) {
            if (job->kind == TransferDirection::Send)
                job->channel->startSend(job->profile, SendRequest{job->file});
            else
                job->channel->startReceive(job->profile);
        }
    });

    bool okTimeout = false;
    const int timeoutSec = parser.value(timeoutOpt).toInt(&okTimeout);
    if (okTimeout && timeoutSec > 0) {
        QTimer::singleShot(timeoutSec * 1000, &app, [&] {
            err() << "TIMEOUT after " << timeoutSec << " s - cancelling all jobs" << Qt::endl;
            for (auto& job : jobs)
                job->channel->cancel();
        });
    }

    const int code = app.exec();
    // Release ports/threads before the job list goes away.
    for (auto& job : jobs) {
        delete job->channel;
        job->channel = nullptr;
    }
    return code;
}
