// GUI tests: the real MainWindow, machine panels and profile editor, driven
// with QTest the way an operator would use them. Transfers go over the
// com0com pairs (COM10<->COM11, COM12<->COM13) with a QSerialPort here as
// the control. Runs on Qt's offscreen platform, so nothing pops up on screen.

#include "LogView.h"
#include "MachinePanel.h"
#include "MainWindow.h"
#include "ProfileEditorDialog.h"
#include "ProfileStore.h"
#include "Theme.h"
#include "TransferLog.h"
#include "TransferTypes.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace nc;

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
        m_ok = m_port.open(QIODevice::ReadWrite);
        if (m_ok) {
            m_port.setDataTerminalReady(true);
            m_port.setRequestToSend(true);
            QObject::connect(&m_port, &QSerialPort::readyRead, &m_port, [this] { m_data += m_port.readAll(); });
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

private:
    QSerialPort m_port;
    QByteArray m_data;
    bool m_ok = false;
};

template <typename T>
T* child(QWidget* parent, const char* name)
{
    T* w = parent->findChild<T*>(QLatin1String(name));
    if (!w)
        qFatal("widget %s not found", name);
    return w;
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

class TestUi : public QObject
{
    Q_OBJECT

    QTemporaryDir m_data;
    QString m_progs;
    QString m_rx;
    MainWindow* m_win = nullptr;
    bool m_havePorts = false;

    MachinePanel* A() { return m_win->panelA(); }
    MachinePanel* B() { return m_win->panelB(); }
    // The state badge and the message under the progress bar, read together.
    QString status(MachinePanel* p)
    {
        return child<QLabel>(p, "stateBadge")->text() + QStringLiteral(": ") + child<QLabel>(p, "status")->text();
    }
    // What Browse or dropping a file does.
    void setFile(MachinePanel* p, const QString& path) { p->setProgramFile(path); }
    void selectProfile(MachinePanel* p, const QString& name)
    {
        auto* combo = child<QComboBox>(p, "profileCombo");
        combo->setCurrentIndex(combo->findText(name));
    }
    void click(MachinePanel* p, const char* button) { QTest::mouseClick(child<QPushButton>(p, button), Qt::LeftButton); }
    bool buttonEnabled(MachinePanel* p, const char* button) { return child<QPushButton>(p, button)->isEnabled(); }

    void requirePorts()
    {
        if (!m_havePorts)
            QSKIP("com0com pairs COM10<->COM11 and COM12<->COM13 are not installed");
    }

private slots:
    void initTestCase()
    {
        qputenv("NCSENDER_DATA_DIR", m_data.path().toLocal8Bit());
        registerTransferMetaTypes();
        m_progs = m_data.filePath(QStringLiteral("programs"));
        m_rx = m_data.filePath(QStringLiteral("rx"));
        QDir().mkpath(m_progs);
        m_havePorts = portExists(QStringLiteral("COM10")) && portExists(QStringLiteral("COM11"))
                      && portExists(QStringLiteral("COM12")) && portExists(QStringLiteral("COM13"));

        MachineProfile a;
        a.name = QStringLiteral("Test A");
        a.portName = QStringLiteral("COM10");
        a.receiveFolder = m_rx;
        a.receiveIdleTimeoutSec = 2;
        a.programFolder = m_progs;
        MachineProfile b = a;
        b.name = QStringLiteral("Test B");
        b.portName = QStringLiteral("COM12");
        ProfileStore store(m_data.filePath(QStringLiteral("profiles.json")));
        store.setProfiles({a, b});
        QVERIFY(store.save());

        QVERIFY(writeFile(m_progs + QStringLiteral("/O1001.nc"), "%\r\nO1001\r\nG00 X0 Y0\r\nM30\r\n%\r\n"));
        QVERIFY(writeFile(m_progs + QStringLiteral("/big.nc"), largeProgram(150)));
        QVERIFY(writeFile(m_progs + QStringLiteral("/blank.nc"), "  \r\n\r\n"));

        m_win = new MainWindow;
        m_win->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
    }

    void cleanupTestCase()
    {
        m_win->setCloseConfirmHandler([](const QString&) { return true; });
        m_win->close();
        delete m_win;
    }

    void init()
    {
        // Every test starts from both panels idle on their own profile.
        QTRY_VERIFY_WITH_TIMEOUT(!A()->isBusy() && !B()->isBusy(), 10000);
        selectProfile(A(), QStringLiteral("Test A"));
        selectProfile(B(), QStringLiteral("Test B"));
    }

    // ---------------------------------------------------------------- basics

    void startsIdleWithProfilesLoaded()
    {
        QCOMPARE(child<QComboBox>(A(), "profileCombo")->currentText(), QStringLiteral("Test A"));
        QCOMPARE(child<QComboBox>(B(), "profileCombo")->currentText(), QStringLiteral("Test B"));
        QVERIFY(buttonEnabled(A(), "sendButton"));
        QVERIFY(buttonEnabled(A(), "receiveButton"));
        QVERIFY(!buttonEnabled(A(), "cancelButton"));
        QVERIFY(status(A()).startsWith(QLatin1String("Idle")));
    }

    void sendWithoutFileExplainsAndClears()
    {
        setFile(A(), QString());
        click(A(), "sendButton");
        QVERIFY2(status(A()).contains(QLatin1String("Choose a program file")), qPrintable(status(A())));
        QVERIFY(!A()->isBusy());
        // Regression: the message used to stay after the user fixed the problem.
        setFile(A(), m_progs + QStringLiteral("/O1001.nc"));
        QVERIFY2(status(A()).startsWith(QLatin1String("Idle")), qPrintable(status(A())));
    }

    void missingFileAndFolderAreExplained()
    {
        setFile(A(), m_progs + QStringLiteral("/nope.nc"));
        QVERIFY(child<QLabel>(A(), "fileInfo")->text().contains(QLatin1String("File not found")));
        click(A(), "sendButton");
        QVERIFY(status(A()).contains(QLatin1String("File not found")));
        setFile(A(), m_progs);
        QVERIFY2(child<QLabel>(A(), "fileInfo")->text().contains(QLatin1String("folder")),
                 qPrintable(child<QLabel>(A(), "fileInfo")->text()));
    }

    void previewShowsProgramAndStats()
    {
        setFile(A(), m_progs + QStringLiteral("/O1001.nc"));
        const QString info = child<QLabel>(A(), "fileInfo")->text();
        QVERIFY2(info.contains(QLatin1String("5 lines")), qPrintable(info));
        QVERIFY2(info.contains(QLatin1String("bytes will be sent")), qPrintable(info));
        QVERIFY(child<QPlainTextEdit>(A(), "preview")->toPlainText().contains(QLatin1String("O1001")));
    }

    // ---------------------------------------------------------------- transfers

    void sendCompletesAndIsLogged()
    {
        requirePorts();
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        setFile(A(), m_progs + QStringLiteral("/O1001.nc"));
        QSignalSpy done(A(), &MachinePanel::transferFinished);
        click(A(), "sendButton");
        QVERIFY(A()->isBusy());
        QVERIFY(!buttonEnabled(A(), "sendButton"));
        QVERIFY(buttonEnabled(A(), "cancelButton"));
        QVERIFY(buttonEnabled(B(), "sendButton")); // the other machine is untouched
        QVERIFY(done.wait(15000));
        QTRY_VERIFY_WITH_TIMEOUT(status(A()).startsWith(QLatin1String("Done")), 2000);
        auto* bar = child<QProgressBar>(A(), "progress");
        QCOMPARE(bar->value(), bar->maximum());
        QVERIFY(buttonEnabled(A(), "sendButton"));
        QVERIFY(!buttonEnabled(A(), "cancelButton"));
        QTest::qWait(200);
        QVERIFY(ctl.data().contains("O1001"));

        const auto entries = m_win->transferLog()->readRecent(1);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().machine, QStringLiteral("Machine A"));
        QCOMPARE(entries.first().result, QStringLiteral("Success"));
        auto* table = m_win->findChild<QTableWidget*>();
        QVERIFY(table && table->rowCount() >= 1);
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("Success"));
    }

    void receiveShowsLiveDataAndSaves()
    {
        requirePorts();
        Control ctl(QStringLiteral("COM13"));
        QVERIFY(ctl.ok());
        QSignalSpy done(B(), &MachinePanel::transferFinished);
        click(B(), "receiveButton");
        QVERIFY(B()->isBusy());
        QTest::qWait(400);
        ctl.send(QByteArray("\0\0\0\x12%\nO2002\nG01 X5\nM30\n%\x14", 26));
        QVERIFY(done.wait(10000));
        const auto r = done.first().first().value<TransferResult>();
        QCOMPARE(static_cast<int>(r.outcome), static_cast<int>(TransferOutcome::Success));
        QVERIFY(child<QPlainTextEdit>(B(), "received")->toPlainText().contains(QLatin1String("O2002")));
        QTRY_VERIFY_WITH_TIMEOUT(status(B()).contains(QLatin1String("saved as")), 2000);
        QVERIFY(QFile::exists(r.filePath));
        QVERIFY(child<QProgressBar>(B(), "progress")->text().contains(QLatin1String("bytes received")));
    }

    void bothMachinesAtOnce()
    {
        requirePorts();
        Control a(QStringLiteral("COM11"));
        Control b(QStringLiteral("COM13"));
        QVERIFY(a.ok() && b.ok());
        setFile(A(), m_progs + QStringLiteral("/big.nc"));
        QSignalSpy doneA(A(), &MachinePanel::transferFinished), doneB(B(), &MachinePanel::transferFinished);
        click(A(), "sendButton");
        click(B(), "receiveButton");
        QTest::qWait(300);
        a.send(QByteArray(1, '\x13')); // control A pauses the send
        b.send("%\nO4004\nM30\n%\n");
        QVERIFY(doneB.wait(8000));
        QTRY_VERIFY_WITH_TIMEOUT(status(A()).startsWith(QLatin1String("Paused")), 3000);
        QVERIFY(A()->isBusy());
        a.send(QByteArray(1, '\x11'));
        QVERIFY(doneA.wait(25000));
        QCOMPARE(static_cast<int>(doneA.first().first().value<TransferResult>().outcome), static_cast<int>(TransferOutcome::Success));
        QCOMPARE(static_cast<int>(doneB.first().first().value<TransferResult>().outcome), static_cast<int>(TransferOutcome::Success));
    }

    void dripFeedRunsEachCycleStartAndRestarts()
    {
        requirePorts();
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        setFile(A(), m_progs + QStringLiteral("/big.nc"));
        QSignalSpy done(A(), &MachinePanel::transferFinished);
        const int logBefore = m_win->transferLog()->readRecent(500).size();

        click(A(), "dripButton");
        QVERIFY(A()->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(status(A()).contains(QLatin1String("press Cycle Start")), 3000);
        QCOMPARE(child<QPushButton>(A(), "dripButton")->text(), QStringLiteral("Restart"));
        QVERIFY(buttonEnabled(A(), "dripButton"));
        QVERIFY(!buttonEnabled(A(), "sendButton"));

        // First Cycle Start, then the operator hits Reset part way through:
        // the control holds the rest with XOFF.
        ctl.send(QByteArray(1, '\x11'));
        QTRY_VERIFY_WITH_TIMEOUT(ctl.data().size() > 500, 5000);
        ctl.send(QByteArray(1, '\x13'));
        QTRY_VERIFY_WITH_TIMEOUT(status(A()).contains(QLatin1String("click Restart")), 3000);

        // Restart throws the rest away and arms again from the top.
        click(A(), "dripButton");
        QVERIFY(done.wait(3000)); // the abandoned run is logged as cancelled
        QTRY_VERIFY_WITH_TIMEOUT(A()->isBusy() && status(A()).contains(QLatin1String("press Cycle Start")), 3000);
        QTest::qWait(300);
        ctl.clear();

        // The next Cycle Start gets the whole program from the start.
        QByteArray program;
        {
            QFile f(m_progs + QStringLiteral("/big.nc"));
            QVERIFY(f.open(QIODevice::ReadOnly));
            program = f.readAll();
        }
        done.clear();
        ctl.send(QByteArray(1, '\x11'));
        QVERIFY(done.wait(25000));
        QCOMPARE(static_cast<int>(done.first().first().value<TransferResult>().outcome),
                 static_cast<int>(TransferOutcome::Success));
        QCOMPARE(ctl.data(), program); // big.nc is already CRLF with both % lines
        QVERIFY(A()->isBusy()); // armed for the next Cycle Start
        QTRY_VERIFY_WITH_TIMEOUT(status(A()).contains(QLatin1String("runs completed: 1")), 3000);

        click(A(), "cancelButton");
        QTRY_VERIFY_WITH_TIMEOUT(!A()->isBusy(), 3000);
        QCOMPARE(child<QPushButton>(A(), "dripButton")->text(), QStringLiteral("Drip Feed"));
        QVERIFY(buttonEnabled(A(), "sendButton"));
        QCOMPARE(m_win->transferLog()->readRecent(500).size(), logBefore + 3); // cancelled, success, cancelled
    }

    void cancelStopsAndReenables()
    {
        requirePorts();
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        setFile(A(), m_progs + QStringLiteral("/big.nc"));
        QSignalSpy done(A(), &MachinePanel::transferFinished);
        click(A(), "sendButton");
        // Well past the first 256-byte write: progress counts only writes the
        // driver has finished, so a cancel inside the first one reads as 0.
        QTRY_VERIFY_WITH_TIMEOUT(ctl.data().size() > 700, 5000);
        click(A(), "cancelButton");
        QVERIFY(done.wait(3000));
        QTRY_VERIFY_WITH_TIMEOUT(status(A()).startsWith(QLatin1String("Cancelled")), 2000);
        QVERIFY(buttonEnabled(A(), "sendButton"));
        auto* bar = child<QProgressBar>(A(), "progress");
        QVERIFY2(bar->value() > 0 && bar->value() < bar->maximum(),
                 qPrintable(QStringLiteral("value %1 of %2, sent %3 bytes, status '%4'")
                                .arg(bar->value())
                                .arg(bar->maximum())
                                .arg(ctl.data().size())
                                .arg(status(A()))));

        // Regression: the next Send on the same panel used to hang at
        // "No progress" after a cancel.
        done.clear();
        setFile(A(), m_progs + QStringLiteral("/O1001.nc"));
        click(A(), "sendButton");
        QVERIFY(done.wait(15000));
        QTRY_VERIFY_WITH_TIMEOUT(status(A()).startsWith(QLatin1String("Done")), 2000);
    }

    void blankFileIsRefusedAndProgressIsNotStale()
    {
        requirePorts();
        setFile(A(), m_progs + QStringLiteral("/blank.nc"));
        QSignalSpy done(A(), &MachinePanel::transferFinished);
        click(A(), "sendButton");
        QVERIFY(done.wait(5000));
        QTRY_VERIFY_WITH_TIMEOUT(status(A()).contains(QLatin1String("no program")), 2000);
        QCOMPARE(child<QProgressBar>(A(), "progress")->text(), QStringLiteral("Nothing sent"));
    }

    void samePortOnBothPanelsIsBlocked()
    {
        requirePorts();
        selectProfile(B(), QStringLiteral("Test A")); // both panels on COM10
        QSignalSpy doneA(A(), &MachinePanel::transferFinished);
        click(A(), "receiveButton");
        QVERIFY(A()->isBusy());
        setFile(B(), m_progs + QStringLiteral("/O1001.nc"));
        click(B(), "sendButton");
        QVERIFY2(status(B()).contains(QLatin1String("in use by Machine A")), qPrintable(status(B())));
        QVERIFY(!B()->isBusy());
        click(A(), "cancelButton");
        QVERIFY(doneA.wait(3000));
    }

    void portHeldByAnotherProgram()
    {
        requirePorts();
        QSerialPort holder(QStringLiteral("COM10"));
        QVERIFY(holder.open(QIODevice::ReadWrite));
        setFile(A(), m_progs + QStringLiteral("/O1001.nc"));
        QSignalSpy done(A(), &MachinePanel::transferFinished);
        click(A(), "sendButton");
        QVERIFY(done.wait(5000));
        QTRY_VERIFY_WITH_TIMEOUT(status(A()).contains(QLatin1String("already in use")), 2000);
        QVERIFY(buttonEnabled(A(), "sendButton"));
    }

    // ---------------------------------------------------------------- profiles

    void deletedProfileIsNotReplacedByAnother()
    {
        // Regression: deleting B's profile used to switch panel B to Machine A's
        // profile, so the next Send went to the wrong machine.
        ProfileStore store(m_data.filePath(QStringLiteral("profiles.json")));
        QVERIFY(store.load());
        const QList<MachineProfile> all = store.profiles(); // Test A, Test B
        auto* comboA = child<QComboBox>(A(), "profileCombo");
        auto* comboB = child<QComboBox>(B(), "profileCombo");

        B()->setProfiles({all.at(0)}); // "Test B" deleted
        QCOMPARE(comboB->currentIndex(), -1);
        QVERIFY(!buttonEnabled(B(), "sendButton"));
        QVERIFY(!buttonEnabled(B(), "receiveButton"));
        QVERIFY2(status(B()).contains(QLatin1String("no longer exists")), qPrintable(status(B())));

        // Profiles restored: B goes back to its own profile, not A's.
        B()->setProfiles(all);
        QCOMPARE(comboB->currentText(), QStringLiteral("Test B"));

        // A rename keeps the selection on the renamed profile.
        QList<MachineProfile> renamed = all;
        renamed[0].name = QStringLiteral("Mill 1");
        A()->setProfiles(renamed, {{QStringLiteral("Test A"), QStringLiteral("Mill 1")}});
        QCOMPARE(comboA->currentText(), QStringLiteral("Mill 1"));
        A()->setProfiles(all, {{QStringLiteral("Mill 1"), QStringLiteral("Test A")}});
        QCOMPARE(comboA->currentText(), QStringLiteral("Test A"));
    }

    void editorDoesNotAutocompletePort()
    {
        ProfileStore store(m_data.filePath(QStringLiteral("profiles.json")));
        QVERIFY(store.load());
        ProfileEditorDialog dlg(store.profiles(), {QStringLiteral("COM10"), QStringLiteral("COM12")});
        dlg.show();
        auto* port = child<QComboBox>(&dlg, "portCombo");
        port->lineEdit()->clear();
        QTest::keyClicks(port->lineEdit(), QStringLiteral("COM1"));
        QTest::keyClick(port->lineEdit(), Qt::Key_Tab);
        QCOMPARE(port->currentText(), QStringLiteral("COM1"));
        dlg.setWarningHandler([](const QString&) {});
        dlg.accept();
        QCOMPARE(dlg.profiles().first().portName, QStringLiteral("COM1"));
    }

    void editorBaudRejectsThousandsSeparator()
    {
        ProfileStore store(m_data.filePath(QStringLiteral("profiles.json")));
        QVERIFY(store.load());
        ProfileEditorDialog dlg(store.profiles(), {});
        dlg.show();
        auto* baud = child<QComboBox>(&dlg, "baudCombo");
        baud->lineEdit()->clear();
        QTest::keyClicks(baud->lineEdit(), QStringLiteral("19,200"));
        QCOMPARE(baud->currentText(), QStringLiteral("19200"));
    }

    void editorRejectsDuplicateNamesAndDeletes()
    {
        ProfileStore store(m_data.filePath(QStringLiteral("profiles.json")));
        QVERIFY(store.load());
        ProfileEditorDialog dlg(store.profiles(), {});
        dlg.show();
        QString warned;
        dlg.setWarningHandler([&](const QString& m) { warned = m; });
        dlg.setConfirmHandler([](const QString&) { return true; });
        auto* list = child<QListWidget>(&dlg, "profileList");
        auto* name = child<QLineEdit>(&dlg, "nameEdit");
        list->setCurrentRow(1);
        name->clear();
        QTest::keyClicks(name, QStringLiteral("test a"));
        dlg.accept();
        QVERIFY2(warned.contains(QLatin1String("unique")), qPrintable(warned));
        QCOMPARE(dlg.result(), int(QDialog::Rejected)); // still open

        // Rename instead, then check the rename map.
        name->clear();
        QTest::keyClicks(name, QStringLiteral("Lathe 7"));
        QTest::mouseClick(child<QPushButton>(&dlg, "addButton"), Qt::LeftButton);
        QCOMPARE(list->count(), 3);
        QTest::mouseClick(child<QPushButton>(&dlg, "deleteButton"), Qt::LeftButton);
        QCOMPARE(list->count(), 2);
        warned.clear();
        dlg.accept();
        QVERIFY2(warned.isEmpty(), qPrintable(warned));
        QCOMPARE(dlg.profiles().size(), 2);
        QCOMPARE(dlg.profiles().at(1).name, QStringLiteral("Lathe 7"));
        QCOMPARE(dlg.renames().value(QStringLiteral("Test B")), QStringLiteral("Lathe 7"));
    }

    // ---------------------------------------------------------------- number of machines

    void showOnlyOneMachine()
    {
        QVERIFY(m_win->setMachineCount(1));
        QCOMPARE(m_win->machineCount(), 1);
        QCOMPARE(m_win->findChildren<MachinePanel*>().size(), 1);
        QVERIFY(m_win->panelB() == nullptr);
        QCOMPARE(A()->title(), QStringLiteral("Machine A"));
        QVERIFY(m_win->setMachineCount(2));
        QCOMPARE(B()->title(), QStringLiteral("Machine B"));
        // B comes back on the profile it had before it was hidden.
        QCOMPARE(child<QComboBox>(B(), "profileCombo")->currentText(), QStringLiteral("Test B"));
    }

    void addMachineCWorksAndIsChecked()
    {
        requirePorts();
        QVERIFY(m_win->setMachineCount(3));
        MachinePanel* C = m_win->panel(2);
        QVERIFY(C);
        QCOMPARE(C->title(), QStringLiteral("Machine C"));
        QCOMPARE(m_win->findChildren<MachinePanel*>().size(), 3);
        // A new panel starts with no profile, so it can't silently use another machine's port.
        QCOMPARE(child<QComboBox>(C, "profileCombo")->currentIndex(), -1);
        QVERIFY(!buttonEnabled(C, "sendButton"));

        // Port conflicts are checked against every other panel, not just A/B.
        selectProfile(C, QStringLiteral("Test B"));
        QSignalSpy doneB(B(), &MachinePanel::transferFinished);
        click(B(), "receiveButton");
        QVERIFY(B()->isBusy());
        setFile(C, m_progs + QStringLiteral("/O1001.nc"));
        click(C, "sendButton");
        QVERIFY2(status(C).contains(QLatin1String("in use by Machine B")), qPrintable(status(C)));

        // A busy machine can't be removed.
        QString why;
        QVERIFY(m_win->setMachineCount(3));
        click(B(), "cancelButton");
        QVERIFY(doneB.wait(3000));

        // C sends on its own channel.
        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        selectProfile(C, QStringLiteral("Test A"));
        QSignalSpy doneC(C, &MachinePanel::transferFinished);
        click(C, "sendButton");
        QVERIFY(C->isBusy());
        QVERIFY(!m_win->setMachineCount(2, &why));
        QVERIFY2(why.contains(QLatin1String("Machine C")), qPrintable(why));
        QCOMPARE(m_win->machineCount(), 3);
        QVERIFY(doneC.wait(15000));
        QCOMPARE(static_cast<int>(doneC.first().first().value<TransferResult>().outcome), static_cast<int>(TransferOutcome::Success));
        QTest::qWait(200);
        QVERIFY(ctl.data().contains("O1001"));
        QCOMPARE(m_win->transferLog()->readRecent(1).first().machine, QStringLiteral("Machine C"));
    }

    void machineCountIsRemembered()
    {
        QVERIFY(m_win->setMachineCount(4));
        {
            MainWindow second;
            QCOMPARE(second.machineCount(), 4);
            QCOMPARE(second.panel(3)->title(), QStringLiteral("Machine D"));
        }
        QVERIFY(!m_win->setMachineCount(4) || m_win->machineCount() == 4);
        QVERIFY(m_win->setMachineCount(99)); // clamped
        QCOMPARE(m_win->machineCount(), MainWindow::kMaxMachines);
        QVERIFY(m_win->setMachineCount(0));
        QCOMPARE(m_win->machineCount(), MainWindow::kMinMachines);
        QVERIFY(m_win->setMachineCount(2));
    }

    // ---------------------------------------------------------------- log and exit

    void lockedLogShowsWarningAndRecovers()
    {
#ifdef Q_OS_WIN
        const QString path = m_win->transferLog()->path();
        QFile touch(path);
        QVERIFY(touch.open(QIODevice::Append));
        touch.close();
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GENERIC_READ, 0,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        setFile(A(), m_progs + QStringLiteral("/blank.nc")); // fails fast, still gets logged
        QSignalSpy done(A(), &MachinePanel::transferFinished);
        click(A(), "sendButton");
        QVERIFY(done.wait(5000));
        QLabel* warning = nullptr;
        for (QLabel* l : m_win->statusBar()->findChildren<QLabel*>()) {
            if (l->text().contains(QLatin1String("not yet written")))
                warning = l;
        }
        QVERIFY2(warning && warning->isVisible(), "no visible warning about unwritten log entries");
        QCOMPARE(m_win->transferLog()->pendingCount(), 1);
        CloseHandle(h);
        QTRY_VERIFY_WITH_TIMEOUT(m_win->transferLog()->pendingCount() == 0, 15000); // automatic retry
        QTRY_VERIFY_WITH_TIMEOUT(!warning->isVisible(), 2000);
#else
        QSKIP("Windows only");
#endif
    }

    void closingWhileBusyCancelsAndLogs()
    {
        requirePorts();
        // A second window on the same data folder, closed mid-receive.
        auto* w = new MainWindow;
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w));
        MachinePanel* p = w->panelB();
        p->findChild<QComboBox*>(QStringLiteral("profileCombo"))->setCurrentIndex(1);
        QTest::mouseClick(p->findChild<QPushButton*>(QStringLiteral("receiveButton")), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(p->isBusy(), 2000);
        QTest::qWait(300);
        bool asked = false;
        w->setCloseConfirmHandler([&](const QString&) {
            asked = true;
            return true;
        });
        QVERIFY(w->close());
        QVERIFY(asked);
        QVERIFY(!p->isBusy());
        delete w;
        // Regression: transfers cancelled at exit used to vanish from the log.
        const auto entries = m_win->transferLog()->readRecent(3);
        bool found = false;
        for (const auto& e : entries)
            found = found || (e.direction == QLatin1String("Receive") && e.result == QLatin1String("Cancelled"));
        QVERIFY(found);
    }

    // Not a check: saves pictures of the window for the README and for
    // reviewing the look. Runs only with NCSENDER_SCREENSHOT_DIR set.
    void screenshots()
    {
        const QString dir = qEnvironmentVariable("NCSENDER_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set NCSENDER_SCREENSHOT_DIR to save screenshots");
        requirePorts();
        QDir().mkpath(dir);
        m_win->resize(1440, 900);
        QTRY_VERIFY_WITH_TIMEOUT(!A()->isBusy() && !B()->isBusy(), 10000);
        m_win->setMachineCount(2);

        Control ctl(QStringLiteral("COM11"));
        QVERIFY(ctl.ok());
        setFile(A(), m_progs + QStringLiteral("/big.nc"));
        setFile(B(), m_progs + QStringLiteral("/O1001.nc"));
        QTest::qWait(300); // let the program views colour their lines
        QVERIFY(m_win->grab().save(dir + QStringLiteral("/idle.png")));

        click(A(), "dripButton");
        QTest::qWait(500);
        ctl.send(QByteArray(1, ''));
        QTRY_VERIFY_WITH_TIMEOUT(ctl.data().size() > 2500, 10000);
        QTest::qWait(400);
        QVERIFY(m_win->grab().save(dir + QStringLiteral("/drip.png")));
        ctl.send(QByteArray(1, ''));
        QTRY_VERIFY_WITH_TIMEOUT(status(A()).startsWith(QLatin1String("Paused")), 3000);
        QVERIFY(m_win->grab().save(dir + QStringLiteral("/paused.png")));
        click(A(), "cancelButton");
        QTRY_VERIFY_WITH_TIMEOUT(!A()->isBusy(), 5000);
        QVERIFY(A()->grab().save(dir + QStringLiteral("/cancelled-panel.png")));

        ProfileStore store(m_data.filePath(QStringLiteral("profiles.json")));
        QVERIFY(store.load());
        ProfileEditorDialog dlg(store.profiles(), {QStringLiteral("COM10"), QStringLiteral("COM12")}, m_win);
        dlg.resize(900, 640);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QVERIFY(dlg.grab().save(dir + QStringLiteral("/profiles.png")));
    }
};

int main(int argc, char* argv[])
{
    // Draw offscreen so the tests never pop windows over the user's desktop.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    Theme::apply(app);
    TestUi t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_ui.moc"
