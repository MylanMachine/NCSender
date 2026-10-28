#pragma once

#include "TransferLog.h"

#include <QWidget>

class QTableWidget;

// Table of recent transfers, newest at the top.
class LogView : public QWidget
{
    Q_OBJECT

public:
    explicit LogView(nc::TransferLog* log, QWidget* parent = nullptr);

    void reload();
    void addEntry(const nc::TransferLogEntry& entry);

private:
    void insertRow(int row, const nc::TransferLogEntry& entry);

    nc::TransferLog* m_log = nullptr;
    QTableWidget* m_table = nullptr;
};
