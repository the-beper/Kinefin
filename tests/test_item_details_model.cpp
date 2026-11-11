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
        QVERIFY(route.startsWith(QByteArrayLiteral("/jellyfin/UserPlayedItems/")
                                 + kMovie.toLatin1() + QByteArrayLiteral("?")));
        QCOMPARE(FakeJellyfin::capturedListingQuery(route)
                     .queryItemValue(QStringLiteral("userId")),
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
              QStringLiteral("Taglines"), QStringLiteral("OriginalTitle")})
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
