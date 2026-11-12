// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "LibrarySubtabController.h"

#include "api/MediaSourceSelection.h"

#include <QJsonArray>

namespace Kinefin {
namespace {
const QString kMovies = QStringLiteral("movies");
const QString kTvShows = QStringLiteral("tvshows");

LibrarySubtabController::Status statusOf(PagedItemsModel::Status status)
{
    switch (status) {
    case PagedItemsModel::Idle:
        break;
    case PagedItemsModel::Loading:
        return LibrarySubtabController::Loading;
    case PagedItemsModel::Ready:
        return LibrarySubtabController::Ready;
    case PagedItemsModel::Failed:
        return LibrarySubtabController::Failed;
    }
    return LibrarySubtabController::Idle;
}

LibrarySubtabController::Status statusOf(GenreRailsModel::Status status)
{
    switch (status) {
    case GenreRailsModel::Idle:
        break;
    case GenreRailsModel::Loading:
        return LibrarySubtabController::Loading;
    case GenreRailsModel::Ready:
        return LibrarySubtabController::Ready;
    case GenreRailsModel::Failed:
        return LibrarySubtabController::Failed;
    }
    return LibrarySubtabController::Idle;
}
} // namespace

LibrarySubtabController::LibrarySubtabController(JellyfinClient *client, GenreRailsModel *genres,
                                                 QObject *parent)
    : QObject(parent), m_client(client), m_genres(genres), m_movieList(client, MovieListChannel),
      m_showList(client, ShowListChannel), m_recommendations(client), m_suggestions(client, false),
      m_upcoming(client, true), m_networks(client, true)
{
    for (LibraryItemsModel *list : {&m_movieList, &m_showList}) {
        const auto stateEdge = [this, list] {
            if (activeList() == list)
                Q_EMIT stateChanged();
        };
        connect(list, &PagedItemsModel::statusChanged, this, stateEdge);
        connect(list, &PagedItemsModel::totalCountChanged, this, stateEdge);
        connect(list, &LibraryItemsModel::viewStateEdited, this,
                [this, list] { forwardEdit(list); });
    }
    if (m_genres) {
        connect(m_genres, &GenreRailsModel::statusChanged, this, [this] {
            if (m_started && m_current == Genres)
                Q_EMIT stateChanged();
        });
    }
    if (!m_client)
        return;
    connect(m_client, &JellyfinClient::movieRecommendationsReady, this,
            &LibrarySubtabController::onCategoriesReady);
    connect(m_client, &JellyfinClient::suggestionsReady, this,
            [this](const QJsonArray &items, int total, int channel) {
                onFlatReady(&m_suggestions, TvSuggestions, SuggestionsChannel, items, total,
                            channel);
            });
    connect(m_client, &JellyfinClient::upcomingReady, this,
            [this](const QJsonArray &items, int total, int channel) {
                onFlatReady(&m_upcoming, UpcomingEpisodes, UpcomingChannel, items, total, channel);
            });
    connect(m_client, &JellyfinClient::studiosReady, this,
            [this](const QJsonArray &items, int total, int channel) {
                onFlatReady(&m_networks, NetworkList, NetworksChannel, items, total, channel);
            });
    connect(m_client, &JellyfinClient::itemsReady, this, &LibrarySubtabController::onItemsReady);
    connect(m_client, &JellyfinClient::listingFailed, this,
            &LibrarySubtabController::onListingFailed);
    // A 401, a disconnect, a server switch and a sign-out all end with this signal
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clearAll(); });
}

LibrarySubtabController::~LibrarySubtabController()
{
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    releaseChannels();
    if (m_genresOpened && m_genres)
        m_genres->clear();
}

QString LibrarySubtabController::subtabId(Subtab subtab)
{
    switch (subtab) {
    case Main:
        break;
    case Suggestions:
        return QStringLiteral("suggestions");
    case Favorites:
        return QStringLiteral("favorites");
    case Collections:
        return QStringLiteral("collections");
    case Genres:
        return QStringLiteral("genres");
    case Upcoming:
        return QStringLiteral("upcoming");
    case Networks:
        return QStringLiteral("networks");
    case Episodes:
        return QStringLiteral("episodes");
    }
    return QStringLiteral("main");
}

std::optional<LibrarySubtabController::Subtab>
LibrarySubtabController::subtabFromId(const QString &id)
{
    for (const Subtab subtab :
         {Main, Suggestions, Favorites, Collections, Genres, Upcoming, Networks, Episodes}) {
        if (subtabId(subtab) == id)
            return subtab;
    }
    return std::nullopt;
}

QList<LibrarySubtabController::Subtab> LibrarySubtabController::subtabsOf(const QString &type)
{
    if (type == kMovies)
        return {Main, Suggestions, Favorites, Collections, Genres};
    if (type == kTvShows)
        return {Main, Suggestions, Upcoming, Genres, Networks, Episodes};
    return {};
}

void LibrarySubtabController::setEnabled(const QString &type, const QList<Subtab> &enabled)
{
    if (type != kMovies && type != kTvShows)
        return;
    const QStringList before = available();
    m_enabled.insert(type, enabled);
    if (available() == before)
        return;
    Q_EMIT availableChanged();
    // The showing sub-tab was turned off: back to the main one, what was saved for it stays
    if (m_open && !isAvailable(m_current))
        show(Main);
}

bool LibrarySubtabController::isAvailable(Subtab subtab) const
{
    return m_open && subtabsOf(m_type).contains(subtab)
           && (subtab == Main || m_enabled.value(m_type).contains(subtab));
}

QStringList LibrarySubtabController::available() const
{
    QStringList ids;
    for (const Subtab subtab : subtabsOf(m_type)) {
        if (isAvailable(subtab))
            ids.append(subtabId(subtab));
    }
    return ids;
}

bool LibrarySubtabController::open(const QString &type, const QString &libraryId,
                                   const QString &libraryName)
{
    if (!m_client || (type != kMovies && type != kTvShows)
        || (!libraryId.isEmpty() && !isValidJellyfinId(libraryId))) {
        close();
        return false;
    }
    if (m_open && type == m_type && libraryId == m_libraryId) {
        m_libraryName = libraryName;
        return true;
    }
    teardown();
    m_open = true;
    m_type = type;
    m_libraryId = libraryId;
    m_libraryName = libraryName;
    Q_EMIT scopeChanged();
    Q_EMIT availableChanged();
    // The sub-tab last used for the type in this session, unless the settings turned it off meanwhile
    Subtab wanted = m_remembered.value(type, Main);
    if (!isAvailable(wanted))
        wanted = Main;
    m_current = wanted;
    m_remembered.insert(type, wanted);
    start(wanted);
    Q_EMIT currentChanged();
    Q_EMIT stateChanged();
    return true;
}

void LibrarySubtabController::close()
{
    teardown();
    const bool wasOpen = m_open;
    m_open = false;
    m_type.clear();
    m_libraryId.clear();
    m_libraryName.clear();
    m_current = Main;
    if (!wasOpen)
        return;
    Q_EMIT scopeChanged();
    Q_EMIT availableChanged();
    Q_EMIT currentChanged();
    Q_EMIT stateChanged();
}

void LibrarySubtabController::clearAll()
{
    close();
    m_remembered.clear();
}

bool LibrarySubtabController::select(Subtab subtab)
{
    if (!isAvailable(subtab))
        return false;
    if (subtab != m_current || !m_started)
        show(subtab);
    return true;
}

bool LibrarySubtabController::select(const QString &id)
{
    const std::optional<Subtab> subtab = subtabFromId(id);
    return subtab && select(*subtab);
}

void LibrarySubtabController::show(Subtab subtab)
{
    teardown();
    m_current = subtab;
    m_remembered.insert(m_type, subtab);
    start(subtab);
    Q_EMIT currentChanged();
    Q_EMIT stateChanged();
}

void LibrarySubtabController::start(Subtab subtab)
{
    m_started = true;
    LibraryItemsModel::Constraint constraint;
    switch (subtab) {
    case Main:
        break;
    case Suggestions:
        request(m_type == kMovies ? Recommendations : TvSuggestions);
        break;
    case Favorites:
        constraint.filters = QStringLiteral("IsFavorite");
        openList(&m_movieList, m_libraryId, kMovies, m_libraryName, constraint);
        break;
    case Collections:
        // The collections have no reliable library: the list is the user's whole set
        openList(&m_movieList, {}, QStringLiteral("boxsets"), {}, constraint);
        break;
    case Genres:
        if (m_genres) {
            m_genresOpened = true;
            m_genres->open(m_type, m_libraryId);
        }
        break;
    case Upcoming:
        request(UpcomingEpisodes);
        break;
    case Networks:
        request(NetworkList);
        break;
    case Episodes:
        constraint.scope = LibraryScope::Episodes;
        constraint.includeItemTypes = QStringLiteral("Episode");
        openList(&m_showList, m_libraryId, kTvShows, m_libraryName, constraint);
        request(EpisodeCounter);
        break;
    }
}

void LibrarySubtabController::teardown()
{
    if (!m_started)
        return;
    m_started = false;
    switch (m_current) {
    case Main:
        break;
    case Suggestions:
        if (m_type == kMovies) {
            m_recommendations.clear();
            setSource(Recommendations, Idle);
            if (m_client)
                m_client->releaseListingChannel(RecommendationsChannel);
        } else {
            m_suggestions.setItems({});
            setSource(TvSuggestions, Idle);
            if (m_client)
                m_client->releaseListingChannel(SuggestionsChannel);
        }
        break;
    case Favorites:
    case Collections:
        m_movieList.clear();
        if (m_client)
            m_client->releaseListingChannel(MovieListChannel);
        break;
    case Genres:
        if (m_genresOpened && m_genres)
            m_genres->clear();
        m_genresOpened = false;
        break;
    case Upcoming:
        m_upcoming.setItems({});
        setSource(UpcomingEpisodes, Idle);
        if (m_client)
            m_client->releaseListingChannel(UpcomingChannel);
        break;
    case Networks:
        clearShowList();
        m_networkId.clear();
        m_networkName.clear();
        m_networks.setItems({});
        m_networksCapped = false;
        setSource(NetworkList, Idle);
        if (m_client)
            m_client->releaseListingChannel(NetworksChannel);
        break;
    case Episodes:
        clearShowList();
        setSource(EpisodeCounter, Idle);
        if (m_client)
            m_client->releaseListingChannel(EpisodeCountChannel);
        if (m_episodeCount != -1) {
            m_episodeCount = -1;
            Q_EMIT episodeCountChanged();
        }
        break;
    }
}

void LibrarySubtabController::releaseChannels()
{
    if (!m_client)
        return;
    for (const int channel :
         {MovieListChannel, ShowListChannel, RecommendationsChannel, SuggestionsChannel,
          UpcomingChannel, NetworksChannel, EpisodeCountChannel})
        m_client->releaseListingChannel(channel);
}

void LibrarySubtabController::clearShowList()
{
    m_showList.clear();
    if (m_client)
        m_client->releaseListingChannel(ShowListChannel);
}

void LibrarySubtabController::openList(LibraryItemsModel *list, const QString &libraryId,
                                       const QString &collectionType, const QString &libraryName,
                                       const LibraryItemsModel::Constraint &constraint)
{
    const LibraryItemsModel::ViewState initial =
        m_provider ? m_provider(scopeKey()) : LibraryItemsModel::ViewState{};
    list->open(libraryId, collectionType, libraryName, initial, constraint);
}

void LibrarySubtabController::request(Source source)
{
    bool sent = false;
    if (m_client) {
        switch (source) {
        case Recommendations:
            sent = m_client->fetchMovieRecommendations(
                m_libraryId, CategoryLimit, RecommendationItemLimit, RecommendationsChannel);
            break;
        case TvSuggestions:
            sent = m_client->fetchSuggestions(QStringLiteral("Series"), 0, SuggestionLimit,
                                              SuggestionsChannel);
            break;
        case UpcomingEpisodes:
            sent = m_client->fetchUpcomingEpisodes(m_libraryId, 0, MaxListed, UpcomingChannel);
            break;
        case NetworkList:
            sent = m_client->fetchStudios(m_libraryId, QStringLiteral("Series"), 0, MaxListed,
                                          NetworksChannel);
            break;
        case EpisodeCounter: {
            ItemQuery query;
            query.parentId = m_libraryId;
            query.includeItemTypes = QStringLiteral("Episode");
            query.recursive = true;
            query.limit = 0;
            sent = m_client->fetchItems(query, EpisodeCountChannel);
            break;
        }
        }
    }
    setSource(source, sent ? Loading : Failed);
}

void LibrarySubtabController::setSource(Source source, Status status)
{
    if (m_sourceStatus[source] == status)
        return;
    m_sourceStatus[source] = status;
    if (source != EpisodeCounter)
        Q_EMIT stateChanged();
}

void LibrarySubtabController::retry()
{
    if (!m_open || !m_started)
        return;
    const auto retrySource = [this](Source source) {
        if (m_sourceStatus[source] == Failed)
            request(source);
    };
    switch (m_current) {
    case Main:
        break;
    case Suggestions:
        retrySource(m_type == kMovies ? Recommendations : TvSuggestions);
        break;
    case Favorites:
    case Collections:
        retryList(&m_movieList);
        break;
    case Genres:
        if (m_genres && m_genres->status() == GenreRailsModel::Failed)
            m_genres->retryList();
        break;
    case Upcoming:
        retrySource(UpcomingEpisodes);
        break;
    case Networks:
        if (m_networkId.isEmpty())
            retrySource(NetworkList);
        else
            retryList(&m_showList);
        break;
    case Episodes:
        retryList(&m_showList);
        retrySource(EpisodeCounter);
        break;
    }
}

void LibrarySubtabController::reload()
{
    if (!m_open || !m_started || m_current == Main)
        return;
    if (m_current == Networks && !m_networkId.isEmpty())
        m_showList.reload();
    else
        show(m_current);
}

void LibrarySubtabController::retryList(LibraryItemsModel *list)
{
    if (list->status() == PagedItemsModel::Failed || list->pageError())
        list->retry();
}

bool LibrarySubtabController::selectNetwork(const QString &studioId)
{
    if (!m_open || !m_started || m_current != Networks || !isValidJellyfinId(studioId))
        return false;
    // Only a network the list shows
    QString id;
    QString name;
    for (int row = 0; row < m_networks.rowCount(); ++row) {
        const QModelIndex index = m_networks.index(row);
        const QString listed = m_networks.data(index, ItemRowModel::ItemIdRole).toString();
        if (listed.compare(studioId, Qt::CaseInsensitive) == 0) {
            id = listed;
            name = m_networks.data(index, ItemRowModel::NameRole).toString();
            break;
        }
    }
    if (id.isEmpty())
        return false;
    m_networkId = id;
    m_networkName = name;
    LibraryItemsModel::Constraint constraint;
    constraint.studioIds = {id};
    // The networks are not tied to the library of the list they were found in
    openList(&m_showList, {}, kTvShows, name, constraint);
    Q_EMIT currentChanged();
    Q_EMIT stateChanged();
    return true;
}

void LibrarySubtabController::closeNetwork()
{
    if (m_networkId.isEmpty())
        return;
    clearShowList();
    m_networkId.clear();
    m_networkName.clear();
    Q_EMIT currentChanged();
    Q_EMIT stateChanged();
}

LibrarySubtabController::Status LibrarySubtabController::status() const
{
    if (!m_open || !m_started)
        return Idle;
    switch (m_current) {
    case Main:
        break;
    case Suggestions:
        return m_sourceStatus[m_type == kMovies ? Recommendations : TvSuggestions];
    case Favorites:
    case Collections:
        return statusOf(m_movieList.status());
    case Genres:
        return m_genres ? statusOf(m_genres->status()) : Failed;
    case Upcoming:
        return m_sourceStatus[UpcomingEpisodes];
    case Networks:
        return m_networkId.isEmpty() ? m_sourceStatus[NetworkList] : statusOf(m_showList.status());
    case Episodes:
        return statusOf(m_showList.status());
    }
    return Idle;
}

bool LibrarySubtabController::isEmpty() const
{
    if (status() != Ready)
        return false;
    switch (m_current) {
    case Main:
        break;
    case Suggestions:
        return m_type == kMovies ? m_recommendations.count() == 0 : m_suggestions.count() == 0;
    case Favorites:
    case Collections:
        return m_movieList.totalCount() == 0;
    case Genres:
        return m_genres && m_genres->rowCount() == 0;
    case Upcoming:
        return m_upcoming.count() == 0;
    case Networks:
        return m_networkId.isEmpty() ? m_networks.count() == 0 : m_showList.totalCount() == 0;
    case Episodes:
        return m_showList.totalCount() == 0;
    }
    return false;
}

bool LibrarySubtabController::crossLibrary() const
{
    if (!m_open)
        return false;
    return m_current == Collections || (m_current == Suggestions && m_type == kTvShows)
           || (m_current == Networks && !m_networkId.isEmpty());
}

QString LibrarySubtabController::scopeKey() const
{
    if (!m_open)
        return {};
    const QString library = m_libraryId.toLower();
    const QString suffix = library.isEmpty() ? QString() : QString(QLatin1Char(':') + library);
    switch (m_current) {
    case Main:
        return m_type + suffix;
    case Favorites:
        return QStringLiteral("movies/favorites") + suffix;
    case Collections:
        return QStringLiteral("movies/collections");
    case Episodes:
        return QStringLiteral("tvshows/episodes") + suffix;
    case Networks:
        return m_networkId.isEmpty()
                   ? QString()
                   : QString(QStringLiteral("tvshows/network:") + m_networkId.toLower());
    case Suggestions:
    case Genres:
    case Upcoming:
        break;
    }
    return {};
}

LibraryItemsModel *LibrarySubtabController::activeList()
{
    if (!m_open || !m_started)
        return nullptr;
    switch (m_current) {
    case Favorites:
    case Collections:
        return &m_movieList;
    case Episodes:
        return &m_showList;
    case Networks:
        return m_networkId.isEmpty() ? nullptr : &m_showList;
    case Main:
    case Suggestions:
    case Genres:
    case Upcoming:
        break;
    }
    return nullptr;
}

void LibrarySubtabController::forwardEdit(const LibraryItemsModel *list)
{
    if (activeList() == list)
        Q_EMIT listViewStateEdited();
}

void LibrarySubtabController::onCategoriesReady(const QJsonArray &categories, int channel)
{
    if (channel != RecommendationsChannel || m_sourceStatus[Recommendations] != Loading)
        return;
    m_recommendations.setCategories(categories);
    setSource(Recommendations, Ready);
}

void LibrarySubtabController::onFlatReady(ItemRowModel *model, Source source, int expectedChannel,
                                          const QJsonArray &items, int total, int channel)
{
    if (channel != expectedChannel || m_sourceStatus[source] != Loading)
        return;
    model->setItems(items);
    if (source == NetworkList)
        m_networksCapped = total > items.size();
    setSource(source, Ready);
}

void LibrarySubtabController::onItemsReady(const QJsonArray &, int total, int channel)
{
    if (channel != EpisodeCountChannel || m_sourceStatus[EpisodeCounter] != Loading)
        return;
    // The envelope already requires the server's TotalRecordCount
    m_episodeCount = total;
    setSource(EpisodeCounter, Ready);
    Q_EMIT episodeCountChanged();
}

void LibrarySubtabController::onListingFailed(const QString &family, int, const QString &,
                                              int channel)
{
    struct Route {
        int channel;
        const char *family;
        Source source;
    };
    static constexpr Route routes[] = {
        {RecommendationsChannel, "MovieRecommendations", Recommendations},
        {SuggestionsChannel, "Suggestions", TvSuggestions},
        {UpcomingChannel, "Upcoming", UpcomingEpisodes},
        {NetworksChannel, "Studios", NetworkList},
        {EpisodeCountChannel, "Items", EpisodeCounter}};
    for (const Route &route : routes) {
        if (route.channel == channel && family == QLatin1String(route.family)
            && m_sourceStatus[route.source] == Loading)
            setSource(route.source, Failed);
    }
}
} // namespace Kinefin
