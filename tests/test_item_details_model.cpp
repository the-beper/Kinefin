// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The details of one item against the fake server: the parsed fields, the wire of the request and of the similar items, the image URLs, caps on untrusted data, the states (not found, mismatch, failure, retry), models that do not disturb each other and the loss of the session
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/ItemDetailsModel.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::ItemDetailsModel;

namespace {
const QString kMovie = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");
const QString kEpisode = QStringLiteral("c2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7");

QByteArray fixtureItem(const QString &name)
{
    const QJsonObject item = QJsonDocument::fromJson(fixture(name))
                                 .object()
                                 .value(QStringLiteral("Items"))
                                 .toArray()
                                 .first()
                                 .toObject();
    return QJsonDocument(item).toJson(QJsonDocument::Compact);
}

QByteArray envelope(const QJsonArray &items)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                     {QStringLiteral("TotalRecordCount"), items.size()}})
        .toJson(QJsonDocument::Compact);
}

struct Fixture {
    Fixture()
    {
        server.itemsById.insert(kMovie, fixtureItem(QStringLiteral("movie_item")));
        server.itemsById.insert(kEpisode, fixtureItem(QStringLiteral("episode_item")));
        ok = server.listen() && login(client, server);
    }

    std::unique_ptr<ItemDetailsModel> open(const QString &id,
                                           int channel = ItemDetailsModel::FirstChannel)
    {
        return std::make_unique<ItemDetailsModel>(&client, id, channel);
    }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    bool ok = false;
};

bool allowed(const Fixture &f, const QUrl &url)
{
    return Kinefin::JellyfinClient::isServerImageUrl(f.client.serverUrl(), url);
}
} // namespace

class TestItemDetailsModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void watchedStateCanBeChangedAndSharedWithAnotherDetailsPage()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto movie = f.open(kMovie);
        auto second = f.open(kMovie, ItemDetailsModel::FirstChannel + 2);
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Ready);
        QTRY_COMPARE(second->status(), ItemDetailsModel::Ready);
        QVERIFY(!movie->played());
        QVERIFY(movie->canResume());
        QSignalSpy updated(&f.client, &Kinefin::JellyfinClient::itemPlayedUpdated);

        QVERIFY(movie->togglePlayed());
        QVERIFY(movie->playedBusy());
        QVERIFY(!movie->togglePlayed());
        QTRY_VERIFY(!movie->playedBusy());
        QCOMPARE(updated.size(), 1);
        QVERIFY(!movie->playedError());
        QVERIFY(movie->played());
        QVERIFY(second->played());
        QVERIFY(!movie->canResume());
        QCOMPARE(f.server.playedWrites.size(), 1);
        QCOMPARE(f.server.playedWrites.constFirst().method, QByteArrayLiteral("POST"));
        const QByteArray route = f.server.playedWrites.constFirst().route;
        QVERIFY(route.startsWith(QByteArray(QByteArrayLiteral("/jellyfin/UserPlayedItems/")
                                            + kMovie.toLatin1() + QByteArrayLiteral("?"))));
        QCOMPARE(FakeJellyfin::capturedListingQuery(route).queryItemValue(QStringLiteral("userId")),
                 f.client.userId());
        QVERIFY(f.server.playedWrites.constFirst().authorization.startsWith("MediaBrowser "));
        QVERIFY(!route.contains("token"));

        movie->reload();
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Ready);
        QVERIFY(movie->played());
        QVERIFY(!movie->canResume());
        QVERIFY(second->togglePlayed());
        QTRY_VERIFY(!second->playedBusy());
        QVERIFY(!second->playedError());
        QVERIFY(!second->played());
        QVERIFY(!movie->played());
        QCOMPARE(updated.size(), 2);
        QCOMPARE(f.server.playedWrites.size(), 2);
        QCOMPARE(f.server.playedWrites.constLast().method, QByteArrayLiteral("DELETE"));
    }

    // A rewatch stopped halfway: the server keeps Played with a resume point, and the toggle finishes it instead of taking the watched state away
    void aRewatchInProgressTogglesToWatched()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonObject item =
            QJsonDocument::fromJson(fixtureItem(QStringLiteral("movie_item"))).object();
        QJsonObject userData = item.value(QStringLiteral("UserData")).toObject();
        userData.insert(QStringLiteral("Played"), true);
        userData.insert(QStringLiteral("PlaybackPositionTicks"), 3'000'000'000.0);
        item.insert(QStringLiteral("UserData"), userData);
        f.server.itemsById.insert(kMovie, QJsonDocument(item).toJson(QJsonDocument::Compact));
        auto movie = f.open(kMovie);
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Ready);
        QVERIFY(movie->played());
        QVERIFY(movie->canResume());
        QVERIFY(movie->togglePlayed());
        QTRY_VERIFY(!movie->playedBusy());
        QCOMPARE(f.server.playedWrites.size(), 1);
        QCOMPARE(f.server.playedWrites.constFirst().method, QByteArrayLiteral("POST"));
        QVERIFY(movie->played());
        QVERIFY(!movie->canResume());
    }

    void aSeriesAndASeasonCanBeMarkedAndWhatHangsBelowIsReadAgain()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString series = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
        f.server.itemsById.insert(series, fixtureItem(QStringLiteral("series_item")));
        auto details = f.open(series);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QVERIFY(!details->playable());
        QVERIFY(details->canMarkPlayed());
        QTRY_COMPARE(details->below()->seasonsStatus(), Kinefin::ItemChildrenModel::Ready);
        QTRY_VERIFY(details->below()->playTargetsReady());
        const int seasonReads = f.server.listingRoutes.value(QStringLiteral("Seasons")).size();
        const int nextUpReads = f.server.listingRoutes.value(QStringLiteral("NextUp")).size();

        QVERIFY(details->togglePlayed());
        QVERIFY(details->playedBusy());
        QTRY_VERIFY(!details->playedBusy());
        QVERIFY(!details->playedError());
        QCOMPARE(f.server.playedWrites.size(), 1);
        QCOMPARE(f.server.playedWrites.constFirst().method, QByteArrayLiteral("POST"));
        QVERIFY(details->played());
        QCOMPARE(details->unplayedCount(), 0);
        // The seasons and the episode to go on with come from the server again
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Seasons")).size(),
                     seasonReads + 1);
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("NextUp")).size(),
                     nextUpReads + 1);

        // The answer of the server brings the real count, which wins over the guess of the model
        f.server.playedWriteRawBody =
            QByteArrayLiteral("{\"Played\":false,\"UnplayedItemCount\":12}");
        QVERIFY(details->togglePlayed());
        QTRY_VERIFY(!details->playedBusy());
        QCOMPARE(f.server.playedWrites.constLast().method, QByteArrayLiteral("DELETE"));
        QVERIFY(!details->played());
        QCOMPARE(details->unplayedCount(), 12);
        f.server.playedWriteRawBody.clear();
        // Without a count in the answer it is not known until the page reads the series again
        QVERIFY(details->togglePlayed());
        QTRY_VERIFY(!details->playedBusy());
        QVERIFY(details->played());
        QVERIFY(details->togglePlayed());
        QTRY_VERIFY(!details->playedBusy());
        QCOMPARE(details->unplayedCount(), -1);

        const QString season = QStringLiteral("11aa22bb33cc44dd55ee66ff778899aa");
        f.server.itemsById.insert(season, fixtureItem(QStringLiteral("season_item")));
        auto seasonDetails = f.open(season, ItemDetailsModel::FirstChannel + 2);
        QTRY_COMPARE(seasonDetails->status(), ItemDetailsModel::Ready);
        QVERIFY(seasonDetails->canMarkPlayed());
        QVERIFY(seasonDetails->togglePlayed());
        QTRY_VERIFY(!seasonDetails->playedBusy());
        QVERIFY(seasonDetails->played());
    }

    void theUnplayedCountFollowsTheUserData()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString series = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
        f.server.itemsById.insert(series, fixtureItem(QStringLiteral("series_item")));
        auto details = f.open(series);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QSignalSpy changed(details.get(), &ItemDetailsModel::detailsChanged);
        Q_EMIT f.client.itemUnplayedCountUpdated(series.toUpper(), 4);
        QCOMPARE(details->unplayedCount(), 4);
        QCOMPARE(changed.size(), 1);
        // The same count again, and another item's, change nothing
        Q_EMIT f.client.itemUnplayedCountUpdated(series, 4);
        Q_EMIT f.client.itemUnplayedCountUpdated(QStringLiteral("bb11cc22dd33ee44ff5566778899aabb"),
                                                 1);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(details->unplayedCount(), 4);
    }

    // The season and the series above an episode are read again after its write, so their counts follow
    void anEpisodeWriteReadsItsSeasonAndSeriesAgain()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto details = f.open(kEpisode);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        const QString season = details->seasonId();
        const QString series = details->seriesId();
        QVERIFY(!season.isEmpty() || !series.isEmpty());
        const int before = f.server.itemsRoutes.size();
        QVERIFY(details->togglePlayed());
        QTRY_VERIFY(!details->playedBusy());
        QTRY_VERIFY(f.server.itemsRoutes.size() > before);
        const QString route = QString::fromUtf8(f.server.itemsRoutes.constLast());
        if (!series.isEmpty())
            QVERIFY(route.contains(series));
        if (!season.isEmpty())
            QVERIFY(route.contains(season));
    }

    void aCollectionCannotBeMarked()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString boxSet = QStringLiteral("bb11cc22dd33ee44ff5566778899aabb");
        f.server.itemsById.insert(boxSet, fixtureItem(QStringLiteral("boxset_item")));
        auto details = f.open(boxSet);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QVERIFY(!details->canMarkPlayed());
        QVERIFY(!details->togglePlayed());
        QVERIFY(f.server.playedWrites.isEmpty());
    }

    void watchedFailuresDoNotChangeStateAndCanBeRetried()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);

        f.server.playedWriteStatus = 500;
        QVERIFY(details->togglePlayed());
        QTRY_VERIFY(!details->playedBusy());
        QVERIFY(details->playedError());
        QVERIFY(!details->played());

        f.server.playedWriteStatus = 200;
        f.server.redirectPlayedWrites = true;
        QVERIFY(details->togglePlayed());
        QTRY_VERIFY(!details->playedBusy());
        QVERIFY(details->playedError());
        QVERIFY(!details->played());

        f.server.redirectPlayedWrites = false;
        for (const QByteArray &response :
             {QByteArrayLiteral("not-json"), QByteArrayLiteral("{\"Played\":false}"),
              QByteArrayLiteral("{\"Played\":true,\"ItemId\":\"bad\"}")}) {
            f.server.playedWriteRawBody = response;
            QVERIFY(details->togglePlayed());
            QTRY_VERIFY(!details->playedBusy());
            QVERIFY(details->playedError());
            QVERIFY(!details->played());
        }
        f.server.playedWriteRawBody.clear();
        f.server.playedWriteStatus = 204;
        QVERIFY(details->togglePlayed());
        QTRY_VERIFY(!details->playedBusy());
        QVERIFY(!details->playedError());
        QVERIFY(details->played());
    }

    void watchedWriteDeduplicatesGuidSpellingsAndExpiresWithSession()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(!f.client.setItemPlayed(QStringLiteral("../bad/path"), true));
        QVERIFY(!f.client.setItemPlayed(QString(), false));
        QCOMPARE(f.server.playedWrites.size(), 0);
        f.server.holdPlayedWrites = true;
        QVERIFY(f.client.setItemPlayed(kMovie, true));
        QTRY_COMPARE(f.server.playedWrites.size(), 1);
        const QString dashed = QStringLiteral("f0e1d2c3-b4a5-9687-7869-5a4b3c2d1e0f");
        QVERIFY(!f.client.setItemPlayed(dashed, false));
        QCOMPARE(f.server.playedWrites.size(), 1);
        QSignalSpy updated(&f.client, &Kinefin::JellyfinClient::itemPlayedUpdated);
        f.client.forgetSession();
        f.server.releaseHeld();
        QTest::qWait(100);
        QVERIFY(updated.isEmpty());
        QVERIFY(!f.client.setItemPlayed(kMovie, false));

        Fixture g;
        QVERIFY(g.ok);
        auto details = g.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        g.server.playedWriteStatus = 401;
        QVERIFY(details->togglePlayed());
        QTRY_COMPARE(details->status(), ItemDetailsModel::Idle);
        QVERIFY(!details->playedBusy());
        QVERIFY(!details->playedError());
        QVERIFY(!g.client.isAuthenticated());
    }

    void favoriteCanBeAddedRemovedAndSharedWithAnotherDetailsPage()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto movie = f.open(kMovie);
        auto second = f.open(kMovie, ItemDetailsModel::FirstChannel + 2);
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Ready);
        QTRY_COMPARE(second->status(), ItemDetailsModel::Ready);
        QVERIFY(!movie->favorite());

        QVERIFY(movie->toggleFavorite());
        QVERIFY(movie->favoriteBusy());
        QVERIFY(!movie->toggleFavorite()); // only one write per item
        QTRY_VERIFY(!movie->favoriteBusy());
        QVERIFY(!movie->favoriteError());
        QVERIFY(movie->favorite());
        QVERIFY(second->favorite());
        QCOMPARE(f.server.favoriteWrites.size(), 1);
        QCOMPARE(f.server.favoriteWrites.constFirst().method, QByteArrayLiteral("POST"));
        QCOMPARE(f.server.favoriteWrites.constFirst().route,
                 QByteArrayLiteral("/jellyfin/UserFavoriteItems/") + kMovie.toLatin1());
        QVERIFY(f.server.favoriteWrites.constFirst().authorization.startsWith("MediaBrowser "));
        QVERIFY(!f.server.favoriteWrites.constFirst().route.contains("token"));

        // A detail reload uses the value persisted by the server.
        movie->reload();
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Ready);
        QVERIFY(movie->favorite());

        QVERIFY(second->toggleFavorite());
        QTRY_VERIFY(!second->favoriteBusy());
        QVERIFY(!second->favoriteError());
        QVERIFY(!second->favorite());
        QVERIFY(!movie->favorite());
        QCOMPARE(f.server.favoriteWrites.size(), 2);
        QCOMPARE(f.server.favoriteWrites.constLast().method, QByteArrayLiteral("DELETE"));
        QCOMPARE(f.server.favoriteWrites.constLast().route,
                 QByteArrayLiteral("/jellyfin/UserFavoriteItems/") + kMovie.toLatin1());
    }

    void favoriteWriteAnsweredBeforeAStaleReloadKeepsItsValue()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QVERIFY(!details->favorite());

        // The write is in flight when a reload asks again, and the server serves that read before it applies the write
        f.server.holdFavoriteWrites = true;
        QVERIFY(details->toggleFavorite());
        QTRY_COMPARE(f.server.heldCount(), qsizetype(1));
        f.server.snapshotItemsReads = true;
        details->reload();
        QTRY_COMPARE(f.server.snapshotCount(), qsizetype(1));

        // The write is answered first, the old read second
        f.server.releaseHeld();
        QTRY_VERIFY(!details->favoriteBusy());
        QVERIFY(!details->favoriteError());
        QVERIFY(details->favorite());
        f.server.releaseSnapshots();
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QVERIFY(details->favorite());

        // A reload that starts after the write is answered is trusted again (the held write never reached the fake's store)
        f.server.snapshotItemsReads = false;
        details->reload();
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QVERIFY(!details->favorite());
    }

    void playedWriteAnsweredBeforeAStaleReloadKeepsItsValue()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QVERIFY(!details->played());

        f.server.holdPlayedWrites = true;
        QVERIFY(details->togglePlayed());
        QTRY_COMPARE(f.server.heldCount(), qsizetype(1));
        f.server.snapshotItemsReads = true;
        details->reload();
        QTRY_COMPARE(f.server.snapshotCount(), qsizetype(1));

        f.server.releaseHeld();
        QTRY_VERIFY(!details->playedBusy());
        QVERIFY(!details->playedError());
        QVERIFY(details->played());
        f.server.releaseSnapshots();
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QVERIFY(details->played());

        // The held write never reached the fake's store, so a trusted reload shows it unplayed again
        f.server.snapshotItemsReads = false;
        details->reload();
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QVERIFY(!details->played());
    }

    void favoriteWriteFailureLeavesOldStateAndCanRetry()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);

        f.server.favoriteWriteStatus = 500;
        QVERIFY(details->toggleFavorite());
        QTRY_VERIFY(!details->favoriteBusy());
        QVERIFY(details->favoriteError());
        QVERIFY(!details->favorite());

        f.server.favoriteWriteStatus = 200;
        f.server.redirectFavoriteWrites = true;
        QVERIFY(details->toggleFavorite());
        QTRY_VERIFY(!details->favoriteBusy());
        QVERIFY(details->favoriteError());
        QVERIFY(!details->favorite());

        f.server.redirectFavoriteWrites = false;
        QVERIFY(details->toggleFavorite());
        QTRY_VERIFY(!details->favoriteBusy());
        QVERIFY(!details->favoriteError());
        QVERIFY(details->favorite());
        QCOMPARE(f.server.favoriteWrites.size(), 3);
    }

    void favoriteRejectsInvalidOrMismatchedSuccessPayload()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);

        for (const QByteArray &response :
             {QByteArrayLiteral("not-json"), QByteArrayLiteral("{\"IsFavorite\":false}"),
              QByteArrayLiteral("{\"IsFavorite\":true,\"ItemId\":\"bad\"}")}) {
            f.server.favoriteWriteRawBody = response;
            QVERIFY(details->toggleFavorite());
            QTRY_VERIFY(!details->favoriteBusy());
            QVERIFY(details->favoriteError());
            QVERIFY(!details->favorite());
        }
        f.server.favoriteWriteRawBody.clear();
        f.server.favoriteWriteStatus = 204;
        QVERIFY(details->toggleFavorite());
        QTRY_VERIFY(!details->favoriteBusy());
        QVERIFY(details->favorite());
        QVERIFY(!details->favoriteError());
    }

    void favoriteRejectsInvalidIdsAndDeduplicatesGuidSpellings()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(!f.client.setItemFavorite(QStringLiteral("../bad/path"), true));
        QVERIFY(!f.client.setItemFavorite(QString(), false));
        QCOMPARE(f.server.favoriteWrites.size(), 0);

        f.server.holdFavoriteWrites = true;
        QVERIFY(f.client.setItemFavorite(kMovie, true));
        QTRY_COMPARE(f.server.favoriteWrites.size(), 1);
        const QString dashed = QStringLiteral("f0e1d2c3-b4a5-9687-7869-5a4b3c2d1e0f");
        QVERIFY(!f.client.setItemFavorite(dashed, false));
        QCOMPARE(f.server.favoriteWrites.size(), 1);
        f.server.releaseHeld();
    }

    void favoriteWriteExpiryDiscardsStateAndStaleReplies()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);

        f.server.favoriteWriteStatus = 401;
        QVERIFY(details->toggleFavorite());
        QTRY_COMPARE(details->status(), ItemDetailsModel::Idle);
        QVERIFY(!details->favoriteBusy());
        QVERIFY(!details->favoriteError());
        QVERIFY(!f.client.isAuthenticated());
        QVERIFY(!f.client.setItemFavorite(kMovie, false));

        // A new login plus a late reply from an old session must not mutate the
        // new user's page or emit an update to Favorites.
        Fixture g;
        QVERIFY(g.ok);
        auto newPage = g.open(kMovie);
        QTRY_COMPARE(newPage->status(), ItemDetailsModel::Ready);
        QSignalSpy successful(&g.client, &Kinefin::JellyfinClient::itemFavoriteUpdated);
        g.server.holdFavoriteWrites = true;
        QVERIFY(newPage->toggleFavorite());
        QTRY_COMPARE(g.server.favoriteWrites.size(), 1);
        g.client.forgetSession();
        QCOMPARE(newPage->status(), ItemDetailsModel::Idle);
        g.server.releaseHeld();
        QTest::qWait(100);
        QVERIFY(successful.isEmpty());
        QVERIFY(!newPage->favoriteBusy());
    }

    void parsesTheMovieFixture()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto details = f.open(kMovie);
        QCOMPARE(details->status(), ItemDetailsModel::Loading);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QCOMPARE(details->name(), QStringLiteral("Arcturus (2012)"));
        QCOMPARE(details->type(), QStringLiteral("Movie"));
        QCOMPARE(details->year(), 2012);
        QCOMPARE(details->runtimeMinutes(), 18);
        QCOMPARE(details->officialRating(), QStringLiteral("PG-13"));
        QCOMPARE(details->communityRating(), 7.1);
        QCOMPARE(details->tagline(), QStringLiteral("In widescreen."));
        QCOMPARE(details->genres(),
                 (QStringList{QStringLiteral("Science Fiction"), QStringLiteral("Mystery")}));
        QCOMPARE(details->directors(), QStringList{QStringLiteral("Ines Okafor")});
        QCOMPARE(details->writers(), QStringList{QStringLiteral("Pavel Lund")});
        QVERIFY(details->overview().contains(QStringLiteral("distress call")));
        QVERIFY(details->playable());
        QVERIFY(details->canResume());
        QVERIFY(!details->played());

        const QVariantList people = details->people();
        QCOMPARE(people.size(), 5);
        QVERIFY(!people.at(0).toMap().value(QStringLiteral("imageUrl")).toUrl().isEmpty());
        // No image tag, and an id that is no id: no image
        QVERIFY(people.at(1).toMap().value(QStringLiteral("imageUrl")).toUrl().isEmpty());
        QVERIFY(people.at(2).toMap().value(QStringLiteral("imageUrl")).toUrl().isEmpty());

        const QVariantList chapters = details->chapters();
        QCOMPARE(chapters.size(), 3);
        QCOMPARE(chapters.at(1).toMap().value(QStringLiteral("time")).toString(),
                 QStringLiteral("0:15:00"));
        QCOMPARE(chapters.at(2).toMap().value(QStringLiteral("time")).toString(),
                 QStringLiteral("1:30:00"));

        const QVariantList sources = details->mediaSources();
        QCOMPARE(sources.size(), 1);
        const QVariantMap source = sources.constFirst().toMap();
        QCOMPARE(source.value(QStringLiteral("container")).toString(), QStringLiteral("mkv"));
        QCOMPARE(source.value(QStringLiteral("video")).toList().size(), 1);
        QCOMPARE(source.value(QStringLiteral("audio")).toList().size(), 1);
        QCOMPARE(source.value(QStringLiteral("subtitles")).toList().size(), 1);
        QVERIFY(source.value(QStringLiteral("subtitles"))
                    .toList()
                    .constFirst()
                    .toMap()
                    .value(QStringLiteral("isExternal"))
                    .toBool());
        // The server's path to the file is never exposed
        QVERIFY(!source.contains(QStringLiteral("path")));
        QVERIFY(!source.contains(QStringLiteral("Path")));
    }

    void parsesDatesStatusCriticRatingAndStudios()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto movie = f.open(kMovie);
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Ready);
        QCOMPARE(movie->premiereDate(), QStringLiteral("2012-06-15"));
        QVERIFY(movie->endDate().isEmpty());
        QVERIFY(movie->seriesStatus().isEmpty());
        QCOMPARE(movie->criticRating(), 88.0);
        QCOMPARE(movie->studios(), QStringList{QStringLiteral("Vega Pictures")});

        const QJsonObject item{
            {QStringLiteral("Id"), kMovie},
            {QStringLiteral("Type"), QStringLiteral("Series")},
            {QStringLiteral("PremiereDate"), QStringLiteral("2015-02-08T00:00:00.0000000Z")},
            {QStringLiteral("EndDate"), QStringLiteral("2022-08-15T00:00:00.0000000Z")},
            {QStringLiteral("Status"), QStringLiteral("Ended")},
            {QStringLiteral("CriticRating"), 97.5},
            {QStringLiteral("Studios"),
             QJsonArray{QJsonObject{{QStringLiteral("Name"), QStringLiteral("AMC")}},
                        QJsonObject{{QStringLiteral("Id"), kMovie}},
                        QStringLiteral("not an object")}}};
        const Kinefin::ItemDetails parsed = Kinefin::parseItemDetails(item);
        QVERIFY(parsed.valid);
        QCOMPARE(parsed.premiereDate, QDate(2015, 2, 8));
        QCOMPARE(parsed.endDate, QDate(2022, 8, 15));
        QCOMPARE(parsed.seriesStatus, QStringLiteral("Ended"));
        QCOMPARE(parsed.criticRating, 97.5);
        // An entry with no name is dropped
        QCOMPARE(parsed.studios, QStringList{QStringLiteral("AMC")});

        // Wrong types and out-of-range values are ignored
        QJsonObject hostile = item;
        hostile.insert(QStringLiteral("PremiereDate"), 20150208);
        hostile.insert(QStringLiteral("EndDate"), QStringLiteral("not a date"));
        hostile.insert(QStringLiteral("Status"), QStringLiteral("Cancelled"));
        hostile.insert(QStringLiteral("CriticRating"), 120);
        hostile.insert(QStringLiteral("Studios"), QStringLiteral("AMC"));
        const Kinefin::ItemDetails ignored = Kinefin::parseItemDetails(hostile);
        QVERIFY(!ignored.premiereDate.isValid());
        QVERIFY(!ignored.endDate.isValid());
        QVERIFY(ignored.seriesStatus.isEmpty());
        QCOMPARE(ignored.criticRating, -1.0);
        QVERIFY(ignored.studios.isEmpty());

        // An item that carries none of the new fields reads as empty
        const Kinefin::ItemDetails bare =
            Kinefin::parseItemDetails(QJsonObject{{QStringLiteral("Id"), kMovie}});
        QVERIFY(!bare.premiereDate.isValid());
        QVERIFY(bare.seriesStatus.isEmpty());
        QCOMPARE(bare.criticRating, -1.0);
    }

    void parsesRemoteTrailers()
    {
        const QJsonObject item{
            {QStringLiteral("Id"), kMovie},
            {QStringLiteral("RemoteTrailers"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("Name"), QStringLiteral("Official")},
                             {QStringLiteral("Url"),
                              QStringLiteral("https://www.youtube.com/watch?v=abc")}},
                 QJsonObject{{QStringLiteral("Name"), QStringLiteral("Insecure")},
                             {QStringLiteral("Url"), QStringLiteral("http://example.com/trailer")}},
                 QJsonObject{{QStringLiteral("Name"), QStringLiteral("File")},
                             {QStringLiteral("Url"), QStringLiteral("file:///etc/passwd")}},
                 QJsonObject{{QStringLiteral("Name"), QStringLiteral("Credentials")},
                             {QStringLiteral("Url"),
                              QStringLiteral("https://user:pass@example.com/trailer")}},
                 QJsonObject{{QStringLiteral("Name"), QStringLiteral("No URL")}}}}};
        const Kinefin::ItemDetails parsed = Kinefin::parseItemDetails(item);
        QVERIFY(parsed.valid);
        QCOMPARE(parsed.trailers.size(), 1);
        QCOMPARE(parsed.trailers.constFirst().name, QStringLiteral("Official"));
        QCOMPARE(parsed.trailers.constFirst().url,
                 QStringLiteral("https://www.youtube.com/watch?v=abc"));
        const Kinefin::ItemDetails bare =
            Kinefin::parseItemDetails(QJsonObject{{QStringLiteral("Id"), kMovie}});
        QVERIFY(bare.trailers.isEmpty());
    }

    void parsesCanDelete()
    {
        const Kinefin::ItemDetails allowed = Kinefin::parseItemDetails(
            QJsonObject{{QStringLiteral("Id"), kMovie}, {QStringLiteral("CanDelete"), true}});
        QVERIFY(allowed.canDelete);
        const Kinefin::ItemDetails absent =
            Kinefin::parseItemDetails(QJsonObject{{QStringLiteral("Id"), kMovie}});
        QVERIFY(!absent.canDelete);
        // Only a bool says yes
        const Kinefin::ItemDetails text = Kinefin::parseItemDetails(QJsonObject{
            {QStringLiteral("Id"), kMovie}, {QStringLiteral("CanDelete"), QStringLiteral("true")}});
        QVERIFY(!text.canDelete);
    }

    void parsesTheDownloadFields()
    {
        const QJsonObject source{{QStringLiteral("Id"), kMovie},
                                 {QStringLiteral("Container"), QStringLiteral("mkv")},
                                 {QStringLiteral("Size"), 1234},
                                 {QStringLiteral("ETag"), QStringLiteral("\"abc\"")}};
        const QJsonObject item{{QStringLiteral("Id"), kMovie},
                               {QStringLiteral("Name"), QStringLiteral("Movie")},
                               {QStringLiteral("Type"), QStringLiteral("Movie")},
                               {QStringLiteral("CanDownload"), false},
                               {QStringLiteral("MediaSources"), QJsonArray{source}}};
        const Kinefin::ItemDetails parsed = Kinefin::parseItemDetails(item);
        QVERIFY(parsed.valid);
        QVERIFY(!parsed.canDownload);
        QCOMPARE(parsed.sources.size(), 1);
        QCOMPARE(parsed.sources.constFirst().container, QStringLiteral("mkv"));
        QCOMPARE(parsed.sources.constFirst().size, 1234);
        QCOMPARE(parsed.sources.constFirst().etag, QStringLiteral("\"abc\""));

        // An absent flag leaves the policy in charge, and the item's Etag spelling still reads as a fallback
        QJsonObject fallback = item;
        fallback.remove(QStringLiteral("CanDownload"));
        fallback.insert(
            QStringLiteral("MediaSources"),
            QJsonArray{QJsonObject{{QStringLiteral("Etag"), QStringLiteral("\"old\"")}}});
        const Kinefin::ItemDetails withoutFlag = Kinefin::parseItemDetails(fallback);
        QVERIFY(withoutFlag.canDownload);
        QCOMPARE(withoutFlag.sources.constFirst().etag, QStringLiteral("\"old\""));
    }

    // An episode the server only knows of has nothing to play or download, whatever CanDownload says; a season of a series without season folders is Virtual too and keeps its download
    void aVirtualEpisodeCannotBePlayedOrDownloaded()
    {
        const QJsonObject episode{{QStringLiteral("Id"), kMovie},
                                  {QStringLiteral("Name"), QStringLiteral("Not here")},
                                  {QStringLiteral("Type"), QStringLiteral("Episode")},
                                  {QStringLiteral("LocationType"), QStringLiteral("Virtual")},
                                  {QStringLiteral("CanDownload"), true}};
        const Kinefin::ItemDetails parsed = Kinefin::parseItemDetails(episode);
        QVERIFY(parsed.valid);
        QVERIFY(parsed.virtualItem);
        QVERIFY(!parsed.canDownload);
        QJsonObject real = episode;
        real.insert(QStringLiteral("LocationType"), QStringLiteral("FileSystem"));
        QVERIFY(!Kinefin::parseItemDetails(real).virtualItem);
        QVERIFY(Kinefin::parseItemDetails(real).canDownload);
        QJsonObject season = episode;
        season.insert(QStringLiteral("Type"), QStringLiteral("Season"));
        QVERIFY(!Kinefin::parseItemDetails(season).virtualItem);
        QVERIFY(Kinefin::parseItemDetails(season).canDownload);
    }

    void mediaBadgesComeFromTheFirstSource()
    {
        Fixture f;
        QVERIFY(f.ok);
        const auto badges = [](const ItemDetailsModel &model) {
            QStringList result;
            for (const QVariant &entry : model.mediaBadges())
                result << entry.toMap().value(QStringLiteral("kind")).toString() + QLatin1Char('=')
                              + entry.toMap().value(QStringLiteral("text")).toString();
            return result;
        };
        auto movie = f.open(kMovie);
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Ready);
        QCOMPARE(badges(*movie),
                 (QStringList{QStringLiteral("resolution=1080p"), QStringLiteral("hdr=HDR10"),
                              QStringLiteral("video=HEVC"),
                              QStringLiteral("audio=Dolby Digital Plus 5.1"),
                              QStringLiteral("subtitles=1")}));

        const auto stream = [](const QString &type, const QJsonObject &fields) {
            QJsonObject object{{QStringLiteral("Type"), type}};
            for (auto it = fields.begin(); it != fields.end(); ++it)
                object.insert(it.key(), it.value());
            return object;
        };
        const QString other = QStringLiteral("0123456789abcdef0123456789abcdef");
        const QJsonObject first{
            {QStringLiteral("Id"), QStringLiteral("a")},
            {QStringLiteral("MediaStreams"),
             QJsonArray{
                 stream(QStringLiteral("Video"),
                        {{QStringLiteral("Codec"), QStringLiteral("someveryverylongcodec")},
                         {QStringLiteral("Width"), 1280},
                         {QStringLiteral("Height"), 720},
                         {QStringLiteral("VideoRangeType"), QStringLiteral("DOVIWithHDR10")}}),
                 stream(QStringLiteral("Audio"), {{QStringLiteral("Codec"), QStringLiteral("aac")},
                                                  {QStringLiteral("Channels"), 2}}),
                 stream(QStringLiteral("Audio"),
                        {{QStringLiteral("Codec"), QStringLiteral("truehd")},
                         {QStringLiteral("ChannelLayout"), QStringLiteral("7.1(wide)")},
                         {QStringLiteral("IsDefault"), true}}),
                 stream(QStringLiteral("Subtitle"), {}), stream(QStringLiteral("Subtitle"), {}),
                 stream(QStringLiteral("Attachment"), {})}}};
        const QJsonObject second{
            {QStringLiteral("Id"), QStringLiteral("b")},
            {QStringLiteral("MediaStreams"),
             QJsonArray{
                 stream(QStringLiteral("Video"), {{QStringLiteral("Codec"), QStringLiteral("av1")},
                                                  {QStringLiteral("Width"), 3840}})}}};
        const QJsonObject item{{QStringLiteral("Id"), other},
                               {QStringLiteral("Type"), QStringLiteral("Movie")},
                               {QStringLiteral("MediaSources"), QJsonArray{first, second}}};
        f.server.itemsById.insert(other, QJsonDocument(item).toJson(QJsonDocument::Compact));
        auto model = f.open(other, ItemDetailsModel::FirstChannel + 2);
        QTRY_COMPARE(model->status(), ItemDetailsModel::Ready);
        // The second source's 4K and AV1 do not show; the unknown codec is upper-cased and capped; the default audio track wins; the Dolby Vision variant is one badge
        QCOMPARE(badges(*model),
                 (QStringList{QStringLiteral("resolution=720p"), QStringLiteral("hdr=Dolby Vision"),
                              QStringLiteral("video=SOMEVERYVERY"),
                              QStringLiteral("audio=TrueHD 7.1"), QStringLiteral("subtitles=2")}));

        // No source, no badge
        auto episode = f.open(kEpisode, ItemDetailsModel::FirstChannel + 4);
        QTRY_COMPARE(episode->status(), ItemDetailsModel::Ready);
        QCOMPARE(badges(*episode),
                 (QStringList{QStringLiteral("resolution=1080p"), QStringLiteral("video=H.264")}));
        const QString empty = QStringLiteral("fedcba9876543210fedcba9876543210");
        f.server.itemsById.insert(empty, QJsonDocument(QJsonObject{{QStringLiteral("Id"), empty}})
                                             .toJson(QJsonDocument::Compact));
        auto none = f.open(empty, ItemDetailsModel::FirstChannel + 6);
        QTRY_COMPARE(none->status(), ItemDetailsModel::Ready);
        QVERIFY(none->mediaBadges().isEmpty());
    }

    void requestCarriesExactlyIdsFieldsUserIdOnItsChannel()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        const QUrlQuery wire =
            FakeJellyfin::capturedListingQuery(f.server.itemsRoutes.constFirst());
        QCOMPARE(wire.queryItemValue(QStringLiteral("ids")), kMovie);
        const QString fields = wire.queryItemValue(QStringLiteral("fields"));
        for (const QString &field :
             {QStringLiteral("Overview"), QStringLiteral("Genres"), QStringLiteral("People"),
              QStringLiteral("Chapters"), QStringLiteral("MediaSources"),
              QStringLiteral("Taglines"), QStringLiteral("OriginalTitle"),
              QStringLiteral("Studios"), QStringLiteral("CanDownload"), QStringLiteral("CanDelete"),
              QStringLiteral("RemoteTrailers")})
            QVERIFY2(fields.contains(field), qPrintable(field));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("parentId")));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("limit")));
        QVERIFY(!wire.queryItemValue(QStringLiteral("userId")).isEmpty());
        // The similar items are asked for once the item is known
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Similar")).size(), 1);
        QTRY_COMPARE(details->similarStatus(), ItemDetailsModel::Ready);
        QVERIFY(details->similar()->count() > 0);
    }

    void imagesPassTheServerImageAllowlist()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto movie = f.open(kMovie);
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Ready);
        for (const QUrl &url : {movie->backdropUrl(), movie->logoUrl(), movie->posterUrl()}) {
            QVERIFY(!url.isEmpty());
            QVERIFY2(allowed(f, url), qPrintable(url.toString()));
        }
        QVERIFY(movie->backdropUrl().toString().contains(QStringLiteral("/Images/Backdrop")));
        QVERIFY(
            allowed(f, movie->people().at(0).toMap().value(QStringLiteral("imageUrl")).toUrl()));

        // An episode shows its parent's backdrop and logo
        auto episode =
            f.open(kEpisode, ItemDetailsModel::FirstChannel + ItemDetailsModel::ChannelsPerModel);
        QTRY_COMPARE(episode->status(), ItemDetailsModel::Ready);
        QVERIFY(episode->backdropUrl().toString().contains(
            QStringLiteral("/Items/aa11bb22cc33dd44ee55ff6677889900/Images/Backdrop")));
        QVERIFY(episode->logoUrl().toString().contains(
            QStringLiteral("/Items/aa11bb22cc33dd44ee55ff6677889900/Images/Logo")));
        QVERIFY(allowed(f, episode->backdropUrl()));
        QVERIFY(allowed(f, episode->logoUrl()));
        QCOMPARE(episode->seasonNumber(), 2);
        QCOMPARE(episode->episodeNumber(), 6);
        QCOMPARE(episode->seriesName(), QStringLiteral("Abyss Station"));
        QCOMPARE(episode->seriesId(), QStringLiteral("aa11bb22cc33dd44ee55ff6677889900"));
    }

    void episodesSkipTheSimilarRequest()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto episode = f.open(kEpisode);
        QTRY_COMPARE(episode->status(), ItemDetailsModel::Ready);
        QTest::qWait(200);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Similar")).size(), 0);
        QCOMPARE(episode->similarStatus(), ItemDetailsModel::Idle);
        QCOMPARE(episode->similar()->count(), 0);
    }

    void untrustedFieldsAreCappedAndTyped()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray people;
        for (int i = 0; i < 150; ++i)
            people.append(QJsonObject{{QStringLiteral("Name"), QStringLiteral("P%1").arg(i)},
                                      {QStringLiteral("Type"), QStringLiteral("Actor")}});
        QJsonArray chapters;
        for (int i = 0; i < 300; ++i)
            chapters.append(
                QJsonObject{{QStringLiteral("Name"), QStringLiteral("C%1").arg(i)},
                            {QStringLiteral("StartPositionTicks"), qint64(i) * 10'000'000}});
        QJsonArray streams;
        for (int i = 0; i < 100; ++i)
            streams.append(QJsonObject{{QStringLiteral("Type"), QStringLiteral("Audio")}});
        QJsonArray sources;
        for (int i = 0; i < 12; ++i)
            sources.append(QJsonObject{{QStringLiteral("Id"), QStringLiteral("s%1").arg(i)},
                                       {QStringLiteral("Path"), QStringLiteral("/secret/path.mkv")},
                                       {QStringLiteral("MediaStreams"), streams}});
        QJsonArray genres;
        for (int i = 0; i < 40; ++i)
            genres.append(QStringLiteral("G%1").arg(i));
        const QJsonObject hostile{{QStringLiteral("Id"), kMovie},
                                  {QStringLiteral("Name"), QString(1000, QLatin1Char('n'))},
                                  {QStringLiteral("Overview"), QString(20000, QLatin1Char('o'))},
                                  {QStringLiteral("ProductionYear"), QStringLiteral("2012")},
                                  {QStringLiteral("RunTimeTicks"), QStringLiteral("long")},
                                  {QStringLiteral("CommunityRating"), 99},
                                  {QStringLiteral("Genres"), genres},
                                  {QStringLiteral("People"), people},
                                  {QStringLiteral("Chapters"), chapters},
                                  {QStringLiteral("MediaSources"), sources},
                                  {QStringLiteral("UserData"), QStringLiteral("not an object")}};
        f.server.itemsById.insert(kMovie, QJsonDocument(hostile).toJson(QJsonDocument::Compact));
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QCOMPARE(details->name().size(), 512);
        QCOMPARE(details->overview().size(), 16384);
        QCOMPARE(details->year(), 0);
        QCOMPARE(details->runtimeMinutes(), -1);
        QCOMPARE(details->communityRating(), -1.0);
        QCOMPARE(details->genres().size(), 20);
        QCOMPARE(details->people().size(), 100);
        QCOMPARE(details->chapters().size(), 200);
        QCOMPARE(details->mediaSources().size(), 8);
        QCOMPARE(details->mediaSources()
                     .constFirst()
                     .toMap()
                     .value(QStringLiteral("audio"))
                     .toList()
                     .size(),
                 64);
        QVERIFY(!details->canResume());
    }

    void attachmentStreamsDoNotCountAgainstTheCap()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray streams;
        for (int i = 0; i < 80; ++i)
            streams.append(QJsonObject{{QStringLiteral("Type"), QStringLiteral("Attachment")}});
        streams.append(QJsonObject{{QStringLiteral("Type"), QStringLiteral("Audio")},
                                   {QStringLiteral("Codec"), QStringLiteral("flac")}});
        const QJsonObject item{
            {QStringLiteral("Id"), kMovie},
            {QStringLiteral("Overview"),
             QStringLiteral("Line one<br>Line two<BR/>Line three<br />end <b>kept</b>")},
            {QStringLiteral("MediaSources"),
             QJsonArray{QJsonObject{{QStringLiteral("Id"), QStringLiteral("s")},
                                    {QStringLiteral("MediaStreams"), streams}}}}};
        f.server.itemsById.insert(kMovie, QJsonDocument(item).toJson(QJsonDocument::Compact));
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        const QVariantMap source = details->mediaSources().constFirst().toMap();
        // The real track survives the 80 fonts before it
        QCOMPARE(source.value(QStringLiteral("audio")).toList().size(), 1);
        // Only <br> becomes a line break, other markup stays literal text
        QCOMPARE(details->overview(),
                 QStringLiteral("Line one\nLine two\nLine three\nend <b>kept</b>"));
    }

    void aDashedIdMatchesItsDashlessAnswer()
    {
        Fixture f;
        QVERIFY(f.ok);
        // The same GUID in the dashed form is a valid request id; the server answers dash-less
        const QString dashed = QStringLiteral("f0e1d2c3-b4a5-9687-7869-5a4b3c2d1e0f");
        f.server.itemsById.insert(dashed, fixtureItem(QStringLiteral("movie_item")));
        auto details = f.open(dashed);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
    }

    void aSeasonShowsNoEpisodeNumber()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString season = QStringLiteral("11aa22bb33cc44dd55ee66ff778899aa");
        f.server.itemsById.insert(season, fixtureItem(QStringLiteral("season_item")));
        f.server.generatedEpisodeTotal = 3;
        auto details = f.open(season);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        // A season's own number is a season number
        QCOMPARE(details->seasonNumber(), 2);
        QCOMPARE(details->episodeNumber(), -1);
        QCOMPARE(details->seriesId(), QStringLiteral("aa11bb22cc33dd44ee55ff6677889900"));
        QTRY_COMPARE(details->below()->episodes()->status(), Kinefin::PagedItemsModel::Ready);
        QCOMPARE(details->below()->episodes()->totalCount(), 3);
    }

    void aSeasonSkipsTheSimilarRequest()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString season = QStringLiteral("11aa22bb33cc44dd55ee66ff778899aa");
        f.server.itemsById.insert(
            season, QJsonDocument(QJsonObject{{QStringLiteral("Id"), season},
                                              {QStringLiteral("Type"), QStringLiteral("Season")},
                                              {QStringLiteral("Name"), QStringLiteral("Season 2")}})
                        .toJson(QJsonDocument::Compact));
        auto details = f.open(season);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QTest::qWait(200);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Similar")).size(), 0);
        QCOMPARE(details->similarStatus(), ItemDetailsModel::Idle);
        QVERIFY(!details->playable());
    }

    void aDestroyedModelReleasesItsChannelAndItsRequest()
    {
        Fixture f;
        QVERIFY(f.ok);
        // A held request that is answered anyway ends in a failure signal: it stays silent only when the model released its channel
        QSignalSpy failures(&f.client, &Kinefin::JellyfinClient::listingFailed);
        f.server.holdListings = true;
        {
            auto details = f.open(kMovie, ItemDetailsModel::FirstChannel + 6);
            QTRY_COMPARE(f.server.itemsRoutes.size(), 1);
        }
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTest::qWait(250);
        QCOMPARE(failures.size(), 0);
        // And the channel can be used again by another model
        auto again = f.open(kMovie, ItemDetailsModel::FirstChannel + 6);
        QTRY_COMPARE(again->status(), ItemDetailsModel::Ready);
    }

    void aLostSessionClearsEveryOpenModel()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto movie = f.open(kMovie, ItemDetailsModel::FirstChannel);
        auto episode =
            f.open(kEpisode, ItemDetailsModel::FirstChannel + ItemDetailsModel::ChannelsPerModel);
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Ready);
        QTRY_COMPARE(episode->status(), ItemDetailsModel::Ready);
        f.server.denyListings = true;
        movie->reload();
        episode->reload();
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Idle);
        QTRY_COMPARE(episode->status(), ItemDetailsModel::Idle);
        QVERIFY(movie->name().isEmpty() && episode->name().isEmpty());
    }

    void anEmptyAnswerIsNotFound()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.itemsById.clear();
        f.server.listingRawBody.insert(QStringLiteral("Items"), envelope({}));
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::NotFound);
        QVERIFY(details->name().isEmpty());
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Similar")).size(), 0);
    }

    void aMismatchedIdFails()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.itemsById.clear();
        f.server.listingRawBody.insert(
            QStringLiteral("Items"),
            envelope(QJsonArray{
                QJsonDocument::fromJson(fixtureItem(QStringLiteral("episode_item"))).object()}));
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Failed);
        QVERIFY(details->name().isEmpty());
    }

    void failureThenReloadRecovers()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Failed);
        f.server.listingStatus.clear();
        details->reload();
        QCOMPARE(details->status(), ItemDetailsModel::Loading);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        QCOMPARE(details->name(), QStringLiteral("Arcturus (2012)"));
    }

    void aFailedSimilarRowIsRetryableAndLeavesTheDetails()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Similar"), 500);
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->similarStatus(), ItemDetailsModel::Failed);
        QCOMPARE(details->status(), ItemDetailsModel::Ready);
        QVERIFY(!details->name().isEmpty());
        f.server.listingStatus.clear();
        details->retrySimilar();
        QCOMPARE(details->similarStatus(), ItemDetailsModel::Loading);
        QTRY_COMPARE(details->similarStatus(), ItemDetailsModel::Ready);
        QVERIFY(details->similar()->count() > 0);
    }

    void reloadKeepsTheOldDataUntilTheAnswer()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto details = f.open(kMovie);
        QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
        f.server.holdListings = true;
        details->reload();
        QCOMPARE(details->status(), ItemDetailsModel::Loading);
        QTRY_COMPARE(f.server.itemsRoutes.size(), 2);
        QCOMPARE(details->name(), QStringLiteral("Arcturus (2012)"));
        // A reload that fails keeps showing nothing new, and one that answers replaces the data
        f.server.holdListings = false;
        f.server.itemsById.insert(
            kMovie, QJsonDocument(QJsonObject{{QStringLiteral("Id"), kMovie},
                                              {QStringLiteral("Type"), QStringLiteral("Movie")},
                                              {QStringLiteral("Name"), QStringLiteral("Renamed")}})
                        .toJson(QJsonDocument::Compact));
        details->reload();
        QTRY_COMPARE(details->name(), QStringLiteral("Renamed"));
        QCOMPARE(details->status(), ItemDetailsModel::Ready);
    }

    void twoModelsDoNotDisturbEachOther()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto movie = f.open(kMovie, ItemDetailsModel::FirstChannel);
        auto episode =
            f.open(kEpisode, ItemDetailsModel::FirstChannel + ItemDetailsModel::ChannelsPerModel);
        QTRY_COMPARE(movie->status(), ItemDetailsModel::Ready);
        QTRY_COMPARE(episode->status(), ItemDetailsModel::Ready);
        QCOMPARE(movie->name(), QStringLiteral("Arcturus (2012)"));
        QCOMPARE(episode->name(), QStringLiteral("Weightless"));
        // The first model is not touched by the second one's reload
        episode->reload();
        QTRY_COMPARE(episode->status(), ItemDetailsModel::Ready);
        QCOMPARE(movie->status(), ItemDetailsModel::Ready);
        QCOMPARE(movie->name(), QStringLiteral("Arcturus (2012)"));
    }

    void aLostSessionClearsEverything()
    {
        {
            Fixture f;
            QVERIFY(f.ok);
            auto details = f.open(kMovie);
            QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
            QTRY_COMPARE(details->similarStatus(), ItemDetailsModel::Ready);
            f.client.disconnectFromServer();
            QCOMPARE(details->status(), ItemDetailsModel::Idle);
            QVERIFY(details->name().isEmpty());
            QCOMPARE(details->similar()->count(), 0);
            QCOMPARE(details->similarStatus(), ItemDetailsModel::Idle);
        }
        {
            // A 401 on a reload expires the session
            Fixture f;
            QVERIFY(f.ok);
            auto details = f.open(kMovie);
            QTRY_COMPARE(details->status(), ItemDetailsModel::Ready);
            f.server.denyListings = true;
            details->reload();
            QTRY_COMPARE(details->status(), ItemDetailsModel::Idle);
            QVERIFY(details->name().isEmpty());
        }
    }
};

QTEST_MAIN(TestItemDetailsModel)
#include "test_item_details_model.moc"
