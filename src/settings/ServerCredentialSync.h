// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

#include "settings/ServerCredentials.h"

namespace Kinefin {
class JellyfinClient;
class TokenStore;

// The key of one user's token on one server in the TokenStore: `<profile>/<serverId>/<userId>`, in the KWallet folder of the app
QString serverTokenKey(const QString &profileId, const QString &serverId, const QString &userId);
// The key one token per server used before several users per server were kept: `<profile>/<serverId>`, read once to migrate, removed by every forget. The migration is one-way, done at the first restore of the user that owns it once the per-user key is written: an older build does not see the per-user keys
QString legacyServerTokenKey(const QString &profileId, const QString &serverId);

// Removes the tokens of every user of every server recorded in the profile's `servers.json` from `store`, under both key forms; `done(ok)` runs once, from the event loop, with ok false when any removal failed. Nothing to remove is a success
void forgetProfileTokens(const QString &profileId, TokenStore &store,
                         std::function<void(bool ok)> done);

// Keeps one profile's sessions in step with a JellyfinClient: a session survives a restart without a password and a revoked one is forgotten. The token goes to the TokenStore (KWallet), the rest of the record to the profile's `servers.json`; nothing secret is ever written to a file. There is one record per (server, user)
// - `loginSucceeded` saves the session: the token first, and only when that succeeded the record (the last entry is the most recently used, a login or a restore moves its entry there). A failed save leaves the session in memory only and emits saveFailed(). The token already stored for that user is forgotten first (the server ended that session by the new login). A user with `askPassword` gets a record but never a token
// - restoreUser() restores one stored user on the probed server (same server id, same base URL, same device id, a transport that may carry credentials; see restorableUsers()) and re-verifies the session against the server. The read is asynchronous: restorePending() is true meanwhile, and restoreFinished() follows whatever the result. A probe never restores by itself: the caller picks the user. The legacy key belongs to one user, the one whose record came from a file without `remembered` (`ownsLegacyKey`): only that user reads it, it is moved to the per-user key and the legacy key removed once that write succeeded, and every forget, sign-in or removal of that user removes it too
// - `sessionExpired` forgets the token of the user that was signed in and keeps the record (remembered=false, the user is asked for the password next time); signOut() and forgetUser() drop the record too. `credentialsCleared` is deliberately not used: it also fires when the app merely closes or switches server, and the credential must survive that
// A `servers.json` that cannot be read is deleted (nothing is merged, and a kept copy would only hold ids); if even that fails, nothing is written for this profile. A record whose server or user id is not a valid id is ignored. A `token` key left by an older snapshot is dropped by rewriting the file at once. When a rewrite after a forget fails, the file is removed so a revoked session cannot come back
class ServerCredentialSync final : public QObject {
    Q_OBJECT

  public:
    // `client` and `store` must outlive this object; `client` carries its device identity before the server is probed
    ServerCredentialSync(JellyfinClient *client, TokenStore *store, const QString &profileId,
                         QObject *parent = nullptr);

    // The stored records, in order of use (the last is the most recently used one)
    QList<ServerCredential> credentials() const { return m_entries; }
    // The records of the probed server that this client may restore, whether or not a token is stored for them (the caller checks `remembered` and `askPassword`): empty before a probe and on a transport that would refuse credentials
    QList<ServerCredential> restorableUsers() const;
    // Whether a record names the probed server: the same public id and the same resolved base URL as restorableUsers() asks for, whether or not any of its users could be restored. A record exists only after a sign-in that succeeded
    bool isSavedServer() const;
    bool restorePending() const { return m_restorePending; }
    // Abandons the restore waiting for the store, if any: its answer is ignored and restoreFinished() does not follow
    void cancelRestore();
    // Starts restoring `userId` of the probed server: false, changing nothing, when no such record is restorable or it has no stored token (not remembered, or asks for the password)
    bool restoreUser(const QString &userId);
    // Forgets the signed-in user's stored session and disconnects the client
    void signOut();
    // Drops every record of a server and every token of it (also one still being saved) without touching the client
    void forgetServer(const QString &serverId);
    // Drops one user's record and token without touching the client
    void forgetUser(const QString &serverId, const QString &userId);
    // Removes the token of one user and marks the record not remembered, without touching the record otherwise or the client: what Switch User and an expired session need
    void forgetToken(const QString &serverId, const QString &userId);
    // Turns "Ask for Password" of a stored user on or off. On removes the token and marks the record not remembered; off leaves the user not remembered until the next sign-in
    void setAskPassword(const QString &serverId, const QString &userId, bool askPassword);
    // Refreshes the display name and avatar tag of a stored user; an unchanged pair is not rewritten
    void updateUser(const QString &serverId, const QString &userId, const QString &username,
                    const QString &imageTag);

  Q_SIGNALS:
    // The stored records changed (a save finished, a record was forgotten, moved or updated)
    void credentialsChanged();
    // A pending restore ended; whether it restored anything shows in the client's state
    void restoreFinished();
    // The token or the record could not be stored: the session lives in memory only
    void saveFailed();

  private:
    void onServerReady();
    void onLoginSucceeded();
    void onSessionExpired(const QString &expiredUserId);
    void finishRestore(const ServerCredential &entry, bool ok, const QString &token, bool legacy);
    bool isProbedServer(const ServerCredential &entry) const;
    void writeToken(const QString &serverId, const QString &userId, const QString &token,
                    std::function<void(bool ok)> stored);
    void dropToken(const QString &serverId, const QString &userId);
    void removeKey(const QString &key);
    void releaseLegacyKey(const QString &serverId, const QString &userId);
    ServerCredential *find(const QString &serverId, const QString &userId);
    void moveToEnd(const QString &serverId, const QString &userId);
    void upsert(const ServerCredential &entry);
    void persistOrDrop();
    bool persist();
    QString tokenKey(const QString &serverId, const QString &userId) const;

    JellyfinClient *m_client;
    TokenStore *m_store;
    QString m_profileId;
    QList<ServerCredential> m_entries;
    bool m_restoring = false;
    bool m_restorePending = false;
    bool m_writable = true;
    // The user whose session is live in the client: the client has already cleared its own user id when it reports an expiry
    QString m_activeUserId;
    // Per token key (server and user): bumped by every forget, so a save that finishes after one is discarded; and a serial per save, so only the newest save of a key may take its token out again
    QHash<QString, quint64> m_forgetEpoch;
    QHash<QString, quint64> m_saveSerial;
    // The newest restore wins; an older read that lands later is ignored
    quint64 m_restoreSerial = 0;
};
} // namespace Kinefin
