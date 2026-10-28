#include "MachinePanel.h"

#include "CodeView.h"
#include "MachineChannel.h"
#include "Theme.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMimeData>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QTabWidget>
#include <QTextCursor>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

using namespace nc;

namespace {

// Files up to this size are read whole: shown, counted and line-tracked.
// Bigger ones show and track their first part.
constexpr qint64 kPreviewLimit = 8LL * 1024 * 1024;
constexpr int kMaxDisplayLine = 2000; // longer lines are cut short, display only

QString number(qint64 n)
{
    return QLocale::system().toString(n);
}

QString humanSize(qint64 n)
{
    if (n < 1024)
        return QStringLiteral("%1 bytes").arg(n);
    if (n < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(QString::number(n / 1024.0, 'f', 1));
    return QStringLiteral("%1 MB").arg(QString::number(n / (1024.0 * 1024.0), 'f', 2));
}

QString css(const QColor& c)
{
    return c.name();
}

// Display-only: show CR LF, LF and lone CR all as line breaks.
QString toDisplayText(const QByteArray& bytes, bool* lastWasCR)
{
    QString out;
    out.reserve(bytes.size());
    bool prevCR = lastWasCR ? *lastWasCR : false;
    int column = 0;
    for (const char c : bytes) {
        if (c == '\r') {
            out.append(QLatin1Char('\n'));
            prevCR = true;
            column = 0;
            continue;
        }
        if (c == '\n') {
            if (!prevCR)
                out.append(QLatin1Char('\n'));
            prevCR = false;
            column = 0;
            continue;
        }
        prevCR = false;
        const uchar u = static_cast<uchar>(c);
        if (u < 0x20 && c != '\t')
            continue; // hide other control codes
        if (++column > kMaxDisplayLine) {
            // A real NC line is never this long (binary file?); a single
            // huge line makes the text box crawl.
            out.append(QLatin1Char('\n'));
            column = 1;
        }
        out.append(QChar::fromLatin1(c));
    }
    if (lastWasCR)
        *lastWasCR = prevCR;
    return out;
}

QPushButton* makeButton(const QString& text, const char* role, const QIcon& icon)
{
    auto* b = new QPushButton(icon, text);
    b->setProperty("size", "big");
    if (role)
        b->setProperty("role", role);
    b->setIconSize(QSize(18, 18));
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

QLabel* fieldLabel(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("fieldLabel"));
    return l;
}

// A label that shortens its text in the middle to fit, so a long folder
// path still shows its start and end. The full text is in the tooltip.
class ElidedLabel : public QLabel
{
public:
    void setFullText(const QString& text)
    {
        m_full = text;
        setToolTip(text);
        update();
    }
    QSize sizeHint() const override { return QSize(fontMetrics().horizontalAdvance(m_full), fontMetrics().height()); }
    QSize minimumSizeHint() const override { return QSize(0, fontMetrics().height()); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(m_full, Qt::ElideMiddle, width()));
    }

private:
    QString m_full;
};

QLabel* mutedLabel()
{
    auto* l = new QLabel;
    l->setProperty("muted", true);
    return l;
}

} // namespace

MachinePanel::MachinePanel(const QString& title, const QString& settingsKey, int defaultProfileIndex,
                           QSettings* settings, QWidget* parent)
    : QFrame(parent)
    , m_settings(settings)
    , m_title(title)
    , m_key(settingsKey)
    , m_defaultProfileIndex(defaultProfileIndex)
{
    setObjectName(QStringLiteral("machineCard"));
    setAcceptDrops(true);
    m_channel = new MachineChannel(title, this);

    // ---- header: machine name and a big state badge
    m_titleLabel = new QLabel(title);
    m_titleLabel->setObjectName(QStringLiteral("cardTitle"));
    m_badge = new QLabel;
    m_badge->setObjectName(QStringLiteral("stateBadge"));
    m_badge->setAlignment(Qt::AlignCenter);
    m_badge->setMinimumWidth(110);

    // ---- profile and file
    m_profileCombo = new QComboBox;
    m_profileCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_profileCombo->setMinimumContentsLength(20);
    m_profileSummary = mutedLabel();
    m_profileSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_portHint = new QLabel;

    m_fileName = new QLabel;
    m_fileName->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_fileName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* folder = new ElidedLabel;
    folder->setProperty("muted", true);
    folder->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_fileFolder = folder;
    m_browseBtn = new QPushButton(Theme::icon(Theme::Icon::Folder, Theme::Color::muted), QStringLiteral("Browse..."));
    m_browseBtn->setToolTip(QStringLiteral("Choose the program to send (you can also drag a file onto this panel)"));
    m_reloadBtn = new QPushButton(Theme::icon(Theme::Icon::Refresh, Theme::Color::muted), QString());
    m_reloadBtn->setToolTip(QStringLiteral("Reload: read the file from disk again"));
    for (QPushButton* b : {m_browseBtn, m_reloadBtn})
        b->setIconSize(QSize(16, 16));
    m_fileInfo = mutedLabel();
    m_fileInfo->setWordWrap(true);

    // ---- program view
    m_preview = new CodeView;
    m_preview->setAcceptDrops(false);
    m_received = new CodeView;
    m_received->setAcceptDrops(false);
    m_received->setPlaceholderText(QStringLiteral("Data punched out from the control appears here live during a Receive."));
    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(false);
    m_tabs->addTab(m_preview, QStringLiteral("Program"));
    m_tabs->addTab(m_received, QStringLiteral("Received data"));
    m_followBtn = new QToolButton;
    m_followBtn->setObjectName(QStringLiteral("followButton"));
    m_followBtn->setText(QStringLiteral("Follow line"));
    m_followBtn->setCheckable(true);
    m_followBtn->setChecked(true);
    m_followBtn->setToolTip(QStringLiteral("Keep the line being sent in view"));
    m_tabs->setCornerWidget(m_followBtn, Qt::TopRightCorner);

    // ---- actions
    const QColor white(Qt::white);
    const QColor paleWhite(0xf2, 0xf6, 0xfd);
    m_sendBtn = makeButton(QStringLiteral("Send"), "primary", Theme::icon(Theme::Icon::Send, white, paleWhite));
    m_sendBtn->setToolTip(QStringLiteral("Send the selected program into the control's memory"));
    m_dripBtn = makeButton(QStringLiteral("Drip Feed"), "drip", Theme::icon(Theme::Icon::Drip, white, paleWhite));
    m_dripBtn->setToolTip(QStringLiteral(
        "Run the program straight from this PC, for programs too big for the control's memory.\n"
        "Put the machine in Remote (RMT/DNC) mode and press Cycle Start. Every Cycle Start runs the "
        "program again from the top until you click Cancel.\n"
        "Restart: if you Reset the machine part way through, click Restart before the next Cycle Start."));
    m_receiveBtn = makeButton(QStringLiteral("Receive"), nullptr, Theme::icon(Theme::Icon::Receive, Theme::Color::text));
    m_receiveBtn->setToolTip(QStringLiteral("Listen for a program punched out from the control"));
    m_cancelBtn = makeButton(QStringLiteral("Cancel"), "danger", Theme::icon(Theme::Icon::Stop, Theme::Color::danger));
    m_cancelBtn->setToolTip(QStringLiteral("Stop the transfer now and free the port"));
    m_folderBtn = new QPushButton(Theme::icon(Theme::Icon::Folder, Theme::Color::muted), QStringLiteral("Received Folder"));
    m_folderBtn->setIconSize(QSize(16, 16));
    m_folderBtn->setToolTip(QStringLiteral("Open the folder where received programs are saved"));

    m_progress = new QProgressBar;
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(false); // the text sits above the bar, where it's always readable
    m_progress->setFormat(QString());
    m_progressLine = new QLabel;
    m_progressLine->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_progressBytes = mutedLabel();

    m_status = mutedLabel();
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setMinimumHeight(m_status->fontMetrics().height() * 2 + 4);
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    // ---- layout
    auto* header = new QHBoxLayout;
    header->addWidget(m_titleLabel);
    header->addStretch(1);
    header->addWidget(m_badge);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(4);
    grid->addWidget(fieldLabel(QStringLiteral("Profile")), 0, 0);
    grid->addWidget(m_profileCombo, 0, 1);
    auto* profileInfo = new QHBoxLayout;
    profileInfo->addWidget(m_profileSummary);
    profileInfo->addSpacing(16);
    profileInfo->addStretch(1);
    profileInfo->addWidget(m_portHint);
    grid->addLayout(profileInfo, 1, 1);
    grid->setRowMinimumHeight(2, 6);
    auto* fileRow = new QHBoxLayout;
    fileRow->setSpacing(10);
    fileRow->addWidget(m_browseBtn);
    fileRow->addWidget(m_fileName);
    fileRow->addWidget(m_fileFolder, 1);
    fileRow->addWidget(m_reloadBtn);
    grid->addWidget(fieldLabel(QStringLiteral("Program")), 3, 0);
    grid->addLayout(fileRow, 3, 1);
    grid->addWidget(m_fileInfo, 4, 1);
    grid->setColumnStretch(1, 1);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(m_sendBtn);
    buttons->addWidget(m_dripBtn);
    buttons->addWidget(m_receiveBtn);
    buttons->addStretch(1);
    buttons->addWidget(m_folderBtn);
    buttons->addWidget(m_cancelBtn);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(16, 12, 16, 14);
    v->setSpacing(8);
    v->addLayout(header);
    v->addLayout(grid);
    v->addWidget(m_tabs, 1);
    v->addLayout(buttons);
    auto* progressText = new QHBoxLayout;
    progressText->addWidget(m_progressLine);
    progressText->addStretch(1);
    progressText->addWidget(m_progressBytes);
    v->addSpacing(2);
    v->addLayout(progressText);
    v->addWidget(m_progress);
    v->addWidget(m_status);

    connect(m_profileCombo, &QComboBox::currentIndexChanged, this, [this] { onProfileChanged(); });
    connect(m_browseBtn, &QPushButton::clicked, this, &MachinePanel::browseFile);
    connect(m_reloadBtn, &QPushButton::clicked, this, &MachinePanel::loadPreview);
    connect(m_followBtn, &QToolButton::toggled, m_preview, &CodeView::setFollow);

    // Object names let the automated GUI tests find each control.
    m_profileCombo->setObjectName(QStringLiteral("profileCombo"));
    m_profileCombo->setPlaceholderText(QStringLiteral("Choose a profile..."));
    m_fileName->setObjectName(QStringLiteral("fileName"));
    m_fileFolder->setObjectName(QStringLiteral("fileFolder"));
    m_fileInfo->setObjectName(QStringLiteral("fileInfo"));
    m_preview->setObjectName(QStringLiteral("preview"));
    m_received->setObjectName(QStringLiteral("received"));
    m_sendBtn->setObjectName(QStringLiteral("sendButton"));
    m_dripBtn->setObjectName(QStringLiteral("dripButton"));
    m_receiveBtn->setObjectName(QStringLiteral("receiveButton"));
    m_cancelBtn->setObjectName(QStringLiteral("cancelButton"));
    m_folderBtn->setObjectName(QStringLiteral("folderButton"));
    m_progress->setObjectName(QStringLiteral("progress"));
    m_status->setObjectName(QStringLiteral("status"));
    m_portHint->setObjectName(QStringLiteral("portHint"));
    connect(m_sendBtn, &QPushButton::clicked, this, &MachinePanel::startSend);
    connect(m_dripBtn, &QPushButton::clicked, this, &MachinePanel::onDripButton);
    connect(m_receiveBtn, &QPushButton::clicked, this, &MachinePanel::startReceive);
    connect(m_cancelBtn, &QPushButton::clicked, this, &MachinePanel::cancelTransfer);
    connect(m_folderBtn, &QPushButton::clicked, this, &MachinePanel::openReceiveFolder);

    connect(m_channel, &MachineChannel::stateChanged, this, &MachinePanel::onStateChanged);
    connect(m_channel, &MachineChannel::progress, this, &MachinePanel::onProgress);
    connect(m_channel, &MachineChannel::dataReceived, this, &MachinePanel::onDataReceived);
    connect(m_channel, &MachineChannel::runFinished, this, &MachinePanel::onRunFinished);
    connect(m_channel, &MachineChannel::finished, this, &MachinePanel::onFinished);

    {
        // Show the file this machine used last, without reading it yet:
        // setProfiles() loads it once the profile is known.
        const QString last = m_settings->value(m_key + QStringLiteral("/file")).toString().trimmed();
        m_filePath = last.isEmpty() ? QString() : QDir::toNativeSeparators(last);
        updateFileLabel();
    }
    m_barState = TransferState::Error; // force the first progress colour
    refreshStatus();
    updateControls();
}

bool MachinePanel::isBusy() const
{
    return m_channel->isBusy();
}

QString MachinePanel::busyPortName() const
{
    return m_channel->activePortName();
}

void MachinePanel::cancelTransfer()
{
    m_dripRestart = false;
    m_channel->cancel();
}

std::optional<MachineProfile> MachinePanel::currentProfile() const
{
    const int idx = m_profileCombo->currentIndex();
    if (idx < 0 || idx >= m_profiles.size())
        return std::nullopt;
    return m_profiles.at(idx);
}

void MachinePanel::setProfiles(const QList<MachineProfile>& profiles, const QHash<QString, QString>& renames)
{
    QString wanted = m_profileCombo->currentIndex() >= 0 ? m_profileCombo->currentText() : QString();
    if (wanted.isEmpty())
        wanted = m_settings->value(m_key + QStringLiteral("/profile")).toString();
    if (renames.contains(wanted))
        wanted = renames.value(wanted);

    m_profiles = profiles;
    QString lostProfile;
    {
        const QSignalBlocker blocker(m_profileCombo);
        m_profileCombo->clear();
        for (const MachineProfile& p : profiles)
            m_profileCombo->addItem(p.name);

        int idx = wanted.isEmpty() ? -1 : m_profileCombo->findText(wanted, Qt::MatchFixedString);
        if (idx < 0 && wanted.isEmpty() && !profiles.isEmpty()) {
            // Nothing chosen yet (first run): suggest a default.
            idx = qMin(m_defaultProfileIndex, int(profiles.size()) - 1);
        } else if (idx < 0 && !wanted.isEmpty()) {
            // The profile this machine was using is gone. Never quietly pick
            // another one: it may be the other machine's, and the next Send
            // would go to the wrong control.
            lostProfile = wanted;
        }
        m_profileCombo->setCurrentIndex(idx);
    }
    onProfileChanged();
    if (!lostProfile.isEmpty())
        showError(QStringLiteral("Profile '%1' no longer exists. Choose the profile for this machine.").arg(lostProfile));
}

void MachinePanel::setAvailablePorts(const QStringList& ports)
{
    m_ports = ports;
    updatePortHint();
}

void MachinePanel::onProfileChanged()
{
    const auto p = currentProfile();
    if (p) {
        m_settings->setValue(m_key + QStringLiteral("/profile"), p->name);
        QString summary = p->summary();
        summary.replace(QStringLiteral("  "), QStringLiteral("  \u00b7  "));
        m_profileSummary->setText(summary);
    } else if (m_profiles.isEmpty()) {
        m_profileSummary->setText(QStringLiteral("No profiles - use Profiles > Edit Profiles to add one."));
    } else {
        m_profileSummary->setText(QStringLiteral("No profile selected."));
    }
    updatePortHint();
    // Only touch the disk when the file changed, not on every profile change
    // (a missing network path can block for many seconds).
    if (m_previewPath != m_filePath)
        loadPreview();
    else
        updateFileInfo();
    clearLocalError();
    updateControls();
}

void MachinePanel::clearLocalError()
{
    if (m_localError && !m_channel->isBusy()) {
        m_localError = false;
        m_state = TransferState::Idle;
        m_stateMessage.clear();
        refreshStatus();
    }
}

void MachinePanel::updatePortHint()
{
    const auto p = currentProfile();
    if (!p) {
        m_portHint->clear();
        return;
    }
    bool present = false;
    for (const QString& port : m_ports) {
        if (port.compare(p->portName, Qt::CaseInsensitive) == 0)
            present = true;
    }
    if (present) {
        m_portHint->setText(QStringLiteral("\u25cf %1 connected").arg(p->portName));
        m_portHint->setToolTip(QStringLiteral("%1 is present on this PC").arg(p->portName));
        m_portHint->setStyleSheet(QStringLiteral("color: %1;").arg(css(Theme::Color::success)));
    } else {
        m_portHint->setText(QStringLiteral("\u25cf %1 not found").arg(p->portName));
        m_portHint->setToolTip(QStringLiteral("%1 is not on this PC. Plug in the USB-serial adapter, then click "
                                              "Refresh Ports (F5).")
                                   .arg(p->portName));
        m_portHint->setStyleSheet(QStringLiteral("color: %1;").arg(css(Theme::Color::warning)));
    }
}

void MachinePanel::browseFile()
{
    QString dir;
    const QFileInfo current(m_filePath);
    if (!m_filePath.isEmpty() && current.absoluteDir().exists())
        dir = current.absolutePath();
    else if (const auto p = currentProfile(); p && !p->programFolder.isEmpty())
        dir = p->programFolder;

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose program for %1").arg(title()), dir,
        QStringLiteral("NC programs (*.nc *.txt *.tap *.cnc *.ncc *.min *.mpf *.eia *.iso *.prg *.ptp);;All files (*)"));
    if (!path.isEmpty())
        setProgramFile(path);
}

void MachinePanel::setProgramFile(const QString& path)
{
    m_filePath = path.trimmed().isEmpty() ? QString() : QDir::toNativeSeparators(path.trimmed());
    m_settings->setValue(m_key + QStringLiteral("/file"), m_filePath);
    updateFileLabel();
    loadPreview();
    clearLocalError();
}

void MachinePanel::updateFileLabel()
{
    auto* folder = static_cast<ElidedLabel*>(m_fileFolder);
    if (m_filePath.isEmpty()) {
        m_fileName->setText(QStringLiteral("No program chosen"));
        m_fileName->setStyleSheet(QStringLiteral("color: %1;").arg(css(Theme::Color::muted)));
        folder->setFullText(QStringLiteral("Browse, or drag a file onto this panel"));
        return;
    }
    const QFileInfo fi(m_filePath);
    m_fileName->setText(fi.fileName().isEmpty() ? m_filePath : fi.fileName());
    m_fileName->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_fileName->setToolTip(m_filePath);
    folder->setFullText(QDir::toNativeSeparators(fi.absolutePath()));
}

void MachinePanel::loadPreview()
{
    m_previewRaw.clear();
    m_previewOk = false;
    m_previewTruncated = false;
    m_previewProblem.clear();
    m_previewModified = QDateTime();
    m_previewFileSize = 0;
    m_previewLines = -1;
    m_lineLengths.clear();
    m_lineEnds.clear();
    m_percentPlan = {};
    m_wireKey.clear();
    m_preview->setActiveLine(-1);
    m_preview->clear();

    const QString path = m_filePath;
    m_previewPath = path;
    if (path.isEmpty()) {
        updateFileInfo();
        return;
    }
    const QFileInfo fi(path);
    if (!fi.exists())
        m_previewProblem = QStringLiteral("File not found: %1").arg(path);
    else if (!fi.isFile())
        m_previewProblem = QStringLiteral("That is a folder, not a program file: %1").arg(path);
    if (!m_previewProblem.isEmpty()) {
        updateFileInfo();
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_previewProblem = QStringLiteral("Cannot open file: %1").arg(f.errorString());
        updateFileInfo();
        return;
    }
    m_previewFileSize = fi.size();
    m_previewModified = fi.lastModified();
    m_previewRaw = f.read(kPreviewLimit);
    m_previewOk = true;
    m_previewTruncated = m_previewFileSize > kPreviewLimit;

    // The view shows exactly the lines the formatter sends, one per line, so
    // "bytes sent" maps straight onto a line of the view.
    const QList<QByteArray> lines = ProgramFormatter::splitLines(ProgramFormatter::cleaned(m_previewRaw));
    ProgramFormatter::PercentPlanner planner;
    QByteArray shown;
    shown.reserve(m_previewRaw.size() + 16);
    m_lineLengths.reserve(lines.size());
    for (const QByteArray& line : lines) {
        planner.addLine(line);
        m_lineLengths.append(line.size());
        int column = 0;
        for (const char c : line) {
            const uchar u = static_cast<uchar>(c);
            if (u < 0x20 && c != '\t')
                continue; // hide control codes
            if (++column > kMaxDisplayLine) {
                shown.append("...");
                break;
            }
            shown.append(c);
        }
        shown.append('\n');
    }
    if (!shown.isEmpty())
        shown.chop(1);
    m_percentPlan = planner.plan();
    if (!m_previewTruncated)
        m_previewLines = int(lines.size());
    m_preview->setPlainText(QString::fromLatin1(shown));
    m_tabs->setCurrentWidget(m_preview);
    updateFileInfo();
}

void MachinePanel::updateFileInfo()
{
    const QString path = m_filePath;
    if (path.isEmpty()) {
        m_fileInfo->clear(); // the file row already says no program is chosen
        m_fileInfo->setStyleSheet(QString());
        return;
    }
    if (!m_previewOk) {
        m_fileInfo->setText(m_previewProblem.isEmpty() ? QStringLiteral("File not found: %1").arg(path) : m_previewProblem);
        m_fileInfo->setStyleSheet(QStringLiteral("color: %1;").arg(css(Theme::Color::danger)));
        return;
    }

    const QString sep = QStringLiteral("   \u00b7   ");
    QString text = m_previewLines >= 0
                       ? QStringLiteral("%1 lines%2%3 on disk").arg(number(m_previewLines), sep, humanSize(m_previewFileSize))
                       : QStringLiteral("%1 on disk").arg(humanSize(m_previewFileSize));
    if (const auto p = currentProfile()) {
        // Only the formatting settings matter; cache so switching profiles is instant.
        const QString key = QStringLiteral("%1|%2|%3|%4").arg(toString(p->lineEnding)).arg(p->ensurePercent).arg(p->leaderCount).arg(p->trailerCount);
        if (key != m_wireKey) {
            m_wireKey = key;
            m_lineEnds = ProgramFormatter::lineEndOffsets(m_lineLengths, m_percentPlan, *p);
            if (!m_previewTruncated)
                m_wireBytes = ProgramFormatter::format(m_previewRaw, *p).size();
        }
        if (!m_previewTruncated)
            text += sep + QStringLiteral("%1 bytes will be sent with this profile").arg(number(m_wireBytes));
    }
    if (m_previewTruncated)
        text += sep + QStringLiteral("showing the first 8 MB");
    if (m_previewFileSize > kMaxNormalSendBytes)
        text += sep + QStringLiteral("too large for Send - use Drip Feed");
    m_fileInfo->setText(text);
    m_fileInfo->setStyleSheet(QString());
}

void MachinePanel::updateControls()
{
    const bool busy = m_channel->isBusy();
    const bool haveProfile = currentProfile().has_value();
    m_profileCombo->setEnabled(!busy);
    m_browseBtn->setEnabled(!busy);
    m_reloadBtn->setEnabled(!busy);
    m_sendBtn->setEnabled(!busy && haveProfile);
    // While a drip feed is armed this button restarts it from the top.
    const bool restart = busy && m_dripActive;
    m_dripBtn->setText(restart ? QStringLiteral("Restart") : QStringLiteral("Drip Feed"));
    m_dripBtn->setIcon(Theme::icon(restart ? Theme::Icon::Refresh : Theme::Icon::Drip, Qt::white, QColor(0xf2, 0xf6, 0xfd)));
    m_dripBtn->setEnabled(busy ? m_dripActive && !m_dripRestart : haveProfile);
    m_receiveBtn->setEnabled(!busy && haveProfile);
    m_cancelBtn->setEnabled(busy);
    m_folderBtn->setEnabled(haveProfile);
    setAcceptDrops(!busy);
}

void MachinePanel::showError(const QString& message)
{
    m_localError = true; // cleared when the user changes something or a transfer starts
    m_state = TransferState::Error;
    m_stateMessage = message;
    refreshStatus();
}

std::optional<MachineProfile> MachinePanel::checkCanSend(const QString& path)
{
    const auto p = currentProfile();
    if (!p) {
        showError(QStringLiteral("Choose a machine profile first."));
        return std::nullopt;
    }
    if (const QString err = p->validate(); !err.isEmpty()) {
        showError(err);
        return std::nullopt;
    }
    if (path.isEmpty()) {
        showError(QStringLiteral("Choose a program file to send first."));
        return std::nullopt;
    }
    if (!QFileInfo::exists(path)) {
        loadPreview();
        showError(QStringLiteral("File not found: %1").arg(path));
        return std::nullopt;
    }
    if (const QString other = m_portCheck ? m_portCheck(p->portName) : QString(); !other.isEmpty()) {
        showError(QStringLiteral("%1 is in use by %2 right now. Each machine needs its own COM port.")
                      .arg(p->portName, other));
        return std::nullopt;
    }
    return p;
}

void MachinePanel::beginSend(const MachineProfile& profile, const SendRequest& request)
{
    loadPreview(); // show exactly what is on disk now
    m_settings->setValue(m_key + QStringLiteral("/file"), request.filePath);
    m_sendingPreview = m_previewOk;

    m_localError = false;
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    setProgressText(QStringLiteral("Starting..."), QString());
    m_channel->startSend(profile, request);
    updateControls();
}

void MachinePanel::startSend()
{
    if (m_channel->isBusy())
        return;
    const QString path = m_filePath;
    if (const auto p = checkCanSend(path))
        beginSend(*p, SendRequest{path});
}

void MachinePanel::onDripButton()
{
    if (m_channel->isBusy()) {
        // Restart: drop whatever is left of this run and arm again from the
        // top once the cancel has gone through (see onFinished).
        if (m_dripActive && !m_dripRestart) {
            m_dripRestart = true;
            m_channel->cancel();
            updateControls();
        }
        return;
    }
    const QString path = m_filePath;
    const auto p = checkCanSend(path);
    if (!p)
        return;
    if (p->flowControl == FlowControl::None) {
        // Without flow control the PC would keep sending after the control's
        // buffer is full, and the machine would alarm part way through a cut.
        showError(QStringLiteral("Drip feed needs flow control so the machine can hold the PC back. Set this "
                                 "profile's flow control to XON/XOFF (Profiles > Edit Profiles)."));
        return;
    }
    m_dripActive = true;
    m_dripRuns = 0;
    beginSend(*p, SendRequest{path, SendRequest::Mode::DripFeed});
}

void MachinePanel::startReceive()
{
    if (m_channel->isBusy())
        return;
    const auto p = currentProfile();
    if (!p) {
        showError(QStringLiteral("Choose a machine profile first."));
        return;
    }
    if (const QString err = p->validate(); !err.isEmpty()) {
        showError(err);
        return;
    }
    if (const QString other = m_portCheck ? m_portCheck(p->portName) : QString(); !other.isEmpty()) {
        showError(QStringLiteral("%1 is in use by %2 right now. Each machine needs its own COM port.")
                      .arg(p->portName, other));
        return;
    }

    m_received->clear();
    m_rxLastWasCR = false;
    m_rxBytes = 0;
    m_sendingPreview = false;
    m_tabs->setCurrentWidget(m_received);
    m_localError = false;
    setProgressText(QStringLiteral("Receiving..."), QString());
    m_progress->setRange(0, 0); // busy indicator until done
    m_channel->startReceive(*p);
    updateControls();
}

void MachinePanel::openReceiveFolder()
{
    const auto p = currentProfile();
    if (!p)
        return;
    const QString folder = p->effectiveReceiveFolder();
    QDir().mkpath(folder);
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void MachinePanel::refreshStatus()
{
    const Theme::StateColors colors = Theme::stateColors(m_state);
    QString badge = toString(m_state);
    if (m_dripActive && m_state == TransferState::WaitingForXon)
        badge = QStringLiteral("Ready");
    m_badge->setText(badge);
    m_badge->setStyleSheet(QStringLiteral("QLabel { background: %1; color: %2; border-radius: 13px; "
                                          "padding: 4px 14px; font-weight: 600; font-size: 10.5pt; }")
                               .arg(css(colors.background), css(colors.text)));

    QString text = m_stateMessage;
    if (text.isEmpty() && m_state == TransferState::Idle)
        text = QStringLiteral("Choose a program, then Send, Drip Feed or Receive.");
    if (m_state == TransferState::Receiving && m_rxBytes > 0)
        text += QStringLiteral("  (%1 bytes)").arg(number(m_rxBytes));
    if (m_dripActive && m_dripRuns > 0)
        text += QStringLiteral("   \u00b7   runs completed: %1").arg(m_dripRuns);
    m_status->setText(text);
    const bool loud = m_state == TransferState::Error || m_state == TransferState::Done
                      || m_state == TransferState::WaitingForXon || m_state == TransferState::Paused;
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(css(loud ? colors.text : Theme::Color::muted)));

    if (m_state != m_barState) {
        m_barState = m_state;
        m_progress->setStyleSheet(QStringLiteral("QProgressBar::chunk { background: %1; border-radius: 7px; }")
                                      .arg(css(colors.bar)));
    }
}

void MachinePanel::setProgressText(const QString& line, const QString& bytes)
{
    m_progressLine->setText(line);
    m_progressBytes->setText(bytes);
    // Kept on the bar too, for screen readers and the GUI tests.
    m_progress->setFormat(line.isEmpty() || bytes.isEmpty() ? line + bytes : line + QStringLiteral("  ") + bytes);
}

void MachinePanel::trackLine(qint64 bytesSent)
{
    const bool live = m_state == TransferState::Sending || m_state == TransferState::Paused
                      || m_state == TransferState::Finishing;
    if (!m_sendingPreview || !live || m_lineEnds.isEmpty()) {
        m_preview->setActiveLine(-1);
        return;
    }
    // The line being sent is the first one whose end hasn't gone out yet.
    const auto it = std::upper_bound(m_lineEnds.cbegin(), m_lineEnds.cend(), bytesSent);
    m_preview->setActiveLine(it == m_lineEnds.cend() ? -1 : int(it - m_lineEnds.cbegin()));
}

void MachinePanel::onStateChanged(TransferState state, const QString& message)
{
    m_localError = false;
    m_state = state;
    m_stateMessage = message;
    refreshStatus();
    if (state == TransferState::Sending)
        m_tabs->setCurrentWidget(m_preview);
    if (m_channel->activeDirection() == TransferDirection::Send)
        trackLine(m_progress->maximum() > 1 ? m_progress->value() : 0);
}

void MachinePanel::onProgress(qint64 done, qint64 total)
{
    if (total > 0) {
        if (m_progress->maximum() != int(total))
            m_progress->setRange(0, int(total));
        m_progress->setValue(int(qMin(done, total)));
        const int percent = int(qMin<qint64>(100, done * 100 / total));
        const QString bytes = QStringLiteral("%1 / %2 bytes  (%3%)").arg(number(done), number(total)).arg(percent);
        trackLine(done);
        QString line;
        if (const int active = m_preview->activeLine(); active >= 0) {
            const QString of = m_previewLines >= 0 ? QStringLiteral(" of %1").arg(number(m_previewLines)) : QString();
            line = QStringLiteral("Line %1%2").arg(number(active + 1), of);
        }
        setProgressText(line, bytes);
    } else {
        m_rxBytes = done;
        refreshStatus();
    }
}

void MachinePanel::onDataReceived(const QByteArray& chunk)
{
    QScrollBar* sb = m_received->verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 2;
    QTextCursor cursor = m_received->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(toDisplayText(chunk, &m_rxLastWasCR));
    if (atBottom)
        sb->setValue(sb->maximum());
}

void MachinePanel::onRunFinished(const TransferResult& result)
{
    ++m_dripRuns;
    onProgress(result.bytesTransferred, result.bytesTotal);
    refreshStatus();
    emit transferFinished(result); // logged like any other send

    // The next run re-reads the file; show the new version if it changed.
    const QFileInfo fi(m_previewPath);
    if (fi.lastModified() != m_previewModified || fi.size() != m_previewFileSize) {
        loadPreview();
        updateFileInfo();
        m_sendingPreview = m_previewOk;
    }
}

void MachinePanel::onFinished(const TransferResult& result)
{
    m_dripActive = false;
    m_sendingPreview = false;
    m_preview->setActiveLine(-1);
    if (result.direction == TransferDirection::Receive) {
        m_rxBytes = result.bytesTransferred;
        m_progress->setRange(0, 1);
        m_progress->setValue(result.outcome == TransferOutcome::Success ? 1 : 0);
        setProgressText(QString(), QStringLiteral("%1 bytes received").arg(number(result.bytesTransferred)));
    } else if (result.bytesTotal > 0) {
        onProgress(result.bytesTransferred, result.bytesTotal);
    } else {
        // Failed before anything could be sent (bad file, port problem).
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
        setProgressText(QString(), QStringLiteral("Nothing sent"));
    }
    refreshStatus();
    updateControls();
    emit transferFinished(result);

    if (m_dripRestart) {
        m_dripRestart = false;
        onDripButton();
    }
}

void MachinePanel::dragEnterEvent(QDragEnterEvent* event)
{
    const QMimeData* mime = event->mimeData();
    if (!m_channel->isBusy() && mime->hasUrls() && !mime->urls().isEmpty() && mime->urls().first().isLocalFile())
        event->acceptProposedAction();
}

void MachinePanel::dropEvent(QDropEvent* event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty() || m_channel->isBusy())
        return;
    setProgramFile(urls.first().toLocalFile());
    event->acceptProposedAction();
}
