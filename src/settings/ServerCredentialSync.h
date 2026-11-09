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

// The key of one session's token in the TokenStore.
QString serverTokenKey(const QString &profileId, const QString &serverId);

// Removes the tokens of every server recorded in the profile's `servers.json` from `store`; `done(ok)` runs once, from the event loop, with ok false when any removal failed. Nothing to remove is a success.
void forgetProfileTokens(const QString &profileId, TokenStore &store,
                         std::function<void(bool ok)> done);

// Keeps one profile's sessions in step with a JellyfinClient: the session survives a restart without a password and a revoked one is forgotten. The token goes to the TokenStore (KWallet), the rest of the record to the profile's `servers.json`; nothing secret is ever written to a file.
// - `loginSucceeded` saves the session: the token first, and only when that succeeded the record (one per server; the last entry is the most recently used, a login or a restore moves its entry there). A failed save leaves the session in memory only and emits saveFailed().
// - `serverReady` restores a stored session when the probed server has the stored server id, the stored base URL and this client's device id, the transport may carry credentials, and the store yields the token; then it re-verifies the session against the server, and its 401 ends in `sessionExpired`. The read is asynchronous: restorePending() is true meanwhile, and restoreFinished() follows whatever the result.
// - `sessionExpired` and signOut() forget the entry and its token. `credentialsCleared` is deliberately not used: it also fires when the app merely closes or switches server, and the credential must survive that.
// A `servers.json` that cannot be read is deleted (nothing is merged, and a kept copy would only hold ids); if even that fails, nothing is written for this profile. A `token` key left by an older snapshot is dropped by rewriting the file at once. When a rewrite after a forget fails, the file is removed so a revoked session cannot come back.
class ServerCredentialSync final : public QObject {
    Q_OBJECT

  public:
    // `client` and `store` must outlive this object; `client` carries its device identity before the server is probed.
    ServerCredentialSync(JellyfinClient *client, TokenStore *store, const QString &profileId,
                         QObject *parent = nullptr);

    // The stored entries, for the bootstrap to pick the server to connect to.
    QList<ServerCredential> credentials() const { return m_entries; }
    bool restorePending() const { return m_restorePending; }
    // Forgets the current server's stored session and disconnects the client.
    void signOut();
    // Drops one stored entry and its token without touching the client (the Servers page's remove action).
    void forgetServer(const QString &serverId) { forget(serverId); }

  Q_SIGNALS:
    // The stored entries changed (a save finished, an entry was forgotten or moved).
    void credentialsChanged();
    // A pending restore ended; whether it restored anything shows in the client's state.
    void restoreFinished();
    // The token or the record could not be stored: the session lives in memory only.
    void saveFailed();

  private:
    void onServerReady(const QString &serverId);
    void onLoginSucceeded();
    void onSessionExpired();
    void forget(const QString &serverId);
    void moveToEnd(const QString &serverId);
    bool persist();
    QString tokenKey(const QString &serverId) const;

    JellyfinClient *m_client;
    TokenStore *m_store;
    QString m_profileId;
    QList<ServerCredential> m_entries;
    bool m_restoring = false;
    bool m_restorePending = false;
    bool m_writable = true;
    // Per server: bumped by every forget, so a save that finishes after one is discarded; and a serial per save, so only the newest save of a server may take its token out again.
    QHash<QString, quint64> m_forgetEpoch;
    QHash<QString, quint64> m_saveSerial;
    // The newest restore wins; an older read that lands later is ignored.
    quint64 m_restoreSerial = 0;
};
} // namespace Kinefin
