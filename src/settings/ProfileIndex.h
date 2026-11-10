// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace Kinefin {
struct ProfileEntry {
    QString id;
    QString name;
    bool operator==(const ProfileEntry &) const = default;
};

struct ProfileIndexData {
    QString defaultId;
    QList<ProfileEntry> profiles;
};

// The index lives in XDG_CONFIG_HOME/kinefin/profiles.json, not XDG_DATA_HOME; future per-profile settings and credentials are separate private data files
QString profileIndexPath();

// Creates the index with the one `default` profile when there is none. An existing file that is not a valid index (not JSON, an invalid or duplicate id, a default that is not listed, no profile at all) is renamed `profiles.json.broken` (replacing an older one) and replaced; the profiles' own directories are left alone
bool ensureDefaultProfileIndex(QString *error = nullptr);

// The validated index; nullopt with `error` set when it is missing or invalid
std::optional<ProfileIndexData> loadProfileIndex(QString *error = nullptr);

// Each of these reads, changes and writes the index under one lock file, so two commands never lose each other's change. `id` is also the profile's name (validProfileName). The default profile can never be removed
bool addProfile(const QString &name, QString *error = nullptr);
bool setDefaultProfile(const QString &id, QString *error = nullptr);
bool removeProfileEntry(const QString &id, QString *error = nullptr);
} // namespace Kinefin
