// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <memory>

namespace Kinefin {
// A profile id is a single path component: 1-64 characters of [A-Za-z0-9._-], not starting with '.' or '-'. Every store refuses anything else, so an id can never leave its directory
bool validProfileId(const QString &profileId);

// What `--create-profile` accepts: the profile's id is its name, 1-64 characters of [A-Za-z0-9_] (safe in file names, D-Bus names and shell)
bool validProfileName(const QString &name);

// `--config-dir`: one absolute root that replaces the XDG locations for this run (`<root>/profiles.json`, `<root>/profiles/<id>`, `<root>/cache/<id>`); an empty root restores the XDG layout. Set it before the first path is read. Keyring entries are keyed by profile and server ids only, so two roots that share a profile id share its tokens
void setProfileRoot(const QString &root);
QString profileRoot();

// `~/.local/share/kinefin/profiles/<id>`, `~/.cache/kinefin/profiles/<id>` and `<data>/logs`; under a profile root, `<root>/profiles/<id>` and `<root>/cache/<id>`. Empty for an invalid `profileId`
QString profileDataDirectory(const QString &profileId);
QString profileCacheDirectory(const QString &profileId);
QString profileLogDirectory(const QString &profileId);

// Creates `path` (and parents) and makes it owner-only (0700); false with `error` set when it cannot
bool ensurePrivateDirectory(const QString &path, QString *error = nullptr);

// Writes `data` to `path` as an owner-only file (0600 before the first byte, committed by rename, so a reader sees the old or the new content whole) inside an owner-only directory; false with `error` set when it cannot
bool writePrivateFile(const QString &path, const QByteArray &data, QString *error = nullptr);

// An advisory lock on a file of the profile, held while the object lives. The kernel drops it when the process ends, so a kill or a crash never leaves the profile locked; a lock file that records a process id cannot promise that, because inside the Flatpak every run is process 2 and an orphaned file looks alive
class ProfileLock {
  public:
    ~ProfileLock();
    ProfileLock(const ProfileLock &) = delete;
    ProfileLock &operator=(const ProfileLock &) = delete;

  private:
    friend std::unique_ptr<ProfileLock> lockProfile(const QString &profileId, QString *error);
    explicit ProfileLock(int descriptor) : m_descriptor(descriptor) {}
    int m_descriptor;
};

// Held for as long as the app runs on the profile, so two sessions sharing one home never share a profile. Null with `error` set when another process holds it or the lock cannot be taken
std::unique_ptr<ProfileLock> lockProfile(const QString &profileId, QString *error = nullptr);

// Whether another process holds the profile's lock right now
bool profileInUse(const QString &profileId);
} // namespace Kinefin
