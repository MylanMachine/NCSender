#include "LogView.h"

#include "Theme.h"

#include <QColor>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

using namespace nc;

namespace {

constexpr int kMaxRows = 500;

enum Column { ColTime, ColMachine, ColDirection, ColFile, ColBytes, ColResult, ColDetail, ColCount };

} // namespace

LogView::LogView(TransferLog* log, QWidget* parent)
    : QWidget(parent)
    , m_log(log)
{
    auto* title = new QLabel(QStringLiteral("Recent transfers"));
    title->setStyleSheet(QStringLiteral("font-size: 11pt; font-weight: 600;"));
    auto* reloadBtn = new QPushButton(QStringLiteral("Reload"));
    auto* openBtn = new QPushButton(QStringLiteral("Open Log File"));
    openBtn->setToolTip(QStringLiteral("Open transfer_log.csv (opens in Excel or your default CSV viewer)"));

    m_table = new QTableWidget(0, ColCount);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Time"), QStringLiteral("Machine"), QStringLiteral("Direction"),
                                        QStringLiteral("File"), QStringLiteral("Bytes"), QStringLiteral("Result"),
                                        QStringLiteral("Detail")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 12);
    m_table->setShowGrid(false);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(ColTime, 160);
    m_table->setColumnWidth(ColMachine, 90);
    m_table->setColumnWidth(ColDirection, 70);
    m_table->setColumnWidth(ColFile, 260);
    m_table->setColumnWidth(ColBytes, 80);
    m_table->setColumnWidth(ColResult, 95);
    m_table->setWordWrap(false);

    auto* header = new QHBoxLayout;
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(reloadBtn);
    header->addWidget(openBtn);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(12, 6, 12, 10);
    v->setSpacing(6);
    v->addLayout(header);
    v->addWidget(m_table, 1);

    connect(reloadBtn, &QPushButton::clicked, this, &LogView::reload);
    connect(openBtn, &QPushButton::clicked, this, [this] {
        if (QFileInfo::exists(m_log->path()))
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_log->path()));
    });
    connect(m_log, &TransferLog::entryAppended, this, &LogView::addEntry);

    reload();
}

void LogView::reload()
{
    m_table->setRowCount(0);
    const QList<TransferLogEntry> entries = m_log->readRecent(kMaxRows);
    for (const TransferLogEntry& e : entries)
        insertRow(m_table->rowCount(), e);
}

void LogView::addEntry(const TransferLogEntry& entry)
{
    insertRow(0, entry);
    while (m_table->rowCount() > kMaxRows)
        m_table->removeRow(m_table->rowCount() - 1);
}

void LogView::insertRow(int row, const TransferLogEntry& e)
{
    m_table->insertRow(row);
    auto set = [&](int col, const QString& text) {
        auto* item = new QTableWidgetItem(text);
        item->setToolTip(text);
        m_table->setItem(row, col, item);
        return item;
    };
    set(ColTime, e.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    set(ColMachine, e.machine + (e.port.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(e.port)));
    set(ColDirection, e.direction);
    QTableWidgetItem* fileItem = set(ColFile, QFileInfo(e.file).fileName());
    fileItem->setToolTip(e.file);
    set(ColBytes, QLocale::system().toString(e.bytes))->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QTableWidgetItem* result = set(ColResult, e.result);
    TransferState look = TransferState::Cancelled;
    if (e.result == QLatin1String("Success"))
        look = TransferState::Done;
    else if (e.result == QLatin1String("Failed"))
        look = TransferState::Error;
    const Theme::StateColors colors = Theme::stateColors(look);
    result->setForeground(colors.text);
    result->setBackground(colors.background);
    QFont bold = result->font();
    bold.setBold(true);
    result->setFont(bold);
    set(ColDetail, e.detail);
}
