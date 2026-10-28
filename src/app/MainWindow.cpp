#include "MainWindow.h"

#include "AppPaths.h"
#include "LogView.h"
#include "MachinePanel.h"
#include "ProfileEditorDialog.h"
#include "TransferLog.h"
#include "Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUrl>

#include <algorithm>

using namespace nc;

namespace {

int portNumber(const QString& name)
{
    static const QRegularExpression re(QStringLiteral("(\\d+)$"));
    const QRegularExpressionMatch m = re.match(name);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_store(AppPaths::profilesFile())
{
    setWindowTitle(QStringLiteral("NCSender - RS232 Program Transfer"));
    setWindowIcon(Theme::icon(Theme::Icon::Send, Theme::Color::accent));
    resize(1280, 860);

    m_settings = new QSettings(AppPaths::settingsFile(), QSettings::IniFormat, this);
    m_log = new TransferLog(AppPaths::logFile(), this);

    QString loadError;
    const bool loaded = m_store.load(&loadError);

    m_logView = new LogView(m_log);
    m_panelArea = new QWidget;

    m_vsplit = new QSplitter(Qt::Vertical);
    m_vsplit->addWidget(m_panelArea);
    m_vsplit->addWidget(m_logView);
    m_vsplit->setStretchFactor(0, 4);
    m_vsplit->setStretchFactor(1, 1);
    m_vsplit->setChildrenCollapsible(false);
    m_vsplit->setSizes({620, 200});
    setCentralWidget(m_vsplit);

    m_portsLabel = new QLabel;
    statusBar()->addPermanentWidget(m_portsLabel, 1);
    m_logWarning = new QLabel;
    m_logWarning->setStyleSheet(QStringLiteral("QLabel { background: #fff3cd; color: #7a4d00; font-weight: bold; }"));
    m_logWarning->hide();
    statusBar()->addPermanentWidget(m_logWarning);
    m_logRetryTimer = new QTimer(this);
    m_logRetryTimer->setInterval(10000);
    connect(m_logRetryTimer, &QTimer::timeout, this, &MainWindow::retryLog);

    buildMenus();

    const int count = std::clamp(m_settings->value(QStringLiteral("window/machineCount"), 2).toInt(), kMinMachines, kMaxMachines);
    setMachineCount(count);
    refreshPorts();

    restoreGeometry(m_settings->value(QStringLiteral("window/geometry")).toByteArray());
    m_vsplit->restoreState(m_settings->value(QStringLiteral("window/vsplit")).toByteArray());

    if (!loaded) {
        QTimer::singleShot(0, this, [this, loadError] {
            QMessageBox::warning(this, QStringLiteral("Profiles"), loadError);
        });
    } else if (!m_store.loadedFromFile()) {
        statusBar()->showMessage(QStringLiteral("No profiles.json found - using example profiles. Use Profiles > "
                                                "Edit Profiles to set up your machines."),
                                 15000);
    }
}

MainWindow::~MainWindow() = default;

MachinePanel* MainWindow::createPanel(int index)
{
    const QChar letter(QLatin1Char(char('A' + index)));
    // A and B suggest the first two profiles on a fresh install. Panels added
    // later start with no profile, so a new panel never silently points at
    // another machine's COM port; the operator picks one.
    const int defaultProfile = index < 2 ? index : -1;
    auto* p = new MachinePanel(QStringLiteral("Machine %1").arg(letter), QStringLiteral("panel%1").arg(letter),
                               defaultProfile, m_settings);
    // Any other panel using the same COM port blocks this one from starting.
    p->setPortCheck([this, p](const QString& port) { return portUser(port, p); });
    connect(p, &MachinePanel::transferFinished, this, &MainWindow::onTransferFinished);
    p->setProfiles(m_store.profiles());
    p->setAvailablePorts(m_portNames);
    return p;
}

bool MainWindow::setMachineCount(int n, QString* why)
{
    n = std::clamp(n, int(kMinMachines), int(kMaxMachines));
    // Only the last panels are removed (C before B), and never one that is busy.
    for (int i = n; i < m_panels.size(); ++i) {
        if (m_panels.at(i)->isBusy()) {
            if (why)
                *why = QStringLiteral("%1 is in the middle of a transfer. Wait for it to finish or cancel it, then "
                                      "remove the machine.")
                           .arg(m_panels.at(i)->title());
            return false;
        }
    }
    while (m_panels.size() > n)
        delete m_panels.takeLast();
    while (m_panels.size() < n)
        m_panels.append(createPanel(int(m_panels.size())));

    layoutPanels();
    m_settings->setValue(QStringLiteral("window/machineCount"), n);
    updateMachineActions();
    return true;
}

void MainWindow::layoutPanels()
{
    // Up to three machines side by side; more go two rows deep.
    delete m_panelArea->layout();
    auto* grid = new QGridLayout(m_panelArea);
    grid->setContentsMargins(12, 12, 12, 6);
    grid->setSpacing(12);
    const int n = int(m_panels.size());
    const int columns = n <= 3 ? n : (n + 1) / 2;
    for (int i = 0; i < n; ++i)
        grid->addWidget(m_panels.at(i), i / columns, i % columns);
    for (int c = 0; c < columns; ++c)
        grid->setColumnStretch(c, 1);
}

void MainWindow::changeMachineCount(int n)
{
    QString why;
    if (!setMachineCount(n, &why))
        QMessageBox::information(this, QStringLiteral("Machines"), why);
}

void MainWindow::updateMachineActions()
{
    const int n = machineCount();
    if (m_addMachineAct)
        m_addMachineAct->setEnabled(n < kMaxMachines);
    if (m_removeMachineAct)
        m_removeMachineAct->setEnabled(n > kMinMachines);
    for (int i = 0; i < m_countActs.size(); ++i)
        m_countActs.at(i)->setChecked(i + 1 == n);
}

QString MainWindow::portUser(const QString& port, const MachinePanel* asker) const
{
    for (const MachinePanel* p : m_panels) {
        if (p != asker && p->isBusy() && p->busyPortName().compare(port, Qt::CaseInsensitive) == 0)
            return p->title();
    }
    return {};
}

bool MainWindow::anyBusy() const
{
    for (const MachinePanel* p : m_panels) {
        if (p->isBusy())
            return true;
    }
    return false;
}

void MainWindow::buildMenus()
{
    const QColor ink = Theme::Color::text;
    auto* refreshAct = new QAction(Theme::icon(Theme::Icon::Refresh, ink), QStringLiteral("Refresh Ports"), this);
    refreshAct->setShortcut(QKeySequence::Refresh);
    refreshAct->setToolTip(QStringLiteral("Re-scan the COM ports on this PC (F5)"));
    connect(refreshAct, &QAction::triggered, this, &MainWindow::refreshPorts);

    auto* profilesAct = new QAction(Theme::icon(Theme::Icon::Profiles, ink), QStringLiteral("Edit Profiles..."), this);
    profilesAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
    connect(profilesAct, &QAction::triggered, this, &MainWindow::editProfiles);

    auto* dataAct = new QAction(Theme::icon(Theme::Icon::Folder, ink), QStringLiteral("Open Data Folder"), this);
    dataAct->setToolTip(QStringLiteral("Folder with profiles.json, settings and the transfer log"));
    connect(dataAct, &QAction::triggered, this, &MainWindow::openDataFolder);

    auto* exitAct = new QAction(QStringLiteral("Exit"), this);
    exitAct->setShortcut(QKeySequence::Quit);
    connect(exitAct, &QAction::triggered, this, &QWidget::close);

    auto* aboutAct = new QAction(QStringLiteral("About NCSender"), this);
    connect(aboutAct, &QAction::triggered, this, &MainWindow::showAbout);

    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(dataAct);
    fileMenu->addSeparator();
    fileMenu->addAction(exitAct);

    QMenu* portsMenu = menuBar()->addMenu(QStringLiteral("&Ports"));
    portsMenu->addAction(refreshAct);

    QMenu* profilesMenu = menuBar()->addMenu(QStringLiteral("P&rofiles"));
    profilesMenu->addAction(profilesAct);

    m_addMachineAct = new QAction(Theme::icon(Theme::Icon::Add, ink), QStringLiteral("Add Machine"), this);
    m_addMachineAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    m_addMachineAct->setToolTip(QStringLiteral("Show another machine panel (up to %1)").arg(kMaxMachines));
    connect(m_addMachineAct, &QAction::triggered, this, [this] { changeMachineCount(machineCount() + 1); });
    m_removeMachineAct = new QAction(Theme::icon(Theme::Icon::Remove, ink), QStringLiteral("Remove Machine"), this);
    m_removeMachineAct->setToolTip(QStringLiteral("Hide the last machine panel"));
    connect(m_removeMachineAct, &QAction::triggered, this, [this] { changeMachineCount(machineCount() - 1); });

    QMenu* machinesMenu = menuBar()->addMenu(QStringLiteral("&Machines"));
    machinesMenu->addAction(m_addMachineAct);
    machinesMenu->addAction(m_removeMachineAct);
    machinesMenu->addSeparator();
    auto* countGroup = new QActionGroup(this);
    for (int i = kMinMachines; i <= kMaxMachines; ++i) {
        QAction* a = machinesMenu->addAction(i == 1 ? QStringLiteral("Show 1 machine")
                                                    : QStringLiteral("Show %1 machines").arg(i));
        a->setCheckable(true);
        countGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] { changeMachineCount(i); });
        m_countActs.append(a);
    }

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    helpMenu->addAction(aboutAct);

    QToolBar* tb = addToolBar(QStringLiteral("Main"));
    tb->setObjectName(QStringLiteral("mainToolbar"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->setIconSize(QSize(18, 18));
    tb->addAction(refreshAct);
    tb->addAction(profilesAct);
    tb->addAction(dataAct);
    tb->addSeparator();
    tb->addAction(m_addMachineAct);
    tb->addAction(m_removeMachineAct);
    updateMachineActions();
}

void MainWindow::refreshPorts()
{
    QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    std::sort(ports.begin(), ports.end(), [](const QSerialPortInfo& a, const QSerialPortInfo& b) {
        return portNumber(a.portName()) < portNumber(b.portName());
    });

    m_portNames.clear();
    QStringList described;
    for (const QSerialPortInfo& p : ports) {
        m_portNames.append(p.portName());
        described.append(p.description().isEmpty() ? p.portName()
                                                    : QStringLiteral("%1 (%2)").arg(p.portName(), p.description()));
    }

    m_portsLabel->setText(m_portNames.isEmpty() ? QStringLiteral("No COM ports found")
                                                : QStringLiteral("COM ports: ") + m_portNames.join(QStringLiteral(", ")));
    m_portsLabel->setToolTip(described.join(QLatin1Char('\n')));
    for (MachinePanel* p : m_panels)
        p->setAvailablePorts(m_portNames);
    statusBar()->showMessage(QStringLiteral("Found %1 COM port(s)").arg(m_portNames.size()), 4000);
}

void MainWindow::editProfiles()
{
    ProfileEditorDialog dlg(m_store.profiles(), m_portNames, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    m_store.setProfiles(dlg.profiles());
    QString error;
    if (!m_store.save(&error))
        QMessageBox::warning(this, QStringLiteral("Profiles NOT saved"),
                             error + QStringLiteral("\n\nThe changes are in use until NCSender is closed, but they "
                                                    "were not written to disk and will be lost on exit."));
    else
        statusBar()->showMessage(QStringLiteral("Profiles saved to %1").arg(QDir::toNativeSeparators(m_store.filePath())), 6000);

    const QHash<QString, QString> renames = dlg.renames();
    for (MachinePanel* p : m_panels)
        p->setProfiles(m_store.profiles(), renames);
}

void MainWindow::openDataFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(AppPaths::dataDir()));
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this, QStringLiteral("About NCSender"),
        QStringLiteral("<b>NCSender %1</b><br>RS232 program sender/receiver for CNC controls.<br><br>"
                       "Up to %3 independent machine channels, each on its own COM port and thread.<br><br>"
                       "Fully offline: this program makes no network connections.<br><br>"
                       "Data folder:<br>%2")
            .arg(QApplication::applicationVersion(), QDir::toNativeSeparators(AppPaths::dataDir()).toHtmlEscaped())
            .arg(kMaxMachines));
}

void MainWindow::onTransferFinished(const TransferResult& result)
{
    QString error;
    m_log->append(TransferLogEntry::fromResult(result), &error);
    updateLogWarning(error);
}

void MainWindow::updateLogWarning(const QString& error)
{
    const int pending = m_log->pendingCount();
    if (pending == 0) {
        m_logWarning->hide();
        m_logRetryTimer->stop();
        return;
    }
    m_logWarning->setText(QStringLiteral("  %1 log entr%2 not yet written - close transfer_log.csv if it is open in "
                                         "Excel. Retrying automatically.  ")
                              .arg(pending)
                              .arg(pending == 1 ? QStringLiteral("y") : QStringLiteral("ies")));
    m_logWarning->setToolTip(error);
    m_logWarning->show();
    if (!m_logRetryTimer->isActive())
        m_logRetryTimer->start();
}

void MainWindow::retryLog()
{
    QString error;
    m_log->flushPending(&error);
    updateLogWarning(error);
}

bool MainWindow::waitForTransfersToStop(int timeoutMs)
{
    // Let the cancelled transfers finish properly so their results (and any
    // partial received program) are logged before the window goes away.
    QElapsedTimer t;
    t.start();
    while (anyBusy() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return !anyBusy();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    QStringList busy;
    for (const MachinePanel* p : m_panels) {
        if (p->isBusy())
            busy << p->title();
    }

    if (!busy.isEmpty()) {
        const auto answer = m_confirmClose
                                ? (m_confirmClose(busy.join(QStringLiteral(" and "))) ? QMessageBox::Yes : QMessageBox::No)
                                : QMessageBox::question(this, QStringLiteral("Transfer in progress"),
                                                        QStringLiteral("A transfer is still running on %1.\n\nCancel it and exit?")
                                                            .arg(busy.join(QStringLiteral(" and "))),
                                                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        for (MachinePanel* p : m_panels)
            p->cancelTransfer();
        waitForTransfersToStop(5000);
    }

    QString logError;
    if (!m_log->flushPending(&logError) && !m_confirmClose) {
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("Transfer log"),
            QStringLiteral("%1\n\nClose the file in the other program and click Retry, or click Ignore to exit "
                           "without writing these entries.")
                .arg(logError),
            QMessageBox::Retry | QMessageBox::Ignore, QMessageBox::Retry);
        if (answer == QMessageBox::Retry && !m_log->flushPending(&logError)) {
            updateLogWarning(logError);
            event->ignore();
            return;
        }
    }

    m_settings->setValue(QStringLiteral("window/geometry"), saveGeometry());
    m_settings->setValue(QStringLiteral("window/vsplit"), m_vsplit->saveState());
    m_settings->sync();
    event->accept();
}
