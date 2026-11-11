// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The sub-tab controller of the Movies and TV Shows pages against the fake server: the available sub-tabs, lazy opening, the fallback when a setting turns the showing one off, the channel of each sub-tab and its release on a switch, late answers, the non-removable Favorites constraint, the episode counter, the cross-library flags, the networks, failures with Retry and the loss of the session; the recommendation rows model and the constrained list model
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/GenreRailsModel.h"
#include "models/LibraryItemsModel.h"
#include "models/LibrarySubtabController.h"
#include "models/RecommendationRowsModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QUrlQuery>
#include <QtTest/QtTest>

#include <algorithm>

using Kinefin::JellyfinClient;
using Kinefin::LibraryItemsModel;
using Kinefin::LibrarySubtabController;
using Kinefin::RecommendationRowsModel;

namespace {
const QString kMovies = QStringLiteral("movies");
const QString kTvShows = QStringLiteral("tvshows");
const QString kMoviesLibrary = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
const QString kOtherMoviesLibrary = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd02");
const QString kShowsLibrary = QStringLiteral("d4c3b2a1d4c3b2a1d4c3b2a1d4c3b2a1");
const QString kOtherShowsLibrary = QStringLiteral("d4c3b2a1d4c3b2a1d4c3b2a1d4c3b2a2");
const QString kNorthern = QStringLiteral("aa00000000000000000000000000ff11");
constexpr int kEpisodeTotal = 4321;

struct Fixture {
    Fixture()
    {
        server.generatedItemTotal = 250;
        // The separate episode counter asks for no row at all
        server.itemsResponder = [this](const QByteArray &route) -> std::pair<int, QByteArray> {
            if (!route.contains("limit=0"))
                return {};
            if (counterStatus != 200)
                return {counterStatus, {}};
            return {200,
                    QJsonDocument(QJsonObject{{QStringLiteral("Items"), QJsonArray()},
                                              {QStringLiteral("StartIndex"), 0},
                                              {QStringLiteral("TotalRecordCount"), kEpisodeTotal}})
                        .toJson(QJsonDocument::Compact)};
        };
        ok = server.listen() && login(client, server);
        tabs.setEnabled(kMovies,
                        {LibrarySubtabController::Suggestions, LibrarySubtabController::Favorites,
                         LibrarySubtabController::Collections, LibrarySubtabController::Genres});
        tabs.setEnabled(kTvShows,
                        {LibrarySubtabController::Suggestions, LibrarySubtabController::Upcoming,
                         LibrarySubtabController::Genres, LibrarySubtabController::Networks,
                         LibrarySubtabController::Episodes});
    }

    // The last request of a family, one query value of it decoded
    QString wire(const QString &family, const char *name, int index = -1) const
    {
        const QList<QByteArray> routes = server.listingRoutes.value(family);
        if (routes.isEmpty())
            return {};
        return FakeJellyfin::capturedListingQuery(index < 0 ? routes.constLast() : routes.at(index))
            .queryItemValue(QString::fromLatin1(name), QUrl::FullyDecoded);
    }
    bool wireHas(const QString &family, const char *name) const
    {
        const QList<QByteArray> routes = server.listingRoutes.value(family);
        return !routes.isEmpty()
               && FakeJellyfin::capturedListingQuery(routes.constLast())
                      .hasQueryItem(QString::fromLatin1(name));
    }
    int requests(const QString &family) const { return server.listingRoutes.value(family).size(); }

    int counterStatus = 200;
    FakeJellyfin server;
    JellyfinClient client;
    Kinefin::GenreRailsModel genres{&client};
    LibrarySubtabController tabs{&client, &genres};
    bool ok = false;
};

QJsonObject recommendation(const QString &type, const QString &baseline, int items,
                           const QString &categoryId = {})
{
    QJsonArray entries;
    for (int i = 0; i < items; ++i) {
        entries.append(QJsonObject{
            {QStringLiteral("Id"), QStringLiteral("%1").arg(i + 1, 32, 16, QLatin1Char('a'))},
            {QStringLiteral("Name"), QStringLiteral("Movie %1").arg(i)},
            {QStringLiteral("Type"), QStringLiteral("Movie")}});
    }
    return {{QStringLiteral("Items"), entries},
            {QStringLiteral("RecommendationType"), type},
            {QStringLiteral("BaselineItemName"), baseline},
            {QStringLiteral("CategoryId"), categoryId}};
}
} // namespace

class TestLibrarySubtabController : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void idsAndSubtabsPerType()
    {
        using S = LibrarySubtabController;
        for (const S::Subtab subtab : {S::Main, S::Suggestions, S::Favorites, S::Collections,
                                       S::Genres, S::Upcoming, S::Networks, S::Episodes}) {
            const std::optional<S::Subtab> back = S::subtabFromId(S::subtabId(subtab));
            QVERIFY(back.has_value());
            QCOMPARE(*back, subtab);
        }
        QVERIFY(!S::subtabFromId(QStringLiteral("nope")));
        QCOMPARE(S::subtabsOf(kMovies), (QList<S::Subtab>{S::Main, S::Suggestions, S::Favorites,
                                                          S::Collections, S::Genres}));
        QCOMPARE(S::subtabsOf(kTvShows), (QList<S::Subtab>{S::Main, S::Suggestions, S::Upcoming,
                                                           S::Genres, S::Networks, S::Episodes}));
        QVERIFY(S::subtabsOf(QStringLiteral("music")).isEmpty());
    }

    void openRefusesWhatItCannotServe()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(!f.tabs.open(QStringLiteral("music"), {}));
        QVERIFY(!f.tabs.isOpen());
        QVERIFY(!f.tabs.open(kMovies, QStringLiteral("bad/id")));
        QVERIFY(!f.tabs.isOpen());
        QVERIFY(f.tabs.available().isEmpty());
        QVERIFY(!f.tabs.select(QStringLiteral("favorites")));
    }

    void availableFollowsTheEnabledSetAndTheMainOneIsAlwaysThere()
    {
        using S = LibrarySubtabController;
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.tabs.available().isEmpty());
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QCOMPARE(f.tabs.available(),
                 (QStringList{QStringLiteral("main"), QStringLiteral("suggestions"),
                              QStringLiteral("favorites"), QStringLiteral("collections"),
                              QStringLiteral("genres")}));
        QCOMPARE(f.tabs.current(), QStringLiteral("main"));
        QSignalSpy changed(&f.tabs, &S::availableChanged);
        // A sub-tab the type does not have is ignored, the main one needs no entry
        f.tabs.setEnabled(kMovies, {S::Favorites, S::Collections, S::Episodes});
        QCOMPARE(f.tabs.available(),
                 (QStringList{QStringLiteral("main"), QStringLiteral("favorites"),
                              QStringLiteral("collections")}));
        QCOMPARE(changed.size(), 1);
        f.tabs.setEnabled(kMovies, {});
        QCOMPARE(f.tabs.available(), QStringList{QStringLiteral("main")});
        QVERIFY(!f.tabs.select(S::Favorites));
        QVERIFY(!f.tabs.select(QStringLiteral("episodes")));
        QVERIFY(f.tabs.open(kTvShows, {}));
        QCOMPARE(f.tabs.available(),
                 (QStringList{QStringLiteral("main"), QStringLiteral("suggestions"),
                              QStringLiteral("upcoming"), QStringLiteral("genres"),
                              QStringLiteral("networks"), QStringLiteral("episodes")}));
    }

    void nothingIsRequestedForASubtabThatIsNotShowing()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QTest::qWait(200);
        QCOMPARE(f.server.listingRequests, 0);
        QCOMPARE(f.tabs.status(), LibrarySubtabController::Idle);
        QVERIFY(!f.tabs.activeList());
        QVERIFY(f.tabs.select(QStringLiteral("favorites")));
        QTRY_VERIFY(f.server.listingRequests > 0);
        // Only the list of the showing sub-tab, and the facets of that list: nothing of the other sub-tabs
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.requests(QStringLiteral("MovieRecommendations")), 0);
        QCOMPARE(f.requests(QStringLiteral("Genres")), 0);
        QCOMPARE(f.requests(QStringLiteral("Upcoming")), 0);
        QCOMPARE(f.requests(QStringLiteral("Studios")), 0);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        // The main sub-tab itself asks for nothing here
        f.tabs.select(QStringLiteral("main"));
        QTest::qWait(100);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
    }

    void favoritesIsTheMovieListOfTheLibraryWithAFixedFilter()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy items(&f.client, &JellyfinClient::itemsReady);
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary, QStringLiteral("Films")));
        QVERIFY(f.tabs.select(LibrarySubtabController::Favorites));
        QCOMPARE(f.tabs.status(), LibrarySubtabController::Loading);
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.tabs.activeList(), f.tabs.movieList());
        QCOMPARE(f.tabs.movieList()->totalCount(), 250);
        QVERIFY(!f.tabs.isEmpty());
        QVERIFY(!f.tabs.crossLibrary());
        QCOMPARE(f.tabs.scopeKey(), QString(QStringLiteral("movies/favorites:") + kMoviesLibrary));
        QCOMPARE(f.wire(QStringLiteral("Items"), "filters"), QStringLiteral("IsFavorite"));
        QCOMPARE(f.wire(QStringLiteral("Items"), "parentId"), kMoviesLibrary);
        QCOMPARE(f.wire(QStringLiteral("Items"), "includeItemTypes"), QStringLiteral("Movie"));
        QCOMPARE(f.wire(QStringLiteral("Items"), "recursive"), QStringLiteral("true"));
        QCOMPARE(items.constFirst().at(2).toInt(), int(LibrarySubtabController::MovieListChannel));
        QCOMPARE(f.tabs.movieList()->libraryName(), QStringLiteral("Films"));
    }

    void theFavoritesFilterCannotBeRemoved()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Favorites));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        LibraryItemsModel *list = f.tabs.activeList();
        QVERIFY(list);
        int seen = f.server.itemsRoutes.size();
        const auto lastFilters = [&f] { return f.wire(QStringLiteral("Items"), "filters"); };

        list->setFilter(LibraryItemsModel::Unplayed);
        QTRY_VERIFY(f.server.itemsRoutes.size() > seen);
        QCOMPARE(lastFilters(), QStringLiteral("IsFavorite,IsUnplayed"));
        seen = f.server.itemsRoutes.size();

        list->clearFilters();
        QTRY_VERIFY(f.server.itemsRoutes.size() > seen);
        QCOMPARE(lastFilters(), QStringLiteral("IsFavorite"));
        QVERIFY(list->filterState().isEmpty());
        QVERIFY(!list->filtered());
        seen = f.server.itemsRoutes.size();

        // Neither the older single choice nor a map without the section removes it
        list->setFilter(LibraryItemsModel::Played);
        QTRY_VERIFY(f.server.itemsRoutes.size() > seen);
        QCOMPARE(lastFilters(), QStringLiteral("IsFavorite,IsPlayed"));
        seen = f.server.itemsRoutes.size();
        list->setFilterValues({{QStringLiteral("favorites"), false}});
        QTRY_VERIFY(f.server.itemsRoutes.size() > seen);
        QCOMPARE(lastFilters(), QStringLiteral("IsFavorite"));

        // Back from another sub-tab the list is still the favorites
        QVERIFY(f.tabs.select(LibrarySubtabController::Collections));
        QVERIFY(f.tabs.select(LibrarySubtabController::Favorites));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(lastFilters(), QStringLiteral("IsFavorite"));
    }

    void collectionsListBoxSetsAcrossTheLibraries()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy items(&f.client, &JellyfinClient::itemsReady);
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Collections));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.wire(QStringLiteral("Items"), "includeItemTypes"), QStringLiteral("BoxSet"));
        QVERIFY(!f.wireHas(QStringLiteral("Items"), "parentId"));
        QVERIFY(f.tabs.crossLibrary());
        QCOMPARE(f.tabs.scopeKey(), QStringLiteral("movies/collections"));
        QCOMPARE(f.tabs.activeList()->sortKeys().size(), 5);
        QCOMPARE(items.constLast().at(2).toInt(), int(LibrarySubtabController::MovieListChannel));
        QVERIFY(f.tabs.select(LibrarySubtabController::Favorites));
        QVERIFY(!f.tabs.crossLibrary());
    }

    void episodesAreListedAndCountedOnTheirOwnChannels()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy items(&f.client, &JellyfinClient::itemsReady);
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        QCOMPARE(f.tabs.episodeCount(), -1);
        QVERIFY(f.tabs.select(LibrarySubtabController::Episodes));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QTRY_COMPARE(f.tabs.episodeCount(), kEpisodeTotal);
        // The list of the library and the separate counter: two requests of the Items family
        QCOMPARE(f.server.itemsRoutes.size(), 2);
        QSet<int> channels;
        for (const QList<QVariant> &args : items)
            channels.insert(args.at(2).toInt());
        QCOMPARE(channels, (QSet<int>{LibrarySubtabController::ShowListChannel,
                                      LibrarySubtabController::EpisodeCountChannel}));
        const QList<QByteArray> routes = f.server.itemsRoutes;
        for (const QByteArray &route : routes) {
            const QUrlQuery query = FakeJellyfin::capturedListingQuery(route);
            QCOMPARE(query.queryItemValue(QStringLiteral("includeItemTypes")),
                     QStringLiteral("Episode"));
            QCOMPARE(query.queryItemValue(QStringLiteral("parentId")), kShowsLibrary);
        }
        const auto withLimit = [&routes](const char *limit) {
            return int(
                std::count_if(routes.cbegin(), routes.cend(),
                              [limit](const QByteArray &route) { return route.contains(limit); }));
        };
        QCOMPARE(withLimit("limit=100"), 1);
        QCOMPARE(withLimit("limit=0"), 1);
        // The list holds its own total, the counter the library's
        QCOMPARE(f.tabs.showList()->totalCount(), 250);
        QCOMPARE(f.tabs.scopeKey(), QString(QStringLiteral("tvshows/episodes:") + kShowsLibrary));
        QCOMPARE(f.tabs.activeList()->sortKeys().size(), 7);
        QVERIFY(f.tabs.activeList()->episodeKindAvailable());
        QVERIFY(!f.tabs.crossLibrary());
    }

    void aFailedEpisodeCounterIsUnknownNotZeroAndRetryAsksAgain()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.counterStatus = 500;
        QVERIFY(f.tabs.open(kTvShows, {}));
        QVERIFY(f.tabs.select(LibrarySubtabController::Episodes));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QTRY_COMPARE(f.server.itemsRoutes.size(), 2);
        QTest::qWait(100);
        QCOMPARE(f.tabs.episodeCount(), -1);
        QVERIFY(!f.wireHas(QStringLiteral("Items"), "parentId"));
        f.counterStatus = 200;
        f.tabs.retry();
        QTRY_COMPARE(f.tabs.episodeCount(), kEpisodeTotal);
        // Leaving the sub-tab forgets the number
        QVERIFY(f.tabs.select(LibrarySubtabController::Upcoming));
        QCOMPARE(f.tabs.episodeCount(), -1);
    }

    void tvSuggestionsAreGlobalAndMovieRecommendationsAreScoped()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy suggestions(&f.client, &JellyfinClient::suggestionsReady);
        QSignalSpy recommendations(&f.client, &JellyfinClient::movieRecommendationsReady);

        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Suggestions));
        QVERIFY(f.tabs.crossLibrary());
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QVERIFY(!f.wireHas(QStringLiteral("Suggestions"), "parentId"));
        QCOMPARE(f.wire(QStringLiteral("Suggestions"), "type"), QStringLiteral("Series"));
        QCOMPARE(suggestions.constFirst().at(2).toInt(),
                 int(LibrarySubtabController::SuggestionsChannel));
        QCOMPARE(f.tabs.suggestions()->count(), 2);
        QVERIFY(f.tabs.scopeKey().isEmpty());
        QVERIFY(!f.tabs.activeList());

        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Suggestions));
        QVERIFY(!f.tabs.crossLibrary());
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.wire(QStringLiteral("MovieRecommendations"), "parentId"), kMoviesLibrary);
        QCOMPARE(recommendations.constFirst().at(1).toInt(),
                 int(LibrarySubtabController::RecommendationsChannel));
        QCOMPARE(f.tabs.recommendations()->count(), 2);
        const QModelIndex first = f.tabs.recommendations()->index(0);
        QCOMPARE(f.tabs.recommendations()->data(first, RecommendationRowsModel::KindRole).toInt(),
                 int(RecommendationRowsModel::SimilarToRecentlyPlayed));
        QCOMPARE(f.tabs.recommendations()
                     ->data(first, RecommendationRowsModel::BaselineNameRole)
                     .toString(),
                 QStringLiteral("Hollow Interval"));
        QCOMPARE(f.tabs.recommendations()->data(first, RecommendationRowsModel::CountRole).toInt(),
                 1);
        QVERIFY(f.tabs.select(LibrarySubtabController::Favorites));
        QCOMPARE(f.tabs.recommendations()->count(), 0);
    }

    void upcomingAsksForTheLibraryOnItsChannel()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy upcoming(&f.client, &JellyfinClient::upcomingReady);
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Upcoming));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.wire(QStringLiteral("Upcoming"), "parentId"), kShowsLibrary);
        QCOMPARE(upcoming.constFirst().at(2).toInt(),
                 int(LibrarySubtabController::UpcomingChannel));
        QCOMPARE(f.tabs.upcoming()->count(), 1);
        QVERIFY(!f.tabs.crossLibrary());
    }

    void selectingANetworkOpensItsSeriesOnTheShowListChannel()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy studios(&f.client, &JellyfinClient::studiosReady);
        QSignalSpy items(&f.client, &JellyfinClient::itemsReady);
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        // Only in the Networks sub-tab
        QVERIFY(!f.tabs.selectNetwork(kNorthern));
        QVERIFY(f.tabs.select(LibrarySubtabController::Networks));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(studios.constFirst().at(2).toInt(), int(LibrarySubtabController::NetworksChannel));
        QCOMPARE(f.wire(QStringLiteral("Studios"), "parentId"), kShowsLibrary);
        QCOMPARE(f.wire(QStringLiteral("Studios"), "includeItemTypes"), QStringLiteral("Series"));
        QCOMPARE(f.tabs.networks()->count(), 2);
        QVERIFY(f.tabs.networkId().isEmpty());
        QVERIFY(!f.tabs.activeList());
        QVERIFY(f.tabs.scopeKey().isEmpty());
        QVERIFY(!f.tabs.crossLibrary());

        // Neither a malformed id nor a valid one the list does not show
        QVERIFY(!f.tabs.selectNetwork(QStringLiteral("bad/id")));
        QVERIFY(!f.tabs.selectNetwork(QStringLiteral("cc00000000000000000000000000ff99")));
        QCOMPARE(f.server.itemsRoutes.size(), 0);

        QVERIFY(f.tabs.selectNetwork(kNorthern));
        QCOMPARE(f.tabs.networkId(), kNorthern);
        QCOMPARE(f.tabs.networkName(), QStringLiteral("Northern Lights"));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        QCOMPARE(f.wire(QStringLiteral("Items"), "studioIds"), kNorthern);
        QCOMPARE(f.wire(QStringLiteral("Items"), "includeItemTypes"), QStringLiteral("Series"));
        QVERIFY(!f.wireHas(QStringLiteral("Items"), "parentId"));
        QCOMPARE(items.constLast().at(2).toInt(), int(LibrarySubtabController::ShowListChannel));
        QCOMPARE(f.tabs.scopeKey(), QString(QStringLiteral("tvshows/network:") + kNorthern));
        QVERIFY(f.tabs.crossLibrary());
        QCOMPARE(f.tabs.activeList(), f.tabs.showList());
        QCOMPARE(f.tabs.activeList()->sortKeys().size(), 8);

        // Back to the networks, which are still there: no new request for them
        f.tabs.closeNetwork();
        QVERIFY(f.tabs.networkId().isEmpty());
        QVERIFY(!f.tabs.activeList());
        QVERIFY(!f.tabs.showList()->isOpen());
        QCOMPARE(f.tabs.networks()->count(), 2);
        QCOMPARE(f.requests(QStringLiteral("Studios")), 1);
        QCOMPARE(f.tabs.status(), LibrarySubtabController::Ready);

        // Leaving the sub-tab forgets the network and the list of networks
        QVERIFY(f.tabs.selectNetwork(kNorthern));
        QVERIFY(f.tabs.select(LibrarySubtabController::Upcoming));
        QVERIFY(f.tabs.networkId().isEmpty());
        QCOMPARE(f.tabs.networks()->count(), 0);
        QVERIFY(!f.tabs.showList()->isOpen());
    }

    void leavingASubtabReleasesItsChannelAndDropsTheAnswerInFlight()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.heldFamilies.insert(QStringLiteral("Upcoming"));
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Upcoming));
        QCOMPARE(f.tabs.status(), LibrarySubtabController::Loading);
        QTRY_COMPARE(f.requests(QStringLiteral("Upcoming")), 1);
        QSignalSpy ready(&f.client, &JellyfinClient::upcomingReady);
        QSignalSpy failed(&f.client, &JellyfinClient::listingFailed);

        QVERIFY(f.tabs.select(LibrarySubtabController::Episodes));
        f.server.releaseHeld();
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QTest::qWait(200);
        QCOMPARE(ready.size(), 0);
        for (const QList<QVariant> &args : failed)
            QVERIFY(args.at(0).toString() != QLatin1String("Upcoming"));
        QCOMPARE(f.tabs.upcoming()->count(), 0);
        QCOMPARE(f.tabs.current(), QStringLiteral("episodes"));

        // The channel is free for the next visit
        f.server.heldFamilies.clear();
        QVERIFY(f.tabs.select(LibrarySubtabController::Upcoming));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.tabs.upcoming()->count(), 1);
    }

    void aLateAnswerNeverUpdatesTheViewThatReplacedIt()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.heldFamilies.insert(QStringLiteral("MovieRecommendations"));
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Suggestions));
        QTRY_COMPARE(f.requests(QStringLiteral("MovieRecommendations")), 1);
        QSignalSpy recommendations(&f.client, &JellyfinClient::movieRecommendationsReady);
        QVERIFY(f.tabs.select(LibrarySubtabController::Favorites));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        f.server.releaseHeld();
        QTest::qWait(200);
        QCOMPARE(recommendations.size(), 0);
        QCOMPARE(f.tabs.recommendations()->count(), 0);
        QCOMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.tabs.current(), QStringLiteral("favorites"));
    }

    void aLibraryChangeRequestsAgainAndOnlyTheNewAnswerCounts()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.heldFamilies.insert(QStringLiteral("Upcoming"));
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Upcoming));
        QTRY_COMPARE(f.requests(QStringLiteral("Upcoming")), 1);
        QSignalSpy failed(&f.client, &JellyfinClient::listingFailed);
        // The same sub-tab for the other library
        QVERIFY(f.tabs.open(kTvShows, kOtherShowsLibrary));
        QCOMPARE(f.tabs.current(), QStringLiteral("upcoming"));
        QCOMPARE(f.tabs.libraryId(), kOtherShowsLibrary);
        QTRY_COMPARE(f.requests(QStringLiteral("Upcoming")), 2);
        QCOMPARE(f.wire(QStringLiteral("Upcoming"), "parentId", 0), kShowsLibrary);
        QCOMPARE(f.wire(QStringLiteral("Upcoming"), "parentId", 1), kOtherShowsLibrary);
        // The two held answers fail alike (status 204): only the one of the current request is heard
        f.server.releaseHeld();
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Failed);
        QTest::qWait(150);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(f.tabs.current(), QStringLiteral("upcoming"));
        QCOMPARE(f.tabs.libraryId(), kOtherShowsLibrary);
    }

    void aFailureKeepsTheSelectionAndRetryRecovers()
    {
        Fixture f;
        QVERIFY(f.ok);
        // A flat sub-tab
        f.server.listingStatus.insert(QStringLiteral("Studios"), 500);
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Networks));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Failed);
        QVERIFY(!f.tabs.isEmpty());
        QCOMPARE(f.tabs.current(), QStringLiteral("networks"));
        QCOMPARE(f.tabs.libraryId(), kShowsLibrary);
        f.server.listingStatus.remove(QStringLiteral("Studios"));
        f.tabs.retry();
        QCOMPARE(f.tabs.status(), LibrarySubtabController::Loading);
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.tabs.networks()->count(), 2);

        // The series list of a network keeps the network
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        QVERIFY(f.tabs.selectNetwork(kNorthern));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Failed);
        QCOMPARE(f.tabs.networkId(), kNorthern);
        f.server.listingStatus.remove(QStringLiteral("Items"));
        f.tabs.retry();
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.tabs.networkId(), kNorthern);
        QCOMPARE(f.tabs.showList()->totalCount(), 250);

        // A paged list
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Favorites));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Failed);
        QCOMPARE(f.tabs.current(), QStringLiteral("favorites"));
        f.server.listingStatus.remove(QStringLiteral("Items"));
        f.tabs.retry();
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QCOMPARE(f.tabs.current(), QStringLiteral("favorites"));
    }

    void anEmptyAnswerIsEmptyNotFailed()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingRawBody.insert(QStringLiteral("Upcoming"),
                                       QByteArrayLiteral("{\"Items\":[],\"TotalRecordCount\":0}"));
        QVERIFY(f.tabs.open(kTvShows, {}));
        QVERIFY(f.tabs.select(LibrarySubtabController::Upcoming));
        QTRY_COMPARE(f.tabs.status(), LibrarySubtabController::Ready);
        QVERIFY(f.tabs.isEmpty());
    }

    void turningTheShowingSubtabOffFallsBackToTheMainOne()
    {
        using S = LibrarySubtabController;
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        QVERIFY(f.tabs.select(S::Episodes));
        QTRY_COMPARE(f.tabs.episodeCount(), kEpisodeTotal);
        QSignalSpy current(&f.tabs, &S::currentChanged);
        f.tabs.setEnabled(kTvShows, {S::Upcoming, S::Networks});
        QCOMPARE(f.tabs.current(), QStringLiteral("main"));
        QCOMPARE(current.size(), 1);
        QVERIFY(!f.tabs.activeList());
        QVERIFY(!f.tabs.showList()->isOpen());
        QCOMPARE(f.tabs.episodeCount(), -1);
        QCOMPARE(f.tabs.status(), S::Idle);
        QVERIFY(!f.tabs.available().contains(QStringLiteral("episodes")));
        // The fallback is what the session remembers
        f.tabs.close();
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        QCOMPARE(f.tabs.current(), QStringLiteral("main"));

        // Turning off a sub-tab that is not showing changes nothing for the showing one
        QVERIFY(f.tabs.select(S::Upcoming));
        QTRY_COMPARE(f.tabs.status(), S::Ready);
        const int requests = f.server.listingRequests;
        f.tabs.setEnabled(kTvShows, {S::Upcoming});
        QCOMPARE(f.tabs.current(), QStringLiteral("upcoming"));
        QCOMPARE(f.server.listingRequests, requests);
    }

    void aSubtabTurnedOffWhileTheTypeIsClosedIsNotRestored()
    {
        using S = LibrarySubtabController;
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QVERIFY(f.tabs.select(S::Collections));
        QTRY_COMPARE(f.tabs.status(), S::Ready);
        f.tabs.close();
        QVERIFY(!f.tabs.isOpen());
        f.tabs.setEnabled(kMovies, {S::Favorites});
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QCOMPARE(f.tabs.current(), QStringLiteral("main"));
    }

    void theSessionRemembersTheSubtabOfEachType()
    {
        using S = LibrarySubtabController;
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QVERIFY(f.tabs.select(S::Favorites));
        QVERIFY(f.tabs.open(kTvShows, {}));
        QCOMPARE(f.tabs.current(), QStringLiteral("main"));
        QVERIFY(f.tabs.select(S::Upcoming));
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QCOMPARE(f.tabs.current(), QStringLiteral("favorites"));
        QTRY_COMPARE(f.tabs.status(), S::Ready);
        // Another library keeps the sub-tab and loads it for that library
        QVERIFY(f.tabs.open(kMovies, kOtherMoviesLibrary));
        QCOMPARE(f.tabs.current(), QStringLiteral("favorites"));
        QCOMPARE(f.tabs.scopeKey(),
                 QString(QStringLiteral("movies/favorites:") + kOtherMoviesLibrary));
        QTRY_COMPARE(f.wire(QStringLiteral("Items"), "parentId"), kOtherMoviesLibrary);
        // Leaving the page releases everything but keeps the memory
        f.tabs.close();
        QVERIFY(!f.tabs.movieList()->isOpen());
        QCOMPARE(f.tabs.current(), QStringLiteral("main"));
        QVERIFY(f.tabs.open(kTvShows, {}));
        QCOMPARE(f.tabs.current(), QStringLiteral("upcoming"));
    }

    void scopeKeysFollowTheStoresKeys()
    {
        using S = LibrarySubtabController;
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.tabs.open(kMovies, {}));
        QCOMPARE(f.tabs.scopeKey(), kMovies);
        QVERIFY(f.tabs.select(S::Favorites));
        QCOMPARE(f.tabs.scopeKey(), QStringLiteral("movies/favorites"));
        QVERIFY(f.tabs.select(S::Genres));
        QVERIFY(f.tabs.scopeKey().isEmpty());
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        QCOMPARE(f.tabs.scopeKey(), QString(QStringLiteral("tvshows:") + kShowsLibrary));
        // Upper case ids are written in lower case
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary.toUpper()));
        QCOMPARE(f.tabs.scopeKey(), QString(QStringLiteral("tvshows:") + kShowsLibrary));
    }

    void genresDriveTheRailsModelAndClearItOnLeaving()
    {
        using S = LibrarySubtabController;
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QVERIFY(f.tabs.select(S::Genres));
        QCOMPARE(f.tabs.status(), S::Loading);
        QTRY_COMPARE(f.tabs.status(), S::Ready);
        QCOMPARE(f.genres.rowCount(), 3);
        QCOMPARE(f.genres.libraryId(), kMoviesLibrary);
        QCOMPARE(f.wire(QStringLiteral("Genres"), "includeItemTypes"), QStringLiteral("Movie"));
        QVERIFY(!f.tabs.isEmpty());
        QVERIFY(f.tabs.select(S::Favorites));
        QCOMPARE(f.genres.rowCount(), 0);
        QCOMPARE(f.genres.status(), Kinefin::GenreRailsModel::Idle);
        // A failing genre list is retried through the controller
        f.server.listingStatus.insert(QStringLiteral("Genres"), 500);
        QVERIFY(f.tabs.select(S::Genres));
        QTRY_COMPARE(f.tabs.status(), S::Failed);
        f.server.listingStatus.remove(QStringLiteral("Genres"));
        f.tabs.retry();
        QTRY_COMPARE(f.tabs.status(), S::Ready);
    }

    void aStoredViewStateOpensTheListAndEditsAreReported()
    {
        using S = LibrarySubtabController;
        Fixture f;
        QVERIFY(f.ok);
        QStringList asked;
        f.tabs.setViewStateProvider([&asked](const QString &key) {
            asked.append(key);
            LibraryItemsModel::ViewState state;
            state.sortBy = QStringLiteral("DateCreated");
            state.descending = true;
            return state;
        });
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QVERIFY(f.tabs.select(S::Favorites));
        QTRY_COMPARE(f.tabs.status(), S::Ready);
        QCOMPARE(asked, QStringList{QString(QStringLiteral("movies/favorites:") + kMoviesLibrary)});
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        QCOMPARE(f.wire(QStringLiteral("Items"), "sortBy"), QStringLiteral("DateCreated,SortName"));
        QCOMPARE(f.wire(QStringLiteral("Items"), "sortOrder"), QStringLiteral("Descending"));
        QCOMPARE(f.wire(QStringLiteral("Items"), "filters"), QStringLiteral("IsFavorite"));

        QSignalSpy edited(&f.tabs, &S::listViewStateEdited);
        f.tabs.activeList()->setViewMode(QStringLiteral("list"));
        QCOMPARE(edited.size(), 1);
        QCOMPARE(f.tabs.activeList()->viewState().viewMode, QStringLiteral("list"));
        // A list that is not showing does not report
        QVERIFY(!f.tabs.select(S::Episodes));
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        f.tabs.showList()->setViewMode(QStringLiteral("list"));
        QCOMPARE(edited.size(), 1);
    }

    void losingTheSessionClearsEverything()
    {
        using S = LibrarySubtabController;
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QVERIFY(f.tabs.select(S::Favorites));
        QTRY_COMPARE(f.tabs.status(), S::Ready);
        f.client.disconnectFromServer();
        QVERIFY(!f.tabs.isOpen());
        QVERIFY(f.tabs.type().isEmpty());
        QVERIFY(f.tabs.libraryId().isEmpty());
        QVERIFY(f.tabs.available().isEmpty());
        QVERIFY(!f.tabs.activeList());
        QCOMPARE(f.tabs.status(), S::Idle);
        QVERIFY(!f.tabs.movieList()->isOpen());
        QCOMPARE(f.tabs.movieList()->rowCount(), 0);
        QVERIFY(f.tabs.scopeKey().isEmpty());
        // The remembered sub-tab went with the session; the enabled set is a setting and stays
        QVERIFY(f.tabs.open(kMovies, kMoviesLibrary));
        QCOMPARE(f.tabs.current(), QStringLiteral("main"));
        QVERIFY(f.tabs.available().contains(QStringLiteral("favorites")));
    }

    void anExpiredSessionOnASubtabRequestClearsEverything()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.denyListings = true;
        QVERIFY(f.tabs.open(kTvShows, kShowsLibrary));
        QVERIFY(f.tabs.select(LibrarySubtabController::Upcoming));
        QTRY_VERIFY(!f.tabs.isOpen());
        QCOMPARE(f.tabs.current(), QStringLiteral("main"));
        QCOMPARE(f.tabs.upcoming()->count(), 0);
        QCOMPARE(f.tabs.status(), LibrarySubtabController::Idle);
    }

    void eachSubtabUsesItsOwnChannelAndNeverTheFavoritesPageOne()
    {
        using S = LibrarySubtabController;
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy items(&f.client, &JellyfinClient::itemsReady);
        QSignalSpy recommendations(&f.client, &JellyfinClient::movieRecommendationsReady);
        QSignalSpy suggestions(&f.client, &JellyfinClient::suggestionsReady);
        QSignalSpy upcoming(&f.client, &JellyfinClient::upcomingReady);
        QSignalSpy studios(&f.client, &JellyfinClient::studiosReady);
        QVERIFY(f.tabs.open(kMovies, {}));
        for (const S::Subtab subtab : {S::Favorites, S::Collections, S::Suggestions}) {
            QVERIFY(f.tabs.select(subtab));
            QTRY_COMPARE(f.tabs.status(), S::Ready);
        }
        QVERIFY(f.tabs.open(kTvShows, {}));
        for (const S::Subtab subtab : {S::Suggestions, S::Upcoming, S::Episodes, S::Networks}) {
            QVERIFY(f.tabs.select(subtab));
            QTRY_COMPARE(f.tabs.status(), S::Ready);
        }
        // Networks is the showing sub-tab and its list is loaded, as selectNetwork requires
        QVERIFY(f.tabs.selectNetwork(kNorthern));
        QTRY_COMPARE(f.tabs.status(), S::Ready);

        QSet<int> channels;
        for (QSignalSpy *spy : {&items, &recommendations, &suggestions, &upcoming, &studios}) {
            for (const QList<QVariant> &args : *spy)
                channels.insert(args.constLast().toInt());
        }
        QCOMPARE(channels,
                 (QSet<int>{S::MovieListChannel, S::ShowListChannel, S::RecommendationsChannel,
                            S::SuggestionsChannel, S::UpcomingChannel, S::NetworksChannel,
                            S::EpisodeCountChannel}));
        QVERIFY(!channels.contains(3082));
    }

    void recommendationRowsAreBoundedAndTheirTextIsPlain()
    {
        RecommendationRowsModel rows(nullptr);
        QSignalSpy counted(&rows, &RecommendationRowsModel::countChanged);
        QJsonArray categories;
        categories.append(recommendation(QStringLiteral("HasLikedDirector"),
                                         QStringLiteral("<b>A Director</b>"), 2,
                                         QStringLiteral("9a8b7c6d5e4f3827160594a3b2c1d0e1")));
        // Dropped: no item at all, and an entry that is not an object
        categories.append(
            recommendation(QStringLiteral("SimilarToLikedItem"), QStringLiteral("X"), 0));
        categories.append(QJsonValue(7));
        categories.append(recommendation(QStringLiteral("SomethingNew"),
                                         QString(500, QLatin1Char('n')), 1,
                                         QStringLiteral("not an id")));
        for (int i = 0; i < 30; ++i)
            categories.append(recommendation(QStringLiteral("HasActorFromRecentlyPlayed"),
                                             QStringLiteral("Actor"), 1));
        rows.setCategories(categories);
        QCOMPARE(rows.count(), int(RecommendationRowsModel::MaxCategories));
        QCOMPARE(counted.size(), 1);
        const auto role = [&rows](int row, int value) { return rows.data(rows.index(row), value); };
        QCOMPARE(role(0, RecommendationRowsModel::KindRole).toInt(),
                 int(RecommendationRowsModel::HasLikedDirector));
        // Markup stays literal text: the view renders it as plain text
        QCOMPARE(role(0, RecommendationRowsModel::BaselineNameRole).toString(),
                 QStringLiteral("<b>A Director</b>"));
        QCOMPARE(role(0, RecommendationRowsModel::CategoryIdRole).toString(),
                 QStringLiteral("9a8b7c6d5e4f3827160594a3b2c1d0e1"));
        QCOMPARE(role(0, RecommendationRowsModel::CountRole).toInt(), 2);
        QCOMPARE(role(1, RecommendationRowsModel::KindRole).toInt(),
                 int(RecommendationRowsModel::Other));
        QVERIFY(role(1, RecommendationRowsModel::CategoryIdRole).toString().isEmpty());
        QVERIFY(role(1, RecommendationRowsModel::BaselineNameRole).toString().size()
                <= Kinefin::FilterOptionsModel::MaxNameLength);
        QVERIFY(role(1, RecommendationRowsModel::ItemsRole).value<QObject *>());
        QVERIFY(!role(rows.count(), RecommendationRowsModel::KindRole).isValid());
        rows.clear();
        QCOMPARE(rows.count(), 0);
        QCOMPARE(counted.size(), 2);
    }

    void aConstrainedListKeepsItsFixedPartAndRefusesABadStudio()
    {
        FakeJellyfin server;
        server.generatedItemTotal = 3;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        LibraryItemsModel list(&client, LibrarySubtabController::MovieListChannel);

        LibraryItemsModel::Constraint bad;
        bad.studioIds = {QStringLiteral("no/pe")};
        QVERIFY(!list.open({}, kTvShows, {}, {}, bad));
        QVERIFY(!list.isOpen());

        LibraryItemsModel::Constraint episodes;
        episodes.scope = Kinefin::LibraryScope::Episodes;
        episodes.includeItemTypes = QStringLiteral("Episode");
        QVERIFY(list.open(kShowsLibrary, kTvShows, {}, {}, episodes));
        QVERIFY(list.episodeKindAvailable());
        QVERIFY(!list.seriesStatusAvailable());
        QCOMPARE(list.sortKeys().size(), 7);
        QTRY_COMPARE(list.status(), Kinefin::PagedItemsModel::Ready);
        QVERIFY(server.itemsRoutes.constFirst().contains("includeItemTypes=Episode"));
        // The plain open of the same type is the series grid again
        QVERIFY(list.open(kShowsLibrary, kTvShows));
        QVERIFY(!list.episodeKindAvailable());
        QVERIFY(list.seriesStatusAvailable());
        QCOMPARE(list.sortKeys().size(), 8);
        QTRY_COMPARE(list.status(), Kinefin::PagedItemsModel::Ready);
        QVERIFY(server.itemsRoutes.constLast().contains("includeItemTypes=Series"));
        QVERIFY(!server.itemsRoutes.constLast().contains("filters="));
    }
};

QTEST_MAIN(TestLibrarySubtabController)
#include "test_library_subtab_controller.moc"
