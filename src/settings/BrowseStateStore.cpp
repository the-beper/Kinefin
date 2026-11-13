// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrowseStateStore.h"

#include "api/MediaSourceSelection.h"
#include "settings/ProfilePaths.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
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
    for (const QLatin1String exact :
         {QLatin1String("movies/favorites"), QLatin1String("movies/collections"),
          QLatin1String("tvshows/favorites"), QLatin1String("tvshows/episodes")}) {
        if (scope == exact)
            return true;
    }
    for (const QLatin1String prefix :
         {QLatin1String("tvshows:"), QLatin1String("movies:"), QLatin1String("boxsets:"),
          QLatin1String("movies/favorites:"), QLatin1String("tvshows/favorites:"),
          QLatin1String("tvshows/episodes:"), QLatin1String("tvshows/network:")}) {
        if (scope.startsWith(prefix))
            return isValidJellyfinId(QStringView(scope).mid(prefix.size()));
    }
    return false;
}

// The scopes that list series, over which there is no resumable filter
bool seriesScope(const QString &scope)
{
    return scope == QLatin1String("tvshows") || scope.startsWith(QLatin1String("tvshows:"))
           || scope == QLatin1String("tvshows/favorites")
           || scope.startsWith(QLatin1String("tvshows/favorites:"))
           || scope.startsWith(QLatin1String("tvshows/network:"));
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

const std::array kStatusFlags{QLatin1String("IsPlayed"), QLatin1String("IsUnplayed"),
                              QLatin1String("IsResumable"), QLatin1String("IsFavorite")};
const std::array kFeatures{QLatin1String("HasSubtitles"), QLatin1String("HasTrailer"),
                           QLatin1String("HasSpecialFeature"), QLatin1String("HasThemeSong"),
                           QLatin1String("HasThemeVideo")};
const std::array kQualities{QLatin1String("HD"), QLatin1String("SD")};
const std::array kVideoTypes{QLatin1String("BluRay"), QLatin1String("Dvd")};
const std::array kSeriesStatus{QLatin1String("Continuing"), QLatin1String("Ended"),
                               QLatin1String("Unreleased")};
const std::array kEpisodeKinds{QLatin1String("All"), QLatin1String("Specials"),
                               QLatin1String("Missing"), QLatin1String("Unaired")};
const std::array kViewModes{QLatin1String("grid"), QLatin1String("list")};
const std::array kImageTypes{QLatin1String("Primary"), QLatin1String("Banner"),
                             QLatin1String("Disc"), QLatin1String("Logo"), QLatin1String("Thumb")};
const std::array kFavoritesGroupings{QLatin1String("Type"), QLatin1String("Library"),
                                     QLatin1String("Genre"), QLatin1String("Year"),
                                     QLatin1String("None")};

template <typename Tokens> bool oneOf(const Tokens &allowed, const QString &token)
{
    return std::any_of(allowed.cbegin(), allowed.cend(),
                       [&token](QLatin1String entry) { return token == entry; });
}

// Known tokens only, each at most once
template <typename Tokens> bool validTokens(const Tokens &allowed, const QStringList &tokens)
{
    return tokens.size() <= qsizetype(allowed.size())
           && std::all_of(tokens.cbegin(), tokens.cend(),
                          [&allowed](const QString &token) { return oneOf(allowed, token); })
           && QSet<QString>(tokens.cbegin(), tokens.cend()).size() == tokens.size();
}

// A SHA-256 digest as `hashText` writes it
bool validHash(const QString &hash)
{
    return hash.size() == 64 && std::all_of(hash.cbegin(), hash.cend(), [](QChar c) {
               const char16_t u = c.unicode();
               return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f');
           });
}

bool validHashes(const QStringList &hashes)
{
    return hashes.size() <= BrowseStateStore::MaxHashes
           && std::all_of(hashes.cbegin(), hashes.cend(), validHash);
}

bool validView(const BrowseStateStore::ViewState &state)
{
    return validToken(state.sortKey) && validTokens(kStatusFlags, state.statusFlags)
           && validTokens(kFeatures, state.features)
           && (state.quality.isEmpty() || oneOf(kQualities, state.quality))
           && validTokens(kVideoTypes, state.videoTypes)
           && state.genreIds.size() <= BrowseStateStore::MaxGenreIds
           && std::all_of(state.genreIds.cbegin(), state.genreIds.cend(),
                          [](const QString &id) { return isValidJellyfinId(id); })
           && validHashes(state.tagHashes) && validHashes(state.ratingHashes)
           && state.years.size() <= BrowseStateStore::MaxYears
           && std::all_of(state.years.cbegin(), state.years.cend(),
                          [](int year) {
                              return year >= BrowseStateStore::MinYear
                                     && year <= BrowseStateStore::MaxYear;
                          })
           && validTokens(kSeriesStatus, state.seriesStatus)
           && oneOf(kEpisodeKinds, state.episodeKind) && oneOf(kViewModes, state.viewMode)
           && oneOf(kImageTypes, state.imageType);
}

// The readers keep the default of an absent member and fail on one of the wrong type
bool readString(const QJsonObject &entry, QLatin1String key, QString &out)
{
    const QJsonValue value = entry.value(key);
    if (value.isUndefined())
        return true;
    if (!value.isString())
        return false;
    out = value.toString();
    return true;
}

bool readBool(const QJsonObject &entry, QLatin1String key, bool &out)
{
    const QJsonValue value = entry.value(key);
    if (value.isUndefined())
        return true;
    if (!value.isBool())
        return false;
    out = value.toBool();
    return true;
}

bool readStrings(const QJsonObject &entry, QLatin1String key, QStringList &out)
{
    const QJsonValue value = entry.value(key);
    if (value.isUndefined())
        return true;
    if (!value.isArray())
        return false;
    for (const QJsonValue &item : value.toArray()) {
        if (!item.isString())
            return false;
        out.append(item.toString());
    }
    return true;
}

bool readYears(const QJsonObject &entry, QLatin1String key, QList<int> &out)
{
    const QJsonValue value = entry.value(key);
    if (value.isUndefined())
        return true;
    if (!value.isArray())
        return false;
    for (const QJsonValue &item : value.toArray()) {
        const double year = item.toDouble(-1);
        // The range is checked by `validView`: this only keeps the conversion defined
        if (!item.isDouble() || year != std::floor(year) || year < 0 || year > 1e9)
            return false;
        out.append(int(year));
    }
    return true;
}

std::optional<BrowseStateStore::ViewState> readViewV2(const QJsonObject &entry)
{
    BrowseStateStore::ViewState state;
    const bool read = readString(entry, QLatin1String("sortKey"), state.sortKey)
                      && readBool(entry, QLatin1String("descending"), state.descending)
                      && readStrings(entry, QLatin1String("statusFlags"), state.statusFlags)
                      && readStrings(entry, QLatin1String("features"), state.features)
                      && readString(entry, QLatin1String("quality"), state.quality)
                      && readBool(entry, QLatin1String("is4K"), state.is4K)
                      && readBool(entry, QLatin1String("is3D"), state.is3D)
                      && readStrings(entry, QLatin1String("videoTypes"), state.videoTypes)
                      && readStrings(entry, QLatin1String("genreIds"), state.genreIds)
                      && readStrings(entry, QLatin1String("tagHashes"), state.tagHashes)
                      && readStrings(entry, QLatin1String("ratingHashes"), state.ratingHashes)
                      && readYears(entry, QLatin1String("years"), state.years)
                      && readStrings(entry, QLatin1String("seriesStatus"), state.seriesStatus)
                      && readString(entry, QLatin1String("episodeKind"), state.episodeKind)
                      && readString(entry, QLatin1String("viewMode"), state.viewMode)
                      && readString(entry, QLatin1String("imageType"), state.imageType)
                      && readBool(entry, QLatin1String("showTitle"), state.showTitle)
                      && readBool(entry, QLatin1String("showYear"), state.showYear)
                      && readBool(entry, QLatin1String("cardLayout"), state.cardLayout);
    if (!read)
        return std::nullopt;
    return state;
}

// The version 1 view: one sort, one filter name and the genres. A filter name that is not known leaves no status flag
std::optional<BrowseStateStore::ViewState> readViewV1(const QJsonObject &entry)
{
    const QJsonValue genreIds = entry.value(QStringLiteral("genreIds"));
    const QJsonValue descending = entry.value(QStringLiteral("descending"));
    const QString filter = entry.value(QStringLiteral("filter")).toString();
    if (!genreIds.isArray() || !descending.isBool() || !validToken(filter))
        return std::nullopt;
    BrowseStateStore::ViewState state;
    state.sortKey = entry.value(QStringLiteral("sort")).toString();
    state.descending = descending.toBool();
    for (const QJsonValue &genre : genreIds.toArray())
        state.genreIds.append(genre.toString());
    if (filter == QLatin1String("Unplayed"))
        state.statusFlags = {QStringLiteral("IsUnplayed")};
    else if (filter == QLatin1String("Played"))
        state.statusFlags = {QStringLiteral("IsPlayed")};
    else if (filter == QLatin1String("Resumable"))
        state.statusFlags = {QStringLiteral("IsResumable")};
    else if (filter == QLatin1String("Favorites"))
        state.statusFlags = {QStringLiteral("IsFavorite")};
    return state;
}

QJsonArray toJson(const QList<int> &years)
{
    QJsonArray array;
    for (const int year : years)
        array.append(year);
    return array;
}
} // namespace

QString BrowseStateStore::hashText(const QString &text)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

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
    if (seriesScope(scopeKey))
        state.statusFlags.removeAll(QStringLiteral("IsResumable"));
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

QString BrowseStateStore::favoritesGrouping(const QString &serverId, const QString &userId) const
{
    const UserState *user = userState(serverId, userId);
    return user && !user->favoritesGrouping.isEmpty() ? user->favoritesGrouping
                                                      : QStringLiteral("Type");
}

void BrowseStateStore::setFavoritesGrouping(const QString &serverId, const QString &userId,
                                            const QString &grouping)
{
    if (!isValidJellyfinId(serverId) || !isValidJellyfinId(userId)
        || !oneOf(kFavoritesGroupings, grouping) || favoritesGrouping(serverId, userId) == grouping)
        return;
    m_servers[serverId][userId].favoritesGrouping = grouping;
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
    if (user.selection.isEmpty() && user.views.isEmpty() && user.favoritesGrouping.isEmpty())
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
                const ViewState &state = view->state;
                views.insert(
                    view.key(),
                    QJsonObject{
                        {QStringLiteral("sortKey"), state.sortKey},
                        {QStringLiteral("descending"), state.descending},
                        {QStringLiteral("statusFlags"),
                         QJsonArray::fromStringList(state.statusFlags)},
                        {QStringLiteral("features"), QJsonArray::fromStringList(state.features)},
                        {QStringLiteral("quality"), state.quality},
                        {QStringLiteral("is4K"), state.is4K},
                        {QStringLiteral("is3D"), state.is3D},
                        {QStringLiteral("videoTypes"),
                         QJsonArray::fromStringList(state.videoTypes)},
                        {QStringLiteral("genreIds"), QJsonArray::fromStringList(state.genreIds)},
                        {QStringLiteral("tagHashes"), QJsonArray::fromStringList(state.tagHashes)},
                        {QStringLiteral("ratingHashes"),
                         QJsonArray::fromStringList(state.ratingHashes)},
                        {QStringLiteral("years"), toJson(state.years)},
                        {QStringLiteral("seriesStatus"),
                         QJsonArray::fromStringList(state.seriesStatus)},
                        {QStringLiteral("episodeKind"), state.episodeKind},
                        {QStringLiteral("viewMode"), state.viewMode},
                        {QStringLiteral("imageType"), state.imageType},
                        {QStringLiteral("showTitle"), state.showTitle},
                        {QStringLiteral("showYear"), state.showYear},
                        {QStringLiteral("cardLayout"), state.cardLayout},
                        {QStringLiteral("seq"), view->seq}});
            }
            QJsonObject stored{{QStringLiteral("selection"), selection},
                               {QStringLiteral("views"), views}};
            if (!user->favoritesGrouping.isEmpty())
                stored.insert(QStringLiteral("favoritesGrouping"), user->favoritesGrouping);
            users.insert(user.key(), stored);
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
    // Version 1 is read and written back as the current one on the next save
    if (!version.isDouble() || version.toInt() < 1
        || !root.value(QStringLiteral("servers")).isObject()) {
        QFile::remove(m_path);
        return;
    }

    qint64 highest = 0;
    loadUsers(root.value(QStringLiteral("servers")).toObject(), version.toInt(), highest);
    m_nextSeq = std::clamp<qint64>(
        std::max<qint64>({root.value(QStringLiteral("nextSeq")).toInteger(1), highest + 1, 1}), 1,
        MaxSeq);
    while (viewCount() > MaxViews && evictOldest()) {
    }
}

void BrowseStateStore::loadUsers(const QJsonObject &servers, int version, qint64 &highest)
{
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
            const QString grouping = stored.value(QStringLiteral("favoritesGrouping")).toString();
            if (oneOf(kFavoritesGroupings, grouping))
                user.favoritesGrouping = grouping;
            const QJsonObject views = stored.value(QStringLiteral("views")).toObject();
            for (const QString &scope : views.keys()) {
                const QJsonObject entry = views.value(scope).toObject();
                const QJsonValue seq = entry.value(QStringLiteral("seq"));
                if (!validScope(scope) || !seq.isDouble() || seq.toInteger() < 0
                    || seq.toInteger() > MaxSeq)
                    continue;
                const std::optional<ViewState> state =
                    version == 1 ? readViewV1(entry) : readViewV2(entry);
                if (!state || !validView(*state))
                    continue;
                highest = std::max<qint64>(highest, seq.toInteger());
                user.views.insert(scope, StoredView{*state, seq.toInteger()});
            }
            if (!user.selection.isEmpty() || !user.views.isEmpty()
                || !user.favoritesGrouping.isEmpty())
                m_servers[serverId].insert(userId, user);
        }
    }
}
} // namespace Kinefin
