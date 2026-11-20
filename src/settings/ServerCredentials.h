// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>

#include "settings/ProfilePaths.h"

namespace Kinefin {
// One user's session record on one server, without any secret: the access token lives in the TokenStore (KWallet), never in this file (SECURITY.md). The record persists the identity a restart needs to reattach without a password: base URL, server id, user id and the per-profile device identity the token is bound to. `remembered` says a token is stored for the user (a file without the key reads as true); `askPassword` means the user never has a token stored and always signs in with the password
struct ServerCredential {
    QString serverId;
    QString url;
    QString userId;
    QString deviceId;
    QString deviceName;
    QString username;
    QString imageTag;
    bool askPassword = false;
    bool remembered = true;
    // Set when the record came from a file without the `remembered` key (one record per server, before per-user wallet keys): its user owns the legacy per-server wallet key. Cleared once the token moved to the per-user key or was forgotten. Not a key of its own: a record that owns the key is saved without `remembered` (it reads as true), so a restart before the migration finished still finds the key
    bool ownsLegacyKey = false;
    bool operator==(const ServerCredential &) const = default;
};

// Whether `serverId` and `userId` are Jellyfin ids (both end up in wallet keys, so a `/` must never get through): the sync ignores any record that fails this
bool hasValidIds(const ServerCredential &credential);

// The file lives per profile so a second profile cannot see another's credential. Empty for an invalid `profileId`. `ServerCredentialSync` is the wiring to `JellyfinClient`, keeping the store the only plaintext-on-disk holder of the token
QString serverCredentialsPath(const QString &profileId);

// Reads the records; an empty list and no error when the file does not exist. A malformed file reports an error and restores nothing (no partial merges, no silent rewrites). A `token` key left by an older snapshot is ignored and reported through `legacyToken`, so the caller can rewrite the file without it
QList<ServerCredential> loadServerCredentials(const QString &profileId, QString *error = nullptr,
                                              bool *legacyToken = nullptr);
// Replaces the list atomically. The containing directory is created 0700 and the file is written 0600 before the first byte lands (the temp file itself is 0600 and commits by rename); it holds no secret, but it stays private like the rest of the profile
bool saveServerCredentials(const QString &profileId, const QList<ServerCredential> &credentials,
                           QString *error = nullptr);
} // namespace Kinefin
