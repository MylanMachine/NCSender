#pragma once

#include "ProfileStore.h"
#include "TransferTypes.h"

#include <QList>
#include <QMainWindow>
#include <functional>

class QAction;
class QLabel;
class QSettings;
class QSplitter;
class QTimer;
class LogView;
class MachinePanel;

namespace nc {
class TransferLog;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    static constexpr int kMinMachines = 1;
    static constexpr int kMaxMachines = 6;

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Show n machine panels (Machine A, B, C, ...). Removing a panel that is
    // in the middle of a transfer is refused; *why says which one.
    bool setMachineCount(int n, QString* why = nullptr);
    int machineCount() const { return int(m_panels.size()); }
    MachinePanel* panel(int index) const { return m_panels.value(index); }

    // For automated tests.
    MachinePanel* panelA() const { return panel(0); }
    MachinePanel* panelB() const { return panel(1); }
    nc::TransferLog* transferLog() const { return m_log; }
    void setCloseConfirmHandler(std::function<bool(const QString&)> f) { m_confirmClose = std::move(f); }

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildMenus();
    void refreshPorts();
    void editProfiles();
    void openDataFolder();
    void showAbout();
    void onTransferFinished(const nc::TransferResult& result);
    void updateLogWarning(const QString& error);
    void retryLog();
    bool waitForTransfersToStop(int timeoutMs);
    bool anyBusy() const;

    MachinePanel* createPanel(int index);
    void layoutPanels();
    void changeMachineCount(int n); // menu handler: shows a message if refused
    void updateMachineActions();
    QString portUser(const QString& port, const MachinePanel* asker) const;

    std::function<bool(const QString&)> m_confirmClose;
    QLabel* m_logWarning = nullptr;
    QTimer* m_logRetryTimer = nullptr;

    nc::ProfileStore m_store;
    nc::TransferLog* m_log = nullptr;
    QSettings* m_settings = nullptr;
    QList<MachinePanel*> m_panels;
    QWidget* m_panelArea = nullptr;
    LogView* m_logView = nullptr;
    QSplitter* m_vsplit = nullptr;
    QLabel* m_portsLabel = nullptr;
    QStringList m_portNames;

    QAction* m_addMachineAct = nullptr;
    QAction* m_removeMachineAct = nullptr;
    QList<QAction*> m_countActs;
};
