// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FavoritesModel.h"

#include "api/MediaSourceSelection.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QVariantMap>

#include <algorithm>

namespace Kinefin {
namespace {
// Remote text for a label: control, format and line-separator characters become spaces, then whitespace is collapsed and the result capped
QString plainLine(const QString &text, int limit)
{
    QString clean = text.left(limit);
    for (QChar &c : clean) {
        const QChar::Category category = c.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            c = QLatin1Char(' ');
    }
    return clean.simplified();
}

// The first valid entry among the first genres of the item
QString primaryGenre(const QJsonValue &value)
{
    const QJsonArray genres = value.toArray();
    for (qsizetype i = 0; i < genres.size() && i < FavoritesModel::MaxGenres; ++i) {
        const QString genre = plainLine(genres.at(i).toString(), FavoritesModel::MaxGenreChars);
        if (!genre.isEmpty())
            return genre;
    }
    return {};
}

// The video types a favorite can have
bool isFavoriteType(const QString &type)
{
    return type == QLatin1String("Movie") || type == QLatin1String("Series")
           || type == QLatin1String("Season") || type == QLatin1String("Episode")
           || type == QLatin1String("BoxSet");
}

// The item types to list in a library of this `CollectionType`, empty for a library that is not scanned
QString includeTypesFor(const QString &collectionType)
{
    if (collectionType == QLatin1String("movies"))
        return QStringLiteral("Movie");
    if (collectionType == QLatin1String("tvshows"))
        return QStringLiteral("Series,Season,Episode");
    if (collectionType == QLatin1String("boxsets"))
        return QStringLiteral("BoxSet");
    return {};
}

// What the card shows as the name: an episode is listed under its series
const QString &displayName(const ItemSummary &item)
{
    return item.type == QLatin1String("Episode") && !item.seriesName.isEmpty() ? item.seriesName
                                                                               : item.name;
}
} // namespace

FavoritesModel::FavoritesModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    if (m_client) {
        connect(m_client, &JellyfinClient::itemsReady, this, &FavoritesModel::onItemsReady);
        connect(m_client, &JellyfinClient::listingFailed, this, &FavoritesModel::onListingFailed);
        // The scan is a snapshot. A successful write from an item page makes it
        // stale, including the groupings built from it.
        connect(m_client, &JellyfinClient::itemFavoriteUpdated, this, [this] {
            if (m_active)
                startLoad();
            else
                discardSnapshot();
        });
        // Watched badges in the Favorites snapshot must also reflect manual changes.
        connect(m_client, &JellyfinClient::itemPlayedUpdated, this, [this] {
            if (m_active)
                startLoad();
            else
                discardSnapshot();
        });
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
}

void FavoritesModel::setLibraries(const QVariantList &libraries)
{
    std::vector<Library> next;
    QSet<QString> seen;
    for (const QVariant &entry : libraries) {
        const QVariantMap map = entry.toMap();
        const QString id = map.value(QStringLiteral("id")).toString();
        const QString types =
            includeTypesFor(map.value(QStringLiteral("collectionType")).toString());
        if (types.isEmpty() || !isValidJellyfinId(id) || seen.contains(id.toLower()))
            continue;
        seen.insert(id.toLower());
        Library library;
        library.id = id;
        library.name = plainLine(map.value(QStringLiteral("name")).toString(), MaxLibraryNameChars);
        library.includeItemTypes = types;
        next.push_back(std::move(library));
    }
    const bool same = std::equal(next.begin(), next.end(), m_libraries.begin(), m_libraries.end(),
                                 [](const Library &a, const Library &b) {
                                     return a.id == b.id && a.name == b.name
                                            && a.includeItemTypes == b.includeItemTypes;
                                 });
    if (same)
        return;
    m_libraries = std::move(next);
    discardSnapshot();
    Q_EMIT librariesChanged();
    if (m_active)
        startLoad();
}

void FavoritesModel::load()
{
    m_active = true;
    if (m_status == Idle)
        startLoad();
}

void FavoritesModel::reload()
{
    m_active = true;
    startLoad();
}

void FavoritesModel::close()
{
    m_active = false;
    if (m_status == Loading)
        discardSnapshot();
    else if (m_client)
        m_client->releaseListingChannel(Channel);
}

void FavoritesModel::clear()
{
    m_active = false;
    discardSnapshot();
    if (!m_libraries.empty()) {
        m_libraries.clear();
        Q_EMIT librariesChanged();
    }
}

int FavoritesModel::librariesDone() const
{
    return int(std::count_if(m_libraries.begin(), m_libraries.end(),
                             [](const Library &library) { return library.done; }));
}

QString FavoritesModel::libraryName(const QString &libraryId) const
{
    const int order = libraryOrder(libraryId);
    return order < 0 ? QString() : m_libraries[size_t(order)].name;
}

int FavoritesModel::libraryOrder(const QString &libraryId) const
{
    for (size_t i = 0; i < m_libraries.size(); ++i) {
        if (m_libraries[i].id == libraryId)
            return int(i);
    }
    return -1;
}

void FavoritesModel::setStatus(Status status)
{
    if (status == m_status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

// Forgets the snapshot and the scan under way and goes back to Idle: nothing of an older scan can arrive afterwards, the channel is released and the generation moves on
void FavoritesModel::discardSnapshot()
{
    ++m_generation;
    m_inFlight = false;
    m_pending = -1;
    if (m_client)
        m_client->releaseListingChannel(Channel);
    m_staging.clear();
    m_stagingIndex.clear();
    m_capRejected = false;
    for (Library &library : m_libraries) {
        library.offset = 0;
        library.total = -1;
        library.done = false;
        library.failed = false;
    }
    if (!m_items.empty()) {
        beginResetModel();
        m_items.clear();
        endResetModel();
        Q_EMIT countChanged();
    }
    if (m_capped || m_libraryFailed) {
        m_capped = false;
        m_libraryFailed = false;
        Q_EMIT noticeChanged();
    }
    setStatus(Idle);
    Q_EMIT progressChanged();
}

void FavoritesModel::startLoad()
{
    discardSnapshot();
    if (!m_client || !m_client->isAuthenticated())
        return;
    m_userId = m_client->userId();
    m_next = 0;
    setStatus(Loading);
    Q_EMIT progressChanged();
    requestNext();
}

// Sends the next page of the next library that has one, or ends the scan. A request the client refuses fails its library, the scan goes on with the others
void FavoritesModel::requestNext()
{
    const int libraries = int(m_libraries.size());
    while (true) {
        if (int(m_staging.size()) >= MaxItems) {
            finish();
            return;
        }
        int pick = -1;
        for (int step = 0; step < libraries; ++step) {
            const int index = (m_next + step) % libraries;
            if (!m_libraries[size_t(index)].done) {
                pick = index;
                break;
            }
        }
        if (pick < 0) {
            finish();
            return;
        }
        const Library &library = m_libraries[size_t(pick)];
        ItemQuery query;
        query.parentId = library.id;
        query.includeItemTypes = library.includeItemTypes;
        query.recursive = true;
        query.filters = QStringLiteral("IsFavorite");
        query.fields = {QStringLiteral("Genres"), QStringLiteral("ChildCount"),
                        QStringLiteral("RecursiveItemCount")};
        query.cardImages = true;
        query.sortBy = QStringLiteral("SortName");
        query.sortOrder = QStringLiteral("Ascending");
        query.startIndex = library.offset;
        query.limit = PageSize;
        m_pending = pick;
        m_sentGeneration = m_generation;
        m_inFlight = true;
        if (m_client->fetchItems(query, Channel))
            return;
        m_inFlight = false;
        failLibrary(pick);
    }
}

void FavoritesModel::failLibrary(int index)
{
    Library &library = m_libraries[size_t(index)];
    library.done = true;
    library.failed = true;
    m_next = (index + 1) % int(m_libraries.size());
    Q_EMIT progressChanged();
}

// A page that cannot be true is a failure, not a success: more rows than asked for, a total below what was already received, or an empty page while the total says more follow
bool FavoritesModel::acceptsPage(const Library &library, const QJsonArray &items, int total) const
{
    if (items.size() > PageSize || total < 0 || total < library.offset + items.size())
        return false;
    return !(items.isEmpty() && library.offset < total);
}

void FavoritesModel::addItems(const Library &library, const QJsonArray &items)
{
    for (const QJsonValue &value : items) {
        const QJsonObject object = value.toObject();
        ItemSummary summary = parseItemSummary(object);
        // The server filters by favorite and by type, so a missing UserData does not drop an item; an id or a type that cannot be right does
        if (!summary.valid || !isFavoriteType(summary.type))
            continue;
        const QString key = summary.id.toLower();
        const auto found = m_stagingIndex.constFind(key);
        if (found != m_stagingIndex.constEnd()) {
            QStringList &ids = m_staging[size_t(found.value())].libraryIds;
            if (!ids.contains(library.id))
                ids.append(library.id);
            continue;
        }
        if (int(m_staging.size()) >= MaxItems) {
            m_capRejected = true;
            continue;
        }
        FavoriteItem favorite;
        favorite.summary = std::move(summary);
        favorite.libraryIds = QStringList{library.id};
        favorite.primaryGenre = primaryGenre(object.value(QStringLiteral("Genres")));
        m_stagingIndex.insert(key, int(m_staging.size()));
        m_staging.push_back(std::move(favorite));
    }
}

void FavoritesModel::onItemsReady(const QJsonArray &items, int total, int channel)
{
    if (channel != Channel || !m_inFlight || m_sentGeneration != m_generation)
        return;
    m_inFlight = false;
    // Another user's session under the same client: nothing of this scan may be shown
    if (m_client->userId() != m_userId) {
        clear();
        return;
    }
    const int index = m_pending;
    Library &library = m_libraries[size_t(index)];
    if (!acceptsPage(library, items, total)) {
        failLibrary(index);
        requestNext();
        return;
    }
    addItems(library, items);
    library.offset += int(items.size());
    library.total = total;
    library.done = library.offset >= total;
    m_next = (index + 1) % int(m_libraries.size());
    Q_EMIT progressChanged();
    requestNext();
}

void FavoritesModel::onListingFailed(const QString &family, int status, const QString &reason,
                                     int channel)
{
    Q_UNUSED(status)
    Q_UNUSED(reason)
    if (family != QLatin1String("Items") || channel != Channel || !m_inFlight
        || m_sentGeneration != m_generation)
        return;
    m_inFlight = false;
    failLibrary(m_pending);
    requestNext();
}

// Publishes the scan: sorted by name (locale collation) then season, episode and id, so the order never depends on the order the pages arrived in
void FavoritesModel::finish()
{
    bool more = m_capRejected;
    bool failed = false;
    for (const Library &library : m_libraries) {
        more = more || !library.done;
        failed = failed || library.failed;
    }
    for (FavoriteItem &favorite : m_staging) {
        std::sort(favorite.libraryIds.begin(), favorite.libraryIds.end(),
                  [this](const QString &a, const QString &b) {
                      return libraryOrder(a) < libraryOrder(b);
                  });
    }
    std::sort(
        m_staging.begin(), m_staging.end(), [this](const FavoriteItem &a, const FavoriteItem &b) {
            const int byName = m_collator.compare(displayName(a.summary), displayName(b.summary));
            if (byName != 0)
                return byName < 0;
            if (a.summary.seasonNumber != b.summary.seasonNumber)
                return a.summary.seasonNumber < b.summary.seasonNumber;
            if (a.summary.episodeNumber != b.summary.episodeNumber)
                return a.summary.episodeNumber < b.summary.episodeNumber;
            return a.summary.id < b.summary.id;
        });
    m_stagingIndex.clear();
    const bool hasItems = !m_staging.empty();
    beginResetModel();
    m_items = std::move(m_staging);
    m_staging.clear();
    endResetModel();
    if (hasItems)
        Q_EMIT countChanged();
    if (m_capped != more || m_libraryFailed != failed) {
        m_capped = more;
        m_libraryFailed = failed;
        Q_EMIT noticeChanged();
    }
    Q_EMIT progressChanged();
    setStatus(failed ? PartialError : more ? Capped : Complete);
}

int FavoritesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QHash<int, QByteArray> FavoritesModel::roleNames() const
{
    QHash<int, QByteArray> names{{ItemIdRole, "itemId"},
                                 {NameRole, "name"},
                                 {EpisodeTitleRole, "episodeTitle"},
                                 {SeasonNumberRole, "seasonNumber"},
                                 {EpisodeNumberRole, "episodeNumber"},
                                 {YearRole, "year"},
                                 {TypeRole, "type"},
                                 {ImageUrlRole, "imageUrl"},
                                 {PlayedRole, "played"},
                                 {UnplayedCountRole, "unplayedCount"},
                                 {PlayedPercentageRole, "playedPercentage"},
                                 {FavoriteRole, "favorite"},
                                 {LibraryIdsRole, "libraryIds"},
                                 {LibraryNamesRole, "libraryNames"},
                                 {GenreRole, "genre"}};
    names.insert(cardRoleNames());
    return names;
}

QVariant FavoritesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0 || index.row() >= count())
        return {};
    const FavoriteItem &favorite = m_items[size_t(index.row())];
    const ItemSummary &item = favorite.summary;
    const bool episode = item.type == QLatin1String("Episode");
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return displayName(item);
    case EpisodeTitleRole:
        return episode ? item.name : QString();
    case SeasonNumberRole:
        return item.seasonNumber;
    case EpisodeNumberRole:
        return item.episodeNumber;
    case ItemIdRole:
        return item.id;
    case YearRole:
        return item.year;
    case TypeRole:
        return item.type;
    case ImageUrlRole:
        return cardImageUrl(m_client, item, false, PosterWidth);
    case PlayedRole:
        return item.played;
    case UnplayedCountRole:
        return item.unplayedCount;
    case PlayedPercentageRole:
        return item.playedPercentage;
    case FavoriteRole:
        return item.favorite;
    case LibraryIdsRole:
        return QVariant::fromValue(favorite.libraryIds);
    case LibraryNamesRole: {
        QStringList names;
        for (const QString &id : favorite.libraryIds)
            names.append(libraryName(id));
        return QVariant::fromValue(names);
    }
    case GenreRole:
        return favorite.primaryGenre;
    default:
        return cardRoleData(item, role);
    }
}
} // namespace Kinefin
