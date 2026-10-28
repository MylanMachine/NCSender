#pragma once

#include "MachineProfile.h"

#include <QList>
#include <QString>
#include <optional>

namespace nc {

// Loads and saves machine profiles as JSON:
//   { "version": 1, "profiles": [ { ...MachineProfile::toJson()... }, ... ] }
class ProfileStore
{
public:
    explicit ProfileStore(const QString& filePath);

    // A missing file is not an error: built-in example profiles are used.
    // Returns false (with *error set) if the file exists but cannot be read or
    // parsed; the example profiles are used in that case too.
    bool load(QString* error = nullptr);

    // Writes atomically and keeps the previous file as <file>.bak.
    bool save(QString* error = nullptr);

    QString filePath() const { return m_path; }
    bool loadedFromFile() const { return m_loadedFromFile; }

    const QList<MachineProfile>& profiles() const { return m_profiles; }
    void setProfiles(const QList<MachineProfile>& profiles) { m_profiles = profiles; }

    // Case-insensitive lookup by name.
    std::optional<MachineProfile> find(const QString& name) const;

    static QList<MachineProfile> defaultProfiles();

private:
    QString m_path;
    QList<MachineProfile> m_profiles;
    bool m_loadedFromFile = false;
};

} // namespace nc
