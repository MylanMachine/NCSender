#include "ProfileEditorDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace nc;

namespace {

QSpinBox* makeSpin(int min, int max, const QString& suffix = QString())
{
    auto* s = new QSpinBox;
    s->setRange(min, max);
    if (!suffix.isEmpty())
        s->setSuffix(suffix);
    return s;
}

QSpinBox* makeHexSpin()
{
    auto* s = new QSpinBox;
    s->setRange(0, 255);
    s->setDisplayIntegerBase(16);
    s->setPrefix(QStringLiteral("0x"));
    s->setToolTip(QStringLiteral("Character code in hex. 0x00 = NUL (tape feed), 0x20 = space"));
    return s;
}

QWidget* withBrowse(QLineEdit* edit, QPushButton* button)
{
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(edit, 1);
    h->addWidget(button);
    return w;
}

template <typename Enum>
void selectData(QComboBox* combo, Enum value)
{
    const int idx = combo->findData(static_cast<int>(value));
    combo->setCurrentIndex(idx < 0 ? 0 : idx);
}

template <typename Enum>
Enum currentEnum(const QComboBox* combo)
{
    return static_cast<Enum>(combo->currentData().toInt());
}

} // namespace

ProfileEditorDialog::ProfileEditorDialog(const QList<MachineProfile>& profiles, const QStringList& ports, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Machine Profiles"));
    resize(900, 720);

    for (const MachineProfile& p : profiles)
        m_entries.append(Entry{p, p.name});

    m_list = new QListWidget;
    m_list->setMinimumWidth(220);
    for (const Entry& e : m_entries)
        m_list->addItem(e.profile.name);

    m_list->setObjectName(QStringLiteral("profileList"));
    m_addBtn = new QPushButton(QStringLiteral("Add"));
    m_addBtn->setObjectName(QStringLiteral("addButton"));
    m_dupBtn = new QPushButton(QStringLiteral("Duplicate"));
    m_dupBtn->setObjectName(QStringLiteral("duplicateButton"));
    m_delBtn = new QPushButton(QStringLiteral("Delete"));
    m_delBtn->setObjectName(QStringLiteral("deleteButton"));
    auto* listButtons = new QHBoxLayout;
    listButtons->addWidget(m_addBtn);
    listButtons->addWidget(m_dupBtn);
    listButtons->addWidget(m_delBtn);

    auto* left = new QVBoxLayout;
    left->addWidget(m_list, 1);
    left->addLayout(listButtons);

    m_form = buildForm();
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_form);

    // Available ports in the port combo; the user can also type any name.
    for (const QString& p : ports)
        m_port->addItem(p);

    auto* body = new QHBoxLayout;
    body->addLayout(left);
    body->addWidget(scroll, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProfileEditorDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ProfileEditorDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addLayout(body, 1);
    root->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, &ProfileEditorDialog::onRowChanged);
    connect(m_addBtn, &QPushButton::clicked, this, &ProfileEditorDialog::addProfile);
    connect(m_dupBtn, &QPushButton::clicked, this, &ProfileEditorDialog::duplicateProfile);
    connect(m_delBtn, &QPushButton::clicked, this, &ProfileEditorDialog::deleteProfile);
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (m_currentRow >= 0 && m_list->item(m_currentRow))
            m_list->item(m_currentRow)->setText(text);
    });

    if (!m_entries.isEmpty())
        m_list->setCurrentRow(0);
    updateButtons();
}

QWidget* ProfileEditorDialog::buildForm()
{
    auto* form = new QWidget;

    // ---- connection
    m_name = new QLineEdit;
    m_name->setObjectName(QStringLiteral("nameEdit"));
    m_port = new QComboBox;
    m_port->setObjectName(QStringLiteral("portCombo"));
    m_port->setEditable(true);
    // No inline autocompletion: typing COM1 must not silently become COM10.
    m_port->setCompleter(nullptr);
    m_port->setToolTip(QStringLiteral("Pick a detected port or type a name such as COM3"));
    m_baud = new QComboBox;
    m_baud->setObjectName(QStringLiteral("baudCombo"));
    m_baud->setEditable(true);
    m_baud->setCompleter(nullptr);
    // C locale: plain digits only, so "9,600" is rejected instead of becoming 0.
    auto* baudValidator = new QIntValidator(50, 4000000, m_baud);
    QLocale plain = QLocale::c();
    plain.setNumberOptions(QLocale::RejectGroupSeparator);
    baudValidator->setLocale(plain);
    m_baud->setValidator(baudValidator);
    for (qint32 b : standardBaudRates())
        m_baud->addItem(QString::number(b));
    m_dataBits = new QComboBox;
    for (int bits : {8, 7, 6, 5})
        m_dataBits->addItem(QString::number(bits), bits);
    m_parity = new QComboBox;
    for (Parity v : allParities())
        m_parity->addItem(displayName(v), static_cast<int>(v));
    m_stopBits = new QComboBox;
    for (StopBits v : allStopBits())
        m_stopBits->addItem(displayName(v), static_cast<int>(v));
    m_flow = new QComboBox;
    for (FlowControl v : allFlowControls())
        m_flow->addItem(displayName(v), static_cast<int>(v));

    auto* conn = new QGroupBox(QStringLiteral("Connection"));
    auto* connForm = new QFormLayout(conn);
    connForm->addRow(QStringLiteral("Profile name:"), m_name);
    connForm->addRow(QStringLiteral("COM port:"), m_port);
    connForm->addRow(QStringLiteral("Baud rate:"), m_baud);
    connForm->addRow(QStringLiteral("Data bits:"), m_dataBits);
    connForm->addRow(QStringLiteral("Parity:"), m_parity);
    connForm->addRow(QStringLiteral("Stop bits:"), m_stopBits);
    connForm->addRow(QStringLiteral("Flow control:"), m_flow);

    // ---- sending
    m_eol = new QComboBox;
    for (LineEnding v : allLineEndings())
        m_eol->addItem(displayName(v), static_cast<int>(v));
    m_ensurePercent = new QCheckBox(QStringLiteral("Add % at start and end of program if missing"));
    m_leaderByte = makeHexSpin();
    m_leaderCount = makeSpin(0, 10000, QStringLiteral(" chars"));
    m_trailerByte = makeHexSpin();
    m_trailerCount = makeSpin(0, 10000, QStringLiteral(" chars"));
    m_charDelay = makeSpin(0, 10000, QStringLiteral(" ms"));
    m_charDelay->setSpecialValueText(QStringLiteral("None (full speed)"));
    m_waitXon = new QCheckBox(QStringLiteral("Wait for XON from the control before sending"));
    m_waitXon->setToolTip(QStringLiteral("Click Send first, then start READ/INPUT at the control. The transfer starts "
                                         "when the control sends XON (DC1).\nLeave off if you put the control in "
                                         "receive mode before clicking Send."));
    m_xonTimeout = makeSpin(0, 86400, QStringLiteral(" s"));
    m_xonTimeout->setSpecialValueText(QStringLiteral("Wait until cancelled"));
    m_programFolder = new QLineEdit;
    auto* progBrowse = new QPushButton(QStringLiteral("Browse..."));
    connect(progBrowse, &QPushButton::clicked, this, [this] {
        const QString d = browseFolder(m_programFolder->text());
        if (!d.isEmpty())
            m_programFolder->setText(d);
    });
    connect(m_waitXon, &QCheckBox::toggled, m_xonTimeout, &QWidget::setEnabled);

    auto* leaderRow = new QWidget;
    auto* leaderLay = new QHBoxLayout(leaderRow);
    leaderLay->setContentsMargins(0, 0, 0, 0);
    leaderLay->addWidget(m_leaderCount);
    leaderLay->addWidget(new QLabel(QStringLiteral("of")));
    leaderLay->addWidget(m_leaderByte);
    leaderLay->addStretch(1);
    auto* trailerRow = new QWidget;
    auto* trailerLay = new QHBoxLayout(trailerRow);
    trailerLay->setContentsMargins(0, 0, 0, 0);
    trailerLay->addWidget(m_trailerCount);
    trailerLay->addWidget(new QLabel(QStringLiteral("of")));
    trailerLay->addWidget(m_trailerByte);
    trailerLay->addStretch(1);

    auto* send = new QGroupBox(QStringLiteral("Sending"));
    auto* sendForm = new QFormLayout(send);
    sendForm->addRow(QStringLiteral("Line ending:"), m_eol);
    sendForm->addRow(QString(), m_ensurePercent);
    sendForm->addRow(QStringLiteral("Leader:"), leaderRow);
    sendForm->addRow(QStringLiteral("Trailer:"), trailerRow);
    sendForm->addRow(QStringLiteral("Character delay:"), m_charDelay);
    sendForm->addRow(QString(), m_waitXon);
    sendForm->addRow(QStringLiteral("XON timeout:"), m_xonTimeout);
    sendForm->addRow(QStringLiteral("Program folder:"), withBrowse(m_programFolder, progBrowse));

    // ---- receiving
    m_receiveFolder = new QLineEdit;
    m_receiveFolder->setPlaceholderText(QStringLiteral("Default: <program folder>\\received"));
    auto* rxBrowse = new QPushButton(QStringLiteral("Browse..."));
    connect(rxBrowse, &QPushButton::clicked, this, [this] {
        const QString d = browseFolder(m_receiveFolder->text().isEmpty() ? m_programFolder->text() : m_receiveFolder->text());
        if (!d.isEmpty())
            m_receiveFolder->setText(d);
    });
    m_receiveExt = new QLineEdit;
    m_receiveExt->setMaximumWidth(100);
    m_rxStartTimeout = makeSpin(0, 86400, QStringLiteral(" s"));
    m_rxStartTimeout->setSpecialValueText(QStringLiteral("Wait until cancelled"));
    m_rxIdleTimeout = makeSpin(0, 3600, QStringLiteral(" s"));
    m_rxIdleTimeout->setSpecialValueText(QStringLiteral("Off (stop only on closing %)"));
    m_stripCtrl = new QCheckBox(QStringLiteral("Remove NUL leader and DC1-DC4 control codes from saved file"));

    auto* rx = new QGroupBox(QStringLiteral("Receiving (punch from control)"));
    auto* rxForm = new QFormLayout(rx);
    rxForm->addRow(QStringLiteral("Save to folder:"), withBrowse(m_receiveFolder, rxBrowse));
    rxForm->addRow(QStringLiteral("File extension:"), m_receiveExt);
    rxForm->addRow(QStringLiteral("Wait for first data:"), m_rxStartTimeout);
    rxForm->addRow(QStringLiteral("Stop after silence:"), m_rxIdleTimeout);
    rxForm->addRow(QString(), m_stripCtrl);
    auto* rxNote = new QLabel(QStringLiteral("Receive stops at the closing %, after the silence timeout, or on Cancel "
                                             "(partial data is kept). Files are named "
                                             "<profile>_<O-number>_<date>_<time>.<ext>."));
    rxNote->setWordWrap(true);
    rxForm->addRow(rxNote);

    auto* v = new QVBoxLayout(form);
    v->addWidget(conn);
    v->addWidget(send);
    v->addWidget(rx);
    v->addStretch(1);
    return form;
}

void ProfileEditorDialog::loadEntry(int row)
{
    const bool valid = row >= 0 && row < m_entries.size();
    m_form->setEnabled(valid);
    if (!valid)
        return;

    m_loading = true;
    const MachineProfile& p = m_entries.at(row).profile;
    m_name->setText(p.name);
    m_port->setCurrentText(p.portName);
    m_baud->setCurrentText(QString::number(p.baudRate));
    m_dataBits->setCurrentIndex(qMax(0, m_dataBits->findData(p.dataBits)));
    selectData(m_parity, p.parity);
    selectData(m_stopBits, p.stopBits);
    selectData(m_flow, p.flowControl);
    selectData(m_eol, p.lineEnding);
    m_ensurePercent->setChecked(p.ensurePercent);
    m_leaderByte->setValue(p.leaderByte);
    m_leaderCount->setValue(p.leaderCount);
    m_trailerByte->setValue(p.trailerByte);
    m_trailerCount->setValue(p.trailerCount);
    m_charDelay->setValue(p.charDelayMs);
    m_waitXon->setChecked(p.waitForXon);
    m_xonTimeout->setValue(p.xonTimeoutSec);
    m_xonTimeout->setEnabled(p.waitForXon);
    m_programFolder->setText(QDir::toNativeSeparators(p.programFolder));
    m_receiveFolder->setText(QDir::toNativeSeparators(p.receiveFolder));
    m_receiveExt->setText(p.receiveExtension);
    m_rxStartTimeout->setValue(p.receiveStartTimeoutSec);
    m_rxIdleTimeout->setValue(p.receiveIdleTimeoutSec);
    m_stripCtrl->setChecked(p.stripReceivedControlChars);
    m_loading = false;
}

void ProfileEditorDialog::storeEntry(int row)
{
    if (m_loading || row < 0 || row >= m_entries.size())
        return;

    MachineProfile& p = m_entries[row].profile;
    p.name = m_name->text().trimmed();
    p.portName = m_port->currentText().trimmed().toUpper();
    p.baudRate = m_baud->currentText().toInt();
    p.dataBits = m_dataBits->currentData().toInt();
    p.parity = currentEnum<Parity>(m_parity);
    p.stopBits = currentEnum<StopBits>(m_stopBits);
    p.flowControl = currentEnum<FlowControl>(m_flow);
    p.lineEnding = currentEnum<LineEnding>(m_eol);
    p.ensurePercent = m_ensurePercent->isChecked();
    p.leaderByte = m_leaderByte->value();
    p.leaderCount = m_leaderCount->value();
    p.trailerByte = m_trailerByte->value();
    p.trailerCount = m_trailerCount->value();
    p.charDelayMs = m_charDelay->value();
    p.waitForXon = m_waitXon->isChecked();
    p.xonTimeoutSec = m_xonTimeout->value();
    p.programFolder = m_programFolder->text().trimmed();
    p.receiveFolder = m_receiveFolder->text().trimmed();
    p.receiveExtension = m_receiveExt->text().trimmed();
    p.receiveStartTimeoutSec = m_rxStartTimeout->value();
    p.receiveIdleTimeoutSec = m_rxIdleTimeout->value();
    p.stripReceivedControlChars = m_stripCtrl->isChecked();

    if (QListWidgetItem* item = m_list->item(row))
        item->setText(p.name);
}

void ProfileEditorDialog::onRowChanged(int row)
{
    storeEntry(m_currentRow);
    m_currentRow = row;
    loadEntry(row);
    updateButtons();
}

QString ProfileEditorDialog::uniqueName(const QString& base) const
{
    auto taken = [this](const QString& n) {
        for (const Entry& e : m_entries) {
            if (e.profile.name.compare(n, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    if (!taken(base))
        return base;
    for (int i = 2;; ++i) {
        const QString n = QStringLiteral("%1 (%2)").arg(base).arg(i);
        if (!taken(n))
            return n;
    }
}

void ProfileEditorDialog::addProfile()
{
    storeEntry(m_currentRow);
    MachineProfile p;
    p.name = uniqueName(QStringLiteral("New Machine"));
    m_entries.append(Entry{p, QString()});
    m_list->addItem(p.name);
    m_list->setCurrentRow(m_entries.size() - 1);
    m_name->setFocus();
    m_name->selectAll();
}

void ProfileEditorDialog::duplicateProfile()
{
    if (m_currentRow < 0)
        return;
    storeEntry(m_currentRow);
    MachineProfile p = m_entries.at(m_currentRow).profile;
    p.name = uniqueName(p.name + QStringLiteral(" copy"));
    m_entries.append(Entry{p, QString()});
    m_list->addItem(p.name);
    m_list->setCurrentRow(m_entries.size() - 1);
}

void ProfileEditorDialog::deleteProfile()
{
    if (m_currentRow < 0 || m_currentRow >= m_entries.size())
        return;
    const QString name = m_entries.at(m_currentRow).profile.name;
    const QString question = QStringLiteral("Delete profile '%1'?").arg(name);
    const bool yes = m_confirm ? m_confirm(question)
                               : QMessageBox::question(this, QStringLiteral("Delete profile"), question) == QMessageBox::Yes;
    if (!yes)
        return;

    // Signals are blocked while the list changes so the row index can never
    // point at the wrong entry; the new selection is applied by hand.
    const int row = m_currentRow;
    m_entries.removeAt(row);
    const int next = qMin(row, int(m_entries.size()) - 1);
    {
        const QSignalBlocker blocker(m_list);
        delete m_list->takeItem(row);
        m_list->setCurrentRow(next);
    }
    m_currentRow = next;
    loadEntry(next);
    updateButtons();
}

void ProfileEditorDialog::updateButtons()
{
    const bool has = m_currentRow >= 0 && m_currentRow < m_entries.size();
    m_dupBtn->setEnabled(has);
    m_delBtn->setEnabled(has);
    m_form->setEnabled(has);
}

QString ProfileEditorDialog::browseFolder(const QString& current)
{
    const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose folder"), current);
    return d.isEmpty() ? QString() : QDir::toNativeSeparators(d);
}

QList<MachineProfile> ProfileEditorDialog::profiles() const
{
    QList<MachineProfile> out;
    for (const Entry& e : m_entries)
        out.append(e.profile);
    return out;
}

QHash<QString, QString> ProfileEditorDialog::renames() const
{
    QHash<QString, QString> map;
    for (const Entry& e : m_entries) {
        if (!e.originalName.isEmpty() && e.originalName != e.profile.name)
            map.insert(e.originalName, e.profile.name);
    }
    return map;
}

void ProfileEditorDialog::accept()
{
    storeEntry(m_currentRow);

    for (int i = 0; i < m_entries.size(); ++i) {
        const QString err = m_entries.at(i).profile.validate();
        QString dup;
        for (int j = 0; j < i && err.isEmpty(); ++j) {
            if (m_entries.at(j).profile.name.compare(m_entries.at(i).profile.name, Qt::CaseInsensitive) == 0)
                dup = QStringLiteral("Two profiles are named '%1'. Profile names must be unique.").arg(m_entries.at(i).profile.name);
        }
        if (!err.isEmpty() || !dup.isEmpty()) {
            m_list->setCurrentRow(i);
            m_lastError = err.isEmpty() ? dup : err;
            if (m_warn)
                m_warn(m_lastError);
            else
                QMessageBox::warning(this, QStringLiteral("Check profile"), m_lastError);
            return;
        }
    }
    QDialog::accept();
}
