#pragma once

#include "DataSource.h"
#include "MachineProfile.h"
#include "ProgramFormatter.h"
#include "TransferTypes.h"

#include <QDateTime>
#include <QFrame>
#include <QHash>
#include <functional>
#include <optional>

class CodeView;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSettings;
class QTabWidget;
class QToolButton;

namespace nc {
class MachineChannel;
}

// UI for one machine, drawn as a card: profile, file, program view,
// send/drip feed/receive/cancel, progress and status. All serial work is
// delegated to its own nc::MachineChannel.
class MachinePanel : public QFrame
{
    Q_OBJECT

public:
    MachinePanel(const QString& title, const QString& settingsKey, int defaultProfileIndex, QSettings* settings,
                 QWidget* parent = nullptr);

    QString title() const { return m_title; }

    // The program to send (empty clears it). Browse and drag-and-drop end up here.
    void setProgramFile(const QString& path);
    QString programFile() const { return m_filePath; }

    // renames maps old profile name -> new name so the selection survives an edit.
    void setProfiles(const QList<nc::MachineProfile>& profiles, const QHash<QString, QString>& renames = {});
    void setAvailablePorts(const QStringList& ports);
    // Returns the title of another panel that is busy on this COM port, or
    // an empty string. Set by MainWindow so any number of panels are checked.
    void setPortCheck(std::function<QString(const QString& port)> check) { m_portCheck = std::move(check); }

    bool isBusy() const;
    QString busyPortName() const;
    void cancelTransfer();

signals:
    void transferFinished(const nc::TransferResult& result);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    std::optional<nc::MachineProfile> currentProfile() const;
    void onProfileChanged();
    void browseFile();
    void updateFileLabel();
    void loadPreview();
    void updateFileInfo();
    void updatePortHint();
    void updateControls();
    std::optional<nc::MachineProfile> checkCanSend(const QString& path);
    void beginSend(const nc::MachineProfile& profile, const nc::SendRequest& request);
    void startSend();
    void onDripButton();
    void startReceive();
    void openReceiveFolder();
    void showError(const QString& message);
    void clearLocalError();
    void refreshStatus();
    void trackLine(qint64 bytesSent);
    void setProgressText(const QString& line, const QString& bytes);

    void onStateChanged(nc::TransferState state, const QString& message);
    void onProgress(qint64 done, qint64 total);
    void onDataReceived(const QByteArray& chunk);
    void onRunFinished(const nc::TransferResult& result);
    void onFinished(const nc::TransferResult& result);

    nc::MachineChannel* m_channel = nullptr;
    QList<nc::MachineProfile> m_profiles;
    QStringList m_ports;
    std::function<QString(const QString&)> m_portCheck;
    QSettings* m_settings = nullptr;
    QString m_title;
    QString m_key;
    int m_defaultProfileIndex = 0;

    QLabel* m_titleLabel = nullptr;
    QLabel* m_badge = nullptr;
    QComboBox* m_profileCombo = nullptr;
    QLabel* m_profileSummary = nullptr;
    QLabel* m_portHint = nullptr;
    QString m_filePath;          // the chosen program, native separators
    QLabel* m_fileName = nullptr;  // its name, bold
    QLabel* m_fileFolder = nullptr; // its folder, shortened to fit
    QPushButton* m_browseBtn = nullptr;
    QPushButton* m_reloadBtn = nullptr;
    QLabel* m_fileInfo = nullptr;
    QTabWidget* m_tabs = nullptr;
    QToolButton* m_followBtn = nullptr;
    CodeView* m_preview = nullptr;
    CodeView* m_received = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QPushButton* m_dripBtn = nullptr;
    QPushButton* m_receiveBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QPushButton* m_folderBtn = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_progressLine = nullptr;  // "Line 91 of 154" above the bar
    QLabel* m_progressBytes = nullptr; // "2,816 / 4,944 bytes  (56%)" above the bar
    QLabel* m_status = nullptr;

    QByteArray m_previewRaw;
    QString m_previewPath;     // path the preview was last loaded from
    QString m_previewProblem;  // why it could not be loaded
    QDateTime m_previewModified;
    qint64 m_previewFileSize = 0;
    int m_previewLines = -1;   // -1 = not counted (file bigger than what was read)
    QList<qint64> m_lineLengths; // each previewed line, for line tracking
    nc::ProgramFormatter::PercentPlanner::Plan m_percentPlan;
    QList<qint64> m_lineEnds;  // wire offset just past each previewed line
    QString m_wireKey;         // formatting settings m_wireBytes / m_lineEnds were computed for
    qint64 m_wireBytes = 0;
    bool m_previewOk = false;
    bool m_previewTruncated = false;
    bool m_localError = false; // status shows a check the panel itself made, not a transfer result
    bool m_rxLastWasCR = false;
    qint64 m_rxBytes = 0;
    bool m_sendingPreview = false; // the running send is the previewed file, so lines can be tracked
    bool m_dripActive = false;  // a drip feed is armed or running on this panel
    bool m_dripRestart = false; // Restart clicked: arm again once the cancel lands
    int m_dripRuns = 0;         // runs completed since Drip Feed was clicked
    nc::TransferState m_state = nc::TransferState::Idle;
    nc::TransferState m_barState = nc::TransferState::Idle;
    QString m_stateMessage;
};
