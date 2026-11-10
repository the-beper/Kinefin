// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The home rows against the fake server: one row per kind and movie or series library on its own listing channel, hidden when empty, failures and retries per row, episodes listed under their series, wide images for landscape rows and untrusted entries
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/HomeRowsModel.h"
#include "models/ItemRowModel.h"
#include "models/LibraryItemsModel.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::HomeRowsModel;
using Kinefin::ItemRowModel;

namespace {
const QString kMovies = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
const QString kSeries = QStringLiteral("7f1b8f4b3e2148c5a8f2b7c9d0e1f2a3");
const QString kMusic = QStringLiteral("2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f");

QVariantMap library(const QString &id, const QString &name, const QString &type)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("collectionType"), type}};
}

QVariantList libraries()
{
    return {library(kMovies, QStringLiteral("Movies"), QStringLiteral("movies")),
            library(kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")),
            library(kMusic, QStringLiteral("Music"), QStringLiteral("music"))};
}

struct Fixture {
    Fixture() { ok = server.listen() && login(client, server); }

    int status(int row) const
    {
        return rows.data(rows.index(row), HomeRowsModel::StatusRole).toInt();
    }
    bool shown(int row) const
    {
        return rows.data(rows.index(row), HomeRowsModel::ShownRole).toBool();
    }
    ItemRowModel *items(int row) const
    {
        return qobject_cast<ItemRowModel *>(
            rows.data(rows.index(row), HomeRowsModel::ItemsRole).value<QObject *>());
    }
    bool allSettled() const
    {
        for (int row = 0; row < rows.rowCount(); ++row) {
            if (status(row) == HomeRowsModel::Loading)
                return false;
        }
        return rows.rowCount() > 0;
    }
    static QByteArray itemsBody(const QJsonArray &items)
    {
        return QJsonDocument(items).toJson(QJsonDocument::Compact);
    }
    static QByteArray envelope(const QJsonArray &items)
    {
        return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                         {QStringLiteral("TotalRecordCount"), items.size()}})
            .toJson(QJsonDocument::Compact);
    }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    HomeRowsModel rows{&client};
    bool ok = false;
};
} // namespace

class TestHomeRowsModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void everyRowLoadsOnItsOwnChannel()
    {
        Fixture f;
        QVERIFY(f.ok);
        QAbstractItemModelTester tester(&f.rows);
        f.rows.load(libraries());
        // Continue Watching, Next Up, and Recently Added for the movie and the series library; music has none
        QCOMPARE(f.rows.rowCount(), 4);
        QTRY_VERIFY(f.allSettled());
        for (int row = 0; row < 4; ++row)
            QCOMPARE(f.status(row), int(HomeRowsModel::Ready));
        QCOMPARE(f.rows.data(f.rows.index(0), HomeRowsModel::KindRole).toInt(),
                 int(HomeRowsModel::ContinueWatching));
        QCOMPARE(f.rows.data(f.rows.index(1), HomeRowsModel::KindRole).toInt(),
                 int(HomeRowsModel::NextUp));
        QCOMPARE(f.rows.data(f.rows.index(2), HomeRowsModel::LibraryNameRole).toString(),
                 QStringLiteral("Movies"));
        QCOMPARE(f.rows.data(f.rows.index(3), HomeRowsModel::LibraryIdRole).toString(), kSeries);
        QCOMPARE(f.items(0)->count(), 2);
        QCOMPARE(f.items(1)->count(), 1);
        QVERIFY(f.items(2)->count() > 0);
        // The continue and next-up rows are wide, the recently added ones are posters
        QVERIFY(f.items(0)->landscape());
        QVERIFY(f.items(1)->landscape());
        QVERIFY(!f.items(2)->landscape());
        // One request per row, the two Latest ones for their own library
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Resume")).size(), 1);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("NextUp")).size(), 1);
        const QList<QByteArray> latest = f.server.listingRoutes.value(QStringLiteral("Latest"));
        QCOMPARE(latest.size(), 2);
        // Parallel requests may arrive in either order: each library has its own
        const bool moviesFirst = latest.at(0).contains(kMovies.toUtf8());
        QVERIFY(latest.at(moviesFirst ? 0 : 1).contains(kMovies.toUtf8()));
        QVERIFY(latest.at(moviesFirst ? 1 : 0).contains(kSeries.toUtf8()));
        QVERIFY(latest.at(0).contains("limit=24"));
        QVERIFY(f.server.listingRoutes.value(QStringLiteral("NextUp"))
                    .constFirst()
                    .contains("nextUpDateCutoff="));
    }

    void anEmptyRowIsHiddenAndItsNeighboursStay()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingRawBody.insert(QStringLiteral("Latest"), QByteArrayLiteral("[]"));
        f.rows.load(libraries());
        QTRY_VERIFY(f.allSettled());
        QVERIFY(f.shown(0));
        QVERIFY(f.shown(1));
        QVERIFY(!f.shown(2));
        QVERIFY(!f.shown(3));
        QCOMPARE(f.status(2), int(HomeRowsModel::Ready));
    }

    void aFailedRowIsAloneAndRetryable()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Resume"), 500);
        f.rows.load(libraries());
        QTRY_VERIFY(f.allSettled());
        QCOMPARE(f.status(0), int(HomeRowsModel::Failed));
        QVERIFY(f.shown(0));
        for (int row = 1; row < 4; ++row)
            QCOMPARE(f.status(row), int(HomeRowsModel::Ready));
        f.server.listingStatus.clear();
        f.rows.retry(1); // a row that did not fail is left alone
        QCOMPARE(f.status(1), int(HomeRowsModel::Ready));
        f.rows.retry(0);
        QCOMPARE(f.status(0), int(HomeRowsModel::Loading));
        QTRY_COMPARE(f.status(0), int(HomeRowsModel::Ready));
        QCOMPARE(f.items(0)->count(), 2);
        // The other rows were not asked again
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("NextUp")).size(), 1);
    }

    void episodesAreListedUnderTheirSeries()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.rows.load(libraries());
        QTRY_VERIFY(f.allSettled());
        const ItemRowModel *nextUp = f.items(1);
        const QModelIndex first = nextUp->index(0);
        QCOMPARE(nextUp->data(first, ItemRowModel::NameRole).toString(),
                 QStringLiteral("Abyss Station"));
        QVERIFY(nextUp->data(first, ItemRowModel::EpisodeTitleRole)
                    .toString()
                    .contains(QStringLiteral("Weightless")));
        QCOMPARE(nextUp->data(first, ItemRowModel::SeasonNumberRole).toInt(), 2);
        QCOMPARE(nextUp->data(first, ItemRowModel::EpisodeNumberRole).toInt(), 6);
        // A movie keeps its own name and has no episode title
        const ItemRowModel *resume = f.items(0);
        QCOMPARE(resume->data(resume->index(0), ItemRowModel::NameRole).toString(),
                 QStringLiteral("Hollow Interval"));
        QVERIFY(
            resume->data(resume->index(0), ItemRowModel::EpisodeTitleRole).toString().isEmpty());
        QCOMPARE(resume->data(resume->index(0), ItemRowModel::SeasonNumberRole).toInt(), -1);
    }

    void landscapeRowsPickWideImages()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString idA = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1");
        const QString idB = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2");
        const QString idC = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa3");
        const QString idD = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa4");
        const auto tags = [](const QString &key, const QString &tag) {
            return QJsonObject{{key, tag}};
        };
        f.server.listingRawBody.insert(
            QStringLiteral("Resume"),
            Fixture::envelope(QJsonArray{
                QJsonObject{{QStringLiteral("Id"), idA},
                            {QStringLiteral("Type"), QStringLiteral("Movie")},
                            {QStringLiteral("ImageTags"),
                             QJsonObject{{QStringLiteral("Thumb"), QStringLiteral("t1")},
                                         {QStringLiteral("Primary"), QStringLiteral("p1")}}}},
                QJsonObject{{QStringLiteral("Id"), idB},
                            {QStringLiteral("Type"), QStringLiteral("Movie")},
                            {QStringLiteral("BackdropImageTags"), QJsonArray{QStringLiteral("b2")}},
                            {QStringLiteral("ImageTags"),
                             tags(QStringLiteral("Primary"), QStringLiteral("p2"))}},
                QJsonObject{{QStringLiteral("Id"), idC},
                            {QStringLiteral("Type"), QStringLiteral("Episode")},
                            {QStringLiteral("ImageTags"),
                             QJsonObject{{QStringLiteral("Thumb"), QStringLiteral("t3")},
                                         {QStringLiteral("Primary"), QStringLiteral("p3")}}}},
                QJsonObject{{QStringLiteral("Id"), idD},
                            {QStringLiteral("Type"), QStringLiteral("Movie")}}}));
        f.rows.load(libraries());
        QTRY_VERIFY(f.allSettled());
        const ItemRowModel *resume = f.items(0);
        const auto url = [resume](int row) {
            return resume->data(resume->index(row), ItemRowModel::ImageUrlRole).toUrl().toString();
        };
        // A movie prefers its thumb, then its backdrop; an episode shows its own image (already wide)
        QVERIFY(url(0).contains(QStringLiteral("/Images/Thumb")));
        QVERIFY(url(1).contains(QStringLiteral("/Images/Backdrop")));
        QVERIFY(url(2).contains(QStringLiteral("/Images/Primary")));
        QVERIFY(url(2).contains(QStringLiteral("tag=p3")));
        QVERIFY(url(3).isEmpty());
        for (int row = 0; row < 3; ++row)
            QVERIFY(
                Kinefin::JellyfinClient::isServerImageUrl(f.client.serverUrl(), QUrl(url(row))));
        // A poster row shows the primary image
        const ItemRowModel *latest = f.items(2);
        QVERIFY(latest->data(latest->index(0), ItemRowModel::ImageUrlRole)
                    .toUrl()
                    .toString()
                    .contains(QStringLiteral("/Images/Primary")));
    }

    void episodesFollowTheWebClientImageChain()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString series = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1");
        const QString parent = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb2");
        const auto episode = [&](const QString &id, const QJsonObject &extra) {
            QJsonObject object{{QStringLiteral("Id"), id},
                               {QStringLiteral("Type"), QStringLiteral("Episode")},
                               {QStringLiteral("SeriesId"), series},
                               {QStringLiteral("ImageTags"),
                                QJsonObject{{QStringLiteral("Primary"), QStringLiteral("own")}}}};
            for (auto it = extra.begin(); it != extra.end(); ++it)
                object.insert(it.key(), it.value());
            return object;
        };
        f.server.listingRawBody.insert(
            QStringLiteral("Resume"),
            Fixture::envelope(
                QJsonArray{// The series thumb wins over everything
                           episode(QStringLiteral("cccccccccccccccccccccccccccccc01"),
                                   {{QStringLiteral("SeriesThumbImageTag"), QStringLiteral("st")},
                                    {QStringLiteral("ParentBackdropItemId"), parent},
                                    {QStringLiteral("ParentBackdropImageTags"),
                                     QJsonArray{QStringLiteral("pb")}}}),
                           // Then the parent's backdrop
                           episode(QStringLiteral("cccccccccccccccccccccccccccccc02"),
                                   {{QStringLiteral("ParentBackdropItemId"), parent},
                                    {QStringLiteral("ParentBackdropImageTags"),
                                     QJsonArray{QStringLiteral("pb")}}}),
                           // Only then the episode's own image
                           episode(QStringLiteral("cccccccccccccccccccccccccccccc03"), {})}));
        f.rows.load(libraries());
        QTRY_VERIFY(f.allSettled());
        const ItemRowModel *resume = f.items(0);
        const auto url = [resume](int row) {
            return resume->data(resume->index(row), ItemRowModel::ImageUrlRole).toUrl().toString();
        };
        QVERIFY(
            url(0).contains(QStringLiteral("/Items/") + series + QStringLiteral("/Images/Thumb")));
        QVERIFY(url(1).contains(QStringLiteral("/Items/") + parent
                                + QStringLiteral("/Images/Backdrop")));
        QVERIFY(url(2).contains(
            QStringLiteral("/Items/cccccccccccccccccccccccccccccc03/Images/Primary")));
    }

    void anEpisodeShowsItsParentThumbAsTheServerFillsIt()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString series = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1");
        const QJsonObject episode{
            {QStringLiteral("Id"), QStringLiteral("cccccccccccccccccccccccccccccc04")},
            {QStringLiteral("Type"), QStringLiteral("Episode")},
            {QStringLiteral("SeriesId"), series},
            {QStringLiteral("ParentThumbItemId"), series},
            {QStringLiteral("ParentThumbImageTag"), QStringLiteral("pt")},
            {QStringLiteral("ParentBackdropItemId"), series},
            {QStringLiteral("ParentBackdropImageTags"), QJsonArray{QStringLiteral("pb")}},
            {QStringLiteral("ImageTags"),
             QJsonObject{{QStringLiteral("Primary"), QStringLiteral("own")}}}};
        f.server.listingRawBody.insert(QStringLiteral("Resume"),
                                       Fixture::envelope(QJsonArray{episode}));
        f.rows.load(libraries());
        QTRY_VERIFY(f.allSettled());
        const ItemRowModel *resume = f.items(0);
        const QString url =
            resume->data(resume->index(0), ItemRowModel::ImageUrlRole).toUrl().toString();
        // The parent's thumb comes before the parent's backdrop and the episode's own image
        QVERIFY(url.contains(QStringLiteral("/Items/") + series + QStringLiteral("/Images/Thumb")));
        QVERIFY(url.contains(QStringLiteral("tag=pt")));
    }

    void rowRequestsAskForTheImageTypesTheCardsShow()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.rows.load(libraries());
        QTRY_VERIFY(f.allSettled());
        for (const QString &family :
             {QStringLiteral("Resume"), QStringLiteral("NextUp"), QStringLiteral("Latest")}) {
            const QUrlQuery wire = FakeJellyfin::capturedListingQuery(
                f.server.listingRoutes.value(family).constFirst());
            QCOMPARE(wire.queryItemValue(QStringLiteral("imageTypeLimit")), QStringLiteral("1"));
            QVERIFY(wire.queryItemValue(QStringLiteral("enableImageTypes"))
                        .contains(QStringLiteral("Thumb")));
            QCOMPARE(wire.queryItemValue(QStringLiteral("fields")),
                     QStringLiteral("PrimaryImageAspectRatio"));
        }
    }

    void reloadWhileLoadingDropsTheOldRequests()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.holdListings = true;
        f.rows.load(libraries());
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Resume")).size(), 1);
        QCOMPARE(f.status(0), int(HomeRowsModel::Loading));
        // The libraries change while everything is still in flight: the new rows are asked for, the old ones are gone
        f.server.holdListings = false;
        f.rows.load({library(kMovies, QStringLiteral("Movies"), QStringLiteral("movies"))});
        QCOMPARE(f.rows.rowCount(), 3);
        QTRY_VERIFY(f.allSettled());
        for (int row = 0; row < 3; ++row)
            QCOMPARE(f.status(row), int(HomeRowsModel::Ready));
        // Two Latest requests of the first load (never answered) and the one of the new library
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Latest")).size(), 3);
        QVERIFY(f.items(2)->count() > 0);
    }

    void aLostSessionWithRowsInFlightLeavesNothingLoading()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.holdListings = true;
        f.rows.load(libraries());
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Latest")).size(), 2);
        // Every pending request now answers 401: the first one expires the session and the rows go away with it
        f.server.holdListings = false;
        f.server.denyListings = true;
        f.rows.reload();
        QTRY_COMPARE(f.rows.rowCount(), 0);
        QVERIFY(!f.client.isAuthenticated());
    }

    void untrustedEntriesAreDroppedAndTheRowIsCapped()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray many;
        for (int i = 0; i < 150; ++i)
            many.append(QJsonObject{
                {QStringLiteral("Id"), QStringLiteral("%1").arg(i + 1, 32, 16, QLatin1Char('0'))},
                {QStringLiteral("Name"), QStringLiteral("Item %1").arg(i)}});
        f.server.listingRawBody.insert(QStringLiteral("Latest"), Fixture::itemsBody(many));
        f.server.listingRawBody.insert(
            QStringLiteral("Resume"),
            Fixture::envelope(QJsonArray{
                QJsonObject{{QStringLiteral("Id"), QStringLiteral("../etc")},
                            {QStringLiteral("Name"), QStringLiteral("<img src=x>")}},
                QJsonObject{{QStringLiteral("Id"), 42}},
                QJsonObject{{QStringLiteral("Name"), QStringLiteral("no id")}},
                QJsonObject{
                    {QStringLiteral("Id"), QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f")},
                    {QStringLiteral("Name"), QString(1000, QLatin1Char('x'))}}}));
        f.rows.load(libraries());
        QTRY_VERIFY(f.allSettled());
        QCOMPARE(f.items(2)->count(), ItemRowModel::MaxItems);
        // Only the entry with a valid id survives, its name capped
        QCOMPARE(f.items(0)->count(), 1);
        QCOMPARE(f.items(0)->data(f.items(0)->index(0), ItemRowModel::NameRole).toString().size(),
                 512);
    }

    void reloadAsksAgainAndLossOfSessionClears()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.rows.load(libraries());
        QTRY_VERIFY(f.allSettled());
        f.rows.reload();
        QCOMPARE(f.rows.rowCount(), 4);
        QCOMPARE(f.status(0), int(HomeRowsModel::Loading));
        QTRY_VERIFY(f.allSettled());
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Resume")).size(), 2);
        f.client.disconnectFromServer();
        QCOMPARE(f.rows.rowCount(), 0);
        // Not signed in: a load has nothing to ask
        f.rows.load(libraries());
        QTRY_VERIFY(f.status(0) == int(HomeRowsModel::Failed));
    }

    void theGridAndTheRowsLoadTogether()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedItemTotal = 250;
        Kinefin::LibraryItemsModel grid(&f.client);
        f.rows.load(libraries());
        QVERIFY(grid.open(kMovies, QStringLiteral("movies"), QStringLiteral("Movies")));
        QTRY_VERIFY(f.allSettled());
        QTRY_COMPARE(grid.status(), Kinefin::LibraryItemsModel::Ready);
        QCOMPARE(grid.rowCount(), 250);
        QCOMPARE(f.items(0)->count(), 2);
    }
};

QTEST_MAIN(TestHomeRowsModel)
#include "test_home_rows_model.moc"
