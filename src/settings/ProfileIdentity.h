// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace Kinefin {
// The default profile's id from `profiles.json` (the index `ensureDefaultProfileIndex` writes). Empty with `error` set when the index is missing, unreadable or names an id that `validProfileId` refuses; nothing is repaired
QString defaultProfileId(QString *error = nullptr);

// The profile's stable device id, kept in `<profile dir>/profile.json` (mode 0600, directory 0700) and created on first use. Jellyfin binds an access token to it, so it must never change between runs: an unreadable or malformed file is an error and is not overwritten. Empty with `error` set on failure
QString profileDeviceId(const QString &profileId, QString *error = nullptr);
} // namespace Kinefin
