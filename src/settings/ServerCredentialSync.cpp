// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServerCredentialSync.h"

#include <QFile>
#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <memory>

#include "api/JellyfinClient.h"
#include "settings/TokenStore.h"

namespace Kinefin {
Q_LOGGING_CATEGORY(credentialsLog, "kinefin.settings.credentials")

ServerCredentialSync::ServerCredentialSync(JellyfinClient *client, TokenStore *store,
                                           const QString &profileId, QObject *parent)
    : QObject(parent), m_client(client), m_store(store), m_profileId(profileId)
{
    QString error;
    bool legacyToken = false;
    m_entries = loadServerCredentials(m_profileId, &error, &legacyToken);
    if (!error.isEmpty()) {
        m_entries.clear();
        const QString path = serverCredentialsPath(m_profileId);
        // Messages are fixed strings: never an entry, never a token
        qCWarning(credentialsLog) << "server credentials not usable:" << error;
        // The file is deleted, not kept aside: a login recreates it
        if (path.isEmpty() || (QFile::exists(path) && !QFile::remove(path))) {
            qCWarning(credentialsLog) << "could not remove the unreadable file";
            m_writable = false;
        }
    } else if (legacyToken) {
        // An older snapshot kept the token in this file: rewrite it without, now
        qCWarning(credentialsLog) << "dropping a token stored in the credentials file";
        // A rewrite that fails must not leave the plaintext token behind: the file goes
        if (!persist() && !QFile::remove(serverCredentialsPath(m_profileId)))
            qCWarning(credentialsLog)
                << "could not remove the credentials file with a legacy token";
    }
    connect(client, &JellyfinClient::serverReady, this, &ServerCredentialSync::onServerReady);
    connect(client, &JellyfinClient::loginSucceeded, this, &ServerCredentialSync::onLoginSucceeded);
    connect(client, &JellyfinClient::sessionExpired, this, &ServerCredentialSync::onSessionExpired);
}

QString serverTokenKey(const QString &profileId, const QString &serverId)
{
    return profileId + QLatin1Char('/') + serverId;
}

QString ServerCredentialSync::tokenKey(const QString &serverId) const
{
    return serverTokenKey(m_profileId, serverId);
}

void forgetProfileTokens(const QString &profileId, TokenStore &store,
                         std::function<void(bool ok)> done)
{
    QString error;
    const QList<ServerCredential> entries = loadServerCredentials(profileId, &error);
    // An unreadable record may hide servers whose tokens are then left in the store
    const bool listed = error.isEmpty();
    if (entries.isEmpty()) {
        QTimer::singleShot(0, [done = std::move(done), listed] { done(listed); });
        return;
    }
    struct Progress {
        qsizetype left;
        bool ok = true;
        std::function<void(bool)> done;
    };
    auto progress = std::make_shared<Progress>(Progress{entries.size(), listed, std::move(done)});
    for (const ServerCredential &entry : entries) {
        store.remove(serverTokenKey(profileId, entry.serverId), [progress](bool ok) {
            progress->ok = progress->ok && ok;
            if (--progress->left == 0)
                progress->done(progress->ok);
        });
    }
}

void ServerCredentialSync::onServerReady(const QString &serverId)
{
    // A new probe supersedes any restore still waiting for the store (another server, or a reconnect)
    m_restorePending = false;
    ++m_restoreSerial;
    for (const ServerCredential &entry : std::as_const(m_entries)) {
        // The token goes only to the server it was issued by: same public id, same resolved base URL (scheme, host, port and path, so a token issued over TLS never travels over plain HTTP and another service on the same host never gets it) and the same device identity
        if (entry.serverId != serverId || entry.deviceId != m_client->deviceId()
            || QUrl(entry.url) != m_client->serverUrl())
            continue;
        // Nothing to ask the store for when the transport would refuse the token anyway
        if (!m_client->canSendCredentials())
            return;
        m_restorePending = true;
        const quint64 serial = ++m_restoreSerial;
        QPointer<ServerCredentialSync> self(this);
        m_store->read(tokenKey(serverId), [self, serial, entry](bool ok, const QString &token) {
            if (!self || serial != self->m_restoreSerial)
                return;
            self->m_restorePending = false;
            // Whatever happened to the client while the store answered (cancelled, another server, a password login) decides: restoreSession() only accepts the probed state
            if (ok && self->m_client->serverId() == entry.serverId
                && QUrl(entry.url) == self->m_client->serverUrl()) {
                self->m_restoring = true;
                const bool restored = self->m_client->restoreSession(entry.userId, token);
                self->m_restoring = false;
                if (restored) {
                    self->moveToEnd(entry.serverId);
                    self->m_client->verifySession();
                }
            } else if (!ok) {
                qCWarning(credentialsLog) << "the stored session could not be read";
            }
            Q_EMIT self->restoreFinished();
        });
        return;
    }
}

void ServerCredentialSync::onLoginSucceeded()
{
    if (m_restoring)
        return;
    if (!m_writable) {
        // Nothing may be stored for this profile (invalid id, or its file cannot be handled): the token never reaches the store either
        Q_EMIT saveFailed();
        return;
    }
    ServerCredential entry;
    entry.serverId = m_client->serverId();
    entry.url = m_client->serverUrl().toString();
    entry.userId = m_client->userId();
    entry.deviceId = m_client->deviceId();
    entry.deviceName = m_client->deviceName();
    const QString key = tokenKey(entry.serverId);
    const quint64 epoch = m_forgetEpoch.value(entry.serverId);
    const quint64 serial = ++m_saveSerial[entry.serverId];
    QPointer<ServerCredentialSync> self(this);
    m_store->write(key, m_client->accessToken(), [self, entry, key, epoch, serial](bool ok) {
        if (!self)
            return;
        if (!ok) {
            qCWarning(credentialsLog) << "could not store the session token";
            Q_EMIT self->saveFailed();
            return;
        }
        if (epoch != self->m_forgetEpoch.value(entry.serverId)) {
            // Signed out or revoked while the token was being written: take it out again, unless a newer save of this server owns the key by now
            if (serial == self->m_saveSerial.value(entry.serverId))
                self->m_store->remove(key, [](bool) {});
            return;
        }
        // The last entry is the most recently used one
        self->m_entries.removeIf(
            [&](const ServerCredential &e) { return e.serverId == entry.serverId; });
        self->m_entries.append(entry);
        if (!self->persist())
            Q_EMIT self->saveFailed();
        Q_EMIT self->credentialsChanged();
    });
}

void ServerCredentialSync::moveToEnd(const QString &serverId)
{
    for (qsizetype i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).serverId == serverId && i != m_entries.size() - 1) {
            m_entries.move(i, m_entries.size() - 1);
            persist();
            Q_EMIT credentialsChanged();
            return;
        }
    }
}

void ServerCredentialSync::onSessionExpired()
{
    forget(m_client->serverId());
}

void ServerCredentialSync::signOut()
{
    forget(m_client->serverId());
    m_client->disconnectFromServer();
}

void ServerCredentialSync::forgetToken(const QString &serverId)
{
    // The same guard as forget(): a token whose save is still in flight is caught by the epoch, one already stored is removed here. The entry and the client stay: only the session is gone
    ++m_forgetEpoch[serverId];
    m_store->remove(tokenKey(serverId), [](bool ok) {
        if (!ok)
            qCWarning(credentialsLog) << "could not remove the session token from the store";
    });
}

void ServerCredentialSync::forget(const QString &serverId)
{
    ++m_forgetEpoch[serverId];
    // Always asked, even without an entry: a token whose save is still in flight is caught by the epoch, one already stored is removed here
    m_store->remove(tokenKey(serverId), [](bool ok) {
        if (!ok)
            qCWarning(credentialsLog) << "could not remove the session token from the store";
    });
    const qsizetype before = m_entries.size();
    m_entries.removeIf([&](const ServerCredential &entry) { return entry.serverId == serverId; });
    if (m_entries.size() == before)
        return;
    // A forgotten session must not come back: when the rewrite fails the whole file goes
    if (!persist() && !QFile::remove(serverCredentialsPath(m_profileId)))
        qCWarning(credentialsLog) << "could not remove the stored credentials";
    Q_EMIT credentialsChanged();
}

bool ServerCredentialSync::persist()
{
    if (!m_writable)
        return false;
    QString error;
    if (saveServerCredentials(m_profileId, m_entries, &error))
        return true;
    qCWarning(credentialsLog) << "could not save server credentials:" << error;
    return false;
}
} // namespace Kinefin
