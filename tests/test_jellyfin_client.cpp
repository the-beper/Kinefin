// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FakeJellyfin.h"
#include "api/DeviceProfile.h"
#include "api/JellyfinClient.h"
#include "models/LibraryFilterState.h"
#include "models/LibraryItemsModel.h"

#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

#include <functional>

namespace {

const QString kAliceId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1");
const QString kBobId = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb2");

// Connects to the fake and signs `name` in (a QTRY_* macro cannot live in a helper that returns a value)
bool loginAs(Kinefin::JellyfinClient &client, const FakeJellyfin &server, const QString &name)
{
    QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
    QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
    if (!prepare(client, server) || !QTest::qWaitFor([&] { return ready.size() == 1; }, 5000))
        return false;
    return client.authenticate(name, QStringLiteral("x"))
           && QTest::qWaitFor([&] { return loggedIn.size() == 1; }, 5000);
}

using PlaylistOp = Kinefin::JellyfinClient::PlaylistOp;

const QString kItemA = QStringLiteral("aaaa0000aaaa0000aaaa0000aaaa0001");
const QString kItemB = QStringLiteral("bbbb0000bbbb0000bbbb0000bbbb0002");
const QString kItemC = QStringLiteral("cccc0000cccc0000cccc0000cccc0003");
const QString kDashedItem = QStringLiteral("11111111-2222-3333-4444-555555555555");

// `count` distinct valid ids
QStringList manyIds(int count)
{
    QStringList ids;
    for (int i = 1; i <= count; ++i)
        ids.append(QString::number(i, 16).rightJustified(32, QLatin1Char('d')));
    return ids;
}

// The PlaylistItemId of every item of a page, in order
QStringList entryIdsOf(const QJsonArray &items)
{
    QStringList ids;
    for (const QJsonValue &item : items)
        ids.append(item.toObject().value(QStringLiteral("PlaylistItemId")).toString());
    return ids;
}

// The item id of one row of a page
QString idAt(const QJsonArray &items, int row)
{
    return items.at(row).toObject().value(QStringLiteral("Id")).toString();
}

// Whether every recorded playlist request carries the session token in its header and nowhere else (not in the request line, not in the body)
bool tokenOnlyInTheHeader(const FakeJellyfin &server)
{
    for (const auto &request : server.playlistRequests) {
        if (!request.authorization.contains(kTokenField) || request.route.contains(kFixtureToken)
            || request.body.contains(kFixtureToken))
            return false;
    }
    return !server.playlistRequests.isEmpty();
}

// The DELETE /Items/{id} requests the fake saw
int deleteRequests(const FakeJellyfin &server)
{
    int count = 0;
    for (const auto &request : server.playlistRequests)
        count += request.method == "DELETE" && request.route.contains("/Items/")
                 && !request.route.contains("/Playlists/");
    return count;
}

// One code point as a string
// Built by hand: QString::fromUcs4 drops a leading U+FEFF as a byte order mark
QString codePoint(char32_t cp)
{
    if (QChar::requiresSurrogates(cp)) {
        const QChar pair[2]{QChar(QChar::highSurrogate(cp)), QChar(QChar::lowSurrogate(cp))};
        return QString(pair, 2);
    }
    return QString(QChar(char16_t(cp)));
}

// `count` copies of one code point
QString repeated(char32_t cp, int count)
{
    QString result;
    for (int i = 0; i < count; ++i)
        result += codePoint(cp);
    return result;
}

QJsonObject publicEntry(const QString &id, const QString &name, const QString &tag = {})
{
    QJsonObject entry{{QStringLiteral("Id"), id}, {QStringLiteral("Name"), name}};
    if (!tag.isEmpty())
        entry.insert(QStringLiteral("PrimaryImageTag"), tag);
    return entry;
}

} // namespace

class TestJellyfinClient : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void bootstrapLoginAndViews()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QSignalSpy views(&client, &Kinefin::JellyfinClient::userViewsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(client.serverId(), QStringLiteral("9c076c52ba5340d59f6baf5b57cf0a5e"));
        QCOMPARE(client.serverUrl(), server.url(QStringLiteral("/jellyfin")));
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(client.isAuthenticated());
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(views.size(), 1);
        const QList<QVariant> result = views.takeFirst();
        QCOMPARE(result.at(0).value<QJsonArray>().size(), 3);
        QCOMPARE(result.at(1).toInt(), 3);
        QCOMPARE(server.viewRequests, 1);
        QVERIFY(!server.lastLoginAuth.contains("Token="));
        QVERIFY(server.lastViewsAuth.contains("Token="));
        // The libraries hidden from Home are libraries too: 12.4 leaves them out without the flag
        QVERIFY(server.lastViewsRoute.contains("includeHidden=true"));
        QCOMPARE(failures.size(), 0);
        client.disconnectFromServer();
        QVERIFY(!client.isAuthenticated());
        QVERIFY(client.userId().isEmpty());
    }

    void mediaSegmentsFilterInvalidAndUnsupportedServers()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.mediaSegmentsBody =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("Items"),
                     QJsonArray{QJsonObject{{QStringLiteral("Type"), QStringLiteral("Intro")},
                                            {QStringLiteral("StartTicks"), qint64(10'000'000)},
                                            {QStringLiteral("EndTicks"), qint64(30'000'000)}},
                                QJsonObject{{QStringLiteral("Type"), QStringLiteral("Recap")},
                                            {QStringLiteral("StartTicks"), qint64(-1)},
                                            {QStringLiteral("EndTicks"), qint64(30'000'000)}},
                                QJsonObject{{QStringLiteral("Type"), QStringLiteral("Invalid")},
                                            {QStringLiteral("StartTicks"), qint64(0)},
                                            {QStringLiteral("EndTicks"), qint64(30'000'000)}}}}})
                .toJson();
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy segments(&client, &Kinefin::JellyfinClient::mediaSegmentsReady);
        QVERIFY(client.fetchMediaSegments(kPlayItem));
        QTRY_COMPARE(segments.size(), 1);
        const auto rows = segments.takeFirst().at(1).toJsonArray();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toObject().value(QStringLiteral("type")).toString(),
                 QStringLiteral("Intro"));
        QVERIFY(server.mediaSegmentsAuth.contains(kTokenField));
        server.mediaSegmentsStatus = 404;
        QVERIFY(client.fetchMediaSegments(kPlayItem));
        QTRY_COMPARE(server.mediaSegmentsRequests, 2);
        QCOMPARE(segments.size(), 0);
        QVERIFY(!client.fetchMediaSegments(QStringLiteral("a/b")));
    }

    void refreshUserDataReportsWhatTheServerHasAndSkipsAPendingWrite()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const auto row = [](const QString &id, bool played, qint64 position, bool favorite) {
            return QJsonDocument(
                       QJsonObject{{QStringLiteral("Id"), id},
                                   {QStringLiteral("Name"), QStringLiteral("Item")},
                                   {QStringLiteral("Type"), QStringLiteral("Movie")},
                                   {QStringLiteral("UserData"),
                                    QJsonObject{{QStringLiteral("Played"), played},
                                                {QStringLiteral("PlaybackPositionTicks"), position},
                                                {QStringLiteral("IsFavorite"), favorite}}}})
                .toJson(QJsonDocument::Compact);
        };
        server.itemsById.insert(kItemA, row(kItemA, false, 3'000'000'000, true));
        server.itemsById.insert(kItemB, row(kItemB, true, 0, false));
        server.itemsById.insert(kItemC, row(kItemC, true, 0, false));
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy favorite(&client, &Kinefin::JellyfinClient::itemFavoriteUpdated);
        QSignalSpy played(&client, &Kinefin::JellyfinClient::itemPlayedUpdated);
        QSignalSpy progress(&client, &Kinefin::JellyfinClient::itemProgressUpdated);

        // An id that is not one, a repeat (another spelling too) and an id with a write pending are left out
        server.holdPlayedWrites = true;
        QVERIFY(client.setItemPlayed(kItemC, true));
        QVERIFY(client.refreshUserData(
            {kItemA, kItemB, kItemA.toUpper(), kItemC, QStringLiteral("../x")}));
        QTRY_COMPARE(played.size(), 2);
        QCOMPARE(played.at(0).at(0).toString(), kItemA);
        QCOMPARE(played.at(0).at(1).toBool(), false);
        QCOMPARE(played.at(1).at(0).toString(), kItemB);
        QCOMPARE(played.at(1).at(1).toBool(), true);
        // A saved position follows the played flag, and only an item that has one
        QCOMPARE(progress.size(), 1);
        QCOMPARE(progress.constFirst().at(0).toString(), kItemA);
        QCOMPARE(progress.constFirst().at(1).toLongLong(), qint64(3'000'000'000));
        QCOMPARE(favorite.size(), 2);
        QCOMPARE(favorite.at(0).at(1).toBool(), true);
        QCOMPARE(favorite.at(1).at(1).toBool(), false);
        const QString route = QString::fromUtf8(server.itemsRoutes.constLast());
        QVERIFY(route.contains(kItemA));
        QVERIFY(route.contains(kItemB));
        QVERIFY(!route.contains(kItemC));
        QVERIFY(!route.contains(QStringLiteral("..")));
        server.holdPlayedWrites = false;
        server.releaseHeld();

        // Nothing to ask: no request
        const int asked = server.itemsRoutes.size();
        QVERIFY(!client.refreshUserData({}));
        QVERIFY(!client.refreshUserData({QStringLiteral("../bad"), QString()}));
        QCOMPARE(server.itemsRoutes.size(), asked);
    }

    // The server names an episode together with its season and not the series: the series is read as well, once
    void refreshUserDataAlsoReadsTheSeriesOfAnEpisodeOnce()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QString series = kItemB;
        const auto row = [&series](const QString &id, const QString &type, bool played) {
            QJsonObject item{
                {QStringLiteral("Id"), id},
                {QStringLiteral("Type"), type},
                {QStringLiteral("UserData"), QJsonObject{{QStringLiteral("Played"), played},
                                                         {QStringLiteral("IsFavorite"), false}}}};
            if (type != QLatin1String("Series"))
                item.insert(QStringLiteral("SeriesId"), series);
            return QJsonDocument(item).toJson(QJsonDocument::Compact);
        };
        server.itemsById.insert(kItemA, row(kItemA, QStringLiteral("Episode"), true));
        server.itemsById.insert(kItemC, row(kItemC, QStringLiteral("Season"), false));
        server.itemsById.insert(series, row(series, QStringLiteral("Series"), false));
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy played(&client, &Kinefin::JellyfinClient::itemPlayedUpdated);
        QVERIFY(client.refreshUserData({kItemA, kItemC}));
        QTRY_COMPARE(played.size(), 3);
        QCOMPARE(played.at(0).at(0).toString(), kItemA);
        QCOMPARE(played.at(1).at(0).toString(), kItemC);
        // The episode and the season share one series: one more request, for it
        QCOMPARE(played.at(2).at(0).toString(), series);
        QCOMPARE(server.itemsRoutes.size(), 2);
        QVERIFY(server.itemsRoutes.constLast().contains(series.toUtf8()));
        // A series row names no series: nothing more is asked
        QTest::qWait(200);
        QCOMPARE(server.itemsRoutes.size(), 2);
        // A series that the answer already holds is not asked for again
        played.clear();
        QVERIFY(client.refreshUserData({kItemA, series}));
        QTRY_COMPARE(played.size(), 2);
        QCOMPARE(server.itemsRoutes.size(), 3);
    }

    // A series or a season says how many episodes are left, in a refresh and in the answer of a write
    void theUnplayedCountOfASeriesComesWithItsUserData()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const auto row = [](const QString &id, double unplayed) {
            return QJsonDocument(
                       QJsonObject{{QStringLiteral("Id"), id},
                                   {QStringLiteral("Type"), QStringLiteral("Series")},
                                   {QStringLiteral("UserData"),
                                    QJsonObject{{QStringLiteral("Played"), false},
                                                {QStringLiteral("UnplayedItemCount"), unplayed}}}})
                .toJson(QJsonDocument::Compact);
        };
        server.itemsById.insert(kItemA, row(kItemA, 7));
        server.itemsById.insert(kItemB, row(kItemB, -3));
        server.itemsById.insert(kItemC, row(kItemC, 1'000'001));
        server.itemsById.insert(kDashedItem, row(kDashedItem, 2.5));
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy counts(&client, &Kinefin::JellyfinClient::itemUnplayedCountUpdated);
        QVERIFY(client.refreshUserData({kItemA, kItemB, kItemC, kDashedItem}));
        QTRY_COMPARE(counts.size(), 1);
        // A negative, a too large and a fractional count are not counts
        QTest::qWait(200);
        QCOMPARE(counts.size(), 1);
        QCOMPARE(counts.constFirst().at(0).toString(), kItemA);
        QCOMPARE(counts.constFirst().at(1).toInt(), 7);

        // The answer of a watched write carries it too
        server.playedWriteRawBody = QByteArrayLiteral("{\"Played\":true,\"UnplayedItemCount\":0}");
        QVERIFY(client.setItemPlayed(kItemC, true));
        QTRY_COMPARE(counts.size(), 2);
        QCOMPARE(counts.constLast().at(0).toString(), kItemC);
        QCOMPARE(counts.constLast().at(1).toInt(), 0);
    }

    // An expired session ends the session, as every other request does
    void refreshUserDataEndsTheSessionOnA401()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy cleared(&client, &Kinefin::JellyfinClient::credentialsCleared);
        QSignalSpy played(&client, &Kinefin::JellyfinClient::itemPlayedUpdated);
        server.denyListings = true;
        QVERIFY(client.refreshUserData({kItemA}));
        QTRY_COMPARE(cleared.size(), 1);
        QVERIFY(!client.isAuthenticated());
        QCOMPARE(played.size(), 0);
    }

    void refreshUserDataAsksInChunksAndAFailureEmitsNothing()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy played(&client, &Kinefin::JellyfinClient::itemPlayedUpdated);
        // 250 ids make three requests, which the server does not know: nothing comes back
        server.itemsById.insert(kItemA, "{}");
        const QStringList ids = manyIds(250);
        server.itemsById.insert(
            ids.first(),
            QJsonDocument(QJsonObject{{QStringLiteral("Id"), ids.first()},
                                      {QStringLiteral("Type"), QStringLiteral("Movie")},
                                      {QStringLiteral("UserData"),
                                       QJsonObject{{QStringLiteral("Played"), true}}}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(client.refreshUserData(ids));
        QTRY_COMPARE(server.itemsRoutes.size(), 3);
        QTRY_COMPARE(played.size(), 1);
        QCOMPARE(played.constFirst().at(0).toString(), ids.first());
        for (const QByteArray &route : std::as_const(server.itemsRoutes))
            QVERIFY(route.count(',') <= Kinefin::JellyfinClient::UserDataChunk - 1);
        // A failed answer reports nothing
        server.listingStatus.insert(QStringLiteral("Items"), 500);
        const int before = played.size();
        QVERIFY(client.refreshUserData({kItemA}));
        QTest::qWait(300);
        QCOMPARE(played.size(), before);
    }

    void subtitleDownloadRequiresSameOriginAndNeverFollowsRedirect()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QUrl allowed = server.url(
            QStringLiteral("/jellyfin/Videos/%1/Subtitles/3/0/Stream.srt").arg(kPlayItem));
        QSignalSpy downloaded(&client, &Kinefin::JellyfinClient::externalSubtitleDownloaded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::externalSubtitleFailed);
        QVERIFY(client.downloadExternalSubtitle(allowed));
        QTRY_COMPARE(downloaded.size(), 1);
        QCOMPARE(downloaded.takeFirst().at(1).toByteArray(), server.subtitleDownloadBody);
        QVERIFY(server.subtitleDownloadAuth.contains(kTokenField));
        const int count = server.subtitleDownloadRequests;
        QVERIFY(!client.downloadExternalSubtitle(
            QUrl(QStringLiteral("https://evil.invalid/Videos/id/Subtitles/0/Stream.srt"))));
        QVERIFY(!client.downloadExternalSubtitle(server.url(QStringLiteral("/jellyfin/Users/Me"))));
        QCOMPARE(server.subtitleDownloadRequests, count);
        server.redirectSubtitleDownload = true;
        QVERIFY(client.downloadExternalSubtitle(allowed));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(downloaded.size(), 0);
        QCOMPARE(server.subtitleDownloadRequests, count + 1);
    }

    void remoteSearchRejectsAnOversizedReply()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        // One byte over the response cap: the client must refuse before parsing it
        server.remoteSearchResults = QByteArray(1'048'577, ' ');
        server.remoteSearchResults[0] = '[';
        server.remoteSearchResults[1'048'576] = ']';
        QSignalSpy finished(&client, &Kinefin::JellyfinClient::remoteSearchFinished);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::remoteSearchFailed);
        QVERIFY(client.remoteSearch(kPlayItem, QStringLiteral("Movie"), QStringLiteral("Arcturus"),
                                    2012));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(finished.size(), 0);
    }

    void newWritesReportRefusals()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QTemporaryDir dir;
        const QString image = dir.filePath(QStringLiteral("poster.png"));
        QFile imageFile(image);
        QVERIFY(imageFile.open(QIODevice::WriteOnly));
        imageFile.write(QByteArray::fromHex("89504e470d0a1a0a00000000"));
        imageFile.close();
        QSignalSpy imageFailed(&client, &Kinefin::JellyfinClient::itemImageUpdateFailed);
        server.imageWriteStatus = 403;
        QVERIFY(client.uploadImage(kPlayItem, QStringLiteral("Primary"), -1,
                                   QUrl::fromLocalFile(image)));
        QTRY_COMPARE(imageFailed.size(), 1);
        QCOMPARE(imageFailed.constFirst().at(3).toInt(), 403);

        const QString subtitle = dir.filePath(QStringLiteral("track.srt"));
        QFile subtitleFile(subtitle);
        QVERIFY(subtitleFile.open(QIODevice::WriteOnly));
        subtitleFile.write("1\n00:00:01,000 --> 00:00:02,000\nHello\n");
        subtitleFile.close();
        QSignalSpy subtitleFailed(&client, &Kinefin::JellyfinClient::itemSubtitleUpdateFailed);
        server.subtitleWriteStatus = 403;
        QVERIFY(client.uploadSubtitle(kPlayItem, QUrl::fromLocalFile(subtitle),
                                      QStringLiteral("ita"), false));
        QTRY_COMPARE(subtitleFailed.size(), 1);
        QCOMPARE(subtitleFailed.constFirst().at(1).toInt(), 403);

        QSignalSpy refreshed(&client, &Kinefin::JellyfinClient::itemRefreshFinished);
        server.refreshStatus = 403;
        QVERIFY(client.refreshItem(kPlayItem));
        QTRY_COMPARE(refreshed.size(), 1);
        QCOMPARE(refreshed.constFirst().at(1).toBool(), false);

        QSignalSpy updateFailed(&client, &Kinefin::JellyfinClient::itemUpdateFailed);
        server.itemUpdateStatus = 403;
        QVERIFY(client.updateItem(
            kPlayItem, QVariantMap{{QStringLiteral("Name"), QStringLiteral("Refused")}}));
        QTRY_COMPARE(updateFailed.size(), 1);
        QCOMPARE(updateFailed.constFirst().at(1).toInt(), 403);
    }

    void remoteSearchTreatsNoContentAsNoMatches()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        server.remoteSearchStatus = 204;
        QSignalSpy finished(&client, &Kinefin::JellyfinClient::remoteSearchFinished);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::remoteSearchFailed);
        QVERIFY(client.remoteSearch(kPlayItem, QStringLiteral("Movie"), QStringLiteral("Arcturus"),
                                    2012));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.constFirst().at(1).toList().size(), 0);
        QCOMPARE(failed.size(), 0);
    }

    void remoteSearchSkipsEntriesThatAreNotObjects()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QByteArray entries =
            server.remoteSearchResults.mid(1, server.remoteSearchResults.size() - 2);
        server.remoteSearchResults = QByteArrayLiteral("[null,") + entries + QByteArrayLiteral("]");
        QSignalSpy finished(&client, &Kinefin::JellyfinClient::remoteSearchFinished);
        QVERIFY(client.remoteSearch(kPlayItem, QStringLiteral("Movie"), QStringLiteral("Arcturus"),
                                    2012));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.constFirst().at(1).toList().size(), 1);
    }

    void providerArtworkUrlsAreNarrowlyAllowed()
    {
        const QList<QUrl> allowed = {
            QUrl(QStringLiteral("https://image.tmdb.org/t/p/w300/x.jpg")),
            QUrl(QStringLiteral("https://artwork.thetvdb.com/x.jpg")),
            QUrl(QStringLiteral("https://img.omdbapi.com/?apikey=x&i=tt1")),
            QUrl(QStringLiteral("https://assets.fanart.tv/x.jpg")),
            QUrl(QStringLiteral("https://coverartarchive.org/x.jpg")),
        };
        for (const QUrl &url : allowed)
            QVERIFY2(Kinefin::JellyfinClient::isProviderImageUrl(url), qPrintable(url.toString()));
        const QList<QUrl> refused = {
            QUrl(QStringLiteral("http://image.tmdb.org/x.jpg")),
            QUrl(QStringLiteral("https://evil.invalid/x.jpg")),
            QUrl(QStringLiteral("https://image.tmdb.org.evil.invalid/x.jpg")),
            QUrl(QStringLiteral("https://user:pass@image.tmdb.org/x.jpg")),
            QUrl(QStringLiteral("https://image.tmdb.org:8443/x.jpg")),
            QUrl(QStringLiteral("https://image.tmdb.org/x.jpg#frag")),
        };
        for (const QUrl &url : refused)
            QVERIFY2(!Kinefin::JellyfinClient::isProviderImageUrl(url), qPrintable(url.toString()));
    }

    void remoteSearchKeepsOnlyProviderArtwork()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        server.remoteSearchResults = QByteArrayLiteral(
            "[{\"Name\":\"Good\",\"ProductionYear\":2012,\"SearchProviderName\":\"TMDB\","
            "\"ProviderIds\":{\"Tmdb\":\"1\"},"
            "\"ImageUrl\":\"https://image.tmdb.org/t/p/w300/x.jpg\"},"
            "{\"Name\":\"Bad\",\"ProductionYear\":2012,\"SearchProviderName\":\"TMDB\","
            "\"ProviderIds\":{\"Tmdb\":\"2\"},"
            "\"ImageUrl\":\"https://evil.invalid/x.jpg\"}]");
        QSignalSpy finished(&client, &Kinefin::JellyfinClient::remoteSearchFinished);
        QVERIFY(client.remoteSearch(kPlayItem, QStringLiteral("Movie"), QStringLiteral("Arcturus"),
                                    2012));
        QTRY_COMPARE(finished.size(), 1);
        const QVariantList results = finished.constFirst().at(1).toList();
        QCOMPARE(results.size(), 2);
        QVERIFY(results.at(0).toMap().value(QStringLiteral("imageUrl")).toUrl().isValid());
        QCOMPARE(results.at(0).toMap().value(QStringLiteral("imageUrl")).toUrl().host(),
                 QStringLiteral("image.tmdb.org"));
        QVERIFY(!results.at(1).toMap().value(QStringLiteral("imageUrl")).toUrl().isValid());
    }

    void trickplayMetadataAndTileAreAuthenticated()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QJsonObject thumbnail{
            {QStringLiteral("Width"), 320},         {QStringLiteral("Height"), 180},
            {QStringLiteral("TileWidth"), 5},       {QStringLiteral("TileHeight"), 4},
            {QStringLiteral("ThumbnailCount"), 40}, {QStringLiteral("Interval"), 10000}};
        const QJsonObject widths{{QStringLiteral("320"), thumbnail}};
        const QJsonObject indexed{{kPlayItem, widths}};
        server.trickplayMetadataBody =
            QJsonDocument(QJsonObject{{QStringLiteral("Trickplay"), indexed}}).toJson();
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy info(&client, &Kinefin::JellyfinClient::playbackItemReady);
        QSignalSpy tile(&client, &Kinefin::JellyfinClient::trickplayTileReady);
        QVERIFY(client.fetchPlaybackItem(kPlayItem));
        QTRY_COMPARE(info.size(), 1);
        QVERIFY(info.takeFirst().at(1).toJsonObject().contains(QStringLiteral("Trickplay")));
        QVERIFY(client.fetchTrickplayTile(kPlayItem, kPlayItem, 320, 2));
        QTRY_COMPARE(tile.size(), 1);
        QCOMPARE(tile.takeFirst().at(3).toByteArray(), server.trickplayTileBody);
        QVERIFY(server.trickplayTileAuth.contains(kTokenField));
        QVERIFY(!client.fetchTrickplayTile(kPlayItem, QStringLiteral("id?bad"), 320, 2));
        QVERIFY(!client.fetchTrickplayTile(kPlayItem, kPlayItem, 5000, 2));
        QCOMPARE(server.trickplayTileRequests, 1);
    }

    void rejectsUnsafeAddressesAndIdentityHeaders()
    {
        Kinefin::JellyfinClient client;
        QVERIFY(!client.connectToServer(QUrl(QStringLiteral("file:///etc/passwd"))));
        QVERIFY(!client.connectToServer(QUrl(QStringLiteral("http://user:pass@example.com"))));
        QVERIFY(!client.connectToServer(QUrl(QStringLiteral("https://example.com/?secret=123"))));
        QVERIFY(!client.setDeviceIdentity(QStringLiteral("bad\r\nInjected: x"),
                                          QStringLiteral("id"), QStringLiteral("0.0.1")));
        QVERIFY(!client.setDeviceIdentity(QStringLiteral("bad\""), QStringLiteral("id"),
                                          QStringLiteral("0.0.1")));
    }

    void followsSafeSameHostPublicRedirect()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.relocatePublicOnSameHost = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(client.serverUrl(), server.url(QStringLiteral("/jellyfin-proxy")));
    }

    void rejectsPublicRedirectToDifferentHost()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.redirectPublicOffHost = true;
        Kinefin::JellyfinClient client;
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(ready.size(), 0);
        QVERIFY(client.serverUrl().isEmpty());
    }

    void rejectsInvalidServerIdentity()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.invalidIdentity = true;
        Kinefin::JellyfinClient client;
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(failures.size(), 1);
        QVERIFY(client.serverId().isEmpty());
    }

    void failedLoginDoesNotAuthenticate()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.denyLogin = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"), QStringLiteral("wrong")));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.takeFirst().at(0).toInt(), 401);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(!client.fetchUserViews());
    }

    void unauthorizedLibraryResponseInvalidatesSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.denyViews = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(expired.size(), 1);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(client.userId().isEmpty());
        QVERIFY(!client.fetchUserViews());
    }

    void authRequestNeverFollowsRedirect()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.redirectViewsOffHost = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(server.viewRequests, 1);
        QVERIFY(client.isAuthenticated());
    }

    void quickConnectApprovalAuthorizesTheSameDevice()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy waiting(&client, &Kinefin::JellyfinClient::quickConnectAwaitingApproval);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QSignalSpy views(&client, &Kinefin::JellyfinClient::userViewsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QVERIFY(client.isQuickConnectPending());
        QTRY_COMPARE(code.size(), 1);
        QCOMPARE(code.constFirst().constFirst().toString(), QStringLiteral("AB12CD"));
        QVERIFY(!code.constFirst().constFirst().toString().contains(
            QStringLiteral("fixture-quick-secret")));
        QCOMPARE(server.quickInitiationRequests, 1);
        QVERIFY(!client.authenticate(QStringLiteral("another"), QStringLiteral("password")));
        QVERIFY(!client.fetchUserViews());
        QVERIFY(client.pollQuickConnect());
        QTRY_COMPARE(waiting.size(), 1);
        QCOMPARE(server.quickStatusRequests, 1);
        QCOMPARE(server.lastQuickPollSecret, QStringLiteral("fixture-quick-secret"));
        QVERIFY(server.lastQuickPollAuth.isEmpty());
        server.approveQuickConnect = true;
        QVERIFY(client.pollQuickConnect());
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(client.isAuthenticated());
        QVERIFY(!client.isQuickConnectPending());
        QCOMPARE(server.quickExchangeRequests, 1);
        QCOMPARE(QJsonDocument::fromJson(server.lastQuickExchangeBody)
                     .object()
                     .value(QStringLiteral("Secret"))
                     .toString(),
                 QStringLiteral("fixture-quick-secret"));
        QVERIFY(server.lastQuickExchangeAuth.contains("DeviceId=\"stable-test-id\""));
        QVERIFY(!server.lastQuickExchangeAuth.contains("Token="));
        QVERIFY(!client.pollQuickConnect());
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(views.size(), 1);
        QVERIFY(server.lastViewsAuth.contains("Token="));
        QCOMPARE(failures.size(), 0);
        client.disconnectFromServer();
        QVERIFY(!client.isAuthenticated());
    }

    void quickConnectDisabledCanFallbackToPassword()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.disableQuickConnect = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.takeFirst().at(0).toInt(), 401);
        QVERIFY(!client.isQuickConnectPending());
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
    }

    void malformedQuickConnectCodeFailsClosed()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.invalidQuickConnectCode = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(failures.size(), 1);
        QVERIFY(!client.isQuickConnectPending());
        QCOMPARE(code.size(), 0);
        QCOMPARE(server.quickStatusRequests, 0);
    }

    void mismatchedQuickConnectSecretCancelsPairing()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.mismatchQuickConnectSecret = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(code.size(), 1);
        QVERIFY(client.pollQuickConnect());
        QTRY_COMPARE(failures.size(), 1);
        QVERIFY(!client.isQuickConnectPending());
        QVERIFY(!client.isAuthenticated());
        QCOMPARE(server.quickExchangeRequests, 0);
    }

    void quickConnectExpiredStatusCancelsPairing()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.rejectQuickConnectStatus = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(code.size(), 1);
        QVERIFY(client.pollQuickConnect());
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(0).toInt(), 404);
        QVERIFY(!client.isQuickConnectPending());
        QVERIFY(!client.isAuthenticated());
        QCOMPARE(server.quickExchangeRequests, 0);
    }

    void quickConnectExchange401DoesNotLogIn()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.approveQuickConnect = true;
        server.denyQuickConnectExchange = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(code.size(), 1);
        QVERIFY(client.pollQuickConnect());
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(0).toInt(), 401);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(!client.isQuickConnectPending());
        QCOMPARE(loggedIn.size(), 0);
        QCOMPARE(server.quickExchangeRequests, 1);
    }

    void cancellingPendingQuickConnectSuppressesLateReplies()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdQuickConnectPoll = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(code.size(), 1);
        QVERIFY(client.pollQuickConnect());
        QVERIFY(!client.pollQuickConnect()); // prevent overlapping polls
        QTRY_COMPARE(server.quickStatusRequests, 1);
        client.cancelQuickConnect();
        QVERIFY(!client.isQuickConnectPending());
        QVERIFY(!client.pollQuickConnect());
        QTest::qWait(40);
        QCOMPARE(loggedIn.size(), 0);
        QCOMPARE(failures.size(), 0);
        QVERIFY(client.initiateQuickConnect()); // reconnect is not required
        QTRY_COMPARE(code.size(), 2);
        client.disconnectFromServer();
        QVERIFY(!client.isAuthenticated());
        QVERIFY(!client.isQuickConnectPending());
    }

    void disconnectCancelsOutstandingRequests()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdViews = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QSignalSpy views(&client, &Kinefin::JellyfinClient::userViewsReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(server.viewRequests, 1);
        client.disconnectFromServer();
        QTest::qWait(40);
        QCOMPARE(views.size(), 0);
        QVERIFY(!client.isAuthenticated());
    }

    void playbackInfoDirect()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 1234567890, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 0);

        // Everything is in the body: no query string, UserId, ticks, bitrate and the golden profile
        QCOMPARE(server.lastPlaybackInfoRoute,
                 QByteArray("/jellyfin/Items/" + kPlayItem.toLatin1() + "/PlaybackInfo"));
        QVERIFY(server.lastPlaybackInfoContentType.startsWith("application/json"));
        QVERIFY(server.lastPlaybackInfoAuth.contains(kTokenField));
        const QJsonObject body = QJsonDocument::fromJson(server.lastPlaybackInfoBody).object();
        QCOMPARE(body.value(QStringLiteral("UserId")).toString(), client.userId());
        QCOMPARE(body.value(QStringLiteral("StartTimeTicks")).toVariant().toLongLong(), 1234567890);
        QCOMPARE(body.value(QStringLiteral("MaxStreamingBitrate")).toVariant().toLongLong(),
                 Kinefin::JellyfinClient::DefaultMaxStreamingBitrate);
        QCOMPARE(
            body.value(QStringLiteral("DeviceProfile")).toObject(),
            QJsonDocument::fromJson(fixture(QStringLiteral("device_profile_default"))).object());

        const auto source = ready.takeFirst().at(0).value<Kinefin::PlaybackSource>();
        QCOMPARE(source.method, Kinefin::PlaybackSource::Method::Direct);
        QCOMPARE(source.itemId, kPlayItem);
        QCOMPARE(source.url.path(), QStringLiteral("/jellyfin/Videos/%1/stream").arg(kPlayItem));
        QCOMPARE(source.url.host(), QStringLiteral("127.0.0.1"));
        QVERIFY(!source.url.toString().contains(QString::fromLatin1(kFixtureToken)));
        QVERIFY(client.streamAuthorization().contains(kTokenField));
    }

    // A lowered stream asks for the same source and streams at its cap; out-of-range choices are refused before any request
    void playbackInfoBodyCarriesTheCapAndTheStreams()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        Kinefin::JellyfinClient::StreamChoice streams;
        streams.mediaSourceId = QStringLiteral("c1b2a3d4e5f6071829304a5b6c7d8e9f");
        streams.audioStreamIndex = 2;
        streams.subtitleStreamIndex = -1;
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 50, Kinefin::buildDeviceProfile({}),
                                           10'000'000, streams));
        QTRY_COMPARE(server.playbackInfoRequests, 1);
        const QJsonObject body = QJsonDocument::fromJson(server.lastPlaybackInfoBody).object();
        QCOMPARE(body.value(QStringLiteral("MaxStreamingBitrate")).toVariant().toLongLong(),
                 10'000'000);
        QCOMPARE(body.value(QStringLiteral("MediaSourceId")).toString(), streams.mediaSourceId);
        QCOMPARE(body.value(QStringLiteral("AudioStreamIndex")).toInt(), 2);
        QCOMPARE(body.value(QStringLiteral("SubtitleStreamIndex")).toInt(), -1);
        // The default request carries none of them
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(server.playbackInfoRequests, 2);
        const QJsonObject plain = QJsonDocument::fromJson(server.lastPlaybackInfoBody).object();
        QVERIFY(!plain.contains(QStringLiteral("MediaSourceId")));
        QVERIFY(!plain.contains(QStringLiteral("AudioStreamIndex")));
        QVERIFY(!plain.contains(QStringLiteral("SubtitleStreamIndex")));
        Kinefin::JellyfinClient::StreamChoice bad;
        bad.mediaSourceId = QStringLiteral("../x");
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({}),
                                            10'000'000, bad));
        bad = {};
        bad.audioStreamIndex = -1;
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({}),
                                            10'000'000, bad));
        bad = {};
        bad.subtitleStreamIndex = -2;
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({}),
                                            10'000'000, bad));
    }

    void playbackInfoTranscode()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.playbackInfoFixture = QStringLiteral("playbackinfo_transcode");
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(ready.size(), 1);
        const auto source = ready.takeFirst().at(0).value<Kinefin::PlaybackSource>();
        QCOMPARE(source.method, Kinefin::PlaybackSource::Method::Transcode);
        // The server base path is kept and the ApiKey the fixture embeds is gone
        QVERIFY(source.url.path().startsWith(QStringLiteral("/jellyfin/videos/")));
        QVERIFY(!source.url.toString().contains(QString::fromLatin1(kFixtureToken)));
        QVERIFY(!QUrlQuery(source.url).hasQueryItem(QStringLiteral("ApiKey")));
    }

    void playbackInfo401ExpiresSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.playbackInfoStatus = 401;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(expired.size(), 1);
        QCOMPARE(failed.takeFirst().at(1).toInt(), 401);
        QCOMPARE(ready.size(), 0);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(client.streamAuthorization().isEmpty());
    }

    void playbackInfoFailuresKeepTheSession()
    {
        // 400 is what a 12.4 server answers for an unknown item (observed), 404 is the documented code
        for (const int status : {400, 404, 500, 302}) {
            FakeJellyfin server;
            QVERIFY(server.listen());
            server.playbackInfoStatus = status == 302 ? 0 : status;
            server.redirectPlaybackInfo = status == 302;
            Kinefin::JellyfinClient client;
            QVERIFY(login(client, server));
            QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
            QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
            QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
            QTRY_COMPARE(failed.size(), 1);
            const QList<QVariant> result = failed.takeFirst();
            QCOMPARE(result.at(0).toString(), kPlayItem);
            QCOMPARE(result.at(1).toInt(), status);
            QCOMPARE(result.at(2).toString(), QStringLiteral("Playback request failed"));
            QCOMPARE(expired.size(), 0);
            QVERIFY(client.isAuthenticated());
        }
    }

    void malformedPlaybackInfoFailsClosed()
    {
        const QByteArray oversized = paddedPlaybackInfo(1'100'000);
        const QList<QPair<QByteArray, QString>> cases{
            {"not json", QStringLiteral("Invalid playback response")},
            {"[]", QStringLiteral("Invalid playback response")},
            {oversized, QStringLiteral("Invalid playback response")},
            {"{\"MediaSources\":[]}", QStringLiteral("Invalid playback response")},
            {"{\"MediaSources\":[],\"PlaySessionId\":\"b6c7d8e9f0a1b2c3\"}",
             QStringLiteral("Invalid playback response")},
            {"{\"MediaSources\":[{\"Id\":\"a1\"}],\"PlaySessionId\":\"b6c7d8e9f0a1b2c3\"}",
             QStringLiteral("No playable media source")},
            {"{\"MediaSources\":[],\"PlaySessionId\":\"x\",\"ErrorCode\":\"NotAllowed\"}",
             QStringLiteral("Playback is not allowed")}};
        for (const auto &[body, reason] : cases) {
            FakeJellyfin server;
            QVERIFY(server.listen());
            server.playbackInfoRawBody = body;
            Kinefin::JellyfinClient client;
            QVERIFY(login(client, server));
            QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
            QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
            QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
            QTRY_COMPARE(failed.size(), 1);
            QCOMPARE(failed.takeFirst().at(2).toString(), reason);
            QCOMPARE(ready.size(), 0);
            QVERIFY(client.isAuthenticated());
        }
    }

    void newPlaybackRequestSupersedesTheHeldOne()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdPlaybackInfo = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        QVERIFY(
            client.requestPlaybackInfo(QStringLiteral("aaaa"), 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(server.playbackInfoRequests, 1);
        server.holdPlaybackInfo = false;
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(ready.size(), 1);
        QTest::qWait(200);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(ready.takeFirst().at(0).value<Kinefin::PlaybackSource>().itemId, kPlayItem);
    }

    void cancelAndDisconnectSuppressPlaybackReplies()
    {
        for (const bool disconnect : {false, true}) {
            FakeJellyfin server;
            QVERIFY(server.listen());
            server.holdPlaybackInfo = true;
            Kinefin::JellyfinClient client;
            QVERIFY(login(client, server));
            QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
            QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
            QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
            QTRY_COMPARE(server.playbackInfoRequests, 1);
            if (disconnect) {
                client.disconnectFromServer();
                QVERIFY(client.streamAuthorization().isEmpty());
            } else {
                client.cancelPlaybackInfo();
                QVERIFY(client.isAuthenticated());
            }
            QTest::qWait(200);
            QCOMPARE(ready.size(), 0);
            QCOMPARE(failed.size(), 0);
        }
    }

    void playbackInfoResponseSizeCap()
    {
        for (const bool oversize : {false, true}) {
            FakeJellyfin server;
            QVERIFY(server.listen());
            server.playbackInfoRawBody = paddedPlaybackInfo(oversize ? 1'100'000 : 900'000);
            Kinefin::JellyfinClient client;
            QVERIFY(login(client, server));
            QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
            QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
            QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
            QTRY_COMPARE(ready.size() + failed.size(), 1);
            QCOMPARE(ready.size(), oversize ? 0 : 1);
        }
    }

    // A 401 on another call ends a held negotiation with sessionExpired and nothing else
    void sessionLostElsewhereEndsPendingPlaybackInfoQuietly()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdPlaybackInfo = true;
        server.denyViews = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(server.playbackInfoRequests, 1);
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(expired.size(), 1);
        QTest::qWait(200);
        QCOMPARE(ready.size(), 0);
        QCOMPARE(failed.size(), 0);
        QVERIFY(client.streamAuthorization().isEmpty());
    }

    void playbackInfoMayBeRequestedFromAFailureSlot()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.playbackInfoStatus = 500;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        int retries = 0;
        QObject::connect(&client, &Kinefin::JellyfinClient::playbackInfoFailed, &client, [&] {
            if (retries++ > 0)
                return;
            server.playbackInfoStatus = 0;
            QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        });
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 1);
    }

    void capabilitiesAreSentAfterLogin()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QTRY_COMPARE(server.capabilitiesRequests, 1);
        QCOMPARE(QJsonDocument::fromJson(server.lastCapabilitiesBody).object(),
                 QJsonDocument::fromJson(fixture(QStringLiteral("client_capabilities_request")))
                     .object());
        QVERIFY(server.lastCapabilitiesAuth.contains(kTokenField));
        // Sent again only on request, never retried by itself
        QVERIFY(client.reportCapabilities());
        QTRY_COMPARE(server.capabilitiesRequests, 2);
    }

    void reportsGoOutInOrderWithTheHeaderOnly()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        const QStringList goldens{QStringLiteral("playback_start_request"),
                                  QStringLiteral("playback_progress_request"),
                                  QStringLiteral("playback_stopped_request")};
        const QByteArrayList routes{"/jellyfin/Sessions/Playing",
                                    "/jellyfin/Sessions/Playing/Progress",
                                    "/jellyfin/Sessions/Playing/Stopped"};
        for (int i = 0; i < 3; ++i) {
            Kinefin::PlaybackReport report;
            report.kind = i == 0   ? Kinefin::PlaybackReport::Kind::Start
                          : i == 1 ? Kinefin::PlaybackReport::Kind::Progress
                                   : Kinefin::PlaybackReport::Kind::Stopped;
            report.itemId = kPlayItem;
            report.mediaSourceId = QStringLiteral("c1b2a3d4e5f6071829304a5b6c7d8e9f");
            report.playSessionId = QStringLiteral("b6c7d8e9f0a1b2c3");
            report.positionTicks = i == 0 ? 600000000 : i == 1 ? 123456789 : 100540000000;
            report.isPaused = i == 1;
            report.method = i == 1 ? Kinefin::PlaybackSource::Method::Transcode
                                   : Kinefin::PlaybackSource::Method::Direct;
            QVERIFY(client.reportPlayback(report));
            QTRY_COMPARE(server.reportLog.size(), i + 1);
            QTRY_COMPARE(settled.size(), i + 1);
        }
        for (int i = 0; i < 3; ++i) {
            const auto &entry = server.reportLog.at(i);
            QCOMPARE(entry.route, routes.at(i));
            QVERIFY(entry.contentType.startsWith("application/json"));
            QVERIFY(entry.auth.contains(kTokenField));
            QVERIFY(!entry.body.contains(kFixtureToken));
            QCOMPARE(QJsonDocument::fromJson(entry.body).object(),
                     QJsonDocument::fromJson(fixture(goldens.at(i))).object());
        }
        QVERIFY(!client.hasPendingPlaybackReports());
    }

    void progressCoalescesBehindTheReportInFlight()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdReports = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        const auto send = [&](Kinefin::PlaybackReport::Kind kind, qint64 ticks,
                              const QString &session) {
            Kinefin::PlaybackReport report;
            report.kind = kind;
            report.itemId = kPlayItem;
            report.mediaSourceId = QStringLiteral("aaaa");
            report.playSessionId = session;
            report.positionTicks = ticks;
            return client.reportPlayback(report);
        };
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Start, 0, QStringLiteral("s1")));
        QTRY_COMPARE(server.reportLog.size(), 1);
        for (qint64 ticks : {1, 2, 3})
            QVERIFY(send(Kinefin::PlaybackReport::Kind::Progress, ticks, QStringLiteral("s1")));
        // One request at a time: nothing else left while the Start is unanswered
        QTest::qWait(100);
        QCOMPARE(server.reportLog.size(), 1);
        server.holdReports = false;
        server.releaseHeld();
        QTRY_COMPARE(settled.size(), 1);
        QCOMPARE(server.reportLog.size(), 2);
        QCOMPARE(QJsonDocument::fromJson(server.reportLog.at(1).body)
                     .object()
                     .value(QStringLiteral("PositionTicks"))
                     .toVariant()
                     .toLongLong(),
                 3);
    }

    void stoppedDropsQueuedProgressAndKeepsOrderAcrossSessions()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdReports = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        const auto send = [&](Kinefin::PlaybackReport::Kind kind, const QString &session) {
            Kinefin::PlaybackReport report;
            report.kind = kind;
            report.itemId = kPlayItem;
            report.mediaSourceId = QStringLiteral("aaaa");
            report.playSessionId = session;
            return client.reportPlayback(report);
        };
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Start, QStringLiteral("s1")));
        QTRY_COMPARE(server.reportLog.size(), 1);
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Progress, QStringLiteral("s1")));
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Stopped, QStringLiteral("s1")));
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Start, QStringLiteral("s2")));
        server.holdReports = false;
        server.releaseHeld();
        QTRY_COMPARE(settled.size(), 1);
        // The old session's Stopped precedes the new session's Start; the queued Progress is gone
        QStringList sequence;
        for (const auto &entry : server.reportLog) {
            sequence << QString::fromLatin1(entry.route.mid(entry.route.lastIndexOf('/') + 1))
                            + QLatin1Char(':')
                            + QJsonDocument::fromJson(entry.body)
                                  .object()
                                  .value(QStringLiteral("PlaySessionId"))
                                  .toString();
        }
        QCOMPARE(sequence, (QStringList{QStringLiteral("Playing:s1"), QStringLiteral("Stopped:s1"),
                                        QStringLiteral("Playing:s2")}));
    }

    void onlyStoppedIsRetriedAndOnlyOnce()
    {
        const auto run = [](Kinefin::PlaybackReport::Kind kind, int status, bool onlyFirst) {
            FakeJellyfin server;
            if (!server.listen())
                return -1;
            server.reportStatus = onlyFirst ? 204 : status;
            server.firstStoppedStatus = onlyFirst ? status : 0;
            Kinefin::JellyfinClient client;
            client.setPlaybackReportRetryDelay(std::chrono::milliseconds(30));
            if (!login(client, server))
                return -1;
            QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
            Kinefin::PlaybackReport report;
            report.kind = kind;
            report.itemId = kPlayItem;
            report.mediaSourceId = QStringLiteral("aaaa");
            report.playSessionId = QStringLiteral("s1");
            if (!client.reportPlayback(report)
                || !QTest::qWaitFor([&] { return settled.size() == 1; }, 5000))
                return -2;
            QTest::qWait(150); // a stray second retry would show up here
            return int(server.reportLog.size());
        };
        using Kind = Kinefin::PlaybackReport::Kind;
        QCOMPARE(run(Kind::Stopped, 503, true), 2);  // 503 once: retried and delivered
        QCOMPARE(run(Kind::Stopped, 503, false), 2); // 503 forever: one retry, then dropped
        QCOMPARE(run(Kind::Stopped, 502, true), 2);
        QCOMPARE(run(Kind::Stopped, 504, true), 2);
        QCOMPARE(run(Kind::Stopped, 400, false), 1); // not a transient status
        QCOMPARE(run(Kind::Stopped, 500, false), 1);
        QCOMPARE(run(Kind::Start, 503, false), 1);    // Start would count a second play
        QCOMPARE(run(Kind::Progress, 503, false), 1); // the next Progress replaces it
    }

    void reportRedirectIsNotFollowedOrRetried()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.redirectReports = true;
        Kinefin::JellyfinClient client;
        client.setPlaybackReportRetryDelay(std::chrono::milliseconds(30));
        QVERIFY(login(client, server));
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        Kinefin::PlaybackReport report;
        report.kind = Kinefin::PlaybackReport::Kind::Stopped;
        report.itemId = kPlayItem;
        report.mediaSourceId = QStringLiteral("aaaa");
        report.playSessionId = QStringLiteral("s1");
        QVERIFY(client.reportPlayback(report));
        QTRY_COMPARE(settled.size(), 1);
        QTest::qWait(150);
        QCOMPARE(server.reportLog.size(), 1);
        QVERIFY(client.isAuthenticated());
    }

    void report401ExpiresTheSessionAndDropsTheQueue()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdReports = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        const auto send = [&](Kinefin::PlaybackReport::Kind kind) {
            Kinefin::PlaybackReport report;
            report.kind = kind;
            report.itemId = kPlayItem;
            report.mediaSourceId = QStringLiteral("aaaa");
            report.playSessionId = QStringLiteral("s1");
            return client.reportPlayback(report);
        };
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Start));
        QTRY_COMPARE(server.reportLog.size(), 1);
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Progress));
        server.reportStatus = 401;
        server.holdReports = false;
        server.releaseHeld();
        QTRY_COMPARE(expired.size(), 1);
        QTest::qWait(100);
        QCOMPARE(server.reportLog.size(), 1); // the queued Progress never went out
        QVERIFY(!client.hasPendingPlaybackReports());
        QCOMPARE(settled.size(), 0);
        QVERIFY(!client.isAuthenticated());
    }

    void disconnectDropsPendingReportsQuietly()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdReports = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        Kinefin::PlaybackReport report;
        report.kind = Kinefin::PlaybackReport::Kind::Start;
        report.itemId = kPlayItem;
        report.mediaSourceId = QStringLiteral("aaaa");
        report.playSessionId = QStringLiteral("s1");
        QVERIFY(client.reportPlayback(report));
        QTRY_COMPARE(server.reportLog.size(), 1);
        report.kind = Kinefin::PlaybackReport::Kind::Progress;
        QVERIFY(client.reportPlayback(report));
        QVERIFY(client.hasPendingPlaybackReports());
        client.disconnectFromServer();
        QVERIFY(!client.hasPendingPlaybackReports());
        server.holdReports = false;
        server.releaseHeld();
        QTest::qWait(150);
        QCOMPARE(settled.size(), 0);
        QCOMPARE(server.reportLog.size(), 1);
    }

    void reportsAreRefusedWithoutARequest()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        Kinefin::PlaybackReport report;
        report.itemId = kPlayItem;
        report.mediaSourceId = QStringLiteral("aaaa");
        report.playSessionId = QStringLiteral("s1");
        QVERIFY(!client.reportPlayback(report)); // not authenticated
        QVERIFY(!client.reportCapabilities());
        QVERIFY(login(client, server));
        report.positionTicks = -1;
        QVERIFY(!client.reportPlayback(report));
        report.positionTicks = 0;
        report.itemId = QStringLiteral("a/b");
        QVERIFY(!client.reportPlayback(report));
        QTest::qWait(100);
        QCOMPARE(server.reportLog.size(), 0);
    }

    void playbackInfoRejectsBadInputWithoutARequest()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        const QJsonObject profile = Kinefin::buildDeviceProfile({});
        // Not authenticated yet
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, 0, profile));
        QVERIFY(client.streamAuthorization().isEmpty());
        QVERIFY(login(client, server));
        QVERIFY(!client.requestPlaybackInfo(QStringLiteral("a/b"), 0, profile));
        QVERIFY(!client.requestPlaybackInfo(QString(), 0, profile));
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, -1, profile));
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, 0, {}));
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, 0, profile, 0));
        QTest::qWait(100);
        QCOMPARE(server.playbackInfoRequests, 0);
    }

    void webSocketRequestDerivation()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(prepare(client, server));
        // Without a session there is no socket request at all, not even a URL to leak
        QVERIFY(client.webSocketRequest().url().isEmpty());
        QVERIFY(login(client, server));
        const QNetworkRequest socketRequest = client.webSocketRequest();
        // The loopback transport keeps plain ws for the /socket path of the base; the token travels only in the header
        const QUrl expected = QUrl(QStringLiteral("ws://127.0.0.1:%1%2")
                                       .arg(server.url().port())
                                       .arg(QStringLiteral("/jellyfin/socket")));
        QCOMPARE(socketRequest.url(), expected);
        QVERIFY(socketRequest.url().query().isEmpty());
        QVERIFY(socketRequest.rawHeader(QByteArray("Authorization")).contains(kTokenField));
        client.disconnectFromServer();
        QVERIFY(client.webSocketRequest().url().isEmpty());
        QVERIFY(!client.webSocketRequest().hasRawHeader(QByteArray("Authorization")));
    }

    void verifySession401Expires()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy cleared(&client, &Kinefin::JellyfinClient::credentialsCleared);
        // Anything but a 401 stays silent
        QVERIFY(client.verifySession());
        QTRY_COMPARE(server.usersMeRequests, 1);
        QVERIFY(server.lastUsersMeAuth.contains(kTokenField));
        QVERIFY(client.isAuthenticated());
        QCOMPARE(expired.count(), 0);
        // The 401 expires the session and forgets the token
        server.denyUsersMe = true;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(expired.count(), 1);
        QCOMPARE(cleared.count(), 1);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(client.userId().isEmpty());
        // Without a session the re-check cannot even be sent
        QVERIFY(!client.verifySession());
        QVERIFY(client.webSocketRequest().url().isEmpty());
    }

    void credentialsClearedOnEveryTokenLoss()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy cleared(&client, &Kinefin::JellyfinClient::credentialsCleared);
        // Loss 1: a 401 on an authenticated route
        server.denyViews = true;
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(cleared.count(), 1);
        // Loss 2: an explicit logout
        QVERIFY(login(client, server));
        QCOMPARE(cleared.count(), 1); // the fresh login had nothing left to forget
        client.disconnectFromServer();
        QCOMPARE(cleared.count(), 2);
        // Loss 3: destruction while a token is still held
        bool destroyedClear = false;
        connect(&client, &Kinefin::JellyfinClient::credentialsCleared, this,
                [&destroyedClear] { destroyedClear = true; });
        {
            Kinefin::JellyfinClient dying;
            connect(&dying, &Kinefin::JellyfinClient::credentialsCleared, this,
                    [&destroyedClear] { destroyedClear = true; });
            QVERIFY(login(dying, server));
        }
        QVERIFY(destroyedClear);
    }

    // --- listings (browse, home rows, series, search, sessions) ---

    void browseEnvelopeFailuresFailClosed()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        // Pagination: a TotalRecordCount smaller than Items.size() is refused, the exact failure fetchUserViews enforces
        server.listingRawBody.insert(
            QStringLiteral("Items"),
            QByteArrayLiteral("{\"Items\":[{\"Id\":\"t1\"}],\"TotalRecordCount\":0}"));
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(2).toString(),
                 QStringLiteral("Invalid listing pagination"));
        QCOMPARE(ready.size(), 0);
        // Malformed payload: an array where the envelope must be
        server.listingRawBody.insert(QStringLiteral("Items"), QByteArrayLiteral("[1,2]"));
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QTRY_COMPARE(failures.size(), 2);
        QCOMPARE(failures.constLast().at(2).toString(), QStringLiteral("Invalid listing response"));
        QCOMPARE(ready.size(), 0);
        // A 401 expires the session; the listing failure signal carries fixed English
        server.denyListings = true;
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QTRY_COMPARE(expired.size(), 1);
        QCOMPARE(failures.size(), 3);
        QCOMPARE(failures.constLast().at(1).toInt(), 401);
        QVERIFY(!client.isAuthenticated());
        QCOMPARE(ready.size(), 0);
        // A redirect is never followed: the identity stays on the resolved base
        QVERIFY(login(client, server));
        server.redirectListings = true;
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QTRY_COMPARE(failures.size(), 4);
        QCOMPARE(failures.constLast().at(1).toInt(), 302);
        QCOMPARE(ready.size(), 0);
    }

    void browseSupersedeKeepsOneOutstandingReply()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        // The second listing is held after it fully transmitted; the third one supersedes it by aborting the held reply. (Superseding a request during its connect phase queues Qt network internals out of the test's control, so the superseded reply is always one that already went out)
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QTRY_COMPARE(ready.size(), 1); // the unheld first answer
        server.holdFirstListings = 1;
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QTRY_COMPARE(server.listingRequests, 2); // the held request fully sent
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QTRY_COMPARE(ready.size(), 2); // the third, immediate answer
        server.releaseHeld();
        QTest::qWait(100);
        QCOMPARE(ready.size(), 2); // the late aborted reply stays silent
        QCOMPARE(failures.size(), 0);
        // A different family is not superseded by an Items call: both stay answerable
        QSignalSpy sessions(&client, &Kinefin::JellyfinClient::sessionsReady);
        QVERIFY(client.fetchSessions(false));
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QTRY_COMPARE(sessions.size(), 1);
        QTRY_COMPARE(ready.size(), 3); // a new Items listing is not superseded
    }

    void browseDisconnectCancelsListingsQuietly()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        server.holdListings = true;
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QVERIFY(client.fetchSessions(false));
        client.disconnectFromServer();
        server.releaseHeld();
        QTest::qWait(100);
        QCOMPARE(ready.size(), 0);
        QCOMPARE(failures.size(), 0);
        // Nothing can be listed while disconnected
        QVERIFY(!client.fetchItems(Kinefin::ItemQuery()));
        QVERIFY(!client.fetchSessions(false));
    }

    void homeRowsResumeLatestNextUp()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy resume(&client, &Kinefin::JellyfinClient::resumeItemsReady);
        QSignalSpy latest(&client, &Kinefin::JellyfinClient::latestItemsReady);
        QSignalSpy nextUp(&client, &Kinefin::JellyfinClient::nextUpReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        // Continue Watching: /UserItems/Resume with the video media type, upstream shape
        QVERIFY(client.fetchResumeItems(12));
        QTRY_COMPARE(resume.size(), 1);
        QJsonArray items = resume.constFirst().constFirst().value<QJsonArray>();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items.at(1).toObject().value(QStringLiteral("SeriesId")).toString(),
                 QStringLiteral("aa11bb22cc33dd44ee55ff6677889900"));
        QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Resume")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("mediaTypes")), QStringLiteral("Video"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("12"));
        QVERIFY(wire.queryItemValue(QStringLiteral("userId"))
                    .contains(QStringLiteral("d5ed5d4e2e5447b58e9f2b6f7bd5c96a")));
        // Recently Added: the bare-array /Items/Latest, per library
        QVERIFY(client.fetchLatestItems(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01"), 8));
        QTRY_COMPARE(latest.size(), 1);
        items = latest.constFirst().constFirst().value<QJsonArray>();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items.at(0).toObject().value(QStringLiteral("Type")).toString(),
                 QStringLiteral("Series")); // upstream's groupItems shape: series first
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Latest")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("parentId")),
                 QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("8"));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("isPlayed")));
        // The optional played filter rides as given, on its own channel
        QVERIFY(client.fetchLatestItems(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01"), 8, 0,
                                        false));
        QTRY_COMPARE(latest.size(), 2);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Latest")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("isPlayed")), QStringLiteral("false"));
        // Next Up: /Shows/NextUp with the rewatch and cutoff knobs
        QVERIFY(client.fetchNextUp({}, {}, QStringLiteral("2025-11-10"), false, 15));
        QTRY_COMPARE(nextUp.size(), 1);
        items = nextUp.constFirst().constFirst().value<QJsonArray>();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.at(0).toObject().value(QStringLiteral("IndexNumber")).toInt(), 6);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("NextUp")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("nextUpDateCutoff")),
                 QStringLiteral("2025-11-10"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("enableRewatching")), QStringLiteral("false"));
        QVERIFY(wire.queryItemValue(QStringLiteral("seriesId")).isEmpty());
        QCOMPARE(failures.size(), 0);
    }

    void browseItemsComposeAndAnswer()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        Kinefin::ItemQuery query;
        query.parentId = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
        query.includeItemTypes = QStringLiteral("Movie,Series");
        query.sortBy = QStringLiteral("SortName");
        query.sortOrder = QStringLiteral("Descending");
        query.startIndex = 24;
        query.limit = 12;
        query.searchTerm = QStringLiteral("hollow interval");
        query.recursive = true;
        query.fields = {QStringLiteral("PrimaryImageAspectRatio"), QStringLiteral("Path"),
                        QStringLiteral("MediaSourceCount")};
        QVERIFY(client.fetchItems(query));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(failures.size(), 0);
        const QJsonArray items = ready.constFirst().constFirst().value<QJsonArray>();
        QCOMPARE(items.size(), 2);
        QCOMPARE(ready.constFirst().at(1).toInt(), 42);
        QCOMPARE(items.at(0).toObject().value(QStringLiteral("Type")).toString(),
                 QStringLiteral("Movie"));
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Items")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("parentId")),
                 QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("includeItemTypes")),
                 QStringLiteral("Movie,Series"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("sortBy")), QStringLiteral("SortName"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("sortOrder")), QStringLiteral("Descending"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("24"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("12"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("recursive")), QStringLiteral("true"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("searchTerm")),
                 QStringLiteral("hollow interval"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("fields")),
                 QStringLiteral("PrimaryImageAspectRatio,Path,MediaSourceCount"));
        QVERIFY(server.lastListingAuth.value(QStringLiteral("Items")).contains(kTokenField));
        // The auth header never rides in the URL
        QVERIFY(!QString::fromUtf8(server.lastListingRoute.value(QStringLiteral("Items")))
                     .contains(QLatin1String("Token")));
    }

    void browseItemsRefuseInvalidQueries()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        Kinefin::ItemQuery good;
        QVERIFY(client.fetchItems(good));
        QTRY_COMPARE(ready.size(), 1);
        // Bad parentId / id shapes, an unknown field and an unknown type are refused without a request; the server never sees a URL it would answer 400 with
        const auto run = [&](const Kinefin::ItemQuery &query) {
            const int before = server.listingRequests;
            QVERIFY(!client.fetchItems(query));
            QCOMPARE(server.listingRequests, before);
        };
        Kinefin::ItemQuery badParent = good;
        badParent.parentId = QStringLiteral("../etc/passwd");
        run(badParent);
        Kinefin::ItemQuery badType = good;
        badType.includeItemTypes = QStringLiteral("Movie,EvilType");
        run(badType);
        Kinefin::ItemQuery badField = good;
        badField.fields = {QStringLiteral("Path"), QStringLiteral("UserData")};
        run(badField); // UserData is not an ItemFields token on 12.4
        Kinefin::ItemQuery badSortOrder = good;
        badSortOrder.sortBy = QStringLiteral("SortName");
        badSortOrder.sortOrder = QStringLiteral("sideways");
        run(badSortOrder);
        Kinefin::ItemQuery badSortBy = good;
        badSortBy.sortBy = QStringLiteral("Sort,Na~me");
        run(badSortBy);
        Kinefin::ItemQuery badFilters = good;
        badFilters.filters = QStringLiteral("IsResumable,IsHaunted");
        run(badFilters);
        // The ids= form still checks every member against the id shape rule (a URL-safety guard, not a GUID checker): a space is refused without a request
        Kinefin::ItemQuery badIds = good;
        badIds.itemIds = {QStringLiteral("bad id")};
        run(badIds);
        QCOMPARE(failures.size(), 0);
        QCOMPARE(ready.size(), 1);
    }

    void browseItemsResolveIdsAndDropTheRest()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        server.listingFixtures.insert(QStringLiteral("Items"), QStringLiteral("movie_item"));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
        Kinefin::ItemQuery query;
        query.parentId = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
        query.searchTerm = QStringLiteral("arcturus");
        query.sortBy = QStringLiteral("SortName");
        query.itemIds = {QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"),
                         QStringLiteral("9a8b7c6d5e4f3827160594a3b2c1d0e1")};
        query.limit = 5;
        QVERIFY(client.fetchItems(query));
        QTRY_COMPARE(ready.size(), 1);
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Items")));
        QCOMPARE(
            wire.queryItemValue(QStringLiteral("ids")),
            QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f,9a8b7c6d5e4f3827160594a3b2c1d0e1"));
        // The ids= form carries the fields only: the other members must not reach the wire
        QVERIFY(wire.queryItemValue(QStringLiteral("parentId")).isEmpty());
        QVERIFY(wire.queryItemValue(QStringLiteral("searchTerm")).isEmpty());
        QVERIFY(wire.queryItemValue(QStringLiteral("sortBy")).isEmpty());
        QVERIFY(wire.queryItemValue(QStringLiteral("limit")).isEmpty());
    }

    void seriesNavigationSeasonsAndEpisodes()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy seasons(&client, &Kinefin::JellyfinClient::seasonsReady);
        QSignalSpy episodes(&client, &Kinefin::JellyfinClient::episodesReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        QVERIFY(client.fetchSeasons(QStringLiteral("aa11bb22cc33dd44ee55ff6677889900")));
        QTRY_COMPARE(seasons.size(), 1);
        QCOMPARE(seasons.constFirst().constFirst().value<QJsonArray>().size(), 3);
        QCOMPARE(seasons.constFirst().at(1).toInt(), 3);
        QCOMPARE(seasons.constFirst().at(2).toInt(), 0); // the default channel
        QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Seasons")));
        QVERIFY(
            server.lastListingRoute.value(QStringLiteral("Seasons"))
                .contains(QByteArrayLiteral("/Shows/aa11bb22cc33dd44ee55ff6677889900/Seasons?")));
        QVERIFY(wire.queryItemValue(QStringLiteral("userId"))
                    .contains(QStringLiteral("d5ed5d4e2e5447b58e9f2b6f7bd5c96a")));
        QVERIFY(wire.queryItemValue(QStringLiteral("enableImageTypes"))
                    .contains(QStringLiteral("Primary")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("imageTypeLimit")), QStringLiteral("1"));
        QVERIFY(client.fetchEpisodes(QStringLiteral("aa11bb22cc33dd44ee55ff6677889900"),
                                     QStringLiteral("11aa22bb33cc44dd55ee66ff778899aa"), 6, 40));
        QTRY_COMPARE(episodes.size(), 1);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Episodes")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("seasonId")),
                 QStringLiteral("11aa22bb33cc44dd55ee66ff778899aa"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("6"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("40"));
        // Only the field the rows show beyond the defaults, and the image types the cards use
        QCOMPARE(wire.queryItemValue(QStringLiteral("fields")), QStringLiteral("Overview"));
        QVERIFY(wire.queryItemValue(QStringLiteral("enableImageTypes"))
                    .contains(QStringLiteral("Backdrop")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("imageTypeLimit")), QStringLiteral("1"));
        // An empty seasonId asks for every season, unpaged:
        QVERIFY(
            client.fetchEpisodes(QStringLiteral("aa11bb22cc33dd44ee55ff6677889900"), {}, -1, -1));
        QTRY_COMPARE(episodes.size(), 2);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Episodes")));
        QVERIFY(wire.queryItemValue(QStringLiteral("seasonId")).isEmpty());
        QVERIFY(wire.queryItemValue(QStringLiteral("limit")).isEmpty());
        // Bad ids are refused without a request (the id shape rule, not a GUID check); a 401 expires the session
        QVERIFY(!client.fetchSeasons(QStringLiteral("no no")));
        QVERIFY(!client.fetchEpisodes(QStringLiteral("aa11bb22cc33dd44ee55ff6677889900"),
                                      QStringLiteral("../x"), 0, 10));
        QCOMPARE(failures.size(), 0);
        server.denyListings = true;
        QVERIFY(client.fetchSeasons(QStringLiteral("aa11bb22cc33dd44ee55ff6677889900")));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constLast().at(1).toInt(), 401);
        QTRY_COMPARE(seasons.size(), 1); // no new ready signal
        QVERIFY(!client.isAuthenticated());
    }

    // The episodes the server only knows of (missing and unaired, LocationType Virtual) are asked away from every listing of episodes, and let in only when a list asks for them or a filter does
    void virtualEpisodesAreLeftOutUnlessAskedFor()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString series = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
        QSignalSpy episodes(&client, &Kinefin::JellyfinClient::episodesReady);
        QSignalSpy items(&client, &Kinefin::JellyfinClient::itemsReady);
        const auto wire = [&server](const QString &family) {
            return FakeJellyfin::capturedListingQuery(server.lastListingRoute.value(family));
        };
        const QString isMissing = QStringLiteral("isMissing");
        // The route of a series lists them for every user: playback, queues and downloads never want them
        QVERIFY(client.fetchEpisodes(series, {}, 0, 10));
        QTRY_COMPARE(episodes.size(), 1);
        QCOMPARE(wire(QStringLiteral("Episodes")).queryItemValue(isMissing),
                 QStringLiteral("false"));
        QVERIFY(client.fetchEpisodes(series, {}, 0, 10, 0, true));
        QTRY_COMPARE(episodes.size(), 2);
        QCOMPARE(wire(QStringLiteral("Episodes")).queryItemValue(isMissing),
                 QStringLiteral("false"));
        // A list that asks for them gets them, and says so in no other way than the missing parameter
        QVERIFY(client.fetchEpisodes(series, {}, 0, 10, 0, false, true));
        QTRY_COMPARE(episodes.size(), 3);
        QVERIFY(!wire(QStringLiteral("Episodes")).hasQueryItem(isMissing));

        // The general listing leaves them out of any query that names the Episode type
        Kinefin::ItemQuery query;
        query.includeItemTypes = QStringLiteral("Movie,Episode");
        int sent = 0;
        const auto fetch = [&](const Kinefin::ItemQuery &q) {
            const bool ok = client.fetchItems(q);
            ++sent;
            return ok && QTest::qWaitFor([&] { return items.size() == sent; }, 5000);
        };
        QVERIFY(fetch(query));
        QCOMPARE(wire(QStringLiteral("Items")).queryItemValue(isMissing), QStringLiteral("false"));
        // A filter that asks for them wins
        Kinefin::ItemQuery missing;
        missing.includeItemTypes = QStringLiteral("Episode");
        missing.isMissing = true;
        QVERIFY(fetch(missing));
        QCOMPARE(wire(QStringLiteral("Items")).queryItemValue(isMissing), QStringLiteral("true"));
        Kinefin::ItemQuery unaired;
        unaired.includeItemTypes = QStringLiteral("Episode");
        unaired.isUnaired = true;
        QVERIFY(fetch(unaired));
        QVERIFY(!wire(QStringLiteral("Items")).hasQueryItem(isMissing));
        QCOMPARE(wire(QStringLiteral("Items")).queryItemValue(QStringLiteral("isUnaired")),
                 QStringLiteral("true"));
        // A listing of other types, and one by ids, is left as it is
        Kinefin::ItemQuery movies;
        movies.includeItemTypes = QStringLiteral("Movie,Series");
        QVERIFY(fetch(movies));
        QVERIFY(!wire(QStringLiteral("Items")).hasQueryItem(isMissing));
        Kinefin::ItemQuery byIds;
        byIds.itemIds = {series};
        byIds.includeItemTypes = QStringLiteral("Episode");
        QVERIFY(fetch(byIds));
        QVERIFY(!wire(QStringLiteral("Items")).hasQueryItem(isMissing));

        // A cleared session forgets the saved option
        client.setDisplayMissingEpisodes(true);
        QVERIFY(client.displayMissingEpisodes());
        server.denyListings = true;
        QVERIFY(client.fetchSeasons(series));
        QTRY_VERIFY(!client.isAuthenticated());
        QVERIFY(!client.displayMissingEpisodes());
    }

    void seasonsAndEpisodesKeepOneRequestPerChannel()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString series = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
        QSignalSpy seasons(&client, &Kinefin::JellyfinClient::seasonsReady);
        QSignalSpy episodes(&client, &Kinefin::JellyfinClient::episodesReady);
        // Two channels of one family are two requests in flight, each answered with its own channel
        QVERIFY(client.fetchSeasons(series, 80001));
        QVERIFY(client.fetchSeasons(series, 80002));
        QVERIFY(client.fetchEpisodes(series, {}, 0, 10, 80001));
        QTRY_COMPARE(seasons.size(), 2);
        QTRY_COMPARE(episodes.size(), 1);
        QSet<int> channels;
        for (const QList<QVariant> &arguments : seasons)
            channels.insert(arguments.at(2).toInt());
        QCOMPARE(channels, (QSet<int>{80001, 80002}));
        QCOMPARE(episodes.constFirst().at(2).toInt(), 80001);
        // A released channel emits nothing for the request it still had
        seasons.clear();
        server.holdListings = true;
        QVERIFY(client.fetchSeasons(series, 80003));
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Seasons")).size(), 3);
        client.releaseListingChannel(80003);
        server.holdListings = false;
        QTest::qWait(200);
        QCOMPARE(seasons.size(), 0);
    }

    void genreIdsRideCommaDelimitedAndBadOnesAreRefused()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
        const QString sciFi = QStringLiteral("aa00000000000000000000000000ff01");
        const QString mystery = QStringLiteral("bb00000000000000000000000000ff02");
        Kinefin::ItemQuery query;
        query.genreIds = {sciFi, mystery};
        QVERIFY(client.fetchItems(query));
        QTRY_COMPARE(ready.size(), 1);
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Items")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("genreIds"), QUrl::FullyDecoded),
                 sciFi + QLatin1Char(',') + mystery);
        const auto run = [&](const QStringList &ids) {
            const int before = server.listingRequests;
            Kinefin::ItemQuery bad;
            bad.genreIds = ids;
            QVERIFY(!client.fetchItems(bad));
            QCOMPARE(server.listingRequests, before);
        };
        run({sciFi, QStringLiteral("Science Fiction")});
        run({QStringLiteral("a|b")});
        QStringList tooMany;
        for (int i = 0; i < Kinefin::MaxGenreIds + 1; ++i)
            tooMany.append(QStringLiteral("%1").arg(i + 1, 32, 16, QLatin1Char('e')));
        run(tooMany);
    }

    void genresAndFiltersCarryTheItemKindsOfALibrary()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy genres(&client, &Kinefin::JellyfinClient::genresReady);
        QSignalSpy filters(&client, &Kinefin::JellyfinClient::filtersReady);
        const QString library = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
        QVERIFY(client.fetchGenres(library, 0, 20, QStringLiteral("Series")));
        QTRY_COMPARE(genres.size(), 1);
        QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Genres")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("Series"));
        QVERIFY(client.fetchFilters(library, QStringLiteral("Movie")));
        QTRY_COMPARE(filters.size(), 1);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Filters")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("Movie"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("recursive")), QStringLiteral("true"));
        const int before = server.listingRequests;
        QVERIFY(!client.fetchGenres(library, 0, 20, QStringLiteral("Movie,EvilType")));
        QVERIFY(!client.fetchFilters(library, QStringLiteral("Movie,EvilType")));
        QCOMPARE(server.listingRequests, before);
    }

    void filtersAndGenresCarryTheirChannel()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy genres(&client, &Kinefin::JellyfinClient::genresReady);
        QSignalSpy filters(&client, &Kinefin::JellyfinClient::filtersReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        // Two channels of one family are two requests in flight, an empty parent sends none
        QVERIFY(client.fetchGenres({}, 0, 20, QStringLiteral("Movie"), 1027));
        QVERIFY(client.fetchGenres({}, 0, 20, QStringLiteral("Series"), 1028));
        QVERIFY(client.fetchFilters({}, QStringLiteral("Movie"), 6));
        QVERIFY(client.fetchFilters({}, QStringLiteral("Series"), 7));
        QTRY_COMPARE(genres.size(), 2);
        QTRY_COMPARE(filters.size(), 2);
        QCOMPARE(QSet<int>({genres.at(0).at(2).toInt(), genres.at(1).at(2).toInt()}),
                 (QSet<int>{1027, 1028}));
        QCOMPARE(QSet<int>({filters.at(0).at(1).toInt(), filters.at(1).at(1).toInt()}),
                 (QSet<int>{6, 7}));
        for (const QByteArray &route : server.listingRoutes.value(QStringLiteral("Genres"))
                                           + server.listingRoutes.value(QStringLiteral("Filters")))
            QVERIFY2(!route.contains("parentId="), route.constData());
        // The same channel again supersedes the pending request of that channel only
        genres.clear();
        server.holdFirstListings = 1;
        QVERIFY(client.fetchGenres({}, 0, 20, QStringLiteral("Movie"), 1027));
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Genres")).size(), 3);
        QVERIFY(client.fetchGenres({}, 0, 20, QStringLiteral("Movie"), 1027));
        QTRY_COMPARE(genres.size(), 1);
        QCOMPARE(genres.constFirst().at(2).toInt(), 1027);
        // A failure names its channel
        server.listingStatus.insert(QStringLiteral("Filters"), 500);
        QVERIFY(client.fetchFilters({}, QStringLiteral("Movie"), 7));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(0).toString(), QStringLiteral("Filters"));
        QCOMPARE(failures.constFirst().at(3).toInt(), 7);
        server.listingStatus.clear();
        server.listingStatus.insert(QStringLiteral("Genres"), 500);
        QVERIFY(client.fetchGenres({}, 0, 20, QStringLiteral("Movie"), 1027));
        QTRY_COMPARE(failures.size(), 2);
        QCOMPARE(failures.constLast().at(0).toString(), QStringLiteral("Genres"));
        QCOMPARE(failures.constLast().at(3).toInt(), 1027);
    }

    void releaseDropsGenresAndFilters()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy genres(&client, &Kinefin::JellyfinClient::genresReady);
        QSignalSpy filters(&client, &Kinefin::JellyfinClient::filtersReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        server.holdListings = true;
        QVERIFY(client.fetchGenres({}, 0, 20, QStringLiteral("Movie"), 1027));
        QVERIFY(client.fetchFilters({}, QStringLiteral("Movie"), 1027));
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Genres")).size(), 1);
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Filters")).size(), 1);
        client.releaseListingChannel(1027);
        server.holdListings = false;
        server.releaseHeld();
        QTest::qWait(200);
        QCOMPARE(genres.size(), 0);
        QCOMPARE(filters.size(), 0);
        QCOMPARE(failures.size(), 0);
        // The channel is usable again
        QVERIFY(client.fetchGenres({}, 0, 20, QStringLiteral("Movie"), 1027));
        QTRY_COMPARE(genres.size(), 1);
    }

    void similarGenresFiltersSessions()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy similar(&client, &Kinefin::JellyfinClient::similarItemsReady);
        QSignalSpy genres(&client, &Kinefin::JellyfinClient::genresReady);
        QSignalSpy filters(&client, &Kinefin::JellyfinClient::filtersReady);
        QSignalSpy sessions(&client, &Kinefin::JellyfinClient::sessionsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        QVERIFY(client.fetchSimilarItems(QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"), 12));
        QTRY_COMPARE(similar.size(), 1);
        QCOMPARE(similar.constFirst().at(1).toInt(), 2);
        QVERIFY(
            server.lastListingRoute.value(QStringLiteral("Similar"))
                .contains(QByteArrayLiteral("/Items/f0e1d2c3b4a5968778695a4b3c2d1e0f/Similar?")));
        QVERIFY(!client.fetchSimilarItems(QStringLiteral("bad id"), 10));
        QVERIFY(client.fetchGenres(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01"), 0, 30));
        QTRY_COMPARE(genres.size(), 1);
        const QJsonArray genreItems = genres.constFirst().constFirst().value<QJsonArray>();
        QCOMPARE(genreItems.size(), 3);
        QCOMPARE(genreItems.at(0).toObject().value(QStringLiteral("Name")).toString(),
                 QStringLiteral("Science Fiction"));
        QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Genres")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("30"));
        QVERIFY(client.fetchFilters(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01")));
        QTRY_COMPARE(filters.size(), 1);
        const QJsonObject filterObject = filters.constFirst().constFirst().value<QJsonObject>();
        QCOMPARE(filterObject.value(QStringLiteral("Genres")).toArray().size(), 3);
        QCOMPARE(filterObject.value(QStringLiteral("Tags")).toArray().size(), 3);
        QCOMPARE(filterObject.value(QStringLiteral("AudioLanguages")).toArray().size(), 2);
        QCOMPARE(filterObject.value(QStringLiteral("SubtitleLanguages")).toArray().size(), 1);
        QVERIFY(client.fetchSessions(true));
        QTRY_COMPARE(sessions.size(), 1);
        const QJsonArray sessionItems = sessions.constFirst().constFirst().value<QJsonArray>();
        QCOMPARE(sessionItems.size(), 2);
        QVERIFY(server.lastListingRoute.value(QStringLiteral("Sessions"))
                    .contains(QByteArrayLiteral("controllableByUserId=")));
        QVERIFY(client.fetchSessions(false));
        QTRY_COMPARE(sessions.size(), 2);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Sessions")));
        QVERIFY(wire.queryItemValue(QStringLiteral("controllableByUserId")).isEmpty());
        // The bare-array validation refuses an object or a nested array
        server.listingRawBody.insert(QStringLiteral("Sessions"), QByteArrayLiteral("{\"x\":1}"));
        QVERIFY(client.fetchSessions(false));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(sessions.size(), 2);
        server.listingRawBody.insert(QStringLiteral("Sessions"),
                                     QByteArrayLiteral("[{},{\"a\":[]},1]"));
        QVERIFY(client.fetchSessions(false));
        QTRY_COMPARE(failures.size(), 2);
        QCOMPARE(sessions.size(), 2);
    }

    void listingChannelsRunSideBySideAndSupersedePerChannel()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy latest(&client, &Kinefin::JellyfinClient::latestItemsReady);
        // Two channels of one family are two requests in flight, each answered with its own channel
        QVERIFY(client.fetchLatestItems(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01"), 5, 1));
        QVERIFY(client.fetchLatestItems(QStringLiteral("7f1b8f4b3e2148c5a8f2b7c9d0e1f2a3"), 5, 2));
        QTRY_COMPARE(latest.size(), 2);
        QSet<int> channels;
        for (const QList<QVariant> &arguments : latest)
            channels.insert(arguments.at(1).toInt());
        QCOMPARE(channels, (QSet<int>{1, 2}));
        QCOMPARE(server.listingRoutes.value(QStringLiteral("Latest")).size(), 2);

        // The same channel again supersedes the pending request and emits only once
        latest.clear();
        server.holdFirstListings = 1;
        QVERIFY(client.fetchLatestItems(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01"), 5, 1));
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Latest")).size(), 3);
        QVERIFY(client.fetchLatestItems(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01"), 5, 1));
        QTRY_COMPARE(latest.size(), 1);
        QCOMPARE(latest.constFirst().at(1).toInt(), 1);
        QTest::qWait(200);
        QCOMPARE(latest.size(), 1);
    }

    void similarItemsTakeAChannel()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy similar(&client, &Kinefin::JellyfinClient::similarItemsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        QVERIFY(
            client.fetchSimilarItems(QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"), 5, 70000));
        QVERIFY(
            client.fetchSimilarItems(QStringLiteral("c2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7"), 5, 70001));
        QTRY_COMPARE(similar.size(), 2);
        QSet<int> channels;
        for (const QList<QVariant> &arguments : similar)
            channels.insert(arguments.at(2).toInt());
        QCOMPARE(channels, (QSet<int>{70000, 70001}));
        server.listingStatus.insert(QStringLiteral("Similar"), 500);
        QVERIFY(
            client.fetchSimilarItems(QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"), 5, 70002));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(0).toString(), QStringLiteral("Similar"));
        QCOMPARE(failures.constFirst().at(3).toInt(), 70002);
    }

    void aTypedTermReachesTheServerPercentEncoded()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        Kinefin::ItemQuery query;
        query.searchTerm = QStringLiteral("C++ & #1 100% é");
        query.cardImages = true;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
        QVERIFY(client.fetchItems(query, 5));
        QTRY_COMPARE(ready.size(), 1);
        const QString route = QString::fromUtf8(server.itemsRoutes.constFirst());
        // A '+' would be read as a space by the server, and & # % would cut or break the query
        QVERIFY2(
            route.contains(QStringLiteral("searchTerm=C%2B%2B%20%26%20%231%20100%25%20%C3%A9")),
            qPrintable(route));
        QVERIFY(!route.contains(QLatin1Char('+')));
        // A typed %25 or %00 is text, not an escape: the percent sign itself is encoded
        Kinefin::ItemQuery escapes;
        escapes.searchTerm = QStringLiteral("100%25 ab%00 %0D%0A");
        QVERIFY(client.fetchItems(escapes, 5));
        QTRY_COMPARE(ready.size(), 2);
        const QString escaped = QString::fromUtf8(server.itemsRoutes.constLast());
        QVERIFY2(escaped.contains(QStringLiteral("searchTerm=100%2525%20ab%2500%20%250D%250A")),
                 qPrintable(escaped));
        // The server decodes it back to the typed term
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(server.itemsRoutes.constFirst());
        QCOMPARE(wire.queryItemValue(QStringLiteral("searchTerm"), QUrl::FullyDecoded),
                 query.searchTerm);
        // Card images ask for one tag of each image type
        QCOMPARE(wire.queryItemValue(QStringLiteral("imageTypeLimit")), QStringLiteral("1"));
        QVERIFY(wire.queryItemValue(QStringLiteral("enableImageTypes"))
                    .contains(QStringLiteral("Primary")));
    }

    void libraryStatusAndFeatureRowsReachTheWire()
    {
        using namespace Kinefin;
        FakeJellyfin server;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &JellyfinClient::itemsReady);
        ItemQuery base;
        base.includeItemTypes = QStringLiteral("Movie");
        base.recursive = true;
        const auto send = [&](const LibraryFilterState &state, LibraryScope scope,
                              const ItemQuery &from) {
            const auto query = composeLibraryQuery(from, scope, state, LibrarySort{});
            const int expected = ready.size() + 1;
            if (!query || !client.fetchItems(*query)
                || !QTest::qWaitFor([&] { return ready.size() == expected; }, 5000))
                return QUrlQuery();
            return FakeJellyfin::capturedListingQuery(server.itemsRoutes.constLast());
        };
        const auto value = [](const QUrlQuery &wire, const char *name) {
            return wire.queryItemValue(QLatin1String(name), QUrl::FullyDecoded);
        };

        // Played and Unplayed are alternatives, Resumable and Favorites join either
        LibraryFilterState state;
        state.played = PlayedState::Played;
        QUrlQuery wire = send(state, LibraryScope::Movies, base);
        QCOMPARE(value(wire, "filters"), QStringLiteral("IsPlayed"));
        state.played = PlayedState::Unplayed;
        state.resumable = true;
        state.favorites = true;
        wire = send(state, LibraryScope::Movies, base);
        QCOMPARE(value(wire, "filters"), QStringLiteral("IsUnplayed,IsResumable,IsFavorite"));
        // The Favorites tab's fixed constraint merges without a duplicate
        ItemQuery favoritesTab = base;
        favoritesTab.filters = QStringLiteral("IsFavorite");
        state = {};
        state.favorites = true;
        state.played = PlayedState::Played;
        wire = send(state, LibraryScope::Movies, favoritesTab);
        QCOMPARE(value(wire, "filters"), QStringLiteral("IsFavorite,IsPlayed"));
        // An empty state adds nothing to the base query
        wire = send({}, LibraryScope::Movies, base);
        for (const char *name :
             {"filters", "hasSubtitles", "hasTrailer", "hasSpecialFeature", "hasThemeSong",
              "hasThemeVideo", "isHd", "is4K", "is3D", "videoTypes", "seriesStatus", "genreIds",
              "tags", "officialRatings", "years", "parentIndexNumber", "isMissing", "isUnaired",
              "studioIds"})
            QVERIFY2(!wire.hasQueryItem(QLatin1String(name)), name);

        // The five features are independent
        const QList<std::pair<LibraryFeature, const char *>> features{
            {LibraryFeature::Subtitles, "hasSubtitles"},
            {LibraryFeature::Trailer, "hasTrailer"},
            {LibraryFeature::SpecialFeatures, "hasSpecialFeature"},
            {LibraryFeature::ThemeSong, "hasThemeSong"},
            {LibraryFeature::ThemeVideo, "hasThemeVideo"}};
        for (const auto &[feature, name] : features) {
            state = {};
            state.features = feature;
            wire = send(state, LibraryScope::Movies, base);
            QCOMPARE(value(wire, name), QStringLiteral("true"));
            for (const auto &other : features) {
                if (other.second != name)
                    QVERIFY2(!wire.hasQueryItem(QLatin1String(other.second)), other.second);
            }
        }
        state = {};
        state.features = LibraryFeature::Subtitles | LibraryFeature::ThemeSong;
        wire = send(state, LibraryScope::Movies, base);
        QCOMPARE(value(wire, "hasSubtitles"), QStringLiteral("true"));
        QCOMPARE(value(wire, "hasThemeSong"), QStringLiteral("true"));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("hasTrailer")));
    }

    void libraryQualityAndVideoRowsReachTheWire()
    {
        using namespace Kinefin;
        FakeJellyfin server;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &JellyfinClient::itemsReady);
        ItemQuery base;
        base.includeItemTypes = QStringLiteral("Movie");
        const auto send = [&](const LibraryFilterState &state) {
            const auto query = composeLibraryQuery(base, LibraryScope::Movies, state, {});
            const int expected = ready.size() + 1;
            if (!query || !client.fetchItems(*query)
                || !QTest::qWaitFor([&] { return ready.size() == expected; }, 5000))
                return QUrlQuery();
            return FakeJellyfin::capturedListingQuery(server.itemsRoutes.constLast());
        };

        // HD and SD are one choice
        LibraryFilterState state;
        state.quality = VideoQuality::HD;
        QUrlQuery wire = send(state);
        QCOMPARE(wire.queryItemValue(QStringLiteral("isHd")), QStringLiteral("true"));
        state.quality = VideoQuality::SD;
        wire = send(state);
        QCOMPARE(wire.queryItemValue(QStringLiteral("isHd")), QStringLiteral("false"));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("is4K")));
        // 4K drops SD (a contradiction), keeps HD
        state.is4K = true;
        wire = send(state);
        QVERIFY(!wire.hasQueryItem(QStringLiteral("isHd")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("is4K")), QStringLiteral("true"));
        state.quality = VideoQuality::HD;
        wire = send(state);
        QCOMPARE(wire.queryItemValue(QStringLiteral("isHd")), QStringLiteral("true"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("is4K")), QStringLiteral("true"));
        state = {};
        state.is3D = true;
        wire = send(state);
        QCOMPARE(wire.queryItemValue(QStringLiteral("is3D")), QStringLiteral("true"));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("isHd")));
        // The video types are one OR list, comma-separated
        state = {};
        state.videoTypes = {QStringLiteral("BluRay"), QStringLiteral("Dvd")};
        wire = send(state);
        QCOMPARE(wire.queryItemValue(QStringLiteral("videoTypes"), QUrl::FullyDecoded),
                 QStringLiteral("BluRay,Dvd"));

        // The client refuses what the composer would never build: SD with 4K, Played with Unplayed
        const int before = server.listingRequests;
        ItemQuery contradictory = base;
        contradictory.isHd = false;
        contradictory.is4K = true;
        QVERIFY(!client.fetchItems(contradictory));
        contradictory = base;
        contradictory.filters = QStringLiteral("IsPlayed,IsUnplayed");
        QVERIFY(!client.fetchItems(contradictory));
        QCOMPARE(server.listingRequests, before);
    }

    void libraryListFiltersComposeWithAndOrAndEscape()
    {
        using namespace Kinefin;
        FakeJellyfin server;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &JellyfinClient::itemsReady);
        ItemQuery base;
        base.parentId = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
        base.includeItemTypes = QStringLiteral("Movie");
        base.recursive = true;
        LibraryFilterState state;
        state.genreIds = {QStringLiteral("aa00000000000000000000000000ff01"),
                          QStringLiteral("bb00000000000000000000000000ff02")};
        state.tags = {QStringLiteral("C++"), QStringLiteral("100% pure"), QStringLiteral("a,b"),
                      QStringLiteral("café")};
        state.officialRatings = {QStringLiteral("PG-13"), QStringLiteral("R")};
        state.years = {2023, 2024};
        state.played = PlayedState::Unplayed;
        state.features = LibraryFeature::Subtitles;
        state.quality = VideoQuality::HD;
        state.videoTypes = {QStringLiteral("Dvd")};
        const auto query = composeLibraryQuery(base, LibraryScope::Movies, state, {});
        QVERIFY(query);
        QVERIFY(client.fetchItems(*query));
        QTRY_COMPARE(ready.size(), 1);
        const QString route = QString::fromUtf8(server.itemsRoutes.constFirst());
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(server.itemsRoutes.constFirst());
        const auto decoded = [&wire](const char *name) {
            return wire.queryItemValue(QLatin1String(name), QUrl::FullyDecoded);
        };
        // Different sections are different parameters (AND), the values of a list are one parameter (OR); ids take commas, texts pipes
        QCOMPARE(
            decoded("genreIds"),
            QStringLiteral("aa00000000000000000000000000ff01,bb00000000000000000000000000ff02"));
        QCOMPARE(decoded("tags"), QStringLiteral("C++|100% pure|a,b|café"));
        QCOMPARE(decoded("officialRatings"), QStringLiteral("PG-13|R"));
        QCOMPARE(decoded("years"), QStringLiteral("2023,2024"));
        QCOMPARE(decoded("filters"), QStringLiteral("IsUnplayed"));
        QCOMPARE(decoded("hasSubtitles"), QStringLiteral("true"));
        QCOMPARE(decoded("isHd"), QStringLiteral("true"));
        QCOMPARE(decoded("videoTypes"), QStringLiteral("Dvd"));
        QCOMPARE(decoded("parentId"), QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01"));
        QCOMPARE(decoded("includeItemTypes"), QStringLiteral("Movie"));
        // Texts are encoded once: a '+' is %2B (never a space), a '%' is %25, a comma inside a tag %2C, the delimiter between tags %7C
        QVERIFY2(route.contains(QStringLiteral("tags=C%2B%2B%7C100%25%20pure%7Ca%2Cb%7Ccaf%C3%A9")),
                 qPrintable(route));
        QVERIFY(!route.contains(QLatin1Char('+')));
    }

    void libraryScopeDecidesWhichSectionsApply()
    {
        using namespace Kinefin;
        LibraryFilterState all;
        all.played = PlayedState::Played;
        all.resumable = true;
        all.favorites = true;
        all.features = LibraryFeature::Subtitles | LibraryFeature::Trailer;
        all.quality = VideoQuality::HD;
        all.is4K = true;
        all.is3D = true;
        all.videoTypes = {QStringLiteral("BluRay")};
        all.genreIds = {QStringLiteral("aa00000000000000000000000000ff01")};
        all.tags = {QStringLiteral("t")};
        all.officialRatings = {QStringLiteral("R")};
        all.years = {2020};
        all.seriesStatus = {QStringLiteral("Ended")};
        all.episodeKind = EpisodeKind::Specials;
        QVERIFY(!all.isEmpty());
        QVERIFY(LibraryFilterState{}.isEmpty());

        const auto compose = [&all](LibraryScope scope) {
            return composeLibraryQuery({}, scope, all, LibrarySort{}).value();
        };
        ItemQuery movies = compose(LibraryScope::Movies);
        QCOMPARE(movies.filters, QStringLiteral("IsPlayed,IsResumable,IsFavorite"));
        QVERIFY(movies.is4K && movies.is3D && movies.isHd == true);
        QCOMPARE(movies.videoTypes, QStringList{QStringLiteral("BluRay")});
        QVERIFY(movies.seriesStatus.isEmpty());
        QCOMPARE(movies.parentIndexNumber, -1);

        // A series grid has no video quality or type, no Resumable, no episode kind; the series status is its own
        ItemQuery series = compose(LibraryScope::Series);
        QCOMPARE(series.filters, QStringLiteral("IsPlayed,IsFavorite"));
        QVERIFY(!series.is4K && !series.is3D && !series.isHd.has_value());
        QVERIFY(series.videoTypes.isEmpty());
        QCOMPARE(series.seriesStatus, QStringList{QStringLiteral("Ended")});
        QVERIFY(series.hasSubtitles && series.hasTrailer);
        QCOMPARE(series.parentIndexNumber, -1);
        QCOMPARE(series.genreIds.size(), 1);
        QCOMPARE(series.years, QList<int>{2020});

        // Episodes keep quality and type and own the episode kind, no series status
        ItemQuery episodes = compose(LibraryScope::Episodes);
        QCOMPARE(episodes.filters, QStringLiteral("IsPlayed,IsFavorite"));
        QVERIFY(episodes.is4K && episodes.is3D);
        QCOMPARE(episodes.videoTypes.size(), 1);
        QVERIFY(episodes.seriesStatus.isEmpty());
        QCOMPARE(episodes.parentIndexNumber, 0);

        // Collections keep the status, genres, tags, ratings and years only
        ItemQuery collections = compose(LibraryScope::Collections);
        QCOMPARE(collections.filters, QStringLiteral("IsPlayed,IsFavorite"));
        QVERIFY(!collections.hasSubtitles && !collections.hasTrailer);
        QVERIFY(!collections.is4K && !collections.isHd.has_value());
        QVERIFY(collections.videoTypes.isEmpty() && collections.seriesStatus.isEmpty());
        QCOMPARE(collections.tags.size(), 1);
        QCOMPARE(collections.officialRatings.size(), 1);

        // The other episode kinds map to their own parameter
        LibraryFilterState kind;
        kind.episodeKind = EpisodeKind::Missing;
        ItemQuery query = composeLibraryQuery({}, LibraryScope::Episodes, kind, {}).value();
        QVERIFY(query.isMissing && !query.isUnaired && query.parentIndexNumber == -1);
        kind.episodeKind = EpisodeKind::Unaired;
        query = composeLibraryQuery({}, LibraryScope::Episodes, kind, {}).value();
        QVERIFY(query.isUnaired && !query.isMissing && query.parentIndexNumber == -1);
        // and is ignored outside the episode view
        query = composeLibraryQuery({}, LibraryScope::Movies, kind, {}).value();
        QVERIFY(!query.isUnaired && !query.isMissing);
    }

    void librarySortKeysPerTabAndRandomIsOnePage()
    {
        using namespace Kinefin;
        // Movies, series and collections have the lists of jellyfin-web 12.4, in its order
        QCOMPARE(librarySortKeys(LibraryScope::Movies),
                 (QStringList{QStringLiteral("SortName"), QStringLiteral("Random"),
                              QStringLiteral("CommunityRating"), QStringLiteral("CriticRating"),
                              QStringLiteral("DateCreated"), QStringLiteral("DatePlayed"),
                              QStringLiteral("OfficialRating"), QStringLiteral("PlayCount"),
                              QStringLiteral("PremiereDate"), QStringLiteral("Runtime")}));
        QCOMPARE(
            librarySortKeys(LibraryScope::Series),
            (QStringList{QStringLiteral("SortName"), QStringLiteral("Random"),
                         QStringLiteral("CommunityRating"), QStringLiteral("DateCreated"),
                         QStringLiteral("DateLastContentAdded"), QStringLiteral("SeriesDatePlayed"),
                         QStringLiteral("OfficialRating"), QStringLiteral("PremiereDate")}));
        QCOMPARE(librarySortKeys(LibraryScope::Collections),
                 (QStringList{QStringLiteral("SortName"), QStringLiteral("CommunityRating"),
                              QStringLiteral("DateCreated"), QStringLiteral("OfficialRating"),
                              QStringLiteral("PremiereDate")}));
        QCOMPARE(librarySortKeys(LibraryScope::Episodes).size(), 7);
        QCOMPARE(librarySortKeys(LibraryScope::Episodes).constFirst(),
                 QStringLiteral("AiredEpisodeOrder"));
        for (const LibraryScope scope : {LibraryScope::Movies, LibraryScope::Series,
                                         LibraryScope::Episodes, LibraryScope::Collections}) {
            const QStringList keys = librarySortKeys(scope);
            QCOMPARE(QSet<QString>(keys.cbegin(), keys.cend()).size(), keys.size());
            QCOMPARE(keys.contains(QStringLiteral("Random")), scope != LibraryScope::Collections);
        }
        // The keys jellyfin-web does not list are gone, and a Random page is refused where it is not offered
        QVERIFY(!librarySortKeys(LibraryScope::Movies).contains(QStringLiteral("ProductionYear")));
        QVERIFY(!librarySortKeys(LibraryScope::Series).contains(QStringLiteral("DatePlayed")));
        QVERIFY(!librarySortKeys(LibraryScope::Series).contains(QStringLiteral("PlayCount")));
        QVERIFY(
            !composeLibraryQuery({}, LibraryScope::Collections, {}, {QStringLiteral("Random")}));
        // A key of another tab is not offered
        QVERIFY(
            !composeLibraryQuery({}, LibraryScope::Series, {}, {QStringLiteral("CriticRating")}));
        QVERIFY(
            !composeLibraryQuery({}, LibraryScope::Collections, {}, {QStringLiteral("Runtime")}));
        QVERIFY(!composeLibraryQuery({}, LibraryScope::Movies, {}, {QStringLiteral("sortname")}));

        // Name sorts alone, the others keep SortName as the tie-break; the direction applies to the whole
        ItemQuery query =
            composeLibraryQuery({}, LibraryScope::Movies, {}, {QStringLiteral("SortName"), true})
                .value();
        QCOMPARE(query.sortBy, QStringLiteral("SortName"));
        QCOMPARE(query.sortOrder, QStringLiteral("Descending"));
        query = composeLibraryQuery({}, LibraryScope::Series, {},
                                    {QStringLiteral("DateLastContentAdded"), false}, 24, 12)
                    .value();
        QCOMPARE(query.sortBy, QStringLiteral("DateLastContentAdded,SortName"));
        QCOMPARE(query.sortOrder, QStringLiteral("Ascending"));
        QCOMPARE(query.startIndex, 24);
        QCOMPARE(query.limit, 12);

        // Random: first page only, at most RandomPageSize, whatever the caller asked
        const LibrarySort random{QStringLiteral("Random"), true};
        query = composeLibraryQuery({}, LibraryScope::Movies, {}, random).value();
        QCOMPARE(query.sortBy, QStringLiteral("Random"));
        QCOMPARE(query.sortOrder, QStringLiteral("Ascending"));
        QCOMPARE(query.startIndex, 0);
        QCOMPARE(query.limit, RandomPageSize);
        QCOMPARE(RandomPageSize, 100);
        query = composeLibraryQuery({}, LibraryScope::Movies, {}, random, 0, 500).value();
        QCOMPARE(query.limit, 100);
        query = composeLibraryQuery({}, LibraryScope::Movies, {}, random, 0, 24).value();
        QCOMPARE(query.limit, 24);
        QVERIFY(!composeLibraryQuery({}, LibraryScope::Movies, {}, random, 100, 100));
        QVERIFY(!composeLibraryQuery({}, LibraryScope::Movies, {}, random, 1, 24));
    }

    void randomReachesTheWire()
    {
        using namespace Kinefin;
        FakeJellyfin server;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &JellyfinClient::itemsReady);
        ItemQuery base;
        base.includeItemTypes = QStringLiteral("Series");
        const auto send = [&](const LibrarySort &sort, int start, int limit) {
            const auto query =
                composeLibraryQuery(base, LibraryScope::Series, {}, sort, start, limit);
            const int expected = ready.size() + 1;
            if (!query || !client.fetchItems(*query)
                || !QTest::qWaitFor([&] { return ready.size() == expected; }, 5000))
                return QUrlQuery();
            return FakeJellyfin::capturedListingQuery(server.itemsRoutes.constLast());
        };

        const QUrlQuery wire = send({QStringLiteral("Random"), false}, -1, -1);
        QCOMPARE(wire.queryItemValue(QStringLiteral("sortBy")), QStringLiteral("Random"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("100"));
    }

    void libraryFilterValuesAreRefusedWithoutARequest()
    {
        using namespace Kinefin;
        FakeJellyfin server;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &JellyfinClient::itemsReady);
        QSignalSpy failures(&client, &JellyfinClient::listingFailed);
        ItemQuery good;
        good.includeItemTypes = QStringLiteral("Movie");
        QVERIFY(client.fetchItems(good));
        QTRY_COMPARE(ready.size(), 1);
        const auto refuse = [&](const char *what, const std::function<void(ItemQuery &)> &mutate) {
            ItemQuery query = good;
            mutate(query);
            const int before = server.listingRequests;
            QVERIFY2(!client.fetchItems(query), what);
            QCOMPARE(server.listingRequests, before);
        };
        const auto many = [](int count, const QString &text) { return QStringList(count, text); };
        refuse("tag with the delimiter", [](ItemQuery &q) { q.tags = {QStringLiteral("a|b")}; });
        refuse("empty tag", [](ItemQuery &q) { q.tags = {QString()}; });
        refuse("control character in a tag",
               [](ItemQuery &q) { q.tags = {QStringLiteral("a\nb")}; });
        refuse("oversized tag", [](ItemQuery &q) { q.tags = {QString(129, QLatin1Char('a'))}; });
        refuse("too many tags", [&](ItemQuery &q) { q.tags = many(51, QStringLiteral("t")); });
        refuse("rating with the delimiter",
               [](ItemQuery &q) { q.officialRatings = {QStringLiteral("PG|R")}; });
        refuse("too many ratings",
               [&](ItemQuery &q) { q.officialRatings = many(51, QStringLiteral("R")); });
        refuse("year zero", [](ItemQuery &q) { q.years = {0}; });
        refuse("year past 9999", [](ItemQuery &q) { q.years = {10000}; });
        refuse("too many years", [](ItemQuery &q) {
            for (int year = 2000; year <= 2050; ++year)
                q.years.append(year);
        });
        refuse("unknown video type",
               [](ItemQuery &q) { q.videoTypes = {QStringLiteral("Floppy")}; });
        refuse("video type list injection",
               [](ItemQuery &q) { q.videoTypes = {QStringLiteral("Dvd,BluRay")}; });
        refuse("unknown series status",
               [](ItemQuery &q) { q.seriesStatus = {QStringLiteral("x")}; });
        refuse("bad studio id", [](ItemQuery &q) { q.studioIds = {QStringLiteral("../x")}; });
        refuse("too many studio ids", [&](ItemQuery &q) {
            q.studioIds = many(51, QStringLiteral("aa00000000000000000000000000ff11"));
        });
        refuse("genre id with the delimiter",
               [](ItemQuery &q) { q.genreIds = {QStringLiteral("aa|bb")}; });
        refuse("negative parent index", [](ItemQuery &q) { q.parentIndexNumber = -2; });
        // Each text is valid, the whole URL is too long: refused, and the pending request of the channel survives
        server.holdFirstListings = 1;
        QVERIFY(client.fetchItems(good, 9));
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Items")).size(), 2);
        ItemQuery huge = good;
        huge.tags = many(50, QString(128, QChar(0x00e9)));
        const int before = server.listingRequests;
        QVERIFY(!client.fetchItems(huge, 9));
        QCOMPARE(server.listingRequests, before);
        // The held request was not superseded by the refused call: the fake's empty answer to it is reported as a failure of its channel
        server.releaseHeld();
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(3).toInt(), 9);
        QCOMPARE(ready.size(), 1);
    }

    void theItemsQueryLengthIsTheBoundFetchItemsEnforces()
    {
        using namespace Kinefin;
        FakeJellyfin server;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &JellyfinClient::itemsReady);
        ItemQuery query;
        query.includeItemTypes = QStringLiteral("Movie");
        const int base = client.itemsQueryChars(query);
        QVERIFY(base > 0 && base < MaxItemsQueryChars);
        // The count is of the wire form: a '+' goes as %2B and a three-byte character as nine characters
        ItemQuery plus = query;
        plus.searchTerm = QStringLiteral("C++");
        ItemQuery plain = query;
        plain.searchTerm = QStringLiteral("Cab");
        QCOMPARE(client.itemsQueryChars(plus), client.itemsQueryChars(plain) + 4);

        // Tags are added up to the last one that fits: the client sends that query and refuses the next
        QStringList tags;
        const auto tagAt = [](int index) {
            return QString(125, QChar(0x00e9))
                   + QStringLiteral("%1").arg(index, 3, 10, QLatin1Char('0'));
        };
        for (int i = 0; i < MaxFilterValues; ++i) {
            tags.append(tagAt(i));
            query.tags = tags;
            if (client.itemsQueryChars(query) > MaxItemsQueryChars)
                break;
        }
        QVERIFY(tags.size() > 1 && tags.size() <= MaxFilterValues);
        QVERIFY(client.itemsQueryChars(query) > MaxItemsQueryChars);
        const int before = server.listingRequests;
        QVERIFY(!client.fetchItems(query));
        QCOMPARE(server.listingRequests, before);
        tags.removeLast();
        query.tags = tags;
        QVERIFY(client.itemsQueryChars(query) <= MaxItemsQueryChars);
        QVERIFY(client.fetchItems(query));
        QTRY_COMPARE(ready.size(), 1);
    }

    void legacyFiltersHaveTheirOwnFamilyAndSignals()
    {
        using namespace Kinefin;
        FakeJellyfin server;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &JellyfinClient::legacyFiltersReady);
        QSignalSpy failed(&client, &JellyfinClient::legacyFiltersFailed);
        QSignalSpy filters2(&client, &JellyfinClient::filtersReady);
        QSignalSpy listingFailures(&client, &JellyfinClient::listingFailed);
        const QString library = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
        QVERIFY(client.fetchLegacyFilters(library, QStringLiteral("Movie"), 3079));
        QVERIFY(client.fetchFilters(library, QStringLiteral("Movie"), 3079));
        QTRY_COMPARE(ready.size(), 1);
        QTRY_COMPARE(filters2.size(), 1);
        QCOMPARE(ready.constFirst().at(1).toInt(), 3079);
        const QJsonObject object = ready.constFirst().constFirst().value<QJsonObject>();
        QCOMPARE(object.value(QStringLiteral("OfficialRatings")).toArray().size(), 3);
        const QByteArray route = server.lastListingRoute.value(QStringLiteral("FiltersLegacy"));
        QVERIFY2(route.contains("/Items/Filters?"), route.constData());
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(route);
        QCOMPARE(wire.queryItemValue(QStringLiteral("parentId")), library);
        QCOMPARE(wire.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("Movie"));
        QVERIFY(wire.hasQueryItem(QStringLiteral("userId")));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("recursive")));
        QVERIFY(
            server.lastListingAuth.value(QStringLiteral("FiltersLegacy")).contains(kTokenField));

        // The bounded facets: bad entries dropped, no duplicates, the server's order
        const LegacyFacets facets = legacyFacetsFrom(object);
        QCOMPARE(facets.officialRatings,
                 (QStringList{QStringLiteral("PG"), QStringLiteral("PG-13"), QStringLiteral("R")}));
        QCOMPARE(facets.years, (QList<int>{2024, 2019, 2012}));
        QJsonArray ratings{QStringLiteral("R"), QStringLiteral("R"),    QStringLiteral("a|b"),
                           QStringLiteral(""),  QStringLiteral("x\ny"), 5};
        QJsonArray years{2020, 2020, 0, 10000, QStringLiteral("2021"), 1999.5, 1999};
        for (int i = 0; i < 400; ++i) {
            ratings.append(QStringLiteral("r%1").arg(i));
            years.append(1000 + i);
        }
        const LegacyFacets bounded = legacyFacetsFrom(QJsonObject{
            {QStringLiteral("OfficialRatings"), ratings}, {QStringLiteral("Years"), years}});
        QCOMPARE(bounded.officialRatings.size(), MaxFacetOptions);
        QCOMPARE(bounded.officialRatings.constFirst(), QStringLiteral("R"));
        QCOMPARE(bounded.officialRatings.at(1), QStringLiteral("r0"));
        QCOMPARE(bounded.years.size(), MaxFacetOptions);
        QCOMPARE(bounded.years.at(0), 2020);
        QVERIFY(!bounded.years.contains(0) && !bounded.years.contains(10000));
        QVERIFY(legacyFacetsFrom({}).officialRatings.isEmpty());
        QVERIFY(legacyFacetsFrom({}).years.isEmpty());

        // A server without the endpoint: its own failure, with the status, and no generic listing failure
        server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 404);
        QVERIFY(client.fetchLegacyFilters({}, QStringLiteral("Series"), 3080));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().at(0).toInt(), 404);
        QCOMPARE(failed.constFirst().at(2).toInt(), 3080);
        QVERIFY(!FakeJellyfin::capturedListingQuery(
                     server.lastListingRoute.value(QStringLiteral("FiltersLegacy")))
                     .hasQueryItem(QStringLiteral("parentId")));
        server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 500);
        QVERIFY(client.fetchLegacyFilters({}, {}, 3080));
        QTRY_COMPARE(failed.size(), 2);
        QCOMPARE(failed.constLast().at(0).toInt(), 500);
        QCOMPARE(listingFailures.size(), 0);
        server.listingStatus.clear();
        server.listingRawBody.insert(QStringLiteral("FiltersLegacy"), "[1, 2]");
        QVERIFY(client.fetchLegacyFilters({}, {}, 3080));
        QTRY_COMPARE(failed.size(), 3);
        QCOMPARE(ready.size(), 1);
        server.listingRawBody.clear();

        // Invalid input is refused without a request
        const int before = server.listingRequests;
        QVERIFY(!client.fetchLegacyFilters(QStringLiteral("../x"), {}, 3079));
        QVERIFY(!client.fetchLegacyFilters(library, QStringLiteral("Movie,EvilType"), 3079));
        QCOMPARE(server.listingRequests, before);

        // A 401 expires the session
        server.denyListings = true;
        QVERIFY(client.fetchLegacyFilters({}, {}, 3079));
        QTRY_COMPARE(failed.size(), 4);
        QCOMPARE(failed.constLast().at(0).toInt(), 401);
        QVERIFY(!client.isAuthenticated());
    }

    void movieRecommendationsRoute()
    {
        using namespace Kinefin;
        FakeJellyfin server;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &JellyfinClient::movieRecommendationsReady);
        QSignalSpy failures(&client, &JellyfinClient::listingFailed);
        const QString library = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
        QVERIFY(client.fetchMovieRecommendations(library, 6, 20, 3074));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready.constFirst().at(1).toInt(), 3074);
        const QJsonArray categories = ready.constFirst().constFirst().value<QJsonArray>();
        QCOMPARE(categories.size(), 2);
        QCOMPARE(categories.at(0).toObject().value(QStringLiteral("Items")).toArray().size(), 1);
        const QByteArray route =
            server.lastListingRoute.value(QStringLiteral("MovieRecommendations"));
        QVERIFY2(route.contains("/Movies/Recommendations?"), route.constData());
        QUrlQuery wire = FakeJellyfin::capturedListingQuery(route);
        QCOMPARE(wire.queryItemValue(QStringLiteral("parentId")), library);
        QCOMPARE(wire.queryItemValue(QStringLiteral("categoryLimit")), QStringLiteral("6"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("itemLimit")), QStringLiteral("20"));
        QVERIFY(wire.hasQueryItem(QStringLiteral("userId")));
        // Every library: no parentId
        QVERIFY(client.fetchMovieRecommendations({}, 1, 1, 3074));
        QTRY_COMPARE(ready.size(), 2);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("MovieRecommendations")));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("parentId")));
        // The limits and the id are checked before the request
        const int before = server.listingRequests;
        QVERIFY(!client.fetchMovieRecommendations(library, 0, 20, 3074));
        QVERIFY(!client.fetchMovieRecommendations(library, 21, 20, 3074));
        QVERIFY(!client.fetchMovieRecommendations(library, 6, 0, 3074));
        QVERIFY(!client.fetchMovieRecommendations(library, 6, 101, 3074));
        QVERIFY(!client.fetchMovieRecommendations(QStringLiteral("a b"), 6, 20, 3074));
        QCOMPARE(server.listingRequests, before);
        // A malformed body fails under its family and channel
        server.listingRawBody.insert(QStringLiteral("MovieRecommendations"), "{\"Items\": []}");
        QVERIFY(client.fetchMovieRecommendations({}, 6, 20, 3074));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(0).toString(), QStringLiteral("MovieRecommendations"));
        QCOMPARE(failures.constFirst().at(3).toInt(), 3074);
        QCOMPARE(ready.size(), 2);
    }

    void suggestionsUpcomingAndStudiosRoutes()
    {
        using namespace Kinefin;
        FakeJellyfin server;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy suggestions(&client, &JellyfinClient::suggestionsReady);
        QSignalSpy upcoming(&client, &JellyfinClient::upcomingReady);
        QSignalSpy studios(&client, &JellyfinClient::studiosReady);
        QSignalSpy failures(&client, &JellyfinClient::listingFailed);
        const QString library = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");

        // Suggestions has no parentId: the answer is global
        QVERIFY(client.fetchSuggestions(QStringLiteral("Series"), 0, 24, 3075));
        QTRY_COMPARE(suggestions.size(), 1);
        QCOMPARE(suggestions.constFirst().at(1).toInt(), 42);
        QCOMPARE(suggestions.constFirst().at(2).toInt(), 3075);
        QByteArray route = server.lastListingRoute.value(QStringLiteral("Suggestions"));
        QVERIFY2(route.contains("/Items/Suggestions?"), route.constData());
        QUrlQuery wire = FakeJellyfin::capturedListingQuery(route);
        QCOMPARE(wire.queryItemValue(QStringLiteral("type")), QStringLiteral("Series"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("24"));
        QVERIFY(wire.hasQueryItem(QStringLiteral("userId")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("enableTotalRecordCount")),
                 QStringLiteral("true"));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("parentId")));
        const int before = server.listingRequests;
        QVERIFY(!client.fetchSuggestions({}, 0, 24, 3075));
        QVERIFY(!client.fetchSuggestions(QStringLiteral("Series,EvilType"), 0, 24, 3075));
        QVERIFY(!client.fetchSuggestions(QStringLiteral("Series,Movie"), 0, 24, 3075));
        QVERIFY(!client.fetchSuggestions(QStringLiteral("Series"), -2, 24, 3075));
        QVERIFY(!client.fetchSuggestions(QStringLiteral("Series"), 0, 0, 3075));
        QVERIFY(!client.fetchSuggestions(QStringLiteral("Series"), 0, 101, 3075));

        // Upcoming episodes, scoped by the library when given
        QVERIFY(client.fetchUpcomingEpisodes(library, 0, 30, 3076));
        QTRY_COMPARE(upcoming.size(), 1);
        QCOMPARE(upcoming.constFirst().at(1).toInt(), 1);
        QCOMPARE(upcoming.constFirst().at(2).toInt(), 3076);
        route = server.lastListingRoute.value(QStringLiteral("Upcoming"));
        QVERIFY2(route.contains("/Shows/Upcoming?"), route.constData());
        wire = FakeJellyfin::capturedListingQuery(route);
        QCOMPARE(wire.queryItemValue(QStringLiteral("parentId")), library);
        QCOMPARE(wire.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("30"));
        QVERIFY(wire.queryItemValue(QStringLiteral("enableImageTypes"))
                    .contains(QStringLiteral("Primary")));
        QVERIFY(client.fetchUpcomingEpisodes({}, -1, 5, 3076));
        QTRY_COMPARE(upcoming.size(), 2);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Upcoming")));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("parentId")));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("startIndex")));
        QVERIFY(!client.fetchUpcomingEpisodes(QStringLiteral("a/b"), 0, 30, 3076));
        QVERIFY(!client.fetchUpcomingEpisodes(library, 0, 0, 3076));
        QVERIFY(!client.fetchUpcomingEpisodes(library, 0, 101, 3076));
        QVERIFY(!client.fetchUpcomingEpisodes(library, -2, 10, 3076));

        // Studios (the networks of the series library)
        QVERIFY(client.fetchStudios(library, QStringLiteral("Series"), 0, 50, 3077));
        QTRY_COMPARE(studios.size(), 1);
        QCOMPARE(studios.constFirst().at(1).toInt(), 2);
        QCOMPARE(studios.constFirst().at(2).toInt(), 3077);
        QCOMPARE(studios.constFirst().constFirst().value<QJsonArray>().size(), 2);
        route = server.lastListingRoute.value(QStringLiteral("Studios"));
        QVERIFY2(route.contains("/Studios?"), route.constData());
        wire = FakeJellyfin::capturedListingQuery(route);
        QCOMPARE(wire.queryItemValue(QStringLiteral("parentId")), library);
        QCOMPARE(wire.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("Series"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("50"));
        QVERIFY(wire.hasQueryItem(QStringLiteral("userId")));
        QVERIFY(client.fetchStudios({}, {}, -1, -1, 3077));
        QTRY_COMPARE(studios.size(), 2);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Studios")));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("parentId")));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("includeItemTypes")));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("limit")));
        QVERIFY(!client.fetchStudios(QStringLiteral("a b"), {}, 0, 10, 3077));
        QVERIFY(!client.fetchStudios(library, QStringLiteral("Series,Evil"), 0, 10, 3077));
        QVERIFY(!client.fetchStudios(library, {}, -2, 10, 3077));
        QVERIFY(!client.fetchStudios(library, {}, 0, -2, 3077));
        QCOMPARE(server.listingRequests, before + 4);

        // Failures name their family and channel
        server.listingStatus.insert(QStringLiteral("Suggestions"), 500);
        server.listingStatus.insert(QStringLiteral("Upcoming"), 500);
        server.listingStatus.insert(QStringLiteral("Studios"), 500);
        QVERIFY(client.fetchSuggestions(QStringLiteral("Series"), 0, 24, 3075));
        QVERIFY(client.fetchUpcomingEpisodes({}, 0, 24, 3076));
        QVERIFY(client.fetchStudios({}, QStringLiteral("Series"), 0, 24, 3077));
        QTRY_COMPARE(failures.size(), 3);
        QSet<QString> families;
        QSet<int> channels;
        for (const QList<QVariant> &arguments : failures) {
            families.insert(arguments.at(0).toString());
            channels.insert(arguments.at(3).toInt());
        }
        QCOMPARE(families, (QSet<QString>{QStringLiteral("Suggestions"), QStringLiteral("Upcoming"),
                                          QStringLiteral("Studios")}));
        QCOMPARE(channels, (QSet<int>{3075, 3076, 3077}));
    }

    void releaseDropsTheSubTabFamilies()
    {
        using namespace Kinefin;
        FakeJellyfin server;
        QVERIFY(server.listen());
        JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy recommendations(&client, &JellyfinClient::movieRecommendationsReady);
        QSignalSpy suggestions(&client, &JellyfinClient::suggestionsReady);
        QSignalSpy upcoming(&client, &JellyfinClient::upcomingReady);
        QSignalSpy studios(&client, &JellyfinClient::studiosReady);
        QSignalSpy legacy(&client, &JellyfinClient::legacyFiltersReady);
        QSignalSpy legacyFailed(&client, &JellyfinClient::legacyFiltersFailed);
        QSignalSpy failures(&client, &JellyfinClient::listingFailed);
        server.holdListings = true;
        QVERIFY(client.fetchMovieRecommendations({}, 6, 20, 90001));
        QVERIFY(client.fetchSuggestions(QStringLiteral("Series"), 0, 24, 90001));
        QVERIFY(client.fetchUpcomingEpisodes({}, 0, 24, 90001));
        QVERIFY(client.fetchStudios({}, QStringLiteral("Series"), 0, 24, 90001));
        QVERIFY(client.fetchLegacyFilters({}, QStringLiteral("Movie"), 90001));
        // Another channel of the same families is not touched
        QVERIFY(client.fetchSuggestions(QStringLiteral("Series"), 0, 24, 90002));
        for (const QString &family : {QStringLiteral("MovieRecommendations"),
                                      QStringLiteral("Suggestions"), QStringLiteral("Upcoming"),
                                      QStringLiteral("Studios"), QStringLiteral("FiltersLegacy")})
            QTRY_VERIFY2(server.listingRoutes.value(family).size() >= 1, qPrintable(family));
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Suggestions")).size(), 2);
        client.releaseListingChannel(90001);
        server.holdListings = false;
        server.releaseHeld();
        // The other channel's held request was not superseded: the fake's empty answer to it is reported as a failure of that channel only
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(0).toString(), QStringLiteral("Suggestions"));
        QCOMPARE(failures.constFirst().at(3).toInt(), 90002);
        QTest::qWait(200);
        QCOMPARE(failures.size(), 1);
        QCOMPARE(suggestions.size(), 0);
        QCOMPARE(recommendations.size(), 0);
        QCOMPARE(upcoming.size(), 0);
        QCOMPARE(studios.size(), 0);
        QCOMPARE(legacy.size(), 0);
        QCOMPARE(legacyFailed.size(), 0);
        failures.clear();
        // The released channel is usable again
        QVERIFY(client.fetchStudios({}, QStringLiteral("Series"), 0, 24, 90001));
        QTRY_COMPARE(studios.size(), 1);
        QVERIFY(client.fetchLegacyFilters({}, QStringLiteral("Movie"), 90001));
        QTRY_COMPARE(legacy.size(), 1);
    }

    void aReleasedChannelCanBeUsedAgainWithoutAStaleAnswer()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::listingFailed);
        server.holdFirstListings = 1;
        QVERIFY(client.fetchItems(Kinefin::ItemQuery(), 7));
        QTRY_COMPARE(server.itemsRoutes.size(), 1);
        client.releaseListingChannel(7);
        // The same channel again answers once; the first request's late answer finds nothing waiting
        QVERIFY(client.fetchItems(Kinefin::ItemQuery(), 7));
        QTRY_COMPARE(ready.size(), 1);
        server.releaseHeld();
        QTest::qWait(250);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 0);
    }

    void listingFailureCarriesItsChannel()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        server.listingStatus.insert(QStringLiteral("Resume"), 500);
        QVERIFY(client.fetchResumeItems(5, 7));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(0).toString(), QStringLiteral("Resume"));
        QCOMPARE(failures.constFirst().at(1).toInt(), 500);
        QCOMPARE(failures.constFirst().at(3).toInt(), 7);
    }

    void cardQueriesPinFields()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const auto fieldsOf = [&](const QString &family) {
            return FakeJellyfin::capturedListingQuery(server.lastListingRoute.value(family))
                .queryItemValue(QStringLiteral("fields"));
        };
        const QString cardFields = QStringLiteral("PrimaryImageAspectRatio,DateCreated");
        QSignalSpy items(&client, &Kinefin::JellyfinClient::itemsReady);
        const auto movies = Kinefin::LibraryItemsModel::browseQuery(QStringLiteral("movies"));
        QVERIFY(movies);
        QVERIFY(client.fetchItems(*movies));
        QTRY_COMPARE(items.size(), 1);
        QCOMPARE(fieldsOf(QStringLiteral("Items")), cardFields);
        const auto series = Kinefin::LibraryItemsModel::browseQuery(QStringLiteral("tvshows"));
        QVERIFY(series);
        QVERIFY(client.fetchItems(*series));
        QTRY_COMPARE(items.size(), 2);
        QCOMPARE(fieldsOf(QStringLiteral("Items")),
                 QStringLiteral("ChildCount,RecursiveItemCount,") + cardFields);
        // A field the caller already named is not repeated
        Kinefin::ItemQuery named;
        named.cardImages = true;
        named.fields = {QStringLiteral("DateCreated")};
        QVERIFY(client.fetchItems(named));
        QTRY_COMPARE(items.size(), 3);
        QCOMPARE(fieldsOf(QStringLiteral("Items")),
                 QStringLiteral("DateCreated,PrimaryImageAspectRatio"));
        QSignalSpy resume(&client, &Kinefin::JellyfinClient::resumeItemsReady);
        QVERIFY(client.fetchResumeItems(10));
        QTRY_COMPARE(resume.size(), 1);
        QCOMPARE(fieldsOf(QStringLiteral("Resume")), cardFields);
        QSignalSpy latest(&client, &Kinefin::JellyfinClient::latestItemsReady);
        QVERIFY(client.fetchLatestItems({}, 10));
        QTRY_COMPARE(latest.size(), 1);
        QCOMPARE(fieldsOf(QStringLiteral("Latest")), cardFields);
        // Next Up has no New badge: it keeps the aspect ratio only
        QSignalSpy nextUp(&client, &Kinefin::JellyfinClient::nextUpReady);
        QVERIFY(client.fetchNextUp({}, {}, {}, false, 10));
        QTRY_COMPARE(nextUp.size(), 1);
        QCOMPARE(fieldsOf(QStringLiteral("NextUp")), QStringLiteral("PrimaryImageAspectRatio"));
    }

    void resumeSendsStartIndex()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy resume(&client, &Kinefin::JellyfinClient::resumeItemsReady);
        QVERIFY(client.fetchResumeItems(100, 8, 200));
        QTRY_COMPARE(resume.size(), 1);
        // The total of the envelope and the channel come back with the items
        QCOMPARE(resume.constFirst().at(1).toInt(), 2);
        QCOMPARE(resume.constFirst().at(2).toInt(), 8);
        QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Resume")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("200"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("100"));
        // The default sends none, 0 is sent, and a start before -1 is refused
        QVERIFY(client.fetchResumeItems(100));
        QTRY_COMPARE(resume.size(), 2);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Resume")));
        QVERIFY(!wire.hasQueryItem(QStringLiteral("startIndex")));
        QVERIFY(client.fetchResumeItems(100, 0, 0));
        QTRY_COMPARE(resume.size(), 3);
        wire = FakeJellyfin::capturedListingQuery(
            server.lastListingRoute.value(QStringLiteral("Resume")));
        QCOMPARE(wire.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
        QVERIFY(!client.fetchResumeItems(100, 0, -2));
    }

    void overCapReplyFails()
    {
        // An envelope padded to exactly the cap is read, one byte more is refused
        const QByteArray head =
            QByteArrayLiteral("{\"Items\":[],\"TotalRecordCount\":0,\"Pad\":\"");
        const QByteArray tail = QByteArrayLiteral("\"}");
        constexpr int cap = 1'048'576;
        for (const bool over : {false, true}) {
            FakeJellyfin server;
            QVERIFY(server.listen());
            server.listingRawBody.insert(
                QStringLiteral("Items"),
                head + QByteArray(cap + (over ? 1 : 0) - head.size() - tail.size(), 'x') + tail);
            Kinefin::JellyfinClient client;
            QVERIFY(login(client, server));
            QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
            QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
            QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
            QTRY_COMPARE(ready.size() + failures.size(), 1);
            QCOMPARE(ready.size(), over ? 0 : 1);
            if (over)
                QCOMPARE(failures.constFirst().at(0).toString(), QStringLiteral("Items"));
        }
    }

    void cardPageFitsReplyCap()
    {
        // 100 items with every field a card reads at its largest, as a server could send them
        const QString tag(64, QLatin1Char('t'));
        const QString id(32, QLatin1Char('a'));
        QJsonArray page;
        for (int row = 0; row < 100; ++row) {
            page.append(QJsonObject{
                {QStringLiteral("Id"), id},
                {QStringLiteral("Name"), QString(512, QLatin1Char('n'))},
                {QStringLiteral("Type"), QStringLiteral("Episode")},
                {QStringLiteral("SeriesName"), QString(512, QLatin1Char('s'))},
                {QStringLiteral("SeriesId"), id},
                {QStringLiteral("SeriesThumbImageTag"), tag},
                {QStringLiteral("SeriesPrimaryImageTag"), tag},
                {QStringLiteral("ParentBackdropItemId"), id},
                {QStringLiteral("ParentBackdropImageTags"), QJsonArray{tag}},
                {QStringLiteral("ParentThumbItemId"), id},
                {QStringLiteral("ParentThumbImageTag"), tag},
                {QStringLiteral("ImageTags"),
                 QJsonObject{{QStringLiteral("Primary"), tag}, {QStringLiteral("Thumb"), tag}}},
                {QStringLiteral("BackdropImageTags"), QJsonArray{tag}},
                {QStringLiteral("ProductionYear"), 2026},
                {QStringLiteral("ParentIndexNumber"), 99},
                {QStringLiteral("IndexNumber"), 99999},
                {QStringLiteral("IndexNumberEnd"), 99999},
                {QStringLiteral("RunTimeTicks"), 99'999'999'999'999.0},
                {QStringLiteral("CommunityRating"), 9.9},
                {QStringLiteral("DateCreated"), QStringLiteral("2026-11-10T12:34:56.1234567Z")},
                {QStringLiteral("ChildCount"), 999'999},
                {QStringLiteral("RecursiveItemCount"), 9'999'999},
                {QStringLiteral("PrimaryImageAspectRatio"), 0.6666666666666666},
                {QStringLiteral("LocationType"), QStringLiteral("Virtual")},
                {QStringLiteral("UserData"),
                 QJsonObject{{QStringLiteral("Played"), false},
                             {QStringLiteral("IsFavorite"), false},
                             {QStringLiteral("UnplayedItemCount"), 999'999},
                             {QStringLiteral("PlayedPercentage"), 99.99999999999},
                             {QStringLiteral("PlaybackPositionTicks"), 99'999'999'999'999.0}}}});
        }
        const QByteArray body =
            QJsonDocument(QJsonObject{{QStringLiteral("Items"), page},
                                      {QStringLiteral("StartIndex"), 0},
                                      {QStringLiteral("TotalRecordCount"), 100}})
                .toJson(QJsonDocument::Compact);
        constexpr int cap = 1'048'576;
        QVERIFY2(body.size() < cap / 4, qPrintable(QString::number(body.size())));
        // The cap is no obstacle in practice: the page arrives whole
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.listingRawBody.insert(QStringLiteral("Items"), body);
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::itemsReady);
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready.constFirst().constFirst().value<QJsonArray>().size(), 100);
    }

    void listingFailuresStayOffRequestFailed()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy requestFailures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy listingFailures(&client, &Kinefin::JellyfinClient::listingFailed);
        server.listingRawBody.insert(QStringLiteral("Items"), QByteArrayLiteral("[1,2]"));
        QVERIFY(client.fetchItems(Kinefin::ItemQuery()));
        QTRY_COMPARE(listingFailures.size(), 1);
        QCOMPARE(listingFailures.constFirst().at(0).toString(), QStringLiteral("Items"));
        QCOMPARE(requestFailures.size(), 0);
    }

    void isServerImageUrlRecognisesOnlyBuiltUrls()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(prepare(client, server));
        const QUrl base = client.serverUrl();
        const QUrl built =
            client.imageUrl(kPlayItem, QStringLiteral("Primary"), 400, QStringLiteral("abc123"));
        QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(base, built));
        QUrl otherPort = built;
        otherPort.setPort(base.port() + 1);
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(base, otherPort));
        QUrl otherHost = built;
        otherHost.setHost(QStringLiteral("example.invalid"));
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(base, otherHost));
        QUrl otherScheme = built;
        otherScheme.setScheme(QStringLiteral("https"));
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(base, otherScheme));
        QUrl credentials = built;
        credentials.setUserName(QStringLiteral("user"));
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(base, credentials));
        QUrl outside = built;
        outside.setPath(base.path() + QStringLiteral("/Users/Me"));
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(base, outside));
        QUrl traversal = built;
        traversal.setPath(base.path() + QStringLiteral("/Items/../Users/Me/Images/Primary"),
                          QUrl::StrictMode);
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(base, traversal));
        QUrl badId = built;
        badId.setPath(base.path() + QStringLiteral("/Items/bad id;x/Images/Primary"));
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(base, badId));
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(QUrl(), built));
        QUrl encodedSlash = built;
        encodedSlash.setPath(base.path() + QStringLiteral("/Items%2F") + kPlayItem
                                 + QStringLiteral("/Images/Primary"),
                             QUrl::StrictMode);
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(base, encodedSlash));
        // A base with an encoded character in its path still matches its own URLs
        const QUrl prefixed(base.toString() + QStringLiteral("/a%20b"));
        QUrl underPrefix = built;
        underPrefix.setPath(prefixed.path() + QStringLiteral("/Items/") + kPlayItem
                            + QStringLiteral("/Images/Primary"));
        QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(prefixed, underPrefix));
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(prefixed, built));
    }

    void imageUrlsAreTokenFreeAndValidated()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(prepare(client, server)); // unauthenticated is fine: the URL stays token-free
        const QUrl url =
            client.imageUrl(QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"),
                            QStringLiteral("Primary"), 360, QStringLiteral("arcturus-primary"));
        QCOMPARE(url, QUrl(server.url(QStringLiteral(
                          "/jellyfin/Items/f0e1d2c3b4a5968778695a4b3c2d1e0f/"
                          "Images/Primary?maxWidth=360&quality=90&tag=arcturus-primary"))));
        QVERIFY(!url.toString().contains(QLatin1String("Token"), Qt::CaseInsensitive));
        // Explicit quality and format:
        const QUrl webp = client.imageUrl(QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"),
                                          QStringLiteral("Backdrop"), 1920, QStringLiteral("ab"),
                                          85, QStringLiteral("webp"));
        QCOMPARE(webp.query().contains(QLatin1String("maxWidth=1920")), true);
        QCOMPARE(webp.query().contains(QLatin1String("format=webp")), true);
        // Refusals: unknown type, bad size, bad tag charset, unknown format, bad id
        QVERIFY(client
                    .imageUrl(QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"),
                              QStringLiteral("Wallpaper"), 360, {})
                    .isEmpty());
        QVERIFY(client
                    .imageUrl(QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"),
                              QStringLiteral("Primary"), 0, {})
                    .isEmpty());
        QVERIFY(client
                    .imageUrl(QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"),
                              QStringLiteral("Primary"), 360, QStringLiteral("tag?x"))
                    .isEmpty());
        QVERIFY(client
                    .imageUrl(QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"),
                              QStringLiteral("Primary"), 360, {}, 90, QStringLiteral("bpg"))
                    .isEmpty());
        QVERIFY(client.imageUrl(QStringLiteral("no token"), QStringLiteral("Primary"), 360, {})
                    .isEmpty());
    }

    void loginKeepsUserNameAndImageTag()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.users = {{kAliceId, "tokenalice", QStringLiteral("Alice"), QStringLiteral("tag-a1")},
                        {kBobId, "tokenbob", QStringLiteral("Bob"), {}},
                        {QStringLiteral("ccccccccccccccccccccccccccccccc3"), "tokenc",
                         QStringLiteral("Line\nBreak"), QStringLiteral("bad tag")},
                        {QStringLiteral("ddddddddddddddddddddddddddddddd4"),
                         "tokend",
                         QString(Kinefin::MaxUserNameChars + 1, QLatin1Char('n')),
                         {}}};
        Kinefin::JellyfinClient client;
        // The fields are already set when loginSucceeded fires
        QString nameAtSignal;
        QString tagAtSignal;
        connect(&client, &Kinefin::JellyfinClient::loginSucceeded, this, [&] {
            nameAtSignal = client.userName();
            tagAtSignal = client.userImageTag();
        });
        QVERIFY(loginAs(client, server, QStringLiteral("Alice")));
        QCOMPARE(client.userId(), kAliceId);
        QCOMPARE(client.userName(), QStringLiteral("Alice"));
        QCOMPARE(client.userImageTag(), QStringLiteral("tag-a1"));
        QCOMPARE(nameAtSignal, QStringLiteral("Alice"));
        QCOMPARE(tagAtSignal, QStringLiteral("tag-a1"));

        // Signing out of memory forgets both
        client.forgetSession();
        QVERIFY(client.userName().isEmpty());
        QVERIFY(client.userImageTag().isEmpty());

        // No avatar: the tag is empty; the name stays
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QVERIFY(client.authenticate(QStringLiteral("Bob"), QStringLiteral("x")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QCOMPARE(client.userName(), QStringLiteral("Bob"));
        QVERIFY(client.userImageTag().isEmpty());

        // A multi-line name, an unsafe tag and an over-long name are each dropped, the login still works
        client.forgetSession();
        QVERIFY(client.authenticate(QStringLiteral("Line\nBreak"), QStringLiteral("x")));
        QTRY_COMPARE(loggedIn.size(), 2);
        QVERIFY(client.isAuthenticated());
        QVERIFY(client.userName().isEmpty());
        QVERIFY(client.userImageTag().isEmpty());
        client.forgetSession();
        QVERIFY(client.authenticate(server.users.at(3).name, QStringLiteral("x")));
        QTRY_COMPARE(loggedIn.size(), 3);
        QVERIFY(client.userName().isEmpty());
    }

    void verifySessionRefreshesUserName()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.users = {{kAliceId, "tokenalice", QStringLiteral("Alice"), {}}};
        Kinefin::JellyfinClient client;
        QVERIFY(loginAs(client, server, QStringLiteral("Alice")));
        QSignalSpy refreshed(&client, &Kinefin::JellyfinClient::userProfileRefreshed);
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);

        server.users[0].name = QStringLiteral("Alice B");
        server.users[0].tag = QStringLiteral("newtag");
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 1);
        QCOMPARE(refreshed.constFirst().at(0).toString(), kAliceId);
        QCOMPARE(refreshed.constFirst().at(1).toString(), QStringLiteral("Alice B"));
        QCOMPARE(refreshed.constFirst().at(2).toString(), QStringLiteral("newtag"));
        QCOMPARE(client.userName(), QStringLiteral("Alice B"));
        QCOMPARE(client.userImageTag(), QStringLiteral("newtag"));

        // An invalid name keeps the last good one, a removed avatar clears the tag
        server.users[0].name = QStringLiteral("Bad\nName");
        server.users[0].tag.clear();
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 2);
        QCOMPARE(client.userName(), QStringLiteral("Alice B"));
        QVERIFY(client.userImageTag().isEmpty());
        QCOMPARE(expired.size(), 0);

        // A reply for another user than the session's is a stale or foreign token: it expires like a 401, refreshing nothing
        server.users[0].id = kBobId;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(expired.size(), 1);
        QCOMPARE(expired.constFirst().constFirst().toString(), kAliceId);
        QCOMPARE(refreshed.size(), 2);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(client.userId().isEmpty());
    }

    void restoredSessionLearnsItsNameFromVerify()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.users = {{kAliceId, "tokenalice", QStringLiteral("Alice"), QStringLiteral("t1")}};
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.restoreSession(kAliceId, QStringLiteral("tokenalice")));
        QVERIFY(client.userName().isEmpty());
        QSignalSpy refreshed(&client, &Kinefin::JellyfinClient::userProfileRefreshed);
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 1);
        QCOMPARE(client.userName(), QStringLiteral("Alice"));
        QCOMPARE(client.userImageTag(), QStringLiteral("t1"));
    }

    void loginReadsTheAdministratorFlag()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(!client.isAdministrator());
        // The flag is already set when loginSucceeded fires
        bool flagAtSignal = false;
        connect(&client, &Kinefin::JellyfinClient::loginSucceeded, this,
                [&] { flagAtSignal = client.isAdministrator(); });
        server.administrator = true;
        QVERIFY(login(client, server));
        QVERIFY(client.isAdministrator());
        QVERIFY(flagAtSignal);

        // false, a missing field and a value that is not a bool all read as not an administrator
        const QList<QJsonValue> notAdministrator{false, QJsonValue(QJsonValue::Undefined),
                                                 QStringLiteral("true"), 1, QJsonValue::Null};
        for (const QJsonValue &value : notAdministrator) {
            client.forgetSession();
            QVERIFY(!client.isAdministrator());
            server.administratorRaw = value;
            QVERIFY(login(client, server));
            QVERIFY2(!client.isAdministrator(), qPrintable(QString::number(value.type())));
        }
        client.forgetSession();
        server.administratorRaw = true;
        QVERIFY(login(client, server));
        QVERIFY(client.isAdministrator());
        client.disconnectFromServer();
        QVERIFY(!client.isAdministrator());
    }

    void downloadPolicyComesFromTheLogin()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(!client.canDownload());
        bool flagAtSignal = false;
        connect(&client, &Kinefin::JellyfinClient::loginSucceeded, this,
                [&] { flagAtSignal = client.canDownload(); });
        server.canDownload = true;
        QVERIFY(login(client, server));
        QVERIFY(client.canDownload());
        QVERIFY(flagAtSignal);

        const QList<QJsonValue> cannotDownload{false, QJsonValue(QJsonValue::Undefined),
                                               QStringLiteral("true"), 1, QJsonValue::Null};
        for (const QJsonValue &value : cannotDownload) {
            client.forgetSession();
            QVERIFY(!client.canDownload());
            server.canDownloadRaw = value;
            QVERIFY(login(client, server));
            QVERIFY2(!client.canDownload(), qPrintable(QString::number(value.type())));
        }
        client.forgetSession();
        server.canDownloadRaw.reset();
        server.canDownload = true;
        QVERIFY(login(client, server));
        QVERIFY(client.canDownload());
        client.disconnectFromServer();
        QVERIFY(!client.canDownload());
    }

    void verifySessionRefreshesTheDownloadPolicy()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.users = {{kAliceId, "tokenalice", QStringLiteral("Alice"), {}}};
        Kinefin::JellyfinClient client;
        QVERIFY(loginAs(client, server, QStringLiteral("Alice")));
        QVERIFY(client.canDownload());
        QSignalSpy refreshed(&client, &Kinefin::JellyfinClient::userProfileRefreshed);
        server.canDownload = false;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 1);
        QVERIFY(!client.canDownload());
        server.canDownload = true;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 2);
        QVERIFY(client.canDownload());
        server.canDownloadRaw = QStringLiteral("yes");
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 3);
        QVERIFY(!client.canDownload());
    }

    void remoteSubtitlesNeedThePolicy()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.remoteSubtitleSearchBody =
            QByteArrayLiteral("[{\"Id\":\"abc123\",\"Name\":\"English\","
                              "\"ProviderName\":\"OpenSubtitles\",\"Format\":\"srt\"}]");
        Kinefin::JellyfinClient client;
        QVERIFY(!client.canManageSubtitles());
        QVERIFY(login(client, server));
        QVERIFY(client.canManageSubtitles());
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::remoteSubtitlesReady);
        QVERIFY(client.searchRemoteSubtitles(kPlayItem, QStringLiteral("eng")));
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(ready.first().at(0).toString(), kPlayItem);
        QCOMPARE(ready.first().at(1).toString(), QStringLiteral("eng"));
        QCOMPARE(ready.first().at(2).toJsonArray().size(), 1);
        QCOMPARE(server.remoteSubtitleSearches.constLast(), QByteArray("eng"));
        QVERIFY(!client.searchRemoteSubtitles(kPlayItem, QStringLiteral("../x")));

        QSignalSpy downloaded(&client, &Kinefin::JellyfinClient::remoteSubtitleDownloaded);
        QVERIFY(client.downloadRemoteSubtitle(kPlayItem, QStringLiteral("abc123")));
        QTRY_COMPARE(downloaded.count(), 1);
        QCOMPARE(server.remoteSubtitleDownloads.constLast(), QByteArray("abc123"));
        QVERIFY(!client.downloadRemoteSubtitle(kPlayItem, QStringLiteral("../escape")));
        QVERIFY(!client.downloadRemoteSubtitle(kPlayItem, QStringLiteral("..")));
        QVERIFY(!client.downloadRemoteSubtitle(kPlayItem, QStringLiteral(".")));

        client.forgetSession();
        QVERIFY(!client.canManageSubtitles());
        server.canManageSubtitles = false;
        QVERIFY(login(client, server));
        QVERIFY(!client.canManageSubtitles());
        QVERIFY(!client.searchRemoteSubtitles(kPlayItem, QStringLiteral("eng")));
        client.forgetSession();
        server.canManageSubtitlesRaw = QStringLiteral("yes");
        QVERIFY(login(client, server));
        QVERIFY(!client.canManageSubtitles());
        client.disconnectFromServer();
    }

    // 12.4 lists a downloaded file only after its queued refresh: the client waits for the item to show it before saying it is there
    void aDownloadedSubtitleIsAnnouncedOnceTheServerListsIt()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.externalSubtitleStreams = 0;
        server.remoteSubtitleVisibleAfterReads = 2;
        Kinefin::JellyfinClient client;
        client.setRemoteSubtitleSaveTiming(20, 10);
        QVERIFY(login(client, server));
        QSignalSpy downloaded(&client, &Kinefin::JellyfinClient::remoteSubtitleDownloaded);
        QSignalSpy notSaved(&client, &Kinefin::JellyfinClient::remoteSubtitleNotSaved);
        QVERIFY(client.downloadRemoteSubtitle(kPlayItem, QStringLiteral("abc123")));
        QTRY_COMPARE(downloaded.size(), 1);
        QCOMPARE(downloaded.first().at(0).toString(), kPlayItem);
        QCOMPARE(notSaved.size(), 0);
        // One read before the download, then the polls until the third read after it shows the file
        QCOMPARE(server.mediaStreamsReads, 4);
        QCOMPARE(server.externalSubtitleStreams, 1);
    }

    // A 204 for a file the server could not save (a read-only media folder) ends in remoteSubtitleNotSaved, once the wait is over
    void aSubtitleTheServerDidNotSaveIsReported()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.externalSubtitleStreams = 0;
        server.remoteSubtitleSaves = false;
        Kinefin::JellyfinClient client;
        client.setRemoteSubtitleSaveTiming(20, 3);
        QVERIFY(login(client, server));
        QSignalSpy downloaded(&client, &Kinefin::JellyfinClient::remoteSubtitleDownloaded);
        QSignalSpy notSaved(&client, &Kinefin::JellyfinClient::remoteSubtitleNotSaved);
        QVERIFY(client.downloadRemoteSubtitle(kPlayItem, QStringLiteral("abc123")));
        QTRY_COMPARE(notSaved.size(), 1);
        QCOMPARE(notSaved.first().at(1).toString(), QStringLiteral("abc123"));
        QCOMPARE(downloaded.size(), 0);
        QCOMPARE(server.mediaStreamsReads, 4);
        // A newer download ends the wait of the one before: only the newer one answers
        server.remoteSubtitleSaves = true;
        server.remoteSubtitleVisibleAfterReads = 1;
        QVERIFY(client.downloadRemoteSubtitle(kPlayItem, QStringLiteral("old111")));
        QVERIFY(client.downloadRemoteSubtitle(kPlayItem, QStringLiteral("new222")));
        QTRY_COMPARE(downloaded.size(), 1);
        QCOMPARE(downloaded.first().at(1).toString(), QStringLiteral("new222"));
        QTest::qWait(200);
        QCOMPARE(downloaded.size(), 1);
        QCOMPARE(notSaved.size(), 1);
    }

    void remoteSubtitleFailuresAreReported()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::remoteSubtitlesReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::remoteSubtitlesFailed);

        server.remoteSubtitleSearchStatus = 500;
        QVERIFY(client.searchRemoteSubtitles(kPlayItem, QStringLiteral("eng")));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.first().at(0).toString(), kPlayItem);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("eng"));
        QCOMPARE(failed.takeFirst().at(2).toInt(), 500);
        QCOMPARE(ready.size(), 0);

        // A body that is not an array of objects breaks the contract, not just the JSON syntax
        server.remoteSubtitleSearchStatus = 0;
        server.remoteSubtitleSearchBody = QByteArrayLiteral("{}");
        QVERIFY(client.searchRemoteSubtitles(kPlayItem, QStringLiteral("eng")));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(ready.size(), 0);
        server.remoteSubtitleSearchBody = QByteArrayLiteral("[\"not an object\"]");
        QVERIFY(client.searchRemoteSubtitles(kPlayItem, QStringLiteral("eng")));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(ready.size(), 0);

        QSignalSpy downloadFailed(&client, &Kinefin::JellyfinClient::remoteSubtitleDownloadFailed);
        server.remoteSubtitleDownloadStatus = 500;
        QVERIFY(client.downloadRemoteSubtitle(kPlayItem, QStringLiteral("abc123")));
        QTRY_COMPARE(downloadFailed.size(), 1);
        QCOMPARE(downloadFailed.first().at(0).toString(), kPlayItem);
        QCOMPARE(downloadFailed.first().at(1).toString(), QStringLiteral("abc123"));
        QCOMPARE(downloadFailed.first().at(2).toInt(), 500);

        // A 401 is the session ending, not a search failure the dialog could explain
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        const int failuresBefore401 = failed.size();
        server.remoteSubtitleSearchStatus = 401;
        QVERIFY(client.searchRemoteSubtitles(kPlayItem, QStringLiteral("eng")));
        QTRY_COMPARE(expired.size(), 1);
        QVERIFY(!client.isAuthenticated());
        QCOMPARE(failed.size(), failuresBefore401);
    }

    void downloadUrlNeedsAnAuthenticatedSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(client.downloadUrl(kPlayItem).isEmpty());
        QVERIFY(login(client, server));
        const QUrl url = client.downloadUrl(kPlayItem);
        QVERIFY(url.isValid());
        QVERIFY(url.path().endsWith(QStringLiteral("/Items/%1/Download").arg(kPlayItem)));
        QVERIFY(!url.hasQuery());
        QVERIFY(!url.toString().contains(QStringLiteral("Token")));
        QVERIFY(client.downloadUrl(QStringLiteral("../escape")).isEmpty());
        QVERIFY(client.downloadUrl(QString()).isEmpty());
        client.forgetSession();
        QVERIFY(client.downloadUrl(kPlayItem).isEmpty());
    }

    void subtitleFileUrlIsBuiltOnlyForValidInput()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        const QString source = QStringLiteral("0123abcd-ef01-2345-6789-abcdef012345");
        QVERIFY(client.subtitleFileUrl(kPlayItem, source, 2, QStringLiteral("srt")).isEmpty());
        QVERIFY(login(client, server));
        const QUrl url = client.subtitleFileUrl(kPlayItem, source, 2, QStringLiteral("srt"));
        QVERIFY(url.isValid());
        QVERIFY(url.path().endsWith(
            QStringLiteral("/Videos/%1/%2/Subtitles/2/0/Stream.srt").arg(kPlayItem, source)));
        QVERIFY(!url.hasQuery());
        QVERIFY(!url.toString().contains(QStringLiteral("Token")));
        for (const QString &extension :
             {QStringLiteral("ass"), QStringLiteral("ssa"), QStringLiteral("vtt")})
            QVERIFY(!client.subtitleFileUrl(kPlayItem, source, 0, extension).isEmpty());
        QVERIFY(!client.subtitleFileUrl(kPlayItem, source, 9999, QStringLiteral("srt")).isEmpty());

        QVERIFY(
            client.subtitleFileUrl(QStringLiteral("../escape"), source, 2, QStringLiteral("srt"))
                .isEmpty());
        QVERIFY(client.subtitleFileUrl(QString(), source, 2, QStringLiteral("srt")).isEmpty());
        QVERIFY(client.subtitleFileUrl(kPlayItem, QStringLiteral("a/b"), 2, QStringLiteral("srt"))
                    .isEmpty());
        QVERIFY(client.subtitleFileUrl(kPlayItem, QString(), 2, QStringLiteral("srt")).isEmpty());
        QVERIFY(client.subtitleFileUrl(kPlayItem, source, -1, QStringLiteral("srt")).isEmpty());
        QVERIFY(client.subtitleFileUrl(kPlayItem, source, 10000, QStringLiteral("srt")).isEmpty());
        for (const QString &extension :
             {QString(), QStringLiteral("sub"), QStringLiteral("SRT"), QStringLiteral("srt/../x"),
              QStringLiteral("srt?x=1"), QStringLiteral("webvtt")})
            QVERIFY(client.subtitleFileUrl(kPlayItem, source, 2, extension).isEmpty());
        client.forgetSession();
        QVERIFY(client.subtitleFileUrl(kPlayItem, source, 2, QStringLiteral("srt")).isEmpty());
    }

    void verifySessionRefreshesTheAdministratorFlag()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.users = {{kAliceId, "tokenalice", QStringLiteral("Alice"), {}}};
        Kinefin::JellyfinClient client;
        QVERIFY(loginAs(client, server, QStringLiteral("Alice")));
        QVERIFY(client.isAdministrator());
        QSignalSpy refreshed(&client, &Kinefin::JellyfinClient::userProfileRefreshed);

        // The flag is set when the signal fires; a demotion on the server shows on the next re-check
        bool flagAtSignal = true;
        connect(&client, &Kinefin::JellyfinClient::userProfileRefreshed, this,
                [&] { flagAtSignal = client.isAdministrator(); });
        server.users[0].administrator = false;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 1);
        QVERIFY(!client.isAdministrator());
        QVERIFY(!flagAtSignal);
        server.users[0].administrator = true;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 2);
        QVERIFY(client.isAdministrator());

        // A non-bool or missing flag in the reply reads as false
        server.administratorRaw = QStringLiteral("yes");
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 3);
        QVERIFY(!client.isAdministrator());
        server.administratorRaw = true;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 4);
        QVERIFY(client.isAdministrator());
        server.administratorRaw = QJsonValue(QJsonValue::Undefined);
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 5);
        QVERIFY(!client.isAdministrator());

        // A failed re-check changes nothing
        server.administratorRaw = true;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 6);
        QVERIFY(client.isAdministrator());
        server.usersMeStatus = 500;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(server.usersMeRequests, 7);
        // A new re-check is refused while one is in flight: once it is sent, the failed reply was handled (QTRY evaluates its expression once more after success, hence the latch)
        bool sent = false;
        QTRY_VERIFY(sent || (sent = client.verifySession()));
        QVERIFY(client.isAdministrator());
    }

    void restoredSessionLearnsTheAdministratorFlagFromVerify()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.users = {{kAliceId, "tokenalice", QStringLiteral("Alice"), {}}};
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.restoreSession(kAliceId, QStringLiteral("tokenalice")));
        QVERIFY(!client.isAdministrator());
        QSignalSpy refreshed(&client, &Kinefin::JellyfinClient::userProfileRefreshed);
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 1);
        QVERIFY(client.isAdministrator());
    }

    void administratorFlagIsClearedWithTheSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.users = {{kAliceId, "tokenalice", QStringLiteral("Alice"), {}},
                        {kBobId, "tokenbob", QStringLiteral("Bob"), {}}};
        Kinefin::JellyfinClient client;
        QVERIFY(loginAs(client, server, QStringLiteral("Alice")));
        QVERIFY(client.isAdministrator());
        // A 401 on the re-check
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        server.deniedUsersMeTokens.insert("tokenalice");
        QVERIFY(client.verifySession());
        QTRY_COMPARE(expired.size(), 1);
        QVERIFY(!client.isAdministrator());
        // A reply for another user than the session's
        server.deniedUsersMeTokens.clear();
        QVERIFY(loginAs(client, server, QStringLiteral("Alice")));
        QVERIFY(client.isAdministrator());
        server.users[0].id = kBobId;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(expired.size(), 2);
        QVERIFY(!client.isAdministrator());
        // Another user signing in after an administrator does not inherit the flag
        server.users[0].id = kAliceId;
        QVERIFY(loginAs(client, server, QStringLiteral("Alice")));
        QVERIFY(client.isAdministrator());
        client.forgetSession();
        server.users[1].administrator = false;
        QVERIFY(loginAs(client, server, QStringLiteral("Bob")));
        QVERIFY(!client.isAdministrator());
    }

    void sessionExpiredNamesTheUser()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.users = {{kAliceId, "tokenalice", QStringLiteral("Alice"), {}},
                        {kBobId, "tokenbob", QStringLiteral("Bob"), {}}};
        Kinefin::JellyfinClient client;
        QVERIFY(loginAs(client, server, QStringLiteral("Bob")));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        server.deniedUsersMeTokens.insert("tokenbob");
        QVERIFY(client.verifySession());
        QTRY_COMPARE(expired.size(), 1);
        QCOMPARE(expired.constFirst().constFirst().toString(), kBobId);
        QVERIFY(client.userId().isEmpty());
        QVERIFY(client.userName().isEmpty());
    }

    void publicUsersAreBoundedAndValidated()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(!client.fetchPublicUsers()); // not probed yet
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QSignalSpy users(&client, &Kinefin::JellyfinClient::publicUsersReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::publicUsersFailed);
        QVERIFY(!client.fetchPublicUsers()); // still probing
        QTRY_COMPARE(ready.size(), 1);

        QJsonArray entries;
        entries.append(publicEntry(QStringLiteral("bad id;x"), QStringLiteral("BadId")));
        entries.append(publicEntry(kAliceId, QStringLiteral("Two\nLines")));
        entries.append(
            publicEntry(kAliceId, QString(Kinefin::MaxUserNameChars + 1, QLatin1Char('n'))));
        entries.append(publicEntry(kAliceId, QStringLiteral("   ")));
        entries.append(publicEntry(kBobId, QStringLiteral("Bob"), QStringLiteral("bad tag")));
        for (int i = 0; i < 150; ++i)
            entries.append(publicEntry(QStringLiteral("%1").arg(i + 1, 32, 16, QLatin1Char('0')),
                                       QStringLiteral("User %1").arg(i),
                                       QStringLiteral("tag%1").arg(i)));
        server.publicUsersRawBody = QJsonDocument(entries).toJson(QJsonDocument::Compact);
        QVERIFY(client.fetchPublicUsers());
        QTRY_COMPARE(users.size(), 1);
        QCOMPARE(failed.size(), 0);
        const auto list = users.constFirst().constFirst().value<QList<Kinefin::PublicUser>>();
        QCOMPARE(list.size(), Kinefin::MaxPublicUsers);
        // The entries with an invalid id or name are dropped, an invalid tag only empties the tag
        QCOMPARE(list.at(0).id, kBobId);
        QCOMPARE(list.at(0).name, QStringLiteral("Bob"));
        QVERIFY(list.at(0).imageTag.isEmpty());
        QCOMPARE(list.at(1).name, QStringLiteral("User 0"));
        QCOMPARE(list.at(1).imageTag, QStringLiteral("tag0"));
        // No credentials go out
        QCOMPARE(server.publicUsersRequests, 1);
        QVERIFY(server.lastPublicUsersAuth.isEmpty());

        // Failures: a status, a body that is not an array, a body with a non-object entry
        server.publicUsersStatus = 500;
        QVERIFY(client.fetchPublicUsers());
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().at(0).toInt(), 500);
        server.publicUsersStatus = 200;
        server.publicUsersRawBody = "{\"Items\":[]}";
        QVERIFY(client.fetchPublicUsers());
        QTRY_COMPARE(failed.size(), 2);
        server.publicUsersRawBody = "[1,2]";
        QVERIFY(client.fetchPublicUsers());
        QTRY_COMPARE(failed.size(), 3);
        QCOMPARE(users.size(), 1);

        // The hidden users are the server's business: the generated list leaves them out
        server.publicUsersRawBody.clear();
        server.users = {{kAliceId, "ta", QStringLiteral("Alice"), QStringLiteral("t1")},
                        {kBobId, "tb", QStringLiteral("Bob"), {}, true}};
        QVERIFY(client.fetchPublicUsers());
        QTRY_COMPARE(users.size(), 2);
        const auto visible = users.constLast().constFirst().value<QList<Kinefin::PublicUser>>();
        QCOMPARE(visible.size(), 1);
        QCOMPARE(visible.constFirst().id, kAliceId);
    }

    void publicUsersWorkWithoutASession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.users = {{kAliceId, "ta", QStringLiteral("Alice"), {}}};
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(!client.isAuthenticated());
        QSignalSpy users(&client, &Kinefin::JellyfinClient::publicUsersReady);
        QVERIFY(client.fetchPublicUsers());
        // A disconnect ends the request without a signal
        client.disconnectFromServer();
        QTest::qWait(100);
        QCOMPARE(users.size(), 0);
        QVERIFY(!client.fetchPublicUsers());
    }

    void userImageUrlPassesAllowlist()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(client.userImageUrl(kAliceId, QStringLiteral("t1")).isEmpty()); // no server yet
        QVERIFY(prepare(client, server));
        const QUrl base = client.serverUrl();
        const QUrl built = client.userImageUrl(kAliceId, QStringLiteral("tag-a1"));
        QCOMPARE(built,
                 QUrl(server.url(
                     QStringLiteral("/jellyfin/UserImage?userId=%1&tag=tag-a1").arg(kAliceId))));
        QVERIFY(!built.toString().contains(QLatin1String("Token"), Qt::CaseInsensitive));
        QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(base, built));
        // The item image shape keeps working
        QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(
            base,
            client.imageUrl(kPlayItem, QStringLiteral("Primary"), 400, QStringLiteral("abc"))));
        // No tag: the parameter is left out and the URL still passes
        const QUrl untagged = client.userImageUrl(kAliceId, {});
        QVERIFY(!untagged.isEmpty());
        QVERIFY(!untagged.query().contains(QLatin1String("tag")));
        QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(base, untagged));
        // Refusals: bad id, bad tag
        QVERIFY(client.userImageUrl(QStringLiteral("bad id"), {}).isEmpty());
        QVERIFY(client.userImageUrl({}, {}).isEmpty());
        QVERIFY(client.userImageUrl(kAliceId, QStringLiteral("tag?x=1")).isEmpty());
        QVERIFY(client.userImageUrl(kAliceId, QStringLiteral("tag&x")).isEmpty());
        // A base with a path prefix
        const QUrl prefixed(base.toString() + QStringLiteral("/a%20b"));
        QUrl underPrefix = built;
        underPrefix.setPath(prefixed.path() + QStringLiteral("/UserImage"));
        QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(prefixed, underPrefix));
        QVERIFY(!Kinefin::JellyfinClient::isServerImageUrl(prefixed, built));
    }

    void allowlistRejectsUserImageWithForeignHostOrExtraPath()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(prepare(client, server));
        const QUrl base = client.serverUrl();
        const QUrl built = client.userImageUrl(kAliceId, QStringLiteral("tag-a1"));
        QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(base, built));
        const auto refused = [&](const QUrl &url) {
            return !Kinefin::JellyfinClient::isServerImageUrl(base, url);
        };
        QUrl otherPort = built;
        otherPort.setPort(base.port() + 1);
        QVERIFY(refused(otherPort));
        QUrl otherHost = built;
        otherHost.setHost(QStringLiteral("example.invalid"));
        QVERIFY(refused(otherHost));
        QUrl otherScheme = built;
        otherScheme.setScheme(QStringLiteral("https"));
        QVERIFY(refused(otherScheme));
        QUrl credentials = built;
        credentials.setUserName(QStringLiteral("user"));
        QVERIFY(refused(credentials));
        QUrl fragment = built;
        fragment.setFragment(QStringLiteral("x"));
        QVERIFY(refused(fragment));
        // Extra path, a different route, a missing prefix, a traversal and an encoded slash
        for (const QString &path :
             {QStringLiteral("/jellyfin/UserImage/extra"), QStringLiteral("/jellyfin/UserImage/"),
              QStringLiteral("/jellyfin/UserImages"), QStringLiteral("/UserImage"),
              QStringLiteral("/jellyfin/Users/Me"), QStringLiteral("/jellyfin/x/../Users/Me"),
              QStringLiteral("/jellyfin/UserImage%2Fextra")}) {
            QUrl url = built;
            url.setPath(path, QUrl::StrictMode);
            QVERIFY2(refused(url), qPrintable(path));
        }
        // The query is exactly the builder's: valid userId, optional valid tag, nothing else, nothing twice
        const QString root = base.toString() + QStringLiteral("/UserImage?");
        const QStringList queries = {
            QString(),
            QStringLiteral("tag=tag-a1"),
            QStringLiteral("userId=bad%20id"),
            QStringLiteral("userId=") + kAliceId + QStringLiteral("&tag=bad%20tag"),
            QStringLiteral("userId=") + kAliceId + QStringLiteral("&maxWidth=96"),
            QStringLiteral("userId=") + kAliceId + QStringLiteral("&format=png"),
            QStringLiteral("userId=") + kAliceId + QStringLiteral("&userId=") + kBobId,
            QStringLiteral("userId=") + kAliceId + QStringLiteral("&api_key=secret"),
        };
        for (const QString &query : queries)
            QVERIFY2(refused(QUrl(root + query)), qPrintable(query));
        QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(
            base, QUrl(root + QStringLiteral("userId=") + kAliceId)));
    }

    void accountCallsRefuseWithoutSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(!client.fetchCurrentUserConfiguration());
        QVERIFY(!client.updateCurrentUserConfiguration(QJsonObject{{QStringLiteral("A"), 1}}));
        QVERIFY(!client.fetchCultures());
        QVERIFY(!client.fetchDisplayHomeSections());
        // Probed but signed out
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        const int requestsBefore = server.requestCount;
        QVERIFY(!client.fetchCurrentUserConfiguration());
        QVERIFY(!client.updateCurrentUserConfiguration(QJsonObject{{QStringLiteral("A"), 1}}));
        QVERIFY(!client.fetchCultures());
        QVERIFY(!client.fetchDisplayHomeSections());
        QTest::qWait(50);
        QCOMPARE(server.requestCount, requestsBefore);
        QVERIFY(server.accountCalls.isEmpty());
        // Signed in, an empty configuration is never sent
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_VERIFY(client.isAuthenticated());
        QVERIFY(!client.updateCurrentUserConfiguration(QJsonObject()));
        QTest::qWait(50);
        QVERIFY(server.accountCalls.isEmpty());
    }

    void accountCallsCarryAuthAndShape()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy user(&client, &Kinefin::JellyfinClient::currentUserConfigurationReady);
        QSignalSpy updated(&client, &Kinefin::JellyfinClient::userConfigurationUpdated);
        QSignalSpy cultures(&client, &Kinefin::JellyfinClient::culturesReady);
        QSignalSpy sections(&client, &Kinefin::JellyfinClient::displayHomeSectionsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy listingFailures(&client, &Kinefin::JellyfinClient::listingFailed);
        QSignalSpy refreshed(&client, &Kinefin::JellyfinClient::userProfileRefreshed);
        QSignalSpy views(&client, &Kinefin::JellyfinClient::userViewsReady);

        QVERIFY(client.fetchCurrentUserConfiguration());
        QTRY_COMPARE(user.size(), 1);
        QCOMPARE(server.usersMeRequests, 1);
        QVERIFY(server.lastUsersMeAuth.contains(kTokenField));
        QCOMPARE(user.constFirst().constFirst().value<QJsonObject>().value(QStringLiteral("Name")),
                 QJsonValue(QStringLiteral("bepi")));

        const QJsonObject configuration{
            {QStringLiteral("AudioLanguagePreference"), QStringLiteral("ita")},
            {QStringLiteral("GroupedFolders"), QJsonArray{QStringLiteral("a1")}},
            {QStringLiteral("SubtitleMode"), QStringLiteral("Smart")},
            {QStringLiteral("Unknown"), QJsonValue::Null}};
        QVERIFY(client.updateCurrentUserConfiguration(configuration));
        QTRY_COMPARE(updated.size(), 1);
        QCOMPARE(server.accountCalls.size(), 1);
        const FakeJellyfin::AccountCall post = server.accountCalls.constFirst();
        QCOMPARE(post.method, QByteArrayLiteral("POST"));
        QCOMPARE(post.route, QByteArrayLiteral("/jellyfin/Users/Configuration"));
        QVERIFY(post.auth.contains(kTokenField));
        QCOMPARE(post.contentType, QByteArrayLiteral("application/json"));
        QCOMPARE(QJsonDocument::fromJson(post.body).object(), configuration);

        QVERIFY(client.fetchCultures());
        QTRY_COMPARE(cultures.size(), 1);
        QCOMPARE(cultures.constFirst().constFirst().value<QJsonArray>().size(), 2);
        const FakeJellyfin::AccountCall get = server.accountCalls.constLast();
        QCOMPARE(get.method, QByteArrayLiteral("GET"));
        QCOMPARE(get.route, QByteArrayLiteral("/jellyfin/Localization/Cultures"));
        QVERIFY(get.auth.contains(kTokenField));

        QVERIFY(client.fetchDisplayHomeSections());
        QTRY_COMPARE(sections.size(), 1);
        const FakeJellyfin::AccountCall display = server.accountCalls.constLast();
        QCOMPARE(display.method, QByteArrayLiteral("GET"));
        QVERIFY(display.route.startsWith("/jellyfin/DisplayPreferences/usersettings?"));
        QVERIFY(display.auth.contains(kTokenField));
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(display.route);
        QCOMPARE(wire.queryItemValue(QStringLiteral("userId")), client.userId());
        QCOMPARE(wire.queryItemValue(QStringLiteral("client")), QStringLiteral("emby"));
        // Only the CustomPrefs object comes back, values as sent
        const QJsonObject prefs = sections.constFirst().constFirst().value<QJsonObject>();
        QCOMPARE(prefs.size(), 2);
        QCOMPARE(prefs.value(QStringLiteral("homesection1")), QJsonValue(QStringLiteral("resume")));

        // None of it touched the other channels
        QCOMPARE(failures.size(), 0);
        QCOMPARE(listingFailures.size(), 0);
        QCOMPARE(refreshed.size(), 0);
        QCOMPARE(views.size(), 0);
    }

    void currentUserKeepsUnknownFieldsAndNulls()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QByteArray raw =
            R"({"Id":"d5ed5d4e2e5447b58e9f2b6f7bd5c96a","Name":"bepi","Future":{"Deep":[1,null,{"X":true}]},
                "PrimaryImageTag":null,
                "Configuration":{"AudioLanguagePreference":null,"SubtitleMode":"FutureMode",
                "OrderedViews":[],"GroupedFolders":null,"NewField":{"a":[1,2]}}})";
        server.usersMeRawBody = raw;
        QSignalSpy user(&client, &Kinefin::JellyfinClient::currentUserConfigurationReady);
        QVERIFY(client.fetchCurrentUserConfiguration());
        QTRY_COMPARE(user.size(), 1);
        QCOMPARE(user.constFirst().constFirst().value<QJsonObject>(),
                 QJsonDocument::fromJson(raw).object());
        const QJsonObject configuration = user.constFirst()
                                              .constFirst()
                                              .value<QJsonObject>()
                                              .value(QStringLiteral("Configuration"))
                                              .toObject();
        QVERIFY(configuration.contains(QStringLiteral("GroupedFolders")));
        QVERIFY(configuration.value(QStringLiteral("GroupedFolders")).isNull());
        QVERIFY(configuration.value(QStringLiteral("NewField")).isObject());
    }

    void updateAcceptsNoContentAndOk()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy updated(&client, &Kinefin::JellyfinClient::userConfigurationUpdated);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::userConfigurationUpdateFailed);
        const QJsonObject configuration{{QStringLiteral("PlayDefaultAudioTrack"), true}};
        for (const int status : {204, 200}) {
            server.userConfigurationStatus = status;
            QVERIFY(client.updateCurrentUserConfiguration(configuration));
            QTRY_COMPARE(updated.size(), status == 204 ? 1 : 2);
        }
        QCOMPARE(failed.size(), 0);
    }

    void accountCallsReportFailures()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy userFailed(&client, &Kinefin::JellyfinClient::currentUserConfigurationFailed);
        QSignalSpy userReady(&client, &Kinefin::JellyfinClient::currentUserConfigurationReady);
        QSignalSpy updateFailed(&client, &Kinefin::JellyfinClient::userConfigurationUpdateFailed);
        QSignalSpy updated(&client, &Kinefin::JellyfinClient::userConfigurationUpdated);
        QSignalSpy culturesFailed(&client, &Kinefin::JellyfinClient::culturesFailed);
        QSignalSpy culturesReady(&client, &Kinefin::JellyfinClient::culturesReady);
        QSignalSpy sectionsFailed(&client, &Kinefin::JellyfinClient::displayHomeSectionsFailed);
        QSignalSpy sectionsReady(&client, &Kinefin::JellyfinClient::displayHomeSectionsReady);
        QSignalSpy requestFailures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy listingFailures(&client, &Kinefin::JellyfinClient::listingFailed);
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);

        // A 5xx, a 4xx and a body that is not what the route promises
        server.usersMeStatus = 500;
        QVERIFY(client.fetchCurrentUserConfiguration());
        QTRY_COMPARE(userFailed.size(), 1);
        QCOMPARE(userFailed.constLast().at(0).toInt(), 500);
        server.usersMeStatus = 404;
        QVERIFY(client.fetchCurrentUserConfiguration());
        QTRY_COMPARE(userFailed.size(), 2);
        QCOMPARE(userFailed.constLast().at(0).toInt(), 404);
        server.usersMeStatus = 204;
        QVERIFY(client.fetchCurrentUserConfiguration());
        QTRY_COMPARE(userFailed.size(), 3);
        server.usersMeStatus = 0;
        for (const QByteArray &body : {QByteArrayLiteral("not json"), QByteArrayLiteral("[]")}) {
            server.usersMeRawBody = body;
            const qsizetype before = userFailed.size();
            QVERIFY(client.fetchCurrentUserConfiguration());
            QTRY_COMPARE(userFailed.size(), before + 1);
            QCOMPARE(userFailed.constLast().at(1).toString(),
                     QStringLiteral("Invalid user response"));
        }
        server.usersMeRawBody.clear();

        for (const int status : {500, 403, 404}) {
            server.userConfigurationStatus = status;
            const qsizetype before = updateFailed.size();
            QVERIFY(client.updateCurrentUserConfiguration(
                QJsonObject{{QStringLiteral("A"), QStringLiteral("b")}}));
            QTRY_COMPARE(updateFailed.size(), before + 1);
            QCOMPARE(updateFailed.constLast().at(0).toInt(), status);
        }

        server.culturesStatus = 503;
        QVERIFY(client.fetchCultures());
        QTRY_COMPARE(culturesFailed.size(), 1);
        QCOMPARE(culturesFailed.constLast().at(0).toInt(), 503);
        server.culturesStatus = 0;
        // Not an array, not JSON, an entry that is not an object
        for (const QByteArray &body :
             {QByteArrayLiteral("{}"), QByteArrayLiteral("garbage"), QByteArrayLiteral("[1]")}) {
            server.culturesRawBody = body;
            const qsizetype before = culturesFailed.size();
            QVERIFY(client.fetchCultures());
            QTRY_COMPARE(culturesFailed.size(), before + 1);
            QCOMPARE(culturesFailed.constLast().at(1).toString(),
                     QStringLiteral("Invalid cultures response"));
        }

        server.displayPreferencesStatus = 404;
        QVERIFY(client.fetchDisplayHomeSections());
        QTRY_COMPARE(sectionsFailed.size(), 1);
        QCOMPARE(sectionsFailed.constLast().at(0).toInt(), 404);
        server.displayPreferencesStatus = 0;
        // Not JSON, not an object, no CustomPrefs, CustomPrefs not an object
        for (const QByteArray &body :
             {QByteArrayLiteral("garbage"), QByteArrayLiteral("[]"), QByteArrayLiteral("{}"),
              QByteArrayLiteral(R"({"CustomPrefs":null})"),
              QByteArrayLiteral(R"({"CustomPrefs":[1]})")}) {
            server.displayPreferencesRawBody = body;
            const qsizetype before = sectionsFailed.size();
            QVERIFY(client.fetchDisplayHomeSections());
            QTRY_COMPARE(sectionsFailed.size(), before + 1);
            QCOMPARE(sectionsFailed.constLast().at(1).toString(),
                     QStringLiteral("Invalid display preferences response"));
        }

        QCOMPARE(userReady.size(), 0);
        QCOMPARE(updated.size(), 0);
        QCOMPARE(culturesReady.size(), 0);
        QCOMPARE(sectionsReady.size(), 0);
        QCOMPARE(requestFailures.size(), 0);
        QCOMPARE(listingFailures.size(), 0);
        QCOMPARE(expired.size(), 0);
        QVERIFY(client.isAuthenticated());
    }

    void accountCallsExpireTheSessionOn401()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy userFailed(&client, &Kinefin::JellyfinClient::currentUserConfigurationFailed);
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        server.denyUsersMe = true;
        QVERIFY(client.fetchCurrentUserConfiguration());
        QTRY_COMPARE(userFailed.size(), 1);
        QCOMPARE(userFailed.constFirst().at(0).toInt(), 401);
        QCOMPARE(expired.size(), 1);
        QVERIFY(!client.isAuthenticated());

        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_VERIFY(client.isAuthenticated());
        QSignalSpy updateFailed(&client, &Kinefin::JellyfinClient::userConfigurationUpdateFailed);
        QSignalSpy updated(&client, &Kinefin::JellyfinClient::userConfigurationUpdated);
        server.userConfigurationStatus = 401;
        QVERIFY(client.updateCurrentUserConfiguration(QJsonObject{{QStringLiteral("A"), 1}}));
        QTRY_COMPARE(updateFailed.size(), 1);
        QCOMPARE(updateFailed.constFirst().at(0).toInt(), 401);
        QCOMPARE(updateFailed.constFirst().at(1).toString(),
                 QStringLiteral("Authentication expired"));
        QCOMPARE(updated.size(), 0);
        QVERIFY(!client.isAuthenticated());
    }

    void accountResponsesAreBounded()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy userFailed(&client, &Kinefin::JellyfinClient::currentUserConfigurationFailed);
        QSignalSpy culturesFailed(&client, &Kinefin::JellyfinClient::culturesFailed);
        QSignalSpy culturesReady(&client, &Kinefin::JellyfinClient::culturesReady);
        QSignalSpy sectionsFailed(&client, &Kinefin::JellyfinClient::displayHomeSectionsFailed);
        QSignalSpy sectionsReady(&client, &Kinefin::JellyfinClient::displayHomeSectionsReady);

        // Over the response size cap
        QJsonObject huge{{QStringLiteral("Id"), QStringLiteral("x")},
                         {QStringLiteral("Padding"), QString(1'100'000, QLatin1Char('a'))}};
        server.usersMeRawBody = QJsonDocument(huge).toJson(QJsonDocument::Compact);
        QVERIFY(client.fetchCurrentUserConfiguration());
        QTRY_COMPARE(userFailed.size(), 1);

        auto culturesBody = [](int count) {
            QJsonArray list;
            for (int i = 0; i < count; ++i)
                list.append(QJsonObject{{QStringLiteral("Name"), QString::number(i)}});
            return QJsonDocument(list).toJson(QJsonDocument::Compact);
        };
        server.culturesRawBody = culturesBody(Kinefin::JellyfinClient::MaxCultures);
        QVERIFY(client.fetchCultures());
        QTRY_COMPARE(culturesReady.size(), 1);
        QCOMPARE(culturesReady.constFirst().constFirst().value<QJsonArray>().size(),
                 Kinefin::JellyfinClient::MaxCultures);
        server.culturesRawBody = culturesBody(Kinefin::JellyfinClient::MaxCultures + 1);
        QVERIFY(client.fetchCultures());
        QTRY_COMPARE(culturesFailed.size(), 1);

        auto prefsBody = [](int count) {
            QJsonObject prefs;
            for (int i = 0; i < count; ++i)
                prefs.insert(QStringLiteral("k%1").arg(i), i);
            return QJsonDocument(QJsonObject{{QStringLiteral("CustomPrefs"), prefs}})
                .toJson(QJsonDocument::Compact);
        };
        server.displayPreferencesRawBody =
            prefsBody(Kinefin::JellyfinClient::MaxDisplayPreferenceEntries);
        QVERIFY(client.fetchDisplayHomeSections());
        QTRY_COMPARE(sectionsReady.size(), 1);
        // Values stay as the server sent them (numbers are not turned into strings)
        QCOMPARE(sectionsReady.constFirst().constFirst().value<QJsonObject>().value(
                     QStringLiteral("k7")),
                 QJsonValue(7));
        server.displayPreferencesRawBody =
            prefsBody(Kinefin::JellyfinClient::MaxDisplayPreferenceEntries + 1);
        QVERIFY(client.fetchDisplayHomeSections());
        QTRY_COMPARE(sectionsFailed.size(), 1);
    }

    void sessionLossEndsAccountCallsSilently()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy any1(&client, &Kinefin::JellyfinClient::currentUserConfigurationReady);
        QSignalSpy any2(&client, &Kinefin::JellyfinClient::currentUserConfigurationFailed);
        QSignalSpy any3(&client, &Kinefin::JellyfinClient::userConfigurationUpdated);
        QSignalSpy any4(&client, &Kinefin::JellyfinClient::userConfigurationUpdateFailed);
        QSignalSpy any5(&client, &Kinefin::JellyfinClient::culturesReady);
        QSignalSpy any6(&client, &Kinefin::JellyfinClient::culturesFailed);
        QSignalSpy any7(&client, &Kinefin::JellyfinClient::displayHomeSectionsReady);
        QSignalSpy any8(&client, &Kinefin::JellyfinClient::displayHomeSectionsFailed);
        const auto startAll = [&] {
            QVERIFY(client.fetchCurrentUserConfiguration());
            QVERIFY(client.updateCurrentUserConfiguration(QJsonObject{{QStringLiteral("A"), 1}}));
            QVERIFY(client.fetchCultures());
            QVERIFY(client.fetchDisplayHomeSections());
        };
        const auto nothingEmitted = [&] {
            for (const QSignalSpy *spy : {&any1, &any2, &any3, &any4, &any5, &any6, &any7, &any8})
                QCOMPARE(spy->size(), 0);
        };
        // Each way of losing the session, before the replies are read
        startAll();
        client.forgetSession();
        QTest::qWait(100);
        nothingEmitted();
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"), QStringLiteral("x")));
        QTRY_VERIFY(client.isAuthenticated());
        startAll();
        client.disconnectFromServer();
        QTest::qWait(100);
        nothingEmitted();
    }

    void lateUpdateReplyNeverReachesALaterSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy updated(&client, &Kinefin::JellyfinClient::userConfigurationUpdated);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::userConfigurationUpdateFailed);
        server.holdAccount = true;
        QVERIFY(client.updateCurrentUserConfiguration(QJsonObject{{QStringLiteral("A"), 1}}));
        QTRY_COMPARE(server.accountCalls.size(), 1);
        // The request reached the server; the session goes away and a new one starts
        client.forgetSession();
        server.holdAccount = false;
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"), QStringLiteral("x")));
        QTRY_VERIFY(client.isAuthenticated());
        server.releaseHeld();
        QTest::qWait(150);
        QCOMPARE(updated.size(), 0);
        QCOMPARE(failed.size(), 0);
        // The new session works normally
        QVERIFY(client.updateCurrentUserConfiguration(QJsonObject{{QStringLiteral("B"), 2}}));
        QTRY_COMPARE(updated.size(), 1);
    }

    void displayPreferencesAreNeverWritten()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy sections(&client, &Kinefin::JellyfinClient::displayHomeSectionsReady);
        QSignalSpy updated(&client, &Kinefin::JellyfinClient::userConfigurationUpdated);
        QSignalSpy user(&client, &Kinefin::JellyfinClient::currentUserConfigurationReady);
        QSignalSpy cultures(&client, &Kinefin::JellyfinClient::culturesReady);
        // The whole account flow, in the order the editor runs it
        QVERIFY(client.fetchCurrentUserConfiguration());
        QVERIFY(client.fetchCultures());
        QVERIFY(client.fetchDisplayHomeSections());
        QTRY_COMPARE(sections.size(), 1);
        QTRY_COMPARE(user.size(), 1);
        QTRY_COMPARE(cultures.size(), 1);
        QVERIFY(client.updateCurrentUserConfiguration(
            QJsonObject{{QStringLiteral("HidePlayedInLatest"), true}}));
        QTRY_COMPARE(updated.size(), 1);
        QVERIFY(client.fetchCurrentUserConfiguration());
        QTRY_COMPARE(user.size(), 2);
        int displayRequests = 0;
        for (const auto &[method, route] : std::as_const(server.requestLog)) {
            if (!route.contains("/DisplayPreferences")) {
                continue;
            }
            ++displayRequests;
            QCOMPARE(method, QByteArrayLiteral("GET"));
        }
        QCOMPARE(displayRequests, 1);
    }

    void playlistCreateSendsTheVideoBody()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        QVERIFY(client.createPlaylist(QStringLiteral("  Weekend  "), {kItemA, kDashedItem}));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(failed.size(), 0);
        const QList<QVariant> result = done.takeFirst();
        QCOMPARE(result.at(0).value<PlaylistOp>(), PlaylistOp::Create);
        QVERIFY(result.at(1).toString().isEmpty());
        QCOMPARE(server.playlists.size(), 1);
        QCOMPARE(result.at(2).toString(), server.playlists.constFirst().id);
        QCOMPARE(server.playlists.constFirst().name, QStringLiteral("Weekend"));
        QCOMPARE(server.playlistItemIds(result.at(2).toString()),
                 (QStringList{kItemA, kDashedItem}));
        QCOMPARE(server.playlistRequests.size(), 1);
        const auto &request = server.playlistRequests.constFirst();
        QCOMPARE(request.method, QByteArrayLiteral("POST"));
        QVERIFY(request.route.endsWith("/jellyfin/Playlists"));
        QVERIFY(request.contentType.startsWith("application/json"));
        QVERIFY(tokenOnlyInTheHeader(server));
        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        const QJsonObject expected{{QStringLiteral("Name"), QStringLiteral("Weekend")},
                                   {QStringLiteral("Ids"), QJsonArray{kItemA, kDashedItem}},
                                   {QStringLiteral("UserId"), client.userId()},
                                   {QStringLiteral("MediaType"), QStringLiteral("Video")},
                                   {QStringLiteral("IsPublic"), false}};
        QVERIFY(body == expected);
    }

    void playlistCreateAcceptsNoItemsAndTheLimits()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QVERIFY(client.createPlaylist(QStringLiteral("Empty"), {}));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(server.playlists.constFirst().entries.isEmpty());
        QVERIFY(client.createPlaylist(QString(Kinefin::MaxPlaylistNameChars, QLatin1Char('n')),
                                      manyIds(Kinefin::MaxPlaylistWriteIds)));
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(server.playlists.size(), 2);
        QCOMPARE(server.playlists.constLast().entries.size(), Kinefin::MaxPlaylistWriteIds);
    }

    void playlistCreateReplyWithoutAValidIdFails()
    {
        const QList<QByteArray> replies{QByteArrayLiteral("{}"),
                                        QByteArrayLiteral("{\"Id\":\"../Users\"}"),
                                        QByteArrayLiteral("{\"Id\":7}"), QByteArrayLiteral("[]"),
                                        QByteArrayLiteral("not json")};
        for (const QByteArray &reply : replies) {
            FakeJellyfin server;
            QVERIFY(server.listen());
            server.playlistCreateRawBody = reply;
            Kinefin::JellyfinClient client;
            QVERIFY(login(client, server));
            QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
            QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
            QVERIFY(client.createPlaylist(QStringLiteral("Name"), {kItemA}));
            QTRY_COMPARE(failed.size(), 1);
            QCOMPARE(failed.constFirst().at(0).value<PlaylistOp>(), PlaylistOp::Create);
            QCOMPARE(failed.constFirst().at(2).toInt(), 200);
            QCOMPARE(done.size(), 0);
            // The slot is free again
            QVERIFY(client.createPlaylist(QStringLiteral("Name"), {kItemA}));
        }
    }

    void playlistAddEncodesTheIds()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QVERIFY(client.addToPlaylist(playlist, {kItemB, kDashedItem}));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.constFirst().at(0).value<PlaylistOp>(), PlaylistOp::Add);
        QCOMPARE(done.constFirst().at(1).toString(), playlist);
        QVERIFY(done.constFirst().at(2).toString().isEmpty());
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemA, kItemB, kDashedItem}));
        QVERIFY(tokenOnlyInTheHeader(server));
        const auto &request = server.playlistRequests.constLast();
        QCOMPARE(request.method, QByteArrayLiteral("POST"));
        QVERIFY(request.route.contains(
            QByteArray("/jellyfin/Playlists/" + playlist.toUtf8() + "/Items?")));
        const QUrlQuery query = FakeJellyfin::capturedListingQuery(request.route);
        QCOMPARE(query.queryItemValue(QStringLiteral("ids")),
                 kItemB + QLatin1Char(',') + kDashedItem);
        QCOMPARE(query.queryItemValue(QStringLiteral("userId")), client.userId());
    }

    void playlistRemoveUsesEntryIds()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist =
            server.seedPlaylist(QStringLiteral("Mine"), {kItemA, kItemB, kItemC});
        const QString entryA = server.entryIdOf(playlist, kItemA);
        const QString entryC = server.entryIdOf(playlist, kItemC);
        QVERIFY(!entryA.isEmpty() && entryA != kItemA);
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        // An item id is no entry id: the server ignores it and still answers 204
        QVERIFY(client.removePlaylistEntries(playlist, {kItemB}));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemA, kItemB, kItemC}));
        QVERIFY(client.removePlaylistEntries(playlist, {entryA, entryC}));
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(done.constLast().at(0).value<PlaylistOp>(), PlaylistOp::Remove);
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemB}));
        const auto &request = server.playlistRequests.constLast();
        QCOMPARE(request.method, QByteArrayLiteral("DELETE"));
        QCOMPARE(FakeJellyfin::capturedListingQuery(request.route)
                     .queryItemValue(QStringLiteral("entryIds")),
                 entryA + QLatin1Char(',') + entryC);
    }

    void playlistMoveUsesTheEntryPath()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist =
            server.seedPlaylist(QStringLiteral("Mine"), {kItemA, kItemB, kItemC});
        const QString entryC = server.entryIdOf(playlist, kItemC);
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        QVERIFY(client.movePlaylistEntry(playlist, entryC, 0));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.constFirst().at(0).value<PlaylistOp>(), PlaylistOp::Move);
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemC, kItemA, kItemB}));
        const auto &request = server.playlistRequests.constLast();
        QCOMPARE(request.method, QByteArrayLiteral("POST"));
        QVERIFY(request.route.endsWith(QByteArray("/jellyfin/Playlists/" + playlist.toUtf8()
                                                  + "/Items/" + entryC.toUtf8() + "/Move/0")));
        // The client sends the index as given: past the end is the server's 500, nothing moves
        QVERIFY(client.movePlaylistEntry(playlist, entryC, 3));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().at(0).value<PlaylistOp>(), PlaylistOp::Move);
        QCOMPARE(failed.constFirst().at(2).toInt(), 500);
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemC, kItemA, kItemB}));
        // The last valid index moves it to the end; an item id is a silent no-op
        QVERIFY(client.movePlaylistEntry(playlist, entryC, 2));
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemA, kItemB, kItemC}));
        QVERIFY(client.movePlaylistEntry(playlist, kItemA, 2));
        QTRY_COMPARE(done.size(), 3);
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemA, kItemB, kItemC}));
    }

    void playlistRenameAndDelete()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Old"), {kItemA});
        const QString other = server.seedPlaylist(QStringLiteral("Other"), {kItemB});
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QVERIFY(client.renamePlaylist(playlist, QStringLiteral(" New name ")));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.constFirst().at(0).value<PlaylistOp>(), PlaylistOp::Rename);
        QCOMPARE(server.playlistById(playlist)->name, QStringLiteral("New name"));
        auto request = server.playlistRequests.constLast();
        QCOMPARE(request.method, QByteArrayLiteral("POST"));
        QVERIFY(request.route.endsWith(QByteArray("/jellyfin/Playlists/" + playlist.toUtf8())));
        QVERIFY(request.contentType.startsWith("application/json"));
        const QJsonObject renamed{{QStringLiteral("Name"), QStringLiteral("New name")}};
        QVERIFY(QJsonDocument::fromJson(request.body).object() == renamed);
        QVERIFY(client.deletePlaylist(playlist));
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(done.constLast().at(0).value<PlaylistOp>(), PlaylistOp::Delete);
        QCOMPARE(done.constLast().at(1).toString(), playlist);
        QVERIFY(!server.playlistById(playlist));
        QVERIFY(server.playlistById(other));
        request = server.playlistRequests.constLast();
        QCOMPARE(request.method, QByteArrayLiteral("DELETE"));
        QVERIFY(request.route.endsWith(QByteArray("/jellyfin/Items/" + playlist.toUtf8())));
        // The delete is preceded by a read of the same item, with the user
        const auto &read = server.playlistRequests.at(server.playlistRequests.size() - 2);
        QCOMPARE(read.method, QByteArrayLiteral("GET"));
        QVERIFY(read.route.startsWith(QByteArray("/jellyfin/Items/" + playlist.toUtf8() + "?")));
        QCOMPARE(
            FakeJellyfin::capturedListingQuery(read.route).queryItemValue(QStringLiteral("userId")),
            client.userId());
        QVERIFY(tokenOnlyInTheHeader(server));
    }

    void playlistRefusedInputSendsNothing()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        const QString entry = server.entryIdOf(playlist, kItemA);
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        QSignalSpy entries(&client, &Kinefin::JellyfinClient::playlistEntriesReady);
        QSignalSpy listingFailures(&client, &Kinefin::JellyfinClient::listingFailed);

        const QStringList badIds{QString(),
                                 QStringLiteral("a/b"),
                                 QStringLiteral("a b"),
                                 QStringLiteral("a?x=1"),
                                 QStringLiteral("..%2F"),
                                 QString(65, QLatin1Char('a'))};
        const auto withChar = [](char16_t c) {
            return QStringLiteral("a") + QChar(c) + QStringLiteral("b");
        };
        const QStringList badNames{QString(),
                                   QStringLiteral("   "),
                                   QString(Kinefin::MaxPlaylistNameChars + 1, QLatin1Char('n')),
                                   withChar(u'\n'),
                                   withChar(u'\r'),
                                   withChar(u'\t'),
                                   withChar(u'\0'),
                                   withChar(0x7f),
                                   withChar(0x85),
                                   withChar(0x2028),
                                   withChar(0x2029),
                                   withChar(0x202e),
                                   withChar(0x200b),
                                   withChar(0xd800),
                                   withChar(0xdc00)};
        for (const QString &id : badIds) {
            QVERIFY2(!client.createPlaylist(QStringLiteral("Name"), {id}), qPrintable(id));
            QVERIFY2(!client.createPlaylist(QStringLiteral("Name"), {kItemA, id}), qPrintable(id));
            QVERIFY2(!client.addToPlaylist(playlist, {id}), qPrintable(id));
            QVERIFY2(!client.addToPlaylist(id, {kItemA}), qPrintable(id));
            QVERIFY2(!client.removePlaylistEntries(playlist, {id}), qPrintable(id));
            QVERIFY2(!client.removePlaylistEntries(id, {entry}), qPrintable(id));
            QVERIFY2(!client.movePlaylistEntry(playlist, id, 0), qPrintable(id));
            QVERIFY2(!client.movePlaylistEntry(id, entry, 0), qPrintable(id));
            QVERIFY2(!client.renamePlaylist(id, QStringLiteral("Name")), qPrintable(id));
            QVERIFY2(!client.deletePlaylist(id), qPrintable(id));
            QVERIFY2(!client.fetchPlaylistEntries(id, 0, 10), qPrintable(id));
        }
        for (const QString &name : badNames) {
            QVERIFY2(!client.createPlaylist(name, {kItemA}), qPrintable(name));
            QVERIFY2(!client.renamePlaylist(playlist, name), qPrintable(name));
        }
        const QStringList tooMany = manyIds(Kinefin::MaxPlaylistWriteIds + 1);
        QVERIFY(!client.createPlaylist(QStringLiteral("Name"), tooMany));
        QVERIFY(!client.addToPlaylist(playlist, tooMany));
        QVERIFY(!client.removePlaylistEntries(playlist, tooMany));
        QVERIFY(!client.addToPlaylist(playlist, {}));
        QVERIFY(!client.removePlaylistEntries(playlist, {}));
        QVERIFY(!client.movePlaylistEntry(playlist, entry, -1));
        QVERIFY(!client.fetchPlaylistEntries(playlist, -1, 10));
        QVERIFY(!client.fetchPlaylistEntries(playlist, 0, 0));
        QVERIFY(!client.fetchPlaylistEntries(playlist, 0, -5));

        // A client without a session refuses everything too
        Kinefin::JellyfinClient offline;
        QVERIFY(!offline.createPlaylist(QStringLiteral("Name"), {kItemA}));
        QVERIFY(!offline.addToPlaylist(playlist, {kItemA}));
        QVERIFY(!offline.removePlaylistEntries(playlist, {entry}));
        QVERIFY(!offline.movePlaylistEntry(playlist, entry, 0));
        QVERIFY(!offline.renamePlaylist(playlist, QStringLiteral("Name")));
        QVERIFY(!offline.deletePlaylist(playlist));
        QVERIFY(!offline.fetchPlaylistEntries(playlist, 0, 10));

        // The session is still good, and a request sent before the check above would have arrived by now
        QSignalSpy views(&client, &Kinefin::JellyfinClient::userViewsReady);
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(views.size(), 1);
        QCOMPARE(server.playlistRequests.size(), 0);
        QCOMPARE(done.size() + failed.size() + entries.size() + listingFailures.size(), 0);
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemA}));
    }

    void playlistWrite401ExpiresTheSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        server.playlistWriteStatus = 401;
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        QVERIFY(client.addToPlaylist(playlist, {kItemB}));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(expired.size(), 1);
        QCOMPARE(done.size(), 0);
        QCOMPARE(failed.constFirst().at(0).value<PlaylistOp>(), PlaylistOp::Add);
        QCOMPARE(failed.constFirst().at(1).toString(), playlist);
        QCOMPARE(failed.constFirst().at(2).toInt(), 401);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(!client.addToPlaylist(playlist, {kItemB}));
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemA}));
    }

    void playlistWriteFailuresCarryTheStatus()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        server.playlistWriteStatus = 403;
        QVERIFY(client.renamePlaylist(playlist, QStringLiteral("New")));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constLast().at(0).value<PlaylistOp>(), PlaylistOp::Rename);
        QCOMPARE(failed.constLast().at(2).toInt(), 403);
        server.playlistWriteStatus = 404;
        QVERIFY(client.deletePlaylist(playlist));
        QTRY_COMPARE(failed.size(), 2);
        QCOMPARE(failed.constLast().at(2).toInt(), 404);
        QVERIFY(client.isAuthenticated());
        QCOMPARE(server.playlistById(playlist)->name, QStringLiteral("Mine"));
        // A playlist the server does not know
        server.playlistWriteStatus = 0;
        QVERIFY(client.deletePlaylist(kItemB));
        QTRY_COMPARE(failed.size(), 3);
        QCOMPARE(failed.constLast().at(2).toInt(), 404);
    }

    void playlistWritesAreOnePerPlaylist()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString first = server.seedPlaylist(QStringLiteral("First"), {kItemA});
        const QString second = server.seedPlaylist(QStringLiteral("Second"), {kItemA});
        server.holdPlaylistWrites = true;
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QVERIFY(client.addToPlaylist(first, {kItemB}));
        QVERIFY(!client.addToPlaylist(first, {kItemC}));
        QVERIFY(!client.deletePlaylist(first));
        QVERIFY(client.addToPlaylist(second, {kItemB}));
        QVERIFY(client.createPlaylist(QStringLiteral("Third"), {kItemA}));
        QVERIFY(!client.createPlaylist(QStringLiteral("Fourth"), {kItemA}));
        QTRY_VERIFY(server.heldPlaylistCount() == 3);
        QCOMPARE(server.playlistRequests.size(), 3);
        server.holdPlaylistWrites = false;
        server.releasePlaylistWrites();
        QTRY_COMPARE(done.size(), 3);
        QVERIFY(client.addToPlaylist(first, {kItemC}));
        QTRY_COMPARE(done.size(), 4);
        QCOMPARE(server.playlistItemIds(first), (QStringList{kItemA, kItemB, kItemC}));
    }

    void playlistWriteDroppedWithTheSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        server.holdPlaylistWrites = true;
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        QVERIFY(client.addToPlaylist(playlist, {kItemB}));
        QTRY_VERIFY(server.heldPlaylistCount() == 1);
        client.disconnectFromServer();
        server.holdPlaylistWrites = false;
        server.releasePlaylistWrites();
        // The aborted write reports nothing, and a new session starts with no write pending
        QVERIFY(login(client, server));
        QVERIFY(client.addToPlaylist(playlist, {kItemC}));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(failed.size(), 0);
    }

    void playlistEntriesPageAndCountTheTotal()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QStringList ids = manyIds(5);
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), ids);
        QSignalSpy entries(&client, &Kinefin::JellyfinClient::playlistEntriesReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::listingFailed);
        QVERIFY(client.fetchPlaylistEntries(playlist, 0, 2, 7));
        QTRY_COMPARE(entries.size(), 1);
        QList<QVariant> page = entries.takeFirst();
        QCOMPARE(page.at(0).value<QJsonArray>().size(), 2);
        QCOMPARE(page.at(1).toInt(), 5);
        QCOMPARE(page.at(2).toInt(), 7);
        QCOMPARE(idAt(page.at(0).value<QJsonArray>(), 0), ids.at(0));
        QVERIFY(client.fetchPlaylistEntries(playlist, 2, 2, 7));
        QTRY_COMPARE(entries.size(), 1);
        page = entries.takeFirst();
        QCOMPARE(idAt(page.at(0).value<QJsonArray>(), 0), ids.at(2));
        QVERIFY(client.fetchPlaylistEntries(playlist, 4, 100, 7));
        QTRY_COMPARE(entries.size(), 1);
        page = entries.takeFirst();
        QCOMPARE(page.at(0).value<QJsonArray>().size(), 1);
        QCOMPARE(page.at(1).toInt(), 5);
        for (const QString &entryId : entryIdsOf(page.at(0).value<QJsonArray>()))
            QVERIFY(Kinefin::isValidJellyfinId(entryId));

        // The route: paged, the row fields, the user, and no Items family request
        const QByteArray route = server.playlistRequests.constLast().route;
        QVERIFY(route.contains(QByteArray("/jellyfin/Playlists/" + playlist.toUtf8() + "/Items?")));
        const QUrlQuery query = FakeJellyfin::capturedListingQuery(route);
        QCOMPARE(query.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("4"));
        QCOMPARE(query.queryItemValue(QStringLiteral("limit")), QStringLiteral("100"));
        QCOMPARE(query.queryItemValue(QStringLiteral("userId")), client.userId());
        QVERIFY(query.queryItemValue(QStringLiteral("fields"))
                    .contains(QStringLiteral("PrimaryImageAspectRatio")));
        QVERIFY(server.listingRoutes.value(QStringLiteral("Items")).isEmpty());
        QVERIFY(tokenOnlyInTheHeader(server));

        // A page larger than the cap is asked for at the cap
        QVERIFY(client.fetchPlaylistEntries(playlist, 0, 1000, 7));
        QTRY_COMPARE(entries.size(), 1);
        entries.clear();
        QCOMPARE(FakeJellyfin::capturedListingQuery(server.playlistRequests.constLast().route)
                     .queryItemValue(QStringLiteral("limit")),
                 QString::number(Kinefin::MaxPlaylistPageSize));

        // An unknown playlist is a listing failure of its own family, on its channel
        QVERIFY(client.fetchPlaylistEntries(kItemC, 0, 10, 9));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(0).toString(), QStringLiteral("PlaylistEntries"));
        QCOMPARE(failures.constFirst().at(1).toInt(), 404);
        QCOMPARE(failures.constFirst().at(3).toInt(), 9);
        QCOMPARE(entries.size(), 0);
    }

    void playlistEntriesSupersedeAndReleaseByChannel()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), manyIds(3));
        QSignalSpy entries(&client, &Kinefin::JellyfinClient::playlistEntriesReady);
        QVERIFY(client.fetchPlaylistEntries(playlist, 0, 1, 5));
        QVERIFY(client.fetchPlaylistEntries(playlist, 1, 1, 5));
        QVERIFY(client.fetchPlaylistEntries(playlist, 2, 1, 6));
        client.releaseListingChannel(6);
        QTRY_COMPARE(entries.size(), 1);
        // Two sentinel requests sent after the others, one after the other: once both are answered no reply of the superseded or the released channel can still be on its way
        QVERIFY(client.fetchPlaylistEntries(playlist, 0, 3, 8));
        QTRY_VERIFY(entries.constLast().at(2).toInt() == 8);
        QVERIFY(client.fetchPlaylistEntries(playlist, 0, 3, 9));
        QTRY_VERIFY(entries.constLast().at(2).toInt() == 9);
        QList<int> channels;
        for (const QList<QVariant> &page : entries)
            channels.append(page.at(2).toInt());
        QCOMPARE(channels.size(), 3);
        QVERIFY(channels.contains(5) && channels.contains(8) && channels.contains(9));
        QVERIFY(!channels.contains(6));
        for (const QList<QVariant> &page : entries) {
            if (page.at(2).toInt() == 5) {
                QCOMPARE(page.at(0).value<QJsonArray>().size(), 1);
                QCOMPARE(idAt(page.at(0).value<QJsonArray>(), 0), manyIds(3).at(1));
            }
        }
    }

    void playlistCopiesOfAnItemShareOneEntryId()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy entries(&client, &Kinefin::JellyfinClient::playlistEntriesReady);
        QVERIFY(client.createPlaylist(QStringLiteral("Twice"), {kItemA, kItemB, kItemA}));
        QTRY_COMPARE(done.size(), 1);
        const QString playlist = done.constFirst().at(2).toString();
        QVERIFY(client.addToPlaylist(playlist, {kItemB}));
        QTRY_COMPARE(done.size(), 2);
        QVERIFY(client.fetchPlaylistEntries(playlist, 0, 100));
        QTRY_COMPARE(entries.size(), 1);
        const QJsonArray items = entries.constFirst().at(0).value<QJsonArray>();
        QCOMPARE(items.size(), 4);
        QCOMPARE(entries.constFirst().at(1).toInt(), 4);
        const QStringList entryIds = entryIdsOf(items);
        QCOMPARE(entryIds.at(0), entryIds.at(2));
        QCOMPARE(entryIds.at(1), entryIds.at(3));
        QVERIFY(entryIds.at(0) != entryIds.at(1));
        // Removing the one entry id takes every copy of the item
        QVERIFY(client.removePlaylistEntries(playlist, {entryIds.at(0)}));
        QTRY_COMPARE(done.size(), 3);
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemB, kItemB}));
    }

    void playlistNameNormalization()
    {
        using Kinefin::normalizedPlaylistName;
        QCOMPARE(normalizedPlaylistName(QStringLiteral("  Weekend \t")), QStringLiteral("Weekend"));
        const QString plain = QStringLiteral("Film – Serie 日本語 é");
        QCOMPARE(normalizedPlaylistName(plain), plain);
        QCOMPARE(normalizedPlaylistName(QStringLiteral("   　 x 　")), QStringLiteral("x"));
        QVERIFY(normalizedPlaylistName({}).isEmpty());
        QVERIFY(normalizedPlaylistName(QStringLiteral("    　")).isEmpty());

        // Only invisible characters: nothing to show, even though the string is not empty
        QVERIFY(normalizedPlaylistName(repeated(0x200b, 3)).isEmpty());
        QVERIFY(
            normalizedPlaylistName(QStringLiteral(" ") + codePoint(0x200b) + QStringLiteral(" "))
                .isEmpty());

        // The limit counts code points: 70 emoji are 140 UTF-16 units and still fit, 129 do not
        const QString seventy = repeated(0x1f600, 70);
        QCOMPARE(seventy.size(), 140);
        QCOMPARE(normalizedPlaylistName(seventy), seventy);
        const QString full = repeated(0x1f600, Kinefin::MaxPlaylistNameChars);
        QCOMPARE(normalizedPlaylistName(full), full);
        QVERIFY(normalizedPlaylistName(full + codePoint(0x1f600)).isEmpty());
        QVERIFY(!normalizedPlaylistName(QString(Kinefin::MaxPlaylistNameChars, QLatin1Char('n')))
                     .isEmpty());
        QVERIFY(normalizedPlaylistName(QString(Kinefin::MaxPlaylistNameChars + 1, QLatin1Char('n')))
                    .isEmpty());

        // Refused anywhere in the name: controls, line and paragraph separators, format characters (bidi, zero-width, BOM, soft hyphen, tags), unassigned code points and noncharacters
        const QList<char32_t> refused{0x00,   0x09,   0x0a,   0x0d,   0x1f,   0x7f,    0x85,
                                      0x9f,   0xad,   0x061c, 0x200b, 0x200c, 0x200d,  0x200e,
                                      0x200f, 0x2028, 0x2029, 0x202a, 0x202b, 0x202c,  0x202d,
                                      0x202e, 0x2066, 0x2067, 0x2068, 0x2069, 0xfeff,  0xe0001,
                                      0x0378, 0xfdd0, 0xfdef, 0xfffe, 0xffff, 0x1fffe, 0x10ffff};
        for (const char32_t cp : refused) {
            const QString c = codePoint(cp);
            const QByteArray label = QByteArray::number(uint(cp), 16);
            QVERIFY2(
                normalizedPlaylistName(QStringLiteral("a") + c + QStringLiteral("b")).isEmpty(),
                label.constData());
            // At the ends the white space (tab, line breaks, NEL, line and paragraph separators) is trimmed away, not refused
            if (QChar::isSpace(cp))
                continue;
            QVERIFY2(normalizedPlaylistName(QStringLiteral("a") + c).isEmpty(), label.constData());
            QVERIFY2(normalizedPlaylistName(c + QStringLiteral("a")).isEmpty(), label.constData());
        }

        // Lone surrogates, a reversed pair included
        const QString high(QChar(char16_t(0xd83d)));
        const QString low(QChar(char16_t(0xde00)));
        QVERIFY(normalizedPlaylistName(QStringLiteral("a") + high).isEmpty());
        QVERIFY(normalizedPlaylistName(low + QStringLiteral("a")).isEmpty());
        QVERIFY(normalizedPlaylistName(QStringLiteral("a") + low + high + QStringLiteral("b"))
                    .isEmpty());
        QVERIFY(!normalizedPlaylistName(QStringLiteral("a") + high + low + QStringLiteral("b"))
                     .isEmpty());

        // The calls apply the same rule
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        const QStringList names{QString(QLatin1Char('a')) + QChar(0x202E) + QLatin1Char('b'),
                                repeated(0x200b, 3), QString(QStringLiteral("a") + high)};
        for (const QString &name : names) {
            QVERIFY(!client.createPlaylist(name, {kItemA}));
            QVERIFY(!client.renamePlaylist(playlist, name));
        }
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QVERIFY(client.createPlaylist(seventy, {kItemA}));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(server.playlists.constLast().name, seventy);
        QCOMPARE(server.playlistRequests.size(), 1);
    }

    void playlistWritesSucceedOnlyOnTheirDocumentedStatus()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA, kItemB});
        const QString entry = server.entryIdOf(playlist, kItemB);
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        const QList<std::pair<PlaylistOp, std::function<bool()>>> others{
            {PlaylistOp::Add, [&] { return client.addToPlaylist(playlist, {kItemC}); }},
            {PlaylistOp::Remove, [&] { return client.removePlaylistEntries(playlist, {entry}); }},
            {PlaylistOp::Move, [&] { return client.movePlaylistEntry(playlist, entry, 0); }},
            {PlaylistOp::Rename,
             [&] { return client.renamePlaylist(playlist, QStringLiteral("New")); }},
            {PlaylistOp::Delete, [&] { return client.deletePlaylist(playlist); }}};
        int expected = 0;
        // A 200 (empty body here) is not the 204 of these writes, whatever a proxy or a server change makes of it
        for (const int status : {200, 201, 202}) {
            server.playlistWriteStatus = status;
            for (const auto &[op, send] : others) {
                QVERIFY(send());
                ++expected;
                QTRY_COMPARE(failed.size(), expected);
                QCOMPARE(failed.constLast().at(0).value<PlaylistOp>(), op);
                QCOMPARE(failed.constLast().at(2).toInt(), status);
            }
        }
        // A create wants its 200
        for (const int status : {201, 204}) {
            server.playlistWriteStatus = status;
            QVERIFY(client.createPlaylist(QStringLiteral("New"), {kItemA}));
            ++expected;
            QTRY_COMPARE(failed.size(), expected);
            QCOMPARE(failed.constLast().at(0).value<PlaylistOp>(), PlaylistOp::Create);
            QCOMPARE(failed.constLast().at(2).toInt(), status);
        }
        QCOMPARE(done.size(), 0);
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemA, kItemB}));
        QCOMPARE(server.playlistById(playlist)->name, QStringLiteral("Mine"));
        QCOMPARE(server.playlists.size(), 1);
        // The slots are free again, and the documented statuses succeed
        server.playlistWriteStatus = 0;
        QVERIFY(client.addToPlaylist(playlist, {kItemC}));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(client.createPlaylist(QStringLiteral("New"), {kItemA}));
        QTRY_COMPARE(done.size(), 2);
    }

    void playlistDeleteNeverDeletesANonPlaylist()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QStringList ids = manyIds(5);
        const QStringList types{QStringLiteral("Movie"), QStringLiteral("BoxSet"),
                                QStringLiteral("playlist"), QStringLiteral("Playlist "),
                                QStringLiteral("Folder")};
        for (qsizetype i = 0; i < ids.size(); ++i)
            server.typedItems.insert(ids.at(i), types.at(i));
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        for (int i = 0; i < ids.size(); ++i) {
            QVERIFY(client.deletePlaylist(ids.at(i)));
            QTRY_COMPARE(failed.size(), i + 1);
            QCOMPARE(failed.constLast().at(0).value<PlaylistOp>(), PlaylistOp::Delete);
            QCOMPARE(failed.constLast().at(1).toString(), ids.at(i));
            QCOMPARE(failed.constLast().at(2).toInt(), 200);
        }
        QCOMPARE(done.size(), 0);
        QCOMPARE(deleteRequests(server), 0);
        QVERIFY(client.isAuthenticated());
        // An id the server does not know ends the same way
        QVERIFY(client.deletePlaylist(kItemA));
        QTRY_COMPARE(failed.size(), 6);
        QCOMPARE(failed.constLast().at(2).toInt(), 404);
        QCOMPARE(deleteRequests(server), 0);
    }

    void playlistDeleteNeedsAValidTypeReply()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        const QByteArray id = playlist.toUtf8();
        const QList<QByteArray> bodies{
            QByteArrayLiteral("{}"),
            QByteArrayLiteral("[]"),
            QByteArrayLiteral("not json"),
            QByteArrayLiteral("{\"Id\":\"") + id + QByteArrayLiteral("\"}"),
            QByteArrayLiteral("{\"Id\":\"") + id + QByteArrayLiteral("\",\"Type\":7}"),
            QByteArrayLiteral("{\"Id\":\"") + id + QByteArrayLiteral("\",\"Type\":\"playlist\"}"),
            QByteArrayLiteral("{\"Type\":\"Playlist\"}"),
            QByteArrayLiteral("{\"Id\":\"") + kItemB.toUtf8()
                + QByteArrayLiteral("\",\"Type\":\"Playlist\"}")};
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        int expected = 0;
        for (const QByteArray &body : bodies) {
            server.playlistItemRawBody = body;
            QVERIFY(client.deletePlaylist(playlist));
            ++expected;
            QTRY_COMPARE(failed.size(), expected);
            QCOMPARE(failed.constLast().at(2).toInt(), 200);
        }
        server.playlistItemRawBody.clear();
        for (const int status : {204, 403, 404, 500}) {
            server.playlistItemStatus = status;
            QVERIFY(client.deletePlaylist(playlist));
            ++expected;
            QTRY_COMPARE(failed.size(), expected);
            QCOMPARE(failed.constLast().at(2).toInt(), status);
        }
        QVERIFY(client.isAuthenticated());
        QCOMPARE(done.size(), 0);
        QCOMPARE(deleteRequests(server), 0);
        QVERIFY(server.playlistById(playlist));
        // A 401 on the read expires the session like any other
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        server.playlistItemStatus = 401;
        QVERIFY(client.deletePlaylist(playlist));
        QTRY_COMPARE(failed.size(), expected + 1);
        QCOMPARE(failed.constLast().at(2).toInt(), 401);
        QCOMPARE(expired.size(), 1);
        QCOMPARE(deleteRequests(server), 0);
    }

    void playlistDeleteNeedsCanDeleteTrue()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        const QByteArray head = QByteArrayLiteral("{\"Id\":\"") + playlist.toUtf8()
                                + QByteArrayLiteral("\",\"Type\":\"Playlist\"");
        // An empty body is the fake's own answer with `CanDelete` false
        const QList<QByteArray> bodies{{},
                                       head + QByteArrayLiteral("}"),
                                       head + QByteArrayLiteral(",\"CanDelete\":false}"),
                                       head + QByteArrayLiteral(",\"CanDelete\":\"true\"}"),
                                       head + QByteArrayLiteral(",\"CanDelete\":1}"),
                                       head + QByteArrayLiteral(",\"CanDelete\":null}")};
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        int expected = 0;
        for (const QByteArray &body : bodies) {
            server.playlistsDeletable = !body.isEmpty();
            server.playlistItemRawBody = body;
            QVERIFY(client.deletePlaylist(playlist));
            ++expected;
            QTRY_COMPARE(failed.size(), expected);
            QCOMPARE(failed.constLast().at(0).value<PlaylistOp>(), PlaylistOp::Delete);
            QCOMPARE(failed.constLast().at(1).toString(), playlist);
            QCOMPARE(failed.constLast().at(2).toInt(), 403);
        }
        QCOMPARE(done.size(), 0);
        QCOMPARE(deleteRequests(server), 0);
        QVERIFY(client.isAuthenticated());
        QVERIFY(server.playlistById(playlist));
        // The slot is free after every refusal, and a playlist the user may delete goes
        server.playlistsDeletable = true;
        server.playlistItemRawBody.clear();
        QVERIFY(client.deletePlaylist(playlist));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.constFirst().at(0).value<PlaylistOp>(), PlaylistOp::Delete);
        QCOMPARE(done.constFirst().at(1).toString(), playlist);
        QCOMPARE(deleteRequests(server), 1);
        QVERIFY(!server.playlistById(playlist));
        QCOMPARE(failed.size(), expected);
    }

    void playlistDeleteHoldsTheSlotAcrossBothSteps()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString first = server.seedPlaylist(QStringLiteral("First"), {kItemA});
        const QString second = server.seedPlaylist(QStringLiteral("Second"), {kItemA});
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        // The read is in flight: the playlist is already taken
        QVERIFY(client.deletePlaylist(second));
        QVERIFY(!client.deletePlaylist(second));
        QVERIFY(!client.renamePlaylist(second, QStringLiteral("New")));
        QVERIFY(!client.addToPlaylist(second, {kItemB}));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!server.playlistById(second));
        // The delete is in flight: the same
        server.holdPlaylistWrites = true;
        QVERIFY(client.deletePlaylist(first));
        QTRY_VERIFY(server.heldPlaylistCount() == 1);
        QVERIFY(!client.deletePlaylist(first));
        QVERIFY(!client.renamePlaylist(first, QStringLiteral("New")));
        QVERIFY(client.addToPlaylist(second, {kItemB}));
        server.holdPlaylistWrites = false;
        server.releasePlaylistWrites();
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(done.constLast().at(0).value<PlaylistOp>(), PlaylistOp::Delete);
        QCOMPARE(done.constLast().at(1).toString(), first);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().at(1).toString(), second);
        QCOMPARE(failed.constFirst().at(2).toInt(), 404);
        QVERIFY(!server.playlistById(first));
        QCOMPARE(deleteRequests(server), 2);
    }

    void playlistTransportErrorEndsWithStatusZeroAndFreesTheSlot()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        // The server applies the write and closes the connection without an answer
        server.dropPlaylistWrites = true;
        QVERIFY(client.addToPlaylist(playlist, {kItemB}));
        QVERIFY(!client.addToPlaylist(playlist, {kItemC}));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constLast().at(0).value<PlaylistOp>(), PlaylistOp::Add);
        QCOMPARE(failed.constLast().at(2).toInt(), 0);
        QVERIFY(client.isAuthenticated());
        QVERIFY(server.playlistItemIds(playlist).contains(kItemB));
        QVERIFY(client.createPlaylist(QStringLiteral("New"), {kItemA}));
        QTRY_COMPARE(failed.size(), 2);
        QCOMPARE(failed.constLast().at(0).value<PlaylistOp>(), PlaylistOp::Create);
        QCOMPARE(failed.constLast().at(2).toInt(), 0);
        QVERIFY(client.deletePlaylist(playlist));
        QTRY_COMPARE(failed.size(), 3);
        QCOMPARE(failed.constLast().at(0).value<PlaylistOp>(), PlaylistOp::Delete);
        QCOMPARE(failed.constLast().at(2).toInt(), 0);
        QCOMPARE(done.size(), 0);
        // The guards are clear
        server.dropPlaylistWrites = false;
        const QString other = server.seedPlaylist(QStringLiteral("Other"), {kItemA});
        QVERIFY(client.addToPlaylist(other, {kItemB}));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(client.createPlaylist(QStringLiteral("Again"), {}));
        QTRY_COMPARE(done.size(), 2);
        // The delete was applied before the connection closed: the playlist is gone
        QVERIFY(client.renamePlaylist(playlist, QStringLiteral("Renamed")));
        QTRY_COMPARE(failed.size(), 4);
        QCOMPARE(failed.constLast().at(2).toInt(), 404);
    }

    void playlistGuardCoversEverySpellingOfTheId()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        const QString dashed = playlist.left(8) + QLatin1Char('-') + playlist.mid(8, 4)
                               + QLatin1Char('-') + playlist.mid(12, 4) + QLatin1Char('-')
                               + playlist.mid(16, 4) + QLatin1Char('-') + playlist.mid(20);
        QVERIFY(Kinefin::isValidJellyfinId(dashed));
        server.holdPlaylistWrites = true;
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QVERIFY(client.addToPlaylist(playlist, {kItemB}));
        QTRY_VERIFY(server.heldPlaylistCount() == 1);
        for (const QString &spelling : {playlist.toUpper(), dashed, dashed.toUpper()}) {
            QVERIFY2(!client.addToPlaylist(spelling, {kItemC}), qPrintable(spelling));
            QVERIFY2(!client.removePlaylistEntries(spelling, {kItemC}), qPrintable(spelling));
            QVERIFY2(!client.movePlaylistEntry(spelling, kItemC, 0), qPrintable(spelling));
            QVERIFY2(!client.renamePlaylist(spelling, QStringLiteral("New")), qPrintable(spelling));
            QVERIFY2(!client.deletePlaylist(spelling), qPrintable(spelling));
        }
        QCOMPARE(server.playlistRequests.size(), 1);
        server.holdPlaylistWrites = false;
        server.releasePlaylistWrites();
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(client.addToPlaylist(playlist.toUpper(), {kItemC}));
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(server.playlistItemIds(playlist), (QStringList{kItemA, kItemB, kItemC}));
    }

    void playlistWrite401DropsTheOtherWritesSilently()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        const QString first = server.seedPlaylist(QStringLiteral("First"), {kItemA});
        const QString second = server.seedPlaylist(QStringLiteral("Second"), {kItemA});
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        server.holdPlaylistWrites = true;
        QVERIFY(client.addToPlaylist(first, {kItemB}));
        QTRY_VERIFY(server.heldPlaylistCount() == 1);
        server.holdPlaylistWrites = false;
        server.playlistWriteStatus = 401;
        QVERIFY(client.addToPlaylist(second, {kItemB}));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().at(1).toString(), second);
        QCOMPARE(failed.constFirst().at(2).toInt(), 401);
        QCOMPARE(expired.size(), 1);
        QVERIFY(!client.isAuthenticated());
        // The write of the first playlist is answered late, to a client that has dropped it
        server.playlistWriteStatus = 0;
        server.releasePlaylistWrites();
        // A new session starts with no write pending, and a write after it ends the wait
        client.disconnectFromServer();
        QVERIFY(login(client, server));
        QVERIFY(client.addToPlaylist(first, {kItemC}));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.constFirst().at(1).toString(), first);
        QCOMPARE(failed.size(), 1);
    }

    void playlistRoutesRequireTheSessionToken()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QString playlist = server.seedPlaylist(QStringLiteral("Mine"), {kItemA});
        QNetworkAccessManager network;
        const auto statusWith = [&](const QByteArray &authorization, const QByteArray &method) {
            QNetworkRequest request(server.url(QStringLiteral("/jellyfin/Playlists/") + playlist
                                               + QStringLiteral("/Items?limit=1")));
            if (!authorization.isEmpty())
                request.setRawHeader("Authorization", authorization);
            QNetworkReply *reply = network.sendCustomRequest(request, method);
            QSignalSpy finished(reply, &QNetworkReply::finished);
            const int status =
                finished.wait(5000)
                    ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                    : -1;
            delete reply;
            return status;
        };
        const auto header = [](const QByteArray &token) {
            return QByteArrayLiteral("MediaBrowser Client=\"t\", Token=\"") + token
                   + QByteArrayLiteral("\"");
        };
        QCOMPARE(statusWith({}, "GET"), 401);
        QCOMPARE(statusWith(QByteArrayLiteral("MediaBrowser Client=\"t\""), "GET"), 401);
        QCOMPARE(statusWith(header({}), "GET"), 401);
        QCOMPARE(statusWith(header("wrong"), "GET"), 401);
        QCOMPARE(statusWith(header(kFixtureToken + "0"), "GET"), 401);
        QCOMPARE(statusWith(header("wrong"), "DELETE"), 401);
        QCOMPARE(server.playlistRequests.size(), 0);
        QCOMPARE(statusWith(header(kFixtureToken), "GET"), 200);
        QCOMPARE(server.playlistRequests.size(), 1);
    }

    void playlistCallsRefuseAnInvalidSessionUserId()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(
            client.restoreSession(QStringLiteral("bad/user"), QString::fromUtf8(kFixtureToken)));
        QVERIFY(client.isAuthenticated());
        QVERIFY(!client.createPlaylist(QStringLiteral("New"), {kItemA}));
        QVERIFY(!client.addToPlaylist(kItemA, {kItemB}));
        QVERIFY(!client.deletePlaylist(kItemA));
        QVERIFY(!client.fetchPlaylistEntries(kItemA, 0, 10));
        QCOMPARE(server.playlistRequests.size(), 0);
    }

    void playlistRequestsKeepTheTokenInTheHeader()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy done(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
        QSignalSpy entries(&client, &Kinefin::JellyfinClient::playlistEntriesReady);
        QVERIFY(client.createPlaylist(QStringLiteral("Mine"), {kItemA}));
        QTRY_COMPARE(done.size(), 1);
        const QString playlist = done.constLast().at(2).toString();
        QVERIFY(client.addToPlaylist(playlist, {kItemB}));
        QTRY_COMPARE(done.size(), 2);
        const QString entryB = server.entryIdOf(playlist, kItemB);
        QVERIFY(client.movePlaylistEntry(playlist, entryB, 0));
        QTRY_COMPARE(done.size(), 3);
        QVERIFY(client.removePlaylistEntries(playlist, {server.entryIdOf(playlist, kItemA)}));
        QTRY_COMPARE(done.size(), 4);
        QVERIFY(client.renamePlaylist(playlist, QStringLiteral("Renamed")));
        QTRY_COMPARE(done.size(), 5);
        QVERIFY(client.fetchPlaylistEntries(playlist, 0, 10));
        QTRY_COMPARE(entries.size(), 1);
        QVERIFY(client.deletePlaylist(playlist));
        QTRY_COMPARE(done.size(), 6);
        // Create, add, move, remove, rename, entries, the read of the delete and the delete
        QCOMPARE(server.playlistRequests.size(), 8);
        QVERIFY(tokenOnlyInTheHeader(server));
    }
};

QTEST_GUILESS_MAIN(TestJellyfinClient)
#include "test_jellyfin_client.moc"
