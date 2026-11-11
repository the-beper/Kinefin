// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrowseStateStore.h"

#include "api/MediaSourceSelection.h"
#include "settings/ProfilePaths.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <limits>

namespace Kinefin {
namespace {
bool validType(const QString &type)
{
    return type == QLatin1String("movies") || type == QLatin1String("tvshows");
}

bool validScope(const QString &scope)
{
    if (validType(scope))
        return true;
    for (const QLatin1String prefix :
         {QLatin1String("tvshows:"), QLatin1String("movies:"), QLatin1String("boxsets:")}) {
        if (scope.startsWith(prefix))
            return isValidJellyfinId(QStringView(scope).mid(prefix.size()));
    }
    return false;
}

bool tvScope(const QString &scope)
{
    return scope == QLatin1String("tvshows") || scope.startsWith(QLatin1String("tvshows:"));
}

// An enum key name: letters and digits only
bool validToken(const QString &token)
{
    if (token.isEmpty() || token.size() > 32)
        return false;
    return std::all_of(token.cbegin(), token.cend(), [](QChar c) {
        const char16_t u = c.unicode();
        return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
    });
}

bool validView(const BrowseStateStore::ViewState &state)
{
    return validToken(state.sort) && validToken(state.filter)
           && state.genreIds.size() <= BrowseStateStore::MaxGenreIds
           && std::all_of(state.genreIds.cbegin(), state.genreIds.cend(),
                          [](const QString &id) { return isValidJellyfinId(id); });
}
} // namespace

BrowseStateStore::BrowseStateStore(QObject *parent) : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1000);
    connect(&m_saveTimer, &QTimer::timeout, this, &BrowseStateStore::flush);
}

BrowseStateStore::~BrowseStateStore()
{
    flush();
}

void BrowseStateStore::setStoragePath(const QString &path)
{
    // What is pending belongs to the file it was meant for
    flush();
    m_path = path;
    load();
}

void BrowseStateStore::setSaveDelay(std::chrono::milliseconds delay)
{
    m_saveTimer.setInterval(delay);
}

const BrowseStateStore::UserState *BrowseStateStore::userState(const QString &serverId,
                                                               const QString &userId) const
{
    const auto server = m_servers.constFind(serverId);
    if (server == m_servers.constEnd())
        return nullptr;
    const auto user = server->constFind(userId);
    return user == server->constEnd() ? nullptr : &user.value();
}

QString BrowseStateStore::selection(const QString &serverId, const QString &userId,
                                    const QString &type) const
{
    const UserState *user = userState(serverId, userId);
    return user ? user->selection.value(type) : QString();
}

void BrowseStateStore::setSelection(const QString &serverId, const QString &userId,
                                    const QString &type, const QString &libraryId)
{
    if (!isValidJellyfinId(serverId) || !isValidJellyfinId(userId) || !validType(type)
        || (!libraryId.isEmpty() && !isValidJellyfinId(libraryId)))
        return;
    if (selection(serverId, userId, type) == libraryId)
        return;
    m_servers[serverId][userId].selection.insert(type, libraryId);
    scheduleSave();
}

std::optional<BrowseStateStore::ViewState> BrowseStateStore::view(const QString &serverId,
                                                                  const QString &userId,
                                                                  const QString &scopeKey) const
{
    const UserState *user = userState(serverId, userId);
    if (!user)
        return std::nullopt;
    const auto stored = user->views.constFind(scopeKey);
    if (stored == user->views.constEnd())
        return std::nullopt;
    ViewState state = stored->state;
    if (state.filter == QLatin1String("Resumable") && tvScope(scopeKey))
        state.filter = QStringLiteral("All");
    return state;
}

void BrowseStateStore::setView(const QString &serverId, const QString &userId,
                               const QString &scopeKey, const ViewState &state)
{
    if (!isValidJellyfinId(serverId) || !isValidJellyfinId(userId) || !validScope(scopeKey)
        || !validView(state))
        return;
    const UserState *user = userState(serverId, userId);
    if (user) {
        const auto stored = user->views.constFind(scopeKey);
        if (stored != user->views.constEnd() && stored->state == state)
            return;
    }
    m_servers[serverId][userId].views.insert(scopeKey, StoredView{state, m_nextSeq++});
    while (viewCount() > MaxViews && evictOldest()) {
    }
    scheduleSave();
}

void BrowseStateStore::forgetServer(const QString &serverId)
{
    if (!m_servers.remove(serverId))
        return;
    scheduleSave();
}

int BrowseStateStore::viewCount() const
{
    int count = 0;
    for (const Users &users : m_servers) {
        for (const UserState &user : users)
            count += int(user.views.size());
    }
    return count;
}

bool BrowseStateStore::evictOldest()
{
    QString serverId;
    QString userId;
    QString scope;
    qint64 oldest = std::numeric_limits<qint64>::max();
    bool found = false;
    for (auto server = m_servers.cbegin(); server != m_servers.cend(); ++server) {
        for (auto user = server->cbegin(); user != server->cend(); ++user) {
            for (auto view = user->views.cbegin(); view != user->views.cend(); ++view) {
                if (!found || view->seq < oldest) {
                    found = true;
                    oldest = view->seq;
                    serverId = server.key();
                    userId = user.key();
                    scope = view.key();
                }
            }
        }
    }
    if (!found)
        return false;
    Users &users = m_servers[serverId];
    UserState &user = users[userId];
    user.views.remove(scope);
    if (user.selection.isEmpty() && user.views.isEmpty())
        users.remove(userId);
    if (users.isEmpty())
        m_servers.remove(serverId);
    return true;
}

void BrowseStateStore::scheduleSave()
{
    if (m_path.isEmpty())
        return;
    m_dirty = true;
    m_saveTimer.start();
}

QByteArray BrowseStateStore::serialize() const
{
    QJsonObject servers;
    for (auto server = m_servers.cbegin(); server != m_servers.cend(); ++server) {
        QJsonObject users;
        for (auto user = server->cbegin(); user != server->cend(); ++user) {
            QJsonObject selection;
            for (auto entry = user->selection.cbegin(); entry != user->selection.cend(); ++entry)
                selection.insert(entry.key(), entry.value());
            QJsonObject views;
            for (auto view = user->views.cbegin(); view != user->views.cend(); ++view) {
                views.insert(view.key(),
                             QJsonObject{{QStringLiteral("sort"), view->state.sort},
                                         {QStringLiteral("descending"), view->state.descending},
                                         {QStringLiteral("filter"), view->state.filter},
                                         {QStringLiteral("genreIds"),
                                          QJsonArray::fromStringList(view->state.genreIds)},
                                         {QStringLiteral("seq"), view->seq}});
            }
            users.insert(user.key(), QJsonObject{{QStringLiteral("selection"), selection},
                                                 {QStringLiteral("views"), views}});
        }
        servers.insert(server.key(), QJsonObject{{QStringLiteral("users"), users}});
    }
    const QJsonObject root{{QStringLiteral("version"), FileVersion},
                           {QStringLiteral("nextSeq"), m_nextSeq},
                           {QStringLiteral("servers"), servers}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void BrowseStateStore::flush()
{
    m_saveTimer.stop();
    if (!m_dirty)
        return;
    m_dirty = false;
    if (m_path.isEmpty())
        return;
    if (m_servers.isEmpty()) {
        // Removing what is not there is a success
        QFile::remove(m_path);
        m_saveFailing = false;
        return;
    }
    QByteArray data = serialize();
    while (data.size() > MaxFileBytes && evictOldest())
        data = serialize();
    if (writePrivateFile(m_path, data)) {
        m_saveFailing = false;
    } else if (!m_saveFailing) {
        m_saveFailing = true;
        Q_EMIT saveFailed();
    }
}

void BrowseStateStore::load()
{
    m_servers.clear();
    m_nextSeq = 1;
    m_dirty = false;
    m_saveTimer.stop();
    if (m_path.isEmpty())
        return;
    const QFileInfo info(m_path);
    // A dangling link does not exist for QFileInfo but is still in the way
    if (!info.isSymLink() && !info.exists())
        return;
    // A link, a pipe or a device is no state file: it is never opened (a pipe would block, /dev/zero would never end)
    if (info.isSymLink() || !info.isFile() || info.size() > MaxFileBytes) {
        QFile::remove(m_path);
        return;
    }
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        QFile::remove(m_path);
        return;
    }
    // The file may have grown since it was measured: one byte past the limit is enough to tell
    const QByteArray bytes = file.read(MaxFileBytes + 1);
    file.close();
    if (bytes.size() > MaxFileBytes) {
        QFile::remove(m_path);
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    const QJsonValue version = root.value(QStringLiteral("version"));
    if (version.isDouble() && version.toInt() > FileVersion) {
        // A later version wrote it: keep it aside rather than losing it
        QFile::remove(m_path + QStringLiteral(".broken"));
        // A file that cannot be set aside is not overwritten with the older format: nothing is saved this run
        if (!QFile::rename(m_path, m_path + QStringLiteral(".broken")))
            m_path.clear();
        return;
    }
    if (!version.isDouble() || version.toInt() != FileVersion
        || !root.value(QStringLiteral("servers")).isObject()) {
        QFile::remove(m_path);
        return;
    }

    qint64 highest = 0;
    const QJsonObject servers = root.value(QStringLiteral("servers")).toObject();
    for (const QString &serverId : servers.keys()) {
        if (!isValidJellyfinId(serverId))
            continue;
        const QJsonObject users =
            servers.value(serverId).toObject().value(QStringLiteral("users")).toObject();
        for (const QString &userId : users.keys()) {
            if (!isValidJellyfinId(userId))
                continue;
            const QJsonObject stored = users.value(userId).toObject();
            UserState user;
            const QJsonObject selection = stored.value(QStringLiteral("selection")).toObject();
            for (const QString &type : selection.keys()) {
                const QJsonValue id = selection.value(type);
                if (validType(type) && id.isString()
                    && (id.toString().isEmpty() || isValidJellyfinId(id.toString())))
                    user.selection.insert(type, id.toString());
            }
            const QJsonObject views = stored.value(QStringLiteral("views")).toObject();
            for (const QString &scope : views.keys()) {
                const QJsonObject entry = views.value(scope).toObject();
                const QJsonValue genreIds = entry.value(QStringLiteral("genreIds"));
                const QJsonValue seq = entry.value(QStringLiteral("seq"));
                const QJsonValue descending = entry.value(QStringLiteral("descending"));
                if (!validScope(scope) || !genreIds.isArray() || !seq.isDouble()
                    || seq.toInteger() < 0 || seq.toInteger() > MaxSeq || !descending.isBool())
                    continue;
                StoredView view;
                view.state.sort = entry.value(QStringLiteral("sort")).toString();
                view.state.filter = entry.value(QStringLiteral("filter")).toString();
                view.state.descending = descending.toBool();
                for (const QJsonValue &genre : genreIds.toArray())
                    view.state.genreIds.append(genre.toString());
                view.seq = seq.toInteger();
                if (!validView(view.state))
                    continue;
                highest = std::max(highest, view.seq);
                user.views.insert(scope, view);
            }
            if (!user.selection.isEmpty() || !user.views.isEmpty())
                m_servers[serverId].insert(userId, user);
        }
    }
    m_nextSeq = std::clamp<qint64>(
        std::max<qint64>({root.value(QStringLiteral("nextSeq")).toInteger(1), highest + 1, 1}), 1,
        MaxSeq);
    while (viewCount() > MaxViews && evictOldest()) {
    }
}
} // namespace Kinefin
