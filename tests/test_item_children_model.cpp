// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// What hangs below an item against the fake server: the seasons and next episode of a series, the paged episodes of a season, the items of a collection, their failures and retries, the release of the channels and the bounds on untrusted fields
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/ItemChildrenModel.h"
#include "models/ItemDetailsModel.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::ItemChildrenModel;
using Kinefin::SeasonEpisodesModel;

namespace {
const QString kSeries = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
const QString kSeason = QStringLiteral("11aa22bb33cc44dd55ee66ff778899aa");
const QString kBoxSet = QStringLiteral("bb11cc22dd33ee44ff5566778899aabb");
constexpr int kChannel = 90001;

QJsonObject fixtureItem(const QString &name)
{
    return QJsonDocument::fromJson(fixture(name))
        .object()
        .value(QStringLiteral("Items"))
        .toArray()
        .first()
        .toObject();
}

Kinefin::ItemDetails detailsOf(const QString &name)
{
    return Kinefin::parseItemDetails(fixtureItem(name));
}

QByteArray envelope(const QJsonArray &items, int total = -1)
{
    return QJsonDocument(
               QJsonObject{{QStringLiteral("Items"), items},
                           {QStringLiteral("TotalRecordCount"), total < 0 ? items.size() : total}})
        .toJson(QJsonDocument::Compact);
}

QJsonObject episodeJson(int number, bool played = false, qint64 position = 0, bool missing = false)
{
    QJsonObject episode{
        {QStringLiteral("Id"), QStringLiteral("%1").arg(number, 32, 16, QLatin1Char('d'))},
        {QStringLiteral("Name"), QStringLiteral("Episode %1").arg(number)},
        {QStringLiteral("Type"), QStringLiteral("Episode")},
        {QStringLiteral("SeriesId"), kSeries},
        {QStringLiteral("ParentIndexNumber"), 1},
        {QStringLiteral("IndexNumber"), number},
        {QStringLiteral("UserData"),
         QJsonObject{{QStringLiteral("Played"), played},
                     {QStringLiteral("PlaybackPositionTicks"), position}}}};
    if (missing)
        episode.insert(QStringLiteral("LocationType"), QStringLiteral("Virtual"));
    return episode;
}

QString episodeId(int number)
{
    return QStringLiteral("%1").arg(number, 32, 16, QLatin1Char('d'));
}

struct Fixture {
    Fixture() { ok = server.listen() && login(client, server); }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    ItemChildrenModel children{&client, kChannel};
    bool ok = false;
};

bool allowed(const Fixture &f, const QUrl &url)
{
    return Kinefin::JellyfinClient::isServerImageUrl(f.client.serverUrl(), url);
}
} // namespace

class TestItemChildrenModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void aSeriesLoadsSeasonsAndNextUpOnItsChannel()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QCOMPARE(f.children.kind(), ItemChildrenModel::Series);
        QCOMPARE(f.children.seasonsStatus(), ItemChildrenModel::Loading);
        QTRY_COMPARE(f.children.seasonsStatus(), ItemChildrenModel::Ready);
        QCOMPARE(f.children.seasons()->count(), 3);
        QTRY_VERIFY(f.children.hasNextUp());
        const QVariantMap next = f.children.nextUp();
        QCOMPARE(next.value(QStringLiteral("seasonNumber")).toInt(), 2);
        QCOMPARE(next.value(QStringLiteral("episodeNumber")).toInt(), 6);
        QVERIFY(allowed(f, next.value(QStringLiteral("imageUrl")).toUrl()));
        // The next episode has no date window (a series last watched long ago still has one) and asks for one entry
        const QUrlQuery nextUp = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("NextUp")).constFirst());
        QCOMPARE(nextUp.queryItemValue(QStringLiteral("seriesId")), kSeries);
        QCOMPARE(nextUp.queryItemValue(QStringLiteral("limit")), QStringLiteral("1"));
        QVERIFY(!nextUp.hasQueryItem(QStringLiteral("nextUpDateCutoff")));
        const QUrlQuery seasons = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("Seasons")).constFirst());
        QVERIFY(seasons.queryItemValue(QStringLiteral("enableImageTypes"))
                    .contains(QStringLiteral("Primary")));
    }

    void aSeasonWithoutAPosterUsesTheSeriesPoster()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QTRY_COMPARE(f.children.seasonsStatus(), ItemChildrenModel::Ready);
        const auto url = [&f](int row) {
            return f.children.seasons()
                ->data(f.children.seasons()->index(row), Kinefin::ItemRowModel::ImageUrlRole)
                .toUrl();
        };
        // Season 1 has its own poster, the Specials only the series'
        QVERIFY(url(0).toString().contains(
            QStringLiteral("/Items/22aa33bb44cc55dd66ee77ff8899aa00/Images/Primary")));
        QVERIFY(url(2).toString().contains(QStringLiteral("/Items/") + kSeries
                                           + QStringLiteral("/Images/Primary")));
        QVERIFY(allowed(f, url(2)));
        // A season's unplayed count rides along for the badge
        QCOMPARE(
            f.children.seasons()
                ->data(f.children.seasons()->index(0), Kinefin::ItemRowModel::UnplayedCountRole)
                .toInt(),
            2);
    }

    void aSeriesStartsFromTheFirstEpisodeOfItsFirstRegularSeason()
    {
        Fixture f;
        QVERIFY(f.ok);
        // The Specials come first in the list the server sends
        f.server.listingRawBody.insert(QStringLiteral("Episodes"),
                                       envelope({episodeJson(1, true), episodeJson(2)}, 12));
        f.server.listingRawBody.insert(QStringLiteral("NextUp"),
                                       envelope({episodeJson(2, false, 5'000'000'000)}));
        const QJsonArray seasons = QJsonDocument::fromJson(fixture(QStringLiteral("shows_seasons")))
                                       .object()
                                       .value(QStringLiteral("Items"))
                                       .toArray();
        f.server.listingRawBody.insert(QStringLiteral("Seasons"),
                                       envelope({seasons.at(2), seasons.at(1), seasons.at(0)}));
        f.children.load(detailsOf(QStringLiteral("series_item")));
        // The buttons wait for every answer
        QVERIFY(!f.children.playTargetsReady());
        QTRY_VERIFY(f.children.hasFirstEpisode());
        QTRY_VERIFY(f.children.playTargetsReady());
        QVERIFY(f.children.hasNextUp());
        QCOMPARE(f.children.firstEpisode().value(QStringLiteral("itemId")).toString(),
                 episodeId(1));
        QCOMPARE(f.children.firstEpisode().value(QStringLiteral("seasonNumber")).toInt(), 1);
        // A few episodes of Season 1 (the first with a file counts), never the Specials or the whole list
        const QUrlQuery first = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("Episodes")).constFirst());
        QCOMPARE(first.queryItemValue(QStringLiteral("seasonId")),
                 QStringLiteral("22aa33bb44cc55dd66ee77ff8899aa00"));
        QCOMPARE(first.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
        QCOMPARE(first.queryItemValue(QStringLiteral("limit")), QStringLiteral("10"));
        // The Next Up entry carries the saved position for the Resume button
        QCOMPARE(f.children.nextUp().value(QStringLiteral("positionTicks")).toLongLong(),
                 qint64(5'000'000'000));
    }

    void aSeriesWithOnlySpecialsStartsFromThem()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QJsonArray seasons = QJsonDocument::fromJson(fixture(QStringLiteral("shows_seasons")))
                                       .object()
                                       .value(QStringLiteral("Items"))
                                       .toArray();
        f.server.listingRawBody.insert(QStringLiteral("Seasons"), envelope({seasons.at(2)}));
        f.server.listingRawBody.insert(QStringLiteral("Episodes"), envelope({episodeJson(7)}));
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QTRY_VERIFY(f.children.hasFirstEpisode());
        const QUrlQuery first = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("Episodes")).constFirst());
        QCOMPARE(first.queryItemValue(QStringLiteral("seasonId")),
                 QStringLiteral("33bb44cc55dd66ee77ff8899aa001122"));
    }

    void aSeriesWithoutSeasonsOrWithAMissingFirstEpisodeHasNone()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingRawBody.insert(QStringLiteral("Seasons"), envelope({}));
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QTRY_COMPARE(f.children.seasonsStatus(), ItemChildrenModel::Ready);
        QTRY_VERIFY(f.children.hasNextUp());
        QVERIFY(!f.children.hasFirstEpisode());
        // Nothing to ask for is an answer too
        QTRY_VERIFY(f.children.playTargetsReady());
        QVERIFY(!f.server.listingRoutes.contains(QStringLiteral("Episodes")));
        // An episode that has no file cannot play: the first one that has takes its place, and none leaves the series without
        f.server.listingRawBody.insert(QStringLiteral("Seasons"),
                                       fixture(QStringLiteral("shows_seasons")));
        f.server.listingRawBody.insert(QStringLiteral("Episodes"),
                                       envelope({episodeJson(1, false, 0, true),
                                                 episodeJson(2, false, 0, true), episodeJson(3)}));
        QSignalSpy answered(&f.client, &Kinefin::JellyfinClient::episodesReady);
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QTRY_COMPARE(answered.size(), 1);
        QTRY_VERIFY(f.children.hasFirstEpisode());
        QCOMPARE(f.children.firstEpisode().value(QStringLiteral("itemId")).toString(),
                 episodeId(3));
        f.server.listingRawBody.insert(QStringLiteral("Episodes"),
                                       envelope({episodeJson(1, false, 0, true)}));
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QTRY_COMPARE(answered.size(), 2);
        QTRY_VERIFY(!f.children.hasFirstEpisode());
        QVERIFY(f.children.playTargetsReady());
    }

    void aSeasonTakesItsFirstEpisodeFromTheListAndAsksForItsFirstUnplayedOne()
    {
        Fixture f;
        QVERIFY(f.ok);
        // The first episode of the list is the one the season starts from: no request of its own
        f.server.listingRawBody.insert(QStringLiteral("Episodes"),
                                       envelope({episodeJson(1, true), episodeJson(2, true)}, 12));
        f.server.itemsResponder = [](const QByteArray &) {
            // The first unplayed one has a saved position; a missing episode before it is skipped
            return std::pair<int, QByteArray>{
                200, envelope(QJsonArray{episodeJson(4, false, 0, true),
                                         episodeJson(5, false, 9'000'000'000)})};
        };
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QVERIFY(!f.children.playTargetsReady());
        QTRY_VERIFY(f.children.hasNextUp());
        QVERIFY(f.children.playTargetsReady());
        QVERIFY(f.children.hasFirstEpisode());
        QCOMPARE(f.children.firstEpisode().value(QStringLiteral("itemId")).toString(),
                 episodeId(1));
        QCOMPARE(f.children.nextUp().value(QStringLiteral("itemId")).toString(), episodeId(5));
        QCOMPARE(f.children.nextUp().value(QStringLiteral("positionTicks")).toLongLong(),
                 qint64(9'000'000'000));
        // One page of the list and one listing of the unplayed ones, both at once
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Episodes")).size(), 1);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        const QUrlQuery query =
            FakeJellyfin::capturedListingQuery(f.server.itemsRoutes.constFirst());
        QCOMPARE(query.queryItemValue(QStringLiteral("parentId")), kSeason);
        QCOMPARE(query.queryItemValue(QStringLiteral("includeItemTypes")),
                 QStringLiteral("Episode"));
        QCOMPARE(query.queryItemValue(QStringLiteral("sortBy")), QStringLiteral("IndexNumber"));
        QCOMPARE(query.queryItemValue(QStringLiteral("filters")), QStringLiteral("IsUnplayed"));
        QVERIFY(!query.hasQueryItem(QStringLiteral("recursive")));
        // Another item drops the answers, a reload of the same one keeps them until it answers
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QVERIFY(f.children.playTargetsReady());
        f.children.clear();
        QVERIFY(!f.children.playTargetsReady());
    }

    void aPlayedWriteReadsTheSeasonTargetsAgain()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingRawBody.insert(QStringLiteral("Episodes"), envelope({episodeJson(1)}, 3));
        f.server.itemsResponder = [](const QByteArray &) {
            return std::pair<int, QByteArray>{200, envelope(QJsonArray{episodeJson(1)})};
        };
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QTRY_VERIFY(f.children.hasNextUp());
        QTRY_VERIFY(f.children.playTargetsReady());
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        // The first episode and then the one to go on with, as a listing each
        f.children.refreshSeasonTargets();
        QTRY_COMPARE(f.server.itemsRoutes.size(), 3);
        QVERIFY(!f.server.itemsRoutes.at(1).contains("filters="));
        QVERIFY(f.server.itemsRoutes.at(2).contains("filters=IsUnplayed"));
    }

    void aSeasonWithNothingUnplayedHasNoEpisodeToGoOnWith()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingRawBody.insert(QStringLiteral("Episodes"),
                                       envelope({episodeJson(1, true)}, 3));
        f.server.itemsResponder = [](const QByteArray &) {
            return std::pair<int, QByteArray>{200, envelope(QJsonArray{})};
        };
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QTRY_VERIFY(f.children.hasFirstEpisode());
        QTRY_VERIFY(f.children.playTargetsReady());
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        QVERIFY(!f.children.hasNextUp());
    }

    void theSeasonTargetsFailQuietlyAndAskOnce()
    {
        Fixture f;
        QVERIFY(f.ok);
        // The listing of unplayed episodes fails: the first episode still comes from the list
        f.server.generatedEpisodeTotal = 3;
        f.server.itemsResponder = [](const QByteArray &) {
            return std::pair<int, QByteArray>{500, QByteArray()};
        };
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QTRY_VERIFY(f.children.playTargetsReady());
        QTest::qWait(100);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        QVERIFY(f.children.hasFirstEpisode());
        QVERIFY(!f.children.hasNextUp());
        QTRY_COMPARE(f.children.episodes()->status(), Kinefin::PagedItemsModel::Ready);
    }

    void aSeasonWhoseEpisodeListFailsHasNoFirstEpisode()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Episodes"), 500);
        f.server.itemsResponder = [](const QByteArray &) {
            return std::pair<int, QByteArray>{200, envelope(QJsonArray{episodeJson(2)})};
        };
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QTRY_VERIFY(f.children.hasNextUp());
        QTRY_VERIFY(f.children.playTargetsReady());
        QVERIFY(!f.children.hasFirstEpisode());
        QTRY_COMPARE(f.children.episodes()->status(), Kinefin::PagedItemsModel::Failed);
    }

    void nothingNextHidesTheEntry()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingRawBody.insert(QStringLiteral("NextUp"), envelope({}));
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QTRY_COMPARE(f.children.seasonsStatus(), ItemChildrenModel::Ready);
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("NextUp")).size(), 1);
        QTest::qWait(150);
        QVERIFY(!f.children.hasNextUp());
        // A failing Next Up stays quiet too
        f.server.listingStatus.insert(QStringLiteral("NextUp"), 500);
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("NextUp")).size(), 2);
        QTest::qWait(150);
        QVERIFY(!f.children.hasNextUp());
        QCOMPARE(f.children.seasonsStatus(), ItemChildrenModel::Ready);
    }

    void aSeasonListsItsEpisodesInPages()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 120;
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QCOMPARE(f.children.kind(), ItemChildrenModel::Season);
        SeasonEpisodesModel *episodes = f.children.episodes();
        QTRY_COMPARE(episodes->status(), Kinefin::PagedItemsModel::Ready);
        QCOMPARE(episodes->totalCount(), 120);
        const QUrlQuery first = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("Episodes")).constFirst());
        QCOMPARE(first.queryItemValue(QStringLiteral("seasonId")), kSeason);
        QCOMPARE(first.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
        QCOMPARE(first.queryItemValue(QStringLiteral("limit")), QStringLiteral("50"));
        QCOMPARE(first.queryItemValue(QStringLiteral("fields")), QStringLiteral("Overview"));
        // Reading a placeholder asks for its page, and only that
        const QModelIndex row = episodes->index(60);
        QVERIFY(!episodes->data(row, SeasonEpisodesModel::LoadedRole).toBool());
        QTRY_VERIFY(episodes->data(row, SeasonEpisodesModel::LoadedRole).toBool());
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Episodes")).size(), 2);
        QVERIFY(f.server.listingRoutes.value(QStringLiteral("Episodes"))
                    .constLast()
                    .contains("startIndex=50"));
        QCOMPARE(episodes->data(row, SeasonEpisodesModel::NameRole).toString(),
                 QStringLiteral("Episode 61"));
    }

    // The episodes the server only knows of: a season list asks for the real ones, and lets the others in (marked missing or unaired by their premiere date) only when the user's option is on
    void virtualEpisodesFollowTheDisplayOption()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonObject unaired = episodeJson(3, false, 0, true);
        unaired.insert(QStringLiteral("PremiereDate"),
                       QDateTime::currentDateTimeUtc().addDays(30).toString(Qt::ISODateWithMs));
        QJsonObject missing = episodeJson(2, false, 0, true);
        missing.insert(QStringLiteral("PremiereDate"),
                       QDateTime::currentDateTimeUtc().addDays(-30).toString(Qt::ISODateWithMs));
        f.server.listingRawBody.insert(QStringLiteral("Episodes"),
                                       envelope(QJsonArray{episodeJson(1), missing, unaired}));
        f.children.load(detailsOf(QStringLiteral("season_item")));
        SeasonEpisodesModel *episodes = f.children.episodes();
        QTRY_COMPARE(episodes->status(), Kinefin::PagedItemsModel::Ready);
        QCOMPARE(FakeJellyfin::capturedListingQuery(
                     f.server.listingRoutes.value(QStringLiteral("Episodes")).constFirst())
                     .queryItemValue(QStringLiteral("isMissing")),
                 QStringLiteral("false"));
        const auto flag = [episodes](int row, int role) {
            return episodes->data(episodes->index(row), role).toBool();
        };
        QVERIFY(!flag(0, SeasonEpisodesModel::VirtualRole));
        QVERIFY(!flag(0, SeasonEpisodesModel::UnairedRole));
        QVERIFY(flag(1, SeasonEpisodesModel::VirtualRole));
        QVERIFY(!flag(1, SeasonEpisodesModel::UnairedRole));
        QVERIFY(flag(2, SeasonEpisodesModel::VirtualRole));
        QVERIFY(flag(2, SeasonEpisodesModel::UnairedRole));

        // With DisplayMissingEpisodes on, the list stops asking them away
        f.client.setDisplayMissingEpisodes(true);
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Episodes")).size(), 2);
        QVERIFY(!FakeJellyfin::capturedListingQuery(
                     f.server.listingRoutes.value(QStringLiteral("Episodes")).constLast())
                     .hasQueryItem(QStringLiteral("isMissing")));
    }

    void episodeRowsCarryNumberTitleRuntimeAndProgress()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.children.load(detailsOf(QStringLiteral("season_item")));
        SeasonEpisodesModel *episodes = f.children.episodes();
        QTRY_COMPARE(episodes->status(), Kinefin::PagedItemsModel::Ready);
        const QModelIndex row = episodes->index(0);
        QCOMPARE(episodes->data(row, SeasonEpisodesModel::ItemIdRole).toString(),
                 QStringLiteral("b1b2c3d4e5f64788990a1b2c3d4e5f60"));
        QCOMPARE(episodes->data(row, SeasonEpisodesModel::SeasonNumberRole).toInt(), 2);
        QCOMPARE(episodes->data(row, SeasonEpisodesModel::EpisodeNumberRole).toInt(), 5);
        QCOMPARE(episodes->data(row, SeasonEpisodesModel::RuntimeMinutesRole).toInt(), 29);
        QVERIFY(!episodes->data(row, SeasonEpisodesModel::PlayedRole).toBool());
        // The server's <br> is a line break, nothing else is interpreted
        QVERIFY(episodes->data(row, SeasonEpisodesModel::OverviewRole)
                    .toString()
                    .contains(QStringLiteral("back.\nNobody")));
        QVERIFY(allowed(f, episodes->data(row, SeasonEpisodesModel::ImageUrlRole).toUrl()));
    }

    void aSeasonWithoutASeriesFailsItsEpisodes()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonObject season = fixtureItem(QStringLiteral("season_item"));
        season.remove(QStringLiteral("SeriesId"));
        f.children.load(Kinefin::parseItemDetails(season));
        QTRY_COMPARE(f.children.episodes()->status(), Kinefin::PagedItemsModel::Failed);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Episodes")).size(), 0);
        // The list refuses without a word: the buttons do not wait for a first page that cannot come
        QTRY_VERIFY(f.children.playTargetsReady());
        QVERIFY(!f.children.hasFirstEpisode());
    }

    void aCollectionListsItsItems()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingRawBody.insert(QStringLiteral("Items"),
                                       fixture(QStringLiteral("boxset_children")));
        f.children.load(detailsOf(QStringLiteral("boxset_item")));
        QCOMPARE(f.children.kind(), ItemChildrenModel::BoxSet);
        QTRY_COMPARE(f.children.itemsStatus(), ItemChildrenModel::Ready);
        QCOMPARE(f.children.items()->count(), 3);
        QCOMPARE(f.children.itemsTotal(), 3);
        const QString route = QString::fromUtf8(f.server.itemsRoutes.constFirst());
        QVERIFY(route.contains(QStringLiteral("parentId=") + kBoxSet));
        QVERIFY(route.contains(QStringLiteral("limit=100")));
        // The collection's own order: no sort, and only its direct children
        QVERIFY(!route.contains(QStringLiteral("sortBy")));
        QVERIFY(!route.contains(QStringLiteral("recursive")));
        // More items than the row holds: the total says so
        f.server.listingRawBody.insert(
            QStringLiteral("Items"),
            envelope(QJsonDocument::fromJson(fixture(QStringLiteral("boxset_children")))
                         .object()
                         .value(QStringLiteral("Items"))
                         .toArray(),
                     250));
        f.children.load(detailsOf(QStringLiteral("boxset_item")));
        QTRY_COMPARE(f.children.itemsTotal(), 250);
    }

    void failedListsRetry()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Seasons"), 500);
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QTRY_COMPARE(f.children.seasonsStatus(), ItemChildrenModel::Failed);
        f.server.listingStatus.clear();
        f.children.retry();
        QCOMPARE(f.children.seasonsStatus(), ItemChildrenModel::Loading);
        QTRY_COMPARE(f.children.seasonsStatus(), ItemChildrenModel::Ready);
        QCOMPARE(f.children.seasons()->count(), 3);

        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        f.children.load(detailsOf(QStringLiteral("boxset_item")));
        QTRY_COMPARE(f.children.itemsStatus(), ItemChildrenModel::Failed);
        f.server.listingStatus.clear();
        f.server.listingRawBody.insert(QStringLiteral("Items"),
                                       fixture(QStringLiteral("boxset_children")));
        f.children.retry();
        QTRY_COMPARE(f.children.itemsStatus(), ItemChildrenModel::Ready);

        f.server.listingStatus.insert(QStringLiteral("Episodes"), 500);
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QTRY_COMPARE(f.children.episodes()->status(), Kinefin::PagedItemsModel::Failed);
        f.server.listingStatus.clear();
        f.children.episodes()->retry();
        QTRY_COMPARE(f.children.episodes()->status(), Kinefin::PagedItemsModel::Ready);
    }

    void aMovieAsksForNoChildren()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.children.load(detailsOf(QStringLiteral("movie_item")));
        QCOMPARE(f.children.kind(), ItemChildrenModel::None);
        QTest::qWait(150);
        for (const QString &family :
             {QStringLiteral("Seasons"), QStringLiteral("Episodes"), QStringLiteral("NextUp")})
            QCOMPARE(f.server.listingRoutes.value(family).size(), 0);
        QCOMPARE(f.server.itemsRoutes.size(), 0);
    }

    void aLostSessionClearsTheChildren()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 10;
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QTRY_COMPARE(f.children.episodes()->status(), Kinefin::PagedItemsModel::Ready);
        f.client.disconnectFromServer();
        QCOMPARE(f.children.kind(), ItemChildrenModel::None);
        QCOMPARE(f.children.episodes()->status(), Kinefin::PagedItemsModel::Idle);
        QCOMPARE(f.children.episodes()->rowCount(), 0);
        QCOMPARE(f.children.seasons()->count(), 0);
    }

    void aDestroyedDetailsModelReleasesBothChannels()
    {
        Fixture f;
        QVERIFY(f.ok);
        // A held request that is answered anyway ends in a failure signal: it only stays silent when the channel was released
        QSignalSpy failures(&f.client, &Kinefin::JellyfinClient::listingFailed);
        f.server.itemsById.insert(kSeries, QJsonDocument(fixtureItem(QStringLiteral("series_item")))
                                               .toJson(QJsonDocument::Compact));
        f.server.heldFamilies = {QStringLiteral("Seasons"), QStringLiteral("NextUp")};
        {
            auto details = std::make_unique<Kinefin::ItemDetailsModel>(
                &f.client, kSeries, Kinefin::ItemDetailsModel::FirstChannel + 20);
            QTRY_COMPARE(details->status(), Kinefin::ItemDetailsModel::Ready);
            // What is below the item is asked on the next channel and is still in flight when the page goes
            QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Seasons")).size(), 1);
        }
        f.server.releaseHeld();
        QTest::qWait(250);
        QCOMPARE(failures.size(), 0);
    }

    void aDestroyedModelWithoutReleaseWouldHaveFailed()
    {
        // The control of the test above: the same held request, answered while its model still lives, does end in a failure signal
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy failures(&f.client, &Kinefin::JellyfinClient::listingFailed);
        f.server.heldFamilies = {QStringLiteral("Seasons")};
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Seasons")).size(), 1);
        f.server.releaseHeld();
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(f.children.seasonsStatus(), ItemChildrenModel::Failed);
    }

    void aReloadOrAnotherItemDropsTheLateReply()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 7;
        QSignalSpy episodes(&f.client, &Kinefin::JellyfinClient::episodesReady);
        // The first season's page is held, the same season is loaded again and answers
        f.server.heldFamilies = {QStringLiteral("Episodes")};
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Episodes")).size(), 1);
        f.server.heldFamilies.clear();
        f.children.load(detailsOf(QStringLiteral("season_item")));
        QTRY_COMPARE(f.children.episodes()->status(), Kinefin::PagedItemsModel::Ready);
        QCOMPARE(f.children.episodes()->totalCount(), 7);
        // The held answer of the first request arrives late: nothing changes
        f.server.releaseHeld();
        QTest::qWait(250);
        QCOMPARE(episodes.size(), 1);
        QCOMPARE(f.children.episodes()->totalCount(), 7);
        QCOMPARE(f.children.episodes()->status(), Kinefin::PagedItemsModel::Ready);
        // Another item loaded into the same model: the old lists go, and a late reply of the old item finds a different kind
        f.server.listingRawBody.insert(QStringLiteral("Items"),
                                       fixture(QStringLiteral("boxset_children")));
        f.children.load(detailsOf(QStringLiteral("boxset_item")));
        QCOMPARE(f.children.kind(), ItemChildrenModel::BoxSet);
        QCOMPARE(f.children.episodes()->rowCount(), 0);
        QCOMPARE(f.children.episodes()->status(), Kinefin::PagedItemsModel::Idle);
        QTRY_COMPARE(f.children.itemsStatus(), ItemChildrenModel::Ready);
    }

    void nextUpOfASeriesAsksForTheOverview()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.children.load(detailsOf(QStringLiteral("series_item")));
        QTRY_VERIFY(f.children.hasNextUp());
        const QUrlQuery nextUp = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("NextUp")).constFirst());
        QVERIFY(
            nextUp.queryItemValue(QStringLiteral("fields")).contains(QStringLiteral("Overview")));
    }

    void untrustedEpisodeFieldsAreCapped()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QJsonObject hostile{
            {QStringLiteral("Id"), QStringLiteral("f1f2f3f4f5f6f7f8f9fafbfcfdfeff01")},
            {QStringLiteral("Type"), QStringLiteral("Episode")},
            {QStringLiteral("Name"), QString(1000, QLatin1Char('n'))},
            {QStringLiteral("Overview"), QString(5000, QLatin1Char('o'))},
            {QStringLiteral("IndexNumber"), QStringLiteral("five")},
            {QStringLiteral("RunTimeTicks"), QStringLiteral("long")},
            {QStringLiteral("ImageTags"), QStringLiteral("not an object")}};
        f.server.listingRawBody.insert(QStringLiteral("Episodes"), envelope(QJsonArray{hostile}));
        f.children.load(detailsOf(QStringLiteral("season_item")));
        SeasonEpisodesModel *episodes = f.children.episodes();
        QTRY_COMPARE(episodes->status(), Kinefin::PagedItemsModel::Ready);
        const QModelIndex row = episodes->index(0);
        QCOMPARE(episodes->data(row, SeasonEpisodesModel::NameRole).toString().size(), 512);
        QCOMPARE(episodes->data(row, SeasonEpisodesModel::OverviewRole).toString().size(), 2048);
        QCOMPARE(episodes->data(row, SeasonEpisodesModel::EpisodeNumberRole).toInt(), -1);
        QCOMPARE(episodes->data(row, SeasonEpisodesModel::RuntimeMinutesRole).toInt(), -1);
        QVERIFY(episodes->data(row, SeasonEpisodesModel::ImageUrlRole).toUrl().isEmpty());
    }
};

QTEST_MAIN(TestItemChildrenModel)
#include "test_item_children_model.moc"
