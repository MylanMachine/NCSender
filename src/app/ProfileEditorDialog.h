#pragma once

#include "MachineProfile.h"

#include <QDialog>
#include <QHash>
#include <functional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

// Edits a working copy of the profile list. Nothing is saved until OK.
class ProfileEditorDialog : public QDialog
{
    Q_OBJECT

public:
    ProfileEditorDialog(const QList<nc::MachineProfile>& profiles, const QStringList& ports, QWidget* parent = nullptr);

    QList<nc::MachineProfile> profiles() const;
    // old name -> new name for profiles that were renamed
    QHash<QString, QString> renames() const;

    void accept() override;

    // For automated tests: replace the Yes/No and warning message boxes.
    void setConfirmHandler(std::function<bool(const QString&)> f) { m_confirm = std::move(f); }
    void setWarningHandler(std::function<void(const QString&)> f) { m_warn = std::move(f); }
    QString lastError() const { return m_lastError; }

private:
    std::function<bool(const QString&)> m_confirm;
    std::function<void(const QString&)> m_warn;
    QString m_lastError;

    struct Entry
    {
        nc::MachineProfile profile;
        QString originalName; // empty for profiles added in this dialog
    };

    QWidget* buildForm();
    void loadEntry(int row);
    void storeEntry(int row);
    void onRowChanged(int row);
    void addProfile();
    void duplicateProfile();
    void deleteProfile();
    void updateButtons();
    QString browseFolder(const QString& current);
    QString uniqueName(const QString& base) const;

    QList<Entry> m_entries;
    int m_currentRow = -1;
    bool m_loading = false;

    QListWidget* m_list = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_dupBtn = nullptr;
    QPushButton* m_delBtn = nullptr;
    QWidget* m_form = nullptr;

    QLineEdit* m_name = nullptr;
    QComboBox* m_port = nullptr;
    QComboBox* m_baud = nullptr;
    QComboBox* m_dataBits = nullptr;
    QComboBox* m_parity = nullptr;
    QComboBox* m_stopBits = nullptr;
    QComboBox* m_flow = nullptr;

    QComboBox* m_eol = nullptr;
    QCheckBox* m_ensurePercent = nullptr;
    QSpinBox* m_leaderByte = nullptr;
    QSpinBox* m_leaderCount = nullptr;
    QSpinBox* m_trailerByte = nullptr;
    QSpinBox* m_trailerCount = nullptr;
    QSpinBox* m_charDelay = nullptr;
    QCheckBox* m_waitXon = nullptr;
    QSpinBox* m_xonTimeout = nullptr;
    QLineEdit* m_programFolder = nullptr;

    QLineEdit* m_receiveFolder = nullptr;
    QLineEdit* m_receiveExt = nullptr;
    QSpinBox* m_rxStartTimeout = nullptr;
    QSpinBox* m_rxIdleTimeout = nullptr;
    QCheckBox* m_stripCtrl = nullptr;
};
