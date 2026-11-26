// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServerCredentialSync.h"

#include <QFile>
#include <QLoggingCategory>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <memory>
#include <utility>

#include "api/JellyfinClient.h"
#include "api/MediaSourceSelection.h"
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
    } else {
        // An id that is not an id would alias another keyring key
        const qsizetype before = m_entries.size();
        m_entries.removeIf([](const ServerCredential &entry) { return !hasValidIds(entry); });
        if (m_entries.size() != before)
            qCWarning(credentialsLog) << "ignoring records with an invalid id";
        if (legacyToken) {
            // An older snapshot kept the token in this file: rewrite it without, now
            qCWarning(credentialsLog) << "dropping a token stored in the credentials file";
            // A rewrite that fails must not leave the plaintext token behind: the file goes
            if (!persist() && !QFile::remove(serverCredentialsPath(m_profileId)))
                qCWarning(credentialsLog)
                    << "could not remove the credentials file with a legacy token";
        }
    }
    connect(client, &JellyfinClient::serverReady, this, &ServerCredentialSync::onServerReady);
    connect(client, &JellyfinClient::loginSucceeded, this, &ServerCredentialSync::onLoginSucceeded);
    connect(client, &JellyfinClient::sessionExpired, this,
            [this](const QString &userId) { onSessionExpired(userId); });
}

QString serverTokenKey(const QString &profileId, const QString &serverId, const QString &userId)
{
    return profileId + QLatin1Char('/') + serverId + QLatin1Char('/') + userId;
}

QString ServerCredentialSync::tokenKey(const QString &serverId, const QString &userId) const
{
    return serverTokenKey(m_profileId, serverId, userId);
}

void forgetProfileTokens(const QString &profileId, TokenStore &store,
                         std::function<void(bool ok)> done)
{
    QString error;
    const QList<ServerCredential> entries = loadServerCredentials(profileId, &error);
    // An unreadable record may hide servers whose tokens are then left in the store
    const bool listed = error.isEmpty();
    // Not filtered by validity: every record that could have left a token behind is covered
    QStringList keys;
    for (const ServerCredential &entry : entries) {
        if (!entry.userId.isEmpty())
            keys.append(serverTokenKey(profileId, entry.serverId, entry.userId));
    }
    keys.removeDuplicates();
    if (keys.isEmpty()) {
        QTimer::singleShot(0, [done = std::move(done), listed] { done(listed); });
        return;
    }
    struct Progress {
        qsizetype left;
        bool ok = true;
        std::function<void(bool)> done;
    };
    auto progress = std::make_shared<Progress>(Progress{keys.size(), listed, std::move(done)});
    for (const QString &key : std::as_const(keys)) {
        store.remove(key, [progress](bool ok) {
            progress->ok = progress->ok && ok;
            if (--progress->left == 0)
                progress->done(progress->ok);
        });
    }
}

QList<ServerCredential> ServerCredentialSync::restorableUsers() const
{
    QList<ServerCredential> users;
    // Nothing to ask the store for when the transport would refuse the token anyway
    if (!m_client->canSendCredentials())
        return users;
    for (const ServerCredential &entry : m_entries) {
        // The token goes only to the server it was issued by: same public id, same resolved base URL (scheme, host, port and path, so a token issued over TLS never travels over plain HTTP and another service on the same host never gets it) and the same device identity
        if (isProbedServer(entry) && entry.deviceId == m_client->deviceId())
            users.append(entry);
    }
    return users;
}

bool ServerCredentialSync::isSavedServer() const
{
    return std::any_of(m_entries.cbegin(), m_entries.cend(),
                       [this](const ServerCredential &entry) { return isProbedServer(entry); });
}

bool ServerCredentialSync::isProbedServer(const ServerCredential &entry) const
{
    return entry.serverId == m_client->serverId() && QUrl(entry.url) == m_client->serverUrl();
}

void ServerCredentialSync::onServerReady()
{
    // A new probe supersedes any restore still waiting for the store (another server, or a reconnect), and ends the session it followed
    m_restorePending = false;
    ++m_restoreSerial;
    m_activeUserId.clear();
}

void ServerCredentialSync::cancelRestore()
{
    m_restorePending = false;
    ++m_restoreSerial;
}

bool ServerCredentialSync::restoreUser(const QString &userId)
{
    ServerCredential entry;
    bool found = false;
    for (const ServerCredential &candidate : restorableUsers()) {
        if (candidate.userId == userId) {
            entry = candidate;
            found = true;
            break;
        }
    }
    if (!found || !entry.remembered || entry.askPassword)
        return false;
    m_restorePending = true;
    const quint64 serial = ++m_restoreSerial;
    QPointer<ServerCredentialSync> self(this);
    m_store->read(tokenKey(entry.serverId, entry.userId),
                  [self, serial, entry](bool ok, const QString &token) {
                      if (!self || serial != self->m_restoreSerial)
                          return;
                      self->finishRestore(entry, ok, token);
                  });
    return true;
}

void ServerCredentialSync::finishRestore(const ServerCredential &entry, bool ok,
                                         const QString &token)
{
    m_restorePending = false;
    // The client moved on while the store answered (cancelled, another server): nobody is waiting for this one any more
    if (m_client->serverId() != entry.serverId || QUrl(entry.url) != m_client->serverUrl())
        return;
    if (ok) {
        // Whatever else happened to the client meanwhile (a password login) decides: restoreSession() only accepts the probed state
        m_restoring = true;
        const bool restored = m_client->restoreSession(entry.userId, token);
        m_restoring = false;
        if (restored) {
            m_activeUserId = entry.userId;
            moveToEnd(entry.serverId, entry.userId);
            m_client->verifySession();
        }
    } else {
        qCWarning(credentialsLog) << "the stored session could not be read";
    }
    Q_EMIT restoreFinished();
}

void ServerCredentialSync::writeToken(const QString &serverId, const QString &userId,
                                      const QString &token, std::function<void(bool ok)> stored)
{
    const QString key = tokenKey(serverId, userId);
    const quint64 epoch = m_forgetEpoch.value(key);
    const quint64 serial = ++m_saveSerial[key];
    QPointer<ServerCredentialSync> self(this);
    m_store->write(key, token, [self, key, epoch, serial, stored = std::move(stored)](bool ok) {
        if (!self)
            return;
        if (!ok) {
            stored(false);
            return;
        }
        if (epoch != self->m_forgetEpoch.value(key)) {
            // Forgotten or revoked while the token was being written: take it out again, unless a newer save of this key owns it by now
            if (serial == self->m_saveSerial.value(key))
                self->m_store->remove(key, [](bool) {});
            return;
        }
        stored(true);
    });
}

void ServerCredentialSync::onLoginSucceeded()
{
    m_activeUserId = m_client->userId();
    if (m_restoring)
        return;
    if (!m_writable) {
        // Nothing may be stored for this profile (invalid id, or its file cannot be handled): the token never reaches the store either
        Q_EMIT saveFailed();
        return;
    }
    // A user already stored keeps what the login does not know: the Ask for Password choice and a name the reply left out
    ServerCredential entry;
    bool known = false;
    if (const ServerCredential *stored = find(m_client->serverId(), m_client->userId())) {
        entry = *stored;
        known = true;
    }
    entry.serverId = m_client->serverId();
    entry.url = m_client->serverUrl().toString();
    entry.userId = m_client->userId();
    entry.deviceId = m_client->deviceId();
    entry.deviceName = m_client->deviceName();
    if (!m_client->userName().isEmpty())
        entry.username = m_client->userName();
    entry.imageTag = m_client->userImageTag();
    if (!hasValidIds(entry)) {
        qCWarning(credentialsLog) << "not storing a session with an invalid id";
        Q_EMIT saveFailed();
        return;
    }
    // The token stored for this user is dead now: the server ends the older session of a user who signs in again on the same device. It goes first, so a save that fails or never finishes leaves nothing to restore
    if (known) {
        forgetToken(entry.serverId, entry.userId);
        m_activeUserId = entry.userId;
    }
    if (entry.askPassword) {
        // Remembered by name only: the token stays in memory
        entry.remembered = false;
        upsert(entry);
        return;
    }
    entry.remembered = true;
    QPointer<ServerCredentialSync> self(this);
    writeToken(entry.serverId, entry.userId, m_client->accessToken(), [self, entry](bool ok) {
        if (!self)
            return;
        if (!ok) {
            qCWarning(credentialsLog) << "could not store the session token";
            Q_EMIT self->saveFailed();
            return;
        }
        self->upsert(entry);
    });
}

void ServerCredentialSync::upsert(const ServerCredential &entry)
{
    // The last entry is the most recently used one
    m_entries.removeIf([&](const ServerCredential &e) {
        return e.serverId == entry.serverId && e.userId == entry.userId;
    });
    m_entries.append(entry);
    if (!persist())
        Q_EMIT saveFailed();
    Q_EMIT credentialsChanged();
}

ServerCredential *ServerCredentialSync::find(const QString &serverId, const QString &userId)
{
    for (ServerCredential &entry : m_entries) {
        if (entry.serverId == serverId && entry.userId == userId)
            return &entry;
    }
    return nullptr;
}

void ServerCredentialSync::moveToEnd(const QString &serverId, const QString &userId)
{
    for (qsizetype i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).serverId == serverId && m_entries.at(i).userId == userId
            && i != m_entries.size() - 1) {
            m_entries.move(i, m_entries.size() - 1);
            persist();
            Q_EMIT credentialsChanged();
            return;
        }
    }
}

void ServerCredentialSync::onSessionExpired(const QString &expiredUserId)
{
    // The client has cleared its own user id by now: it names the user that expired, and the one the sync restored or saw log in is the fallback
    const QString userId = expiredUserId.isEmpty() ? m_activeUserId : expiredUserId;
    if (userId.isEmpty())
        return;
    // The record stays: the user is asked for the password again
    forgetToken(m_client->serverId(), userId);
}

void ServerCredentialSync::signOut()
{
    const QString serverId = m_client->serverId();
    const QString userId = m_client->userId().isEmpty() ? m_activeUserId : m_client->userId();
    if (!userId.isEmpty())
        forgetUser(serverId, userId);
    m_activeUserId.clear();
    m_client->disconnectFromServer();
}

void ServerCredentialSync::removeKey(const QString &key)
{
    m_store->remove(key, [](bool ok) {
        if (!ok)
            qCWarning(credentialsLog) << "could not remove the session token from the store";
    });
}

void ServerCredentialSync::dropToken(const QString &serverId, const QString &userId)
{
    // Always asked, even without a record: a token whose save is still in flight is caught by the epoch, one already stored is removed here
    const QString key = tokenKey(serverId, userId);
    ++m_forgetEpoch[key];
    removeKey(key);
}

void ServerCredentialSync::forgetToken(const QString &serverId, const QString &userId)
{
    ServerCredential *entry = find(serverId, userId);
    const bool changed = entry && entry->remembered;
    dropToken(serverId, userId);
    if (m_activeUserId == userId && m_client->serverId() == serverId)
        m_activeUserId.clear();
    if (!changed)
        return;
    entry->remembered = false;
    persist();
    Q_EMIT credentialsChanged();
}

void ServerCredentialSync::forgetUser(const QString &serverId, const QString &userId)
{
    dropToken(serverId, userId);
    if (m_activeUserId == userId && m_client->serverId() == serverId)
        m_activeUserId.clear();
    const qsizetype before = m_entries.size();
    m_entries.removeIf(
        [&](const ServerCredential &e) { return e.serverId == serverId && e.userId == userId; });
    if (m_entries.size() == before)
        return;
    persistOrDrop();
    Q_EMIT credentialsChanged();
}

void ServerCredentialSync::forgetServer(const QString &serverId)
{
    // The users of the server: the stored ones and any whose first save is still in flight
    QStringList users;
    for (const ServerCredential &entry : std::as_const(m_entries)) {
        if (entry.serverId == serverId)
            users.append(entry.userId);
    }
    const QString prefix = tokenKey(serverId, QString());
    for (auto it = m_saveSerial.cbegin(); it != m_saveSerial.cend(); ++it) {
        if (it.key().startsWith(prefix))
            users.append(it.key().mid(prefix.size()));
    }
    users.removeDuplicates();
    for (const QString &userId : std::as_const(users)) {
        ++m_forgetEpoch[tokenKey(serverId, userId)];
        removeKey(tokenKey(serverId, userId));
    }
    if (m_client->serverId() == serverId)
        m_activeUserId.clear();
    const qsizetype before = m_entries.size();
    m_entries.removeIf([&](const ServerCredential &entry) { return entry.serverId == serverId; });
    if (m_entries.size() == before)
        return;
    persistOrDrop();
    Q_EMIT credentialsChanged();
}

void ServerCredentialSync::setAskPassword(const QString &serverId, const QString &userId,
                                          bool askPassword)
{
    ServerCredential *entry = find(serverId, userId);
    if (!entry || entry->askPassword == askPassword)
        return;
    entry->askPassword = askPassword;
    if (askPassword) {
        entry->remembered = false;
        dropToken(serverId, userId);
    }
    persist();
    Q_EMIT credentialsChanged();
}

void ServerCredentialSync::updateUser(const QString &serverId, const QString &userId,
                                      const QString &username, const QString &imageTag)
{
    ServerCredential *entry = find(serverId, userId);
    if (!entry)
        return;
    const QString name = username.isEmpty() ? entry->username : username;
    const QString tag = isValidJellyfinId(imageTag) ? imageTag : QString();
    if (entry->username == name && entry->imageTag == tag)
        return;
    entry->username = name;
    entry->imageTag = tag;
    persist();
    Q_EMIT credentialsChanged();
}

void ServerCredentialSync::persistOrDrop()
{
    // A forgotten session must not come back: when the rewrite fails the whole file goes
    if (!persist() && !QFile::remove(serverCredentialsPath(m_profileId)))
        qCWarning(credentialsLog) << "could not remove the stored credentials";
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
