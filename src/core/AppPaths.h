#pragma once

#include <QString>

namespace nc::AppPaths {

// Folder for profiles.json, ncsender.ini and the transfer log.
// This is the folder containing the exe when it is writable (the normal case:
// copy the bin folder anywhere and run it). If the exe sits somewhere
// read-only, such as Program Files, it falls back to the per-user local
// AppData folder.
QString dataDir();

QString profilesFile();
QString settingsFile();
QString logFile();

} // namespace nc::AppPaths
