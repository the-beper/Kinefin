// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFlags>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "api/JellyfinClient.h"

namespace Kinefin {

// What a library grid lists: the scope decides which filter sections apply and which sort keys exist
enum class LibraryScope { Movies, Series, Episodes, Collections };

enum class PlayedState { Any, Played, Unplayed };

enum class VideoQuality { Any, HD, SD };

// A single choice: the options exclude each other, an AND of them could never match
enum class EpisodeKind { All, Specials, Missing, Unaired };

enum class LibraryFeature : unsigned {
    Subtitles = 1,
    Trailer = 2,
    SpecialFeatures = 4,
    ThemeSong = 8,
    ThemeVideo = 16
};
Q_DECLARE_FLAGS(LibraryFeatures, LibraryFeature)
Q_DECLARE_OPERATORS_FOR_FLAGS(LibraryFeatures)

// The filters of one library view, as typed values: the one source of the query, composed by composeLibraryQuery(). Played/Unplayed and HD/SD exclude each other by construction; a 4K choice drops SD when composed. The text values (tags, officialRatings) and the ids are checked by JellyfinClient::fetchItems, which refuses a bad one without a request
struct LibraryFilterState {
    PlayedState played = PlayedState::Any;
    bool resumable = false;
    bool favorites = false;
    LibraryFeatures features;
    VideoQuality quality = VideoQuality::Any;
    bool is4K = false;
    bool is3D = false;
    QStringList videoTypes;
    QStringList genreIds;
    QStringList tags;
    QStringList officialRatings;
    QList<int> years;
    QStringList seriesStatus;
    EpisodeKind episodeKind = EpisodeKind::All;

    bool operator==(const LibraryFilterState &) const = default;

    bool isEmpty() const { return *this == LibraryFilterState{}; }

    // The state with the sections that do not apply to `scope` cleared: Resumable only for movies, video quality and type for movies and episodes, features not for collections, series status only for series, the episode kind only for episodes
    LibraryFilterState forScope(LibraryScope scope) const
    {
        LibraryFilterState state = *this;
        if (scope != LibraryScope::Movies)
            state.resumable = false;
        if (scope == LibraryScope::Series || scope == LibraryScope::Collections) {
            state.quality = VideoQuality::Any;
            state.is4K = false;
            state.is3D = false;
            state.videoTypes.clear();
        }
        if (scope == LibraryScope::Collections)
            state.features = {};
        if (scope != LibraryScope::Series)
            state.seriesStatus.clear();
        if (scope != LibraryScope::Episodes)
            state.episodeKind = EpisodeKind::All;
        return state;
    }
};

// The sort keys a scope offers, as `sortBy` wire tokens in the order of the picker: movies, series and collections have the lists of jellyfin-web 12.4 (10, 8 and 5 keys; its name sort also tie-breaks by ProductionYear, which Kinefin does not send), episodes have their own 7. The first is the default
inline QStringList librarySortKeys(LibraryScope scope)
{
    switch (scope) {
    case LibraryScope::Movies:
        return {QStringLiteral("SortName"),        QStringLiteral("Random"),
                QStringLiteral("CommunityRating"), QStringLiteral("CriticRating"),
                QStringLiteral("DateCreated"),     QStringLiteral("DatePlayed"),
                QStringLiteral("OfficialRating"),  QStringLiteral("PlayCount"),
                QStringLiteral("PremiereDate"),    QStringLiteral("Runtime")};
    case LibraryScope::Series:
        return {QStringLiteral("SortName"),
                QStringLiteral("Random"),
                QStringLiteral("CommunityRating"),
                QStringLiteral("DateCreated"),
                QStringLiteral("DateLastContentAdded"),
                QStringLiteral("SeriesDatePlayed"),
                QStringLiteral("OfficialRating"),
                QStringLiteral("PremiereDate")};
    case LibraryScope::Episodes:
        return {QStringLiteral("AiredEpisodeOrder"),
                QStringLiteral("PremiereDate"),
                QStringLiteral("DateCreated"),
                QStringLiteral("SortName"),
                QStringLiteral("Runtime"),
                QStringLiteral("DatePlayed"),
                QStringLiteral("Random")};
    case LibraryScope::Collections:
        return {QStringLiteral("SortName"), QStringLiteral("CommunityRating"),
                QStringLiteral("DateCreated"), QStringLiteral("OfficialRating"),
                QStringLiteral("PremiereDate")};
    }
    return {};
}

// One sort choice: a key of librarySortKeys() and a direction (meaningless for Random)
struct LibrarySort {
    QString key = QStringLiteral("SortName");
    bool descending = false;
};

inline bool isRandomSort(const QString &key)
{
    return key == QLatin1String("Random");
}

// Turns `base` (the query of the view: parent, item types, recursion, card images, fixed constraints such as the Favorites tab's IsFavorite filter, search term) plus the filters and sort into the ItemQuery of one page: the sections that do not apply to `scope` are left out, the state's status tokens are merged into `base.filters`. Sorting other than by name or Random keeps SortName as the tie-break, and the Episodes scope sorts its episode order and names within each series. Random is one page, `startIndex` 0 and `limit` at most RandomPageSize: a later start index is refused. Returns nullopt for a sort key the scope does not offer or a Random page past the first; the values themselves are validated by JellyfinClient::fetchItems
inline std::optional<ItemQuery> composeLibraryQuery(const ItemQuery &base, LibraryScope scope,
                                                    const LibraryFilterState &filterState,
                                                    const LibrarySort &sort, int startIndex = -1,
                                                    int limit = -1)
{
    if (!librarySortKeys(scope).contains(sort.key))
        return std::nullopt;
    const bool random = isRandomSort(sort.key);
    if (random && startIndex > 0)
        return std::nullopt;

    const LibraryFilterState state = filterState.forScope(scope);
    ItemQuery query = base;

    query.sortBy = sort.key;
    if (sort.key != QLatin1String("SortName") && !random)
        query.sortBy += QStringLiteral(",SortName");
    // Across a whole library the episodes' own order and names run season by season over every series at once (all the S1E1 first, observed on 12.4): the series comes first
    if (scope == LibraryScope::Episodes
        && (sort.key == QLatin1String("AiredEpisodeOrder")
            || sort.key == QLatin1String("SortName")))
        query.sortBy.prepend(QStringLiteral("SeriesSortName,"));
    query.sortOrder =
        sort.descending && !random ? QStringLiteral("Descending") : QStringLiteral("Ascending");
    if (random) {
        query.startIndex = 0;
        query.limit = limit < 0 || limit > RandomPageSize ? RandomPageSize : limit;
    } else {
        query.startIndex = startIndex;
        query.limit = limit;
    }

    QStringList tokens =
        base.filters.isEmpty() ? QStringList() : base.filters.split(QLatin1Char(','));
    const auto addToken = [&tokens](const QString &token) {
        if (!tokens.contains(token))
            tokens.append(token);
    };
    if (state.played == PlayedState::Played)
        addToken(QStringLiteral("IsPlayed"));
    else if (state.played == PlayedState::Unplayed)
        addToken(QStringLiteral("IsUnplayed"));
    if (state.resumable)
        addToken(QStringLiteral("IsResumable"));
    if (state.favorites)
        addToken(QStringLiteral("IsFavorite"));
    query.filters = tokens.join(QLatin1Char(','));

    query.hasSubtitles = state.features.testFlag(LibraryFeature::Subtitles);
    query.hasTrailer = state.features.testFlag(LibraryFeature::Trailer);
    query.hasSpecialFeature = state.features.testFlag(LibraryFeature::SpecialFeatures);
    query.hasThemeSong = state.features.testFlag(LibraryFeature::ThemeSong);
    query.hasThemeVideo = state.features.testFlag(LibraryFeature::ThemeVideo);

    // 4K is HD: it drops the SD choice, which could never match
    query.is4K = state.is4K;
    query.is3D = state.is3D;
    if (state.quality == VideoQuality::HD)
        query.isHd = true;
    else if (state.quality == VideoQuality::SD && !state.is4K)
        query.isHd = false;

    query.videoTypes = state.videoTypes;
    query.seriesStatus = state.seriesStatus;
    if (!state.genreIds.isEmpty())
        query.genreIds = state.genreIds;
    query.tags = state.tags;
    query.officialRatings = state.officialRatings;
    query.years = state.years;
    switch (state.episodeKind) {
    case EpisodeKind::All:
        break;
    case EpisodeKind::Specials:
        query.parentIndexNumber = 0;
        break;
    case EpisodeKind::Missing:
        query.isMissing = true;
        break;
    case EpisodeKind::Unaired:
        query.isUnaired = true;
        break;
    }

    return query;
}

// The most option values of one legacy facet (ratings, years) taken from the server
constexpr int MaxFacetOptions = 300;

// The most tags kept in memory from the server's answer (2225 on a real movie library): the memory cap, not what the filter dialog lists at once, which is FilterOptionsModel::MaxShownTags until a search narrows it
constexpr int MaxTagOptions = 5000;

// The selectable values of `GET /Items/Filters`: official ratings (text) and production years
struct LegacyFacets {
    QStringList officialRatings;
    QList<int> years;
};

// Bounds the legacy filter object: at most MaxFacetOptions values per facet, ratings that pass isValidFilterText (the others are dropped), years from 1 to 9999, no duplicates, the server's order kept. A missing or malformed member gives an empty facet
inline LegacyFacets legacyFacetsFrom(const QJsonObject &filters)
{
    LegacyFacets facets;
    const QJsonArray ratings = filters.value(QStringLiteral("OfficialRatings")).toArray();
    for (const QJsonValue &value : ratings) {
        const QString text = value.toString();
        if (isValidFilterText(text) && !facets.officialRatings.contains(text))
            facets.officialRatings.append(text);
        if (facets.officialRatings.size() >= MaxFacetOptions)
            break;
    }
    const QJsonArray years = filters.value(QStringLiteral("Years")).toArray();
    for (const QJsonValue &value : years) {
        const int year = value.toInt(0);
        if (value.isDouble() && year >= 1 && year <= 9999 && !facets.years.contains(year))
            facets.years.append(year);
        if (facets.years.size() >= MaxFacetOptions)
            break;
    }
    return facets;
}

} // namespace Kinefin
