// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLockFile>
#include <QString>

#include <memory>

namespace Kinefin {
// A profile id is a single path component: 1-64 characters of [A-Za-z0-9._-], not starting with '.' or '-'. Every store refuses anything else, so an id can never leave its directory
bool validProfileId(const QString &profileId);

// What `--create-profile` accepts: the profile's id is its name, 1-64 characters of [A-Za-z0-9_] (safe in file names, D-Bus names and shell)
bool validProfileName(const QString &name);

// `~/.local/share/kinefin/profiles/<id>`, `~/.cache/kinefin/profiles/<id>` and `<data>/logs`; empty for an invalid `profileId`
QString profileDataDirectory(const QString &profileId);
QString profileCacheDirectory(const QString &profileId);
QString profileLogDirectory(const QString &profileId);

// Creates `path` (and parents) and makes it owner-only (0700); false with `error` set when it cannot
bool ensurePrivateDirectory(const QString &path, QString *error = nullptr);

// Held for as long as the app runs on the profile, so two sessions sharing one home never share a profile. Null with `error` set when another process holds it or the lock cannot be taken
std::unique_ptr<QLockFile> lockProfile(const QString &profileId, QString *error = nullptr);

// Whether another process holds the profile's lock right now
bool profileInUse(const QString &profileId);
} // namespace Kinefin
