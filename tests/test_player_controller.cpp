// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The player controller without a renderer: the engine is not attached, so an item stays loading and nothing is negotiated, but the item's own request, the playlist of a series and the end-of-file decision all run against the fake server; the tests of a repeat after a real end start the engine on its own (vo=null, ao=null) to give it the position and the duration of a played file
#include "FakeJellyfin.h"

#include "models/PlayQueue.h"
#include "player/PlaybackEngine.h"
#include "player/PlayerController.h"
#include "settings/ClientSettings.h"

#include <MpvController>

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Kinefin::PlayerController;

namespace {
const QString kMovie = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");
const QString kOtherMovie = QStringLiteral("0f1e2d3c4b5a69788796a5b4c3d2e1f0");
const QString kSeries = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
constexpr int kEpisodeChannel = 4096;

// The ids the fake server generates for its episodes
QString episodeId(int number)
{
    return QStringLiteral("%1").arg(number, 32, 16, QLatin1Char('e'));
}

QByteArray movieBody(const QString &id, const QString &name)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("Id"), id},
                                     {QStringLiteral("Name"), name},
                                     {QStringLiteral("Type"), QStringLiteral("Movie")}})
        .toJson(QJsonDocument::Compact);
}

QJsonObject episodeObject(const QString &id, const QString &name, int season, int number)
{
    QJsonObject episode{{QStringLiteral("Id"), id},
                        {QStringLiteral("Name"), name},
                        {QStringLiteral("Type"), QStringLiteral("Episode")},
                        {QStringLiteral("SeriesName"), QStringLiteral("Abyss Station")},
                        {QStringLiteral("SeriesId"), kSeries}};
    if (season >= 0)
        episode.insert(QStringLiteral("ParentIndexNumber"), season);
    if (number >= 0)
        episode.insert(QStringLiteral("IndexNumber"), number);
    return episode;
}

QByteArray toBody(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// The settings file of a test, with `conf` as its content: `video.aspect` is hidden from the UI, so only the file can hold a value other than the default
QString settingsFile(const QTemporaryDir &dir, const QByteArray &conf)
{
    const QString path = dir.filePath(QStringLiteral("kinefin.conf"));
    QFile file(path);
    if (!conf.isEmpty() && (!file.open(QIODevice::WriteOnly) || file.write(conf) != conf.size()))
        return {};
    return path;
}

struct Chain {
    explicit Chain(const QByteArray &conf = {})
        : queue(&client), settings(settingsFile(dir, conf)),
          // An invalid profile id keeps the controller's memory files off the disk
          controller(client, queue, settings, QString())
    {
        ok = server.listen() && login(client, server);
    }
    // The episode every playlist test plays, the third of a series of five
    void serveEpisodeThree()
    {
        server.generatedEpisodeTotal = 5;
        server.trickplayMetadataBody =
            toBody(episodeObject(episodeId(3), QStringLiteral("Weightless"), 2, 6));
    }
    int episodeRequests() const
    {
        return server.listingRoutes.value(QStringLiteral("Episodes")).size();
    }
    Kinefin::PlaybackEngine *engine() { return controller.findChild<Kinefin::PlaybackEngine *>(); }
    // Plays the item to an end the repeat modes accept or not: mpv says where it was (`position` of `duration`, in seconds), the file loaded, `playedMs` of wall-clock time went by, then mpv says eof. The engine must be initialised, which is only done once the item has asked for its source
    void endsAfter(int playedMs, double position, double duration = 100)
    {
        auto *mpv = engine()->controller();
        Q_EMIT mpv->propertyChanged(QStringLiteral("duration"), duration);
        Q_EMIT mpv->propertyChanged(QStringLiteral("time-pos"), position);
        Q_EMIT engine()->fileLoaded();
        if (playedMs > 0)
            QTest::qWait(playedMs);
        Q_EMIT engine()->streamEnded(QStringLiteral("eof"));
    }

    QTemporaryDir dir;
    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    Kinefin::PlayQueue queue;
    Kinefin::ClientSettings settings;
    PlayerController controller;
    bool ok = false;
};
} // namespace

class TestPlayerController : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void movieTitleComesFromTheServer()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.trickplayMetadataBody = movieBody(kMovie, QStringLiteral("Big Movie"));
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QVERIFY(chain.controller.title().isEmpty());
        QTRY_COMPARE(chain.controller.title(), QStringLiteral("Big Movie"));
        QVERIFY(chain.controller.subtitle().isEmpty());
        QCOMPARE(chain.server.trickplayMetadataRequests, 1);
    }

    void episodeTitleIsTheSeriesAndTheSubtitleTheNumberedEpisode()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.trickplayMetadataBody =
            toBody(episodeObject(episodeId(1), QStringLiteral("Weightless"), 2, 6));
        QVERIFY(chain.controller.playItem(episodeId(1), 0));
        QTRY_COMPARE(chain.controller.title(), QStringLiteral("Abyss Station"));
        QCOMPARE(chain.controller.subtitle(), QStringLiteral("S2:E6 — Weightless"));
    }

    void episodeSubtitleFollowsTheOtherLabelForms()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QJsonObject range = episodeObject(episodeId(1), QStringLiteral("Weightless"), 2, 6);
        range.insert(QStringLiteral("IndexNumberEnd"), 7);
        chain.server.trickplayMetadataBody = toBody(range);
        QVERIFY(chain.controller.playItem(episodeId(1), 0));
        QTRY_COMPARE(chain.controller.subtitle(), QStringLiteral("S2:E6–E7 — Weightless"));
        chain.server.trickplayMetadataBody =
            toBody(episodeObject(episodeId(1), QStringLiteral("Weightless"), -1, 6));
        QVERIFY(chain.controller.playItem(episodeId(1), 0));
        QTRY_COMPARE(chain.controller.subtitle(), QStringLiteral("E6 — Weightless"));
        chain.server.trickplayMetadataBody =
            toBody(episodeObject(episodeId(1), QStringLiteral("Weightless"), 2, -1));
        QVERIFY(chain.controller.playItem(episodeId(1), 0));
        QTRY_COMPARE(chain.controller.subtitle(), QStringLiteral("Weightless"));
    }

    void aLateReplyForThePreviousItemIsIgnored()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.trickplayMetadataBody = movieBody(kMovie, QStringLiteral("Big Movie"));
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTRY_COMPARE(chain.controller.title(), QStringLiteral("Big Movie"));
        chain.server.trickplayMetadataBody = movieBody(kOtherMovie, QStringLiteral("Other"));
        QVERIFY(chain.controller.playItem(kOtherMovie, 0));
        // The new item starts with no name, and the answer of the old one does not give it one
        QVERIFY(chain.controller.title().isEmpty());
        Q_EMIT chain.client.playbackItemReady(
            kMovie,
            QJsonDocument::fromJson(movieBody(kMovie, QStringLiteral("Big Movie"))).object());
        QVERIFY(chain.controller.title().isEmpty());
        QTRY_COMPARE(chain.controller.title(), QStringLiteral("Other"));
    }

    void aFailedItemRequestLeavesNoTitle()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.trickplayMetadataStatus = 500;
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTRY_COMPARE(chain.server.trickplayMetadataRequests, 1);
        QTest::qWait(100);
        QVERIFY(chain.controller.title().isEmpty());
        QVERIFY(chain.controller.subtitle().isEmpty());
    }

    void theTitleIsClearedWhenTheAccountLeaves()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        QVERIFY(!chain.controller.title().isEmpty());
        QVERIFY(!chain.controller.subtitle().isEmpty());
        chain.controller.onAccountLeaving();
        QVERIFY(chain.controller.title().isEmpty());
        QVERIFY(chain.controller.subtitle().isEmpty());
        // The episodes of the series go with the account
        QVERIFY(!chain.controller.hasNext());
        QVERIFY(!chain.controller.hasPrevious());
    }

    void playbackTimeTextShapes()
    {
        constexpr qint64 second = 10'000'000;
        QCOMPARE(Kinefin::playbackTimeText((3600 + 24 * 60 + 10) * second, true),
                 QStringLiteral("01:24:10"));
        QCOMPARE(Kinefin::playbackTimeText((5 * 60 + 7) * second, false), QStringLiteral("05:07"));
        QCOMPARE(Kinefin::playbackTimeText((5 * 60 + 7) * second, true),
                 QStringLiteral("00:05:07"));
        QCOMPARE(Kinefin::playbackTimeText(0, false), QStringLiteral("00:00"));
        QCOMPARE(Kinefin::playbackTimeText(-5 * second, false), QStringLiteral("00:00"));
        QCOMPARE(Kinefin::playbackTimeText(-5 * second, true), QStringLiteral("00:00:00"));
        QCOMPARE(Kinefin::playbackTimeText(second - 1, false), QStringLiteral("00:00"));
        // Without hours the minutes keep counting
        QCOMPARE(Kinefin::playbackTimeText(84 * 60 * second, false), QStringLiteral("84:00"));
        QCOMPARE(Kinefin::playbackTimeText(100 * 3600 * second, true), QStringLiteral("100:00:00"));
    }

    void timeTextShowsNoHoursWhileTheDurationIsShort()
    {
        Chain chain;
        QVERIFY(chain.ok);
        // The duration is unknown without mpv, so it counts as shorter than an hour
        QCOMPARE(chain.controller.timeText(10'000'000LL * 307), QStringLiteral("05:07"));
    }

    void anEpisodePlayedAloneGetsThePlaylistOfItsSeries()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        QVERIFY(chain.controller.hasPrevious());
        // One page holds a short series, in airing order
        QCOMPARE(chain.episodeRequests(), 1);
        const QByteArray route =
            chain.server.listingRoutes.value(QStringLiteral("Episodes")).first();
        QVERIFY(route.contains(QByteArray(QByteArrayLiteral("/Shows/") + kSeries.toLatin1()
                                          + QByteArrayLiteral("/Episodes"))));
        QVERIFY(route.contains("limit=100"));
        QVERIFY(!route.contains("seasonId"));
        // The saved queue knows nothing of it
        QCOMPARE(chain.controller.queueCount(), 0);
        QCOMPARE(chain.controller.currentIndex(), -1);
        QCOMPARE(chain.queue.count(), 0);

        QVERIFY(chain.controller.next());
        QCOMPARE(chain.controller.itemId(), episodeId(4));
        QVERIFY(chain.controller.next());
        QCOMPARE(chain.controller.itemId(), episodeId(5));
        QVERIFY(!chain.controller.hasNext());
        QVERIFY(!chain.controller.next());
        QCOMPARE(chain.controller.itemId(), episodeId(5));
        QVERIFY(chain.controller.previous());
        QCOMPARE(chain.controller.itemId(), episodeId(4));
        QVERIFY(chain.controller.previous());
        QVERIFY(chain.controller.previous());
        QCOMPARE(chain.controller.itemId(), episodeId(2));
        QVERIFY(chain.controller.previous());
        QCOMPARE(chain.controller.itemId(), episodeId(1));
        QVERIFY(!chain.controller.hasPrevious());
        QVERIFY(!chain.controller.previous());
        // Stepping through the series never asks for it again
        QTest::qWait(100);
        QCOMPARE(chain.episodeRequests(), 1);
    }

    void theFirstAndTheLastEpisodeStepOneWayOnly()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        QVERIFY(chain.controller.playItem(episodeId(1), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        QVERIFY(!chain.controller.hasPrevious());
        QVERIFY(chain.controller.playItem(episodeId(5), 0));
        QTRY_VERIFY(chain.controller.hasPrevious());
        QVERIFY(!chain.controller.hasNext());
    }

    void aMovieGetsNoPlaylist()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.generatedEpisodeTotal = 5;
        chain.server.trickplayMetadataBody = movieBody(kMovie, QStringLiteral("Big Movie"));
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTRY_COMPARE(chain.controller.title(), QStringLiteral("Big Movie"));
        QTest::qWait(100);
        QCOMPARE(chain.episodeRequests(), 0);
        QVERIFY(!chain.controller.hasNext());
        QVERIFY(!chain.controller.hasPrevious());
        QVERIFY(!chain.controller.next());
    }

    void aSeriesListForAnItemThatWasLeftIsIgnored()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        chain.server.holdListings = true;
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_COMPARE(chain.episodeRequests(), 1);
        // Another item starts while the list is on its way
        chain.server.trickplayMetadataBody = movieBody(kMovie, QStringLiteral("Big Movie"));
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QJsonArray episodes;
        for (int number = 1; number <= 5; ++number)
            episodes.append(episodeObject(episodeId(number), QStringLiteral("Episode"), 1, number));
        Q_EMIT chain.client.episodesReady(episodes, 5, kEpisodeChannel);
        QTRY_COMPARE(chain.controller.title(), QStringLiteral("Big Movie"));
        QVERIFY(!chain.controller.hasNext());
        QVERIFY(!chain.controller.hasPrevious());
    }

    void theEndOfAnEpisodeFollowsTheAutoplayPreference()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        Kinefin::PlaybackEngine *engine = chain.engine();
        QVERIFY(engine);
        QSignalSpy finished(&chain.controller, &PlayerController::finished);

        // Autoplay off: the end of the file ends the playback, the buttons still work
        chain.controller.setNextEpisodeAutoPlay(false);
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        // No mpv answers, so the item is made not loading the way a refused PlaybackInfo does
        Q_EMIT chain.client.playbackInfoFailed(episodeId(3), 500, QString());
        QVERIFY(!chain.controller.loading());
        Q_EMIT engine->streamEnded(QStringLiteral("eof"));
        QCOMPARE(finished.size(), 1);
        QVERIFY(chain.controller.itemId().isEmpty());
        QVERIFY(!chain.controller.hasNext());

        // Autoplay on: the end of the file starts the next episode
        chain.controller.setNextEpisodeAutoPlay(true);
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        Q_EMIT chain.client.playbackInfoFailed(episodeId(3), 500, QString());
        Q_EMIT engine->streamEnded(QStringLiteral("eof"));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(chain.controller.itemId(), episodeId(4));
        QVERIFY(chain.controller.hasPrevious());
    }

    void aLongSeriesIsReadPageByPage()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.generatedEpisodeTotal = 250;
        chain.server.trickplayMetadataBody =
            toBody(episodeObject(episodeId(230), QStringLiteral("Late"), 3, 10));
        QVERIFY(chain.controller.playItem(episodeId(230), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        QVERIFY(chain.controller.hasPrevious());
        QCOMPARE(chain.episodeRequests(), 3);
        const QList<QByteArray> routes =
            chain.server.listingRoutes.value(QStringLiteral("Episodes"));
        QVERIFY(routes.at(0).contains("startIndex=0"));
        QVERIFY(routes.at(1).contains("startIndex=100"));
        QVERIFY(routes.at(2).contains("startIndex=200"));
        QVERIFY(chain.controller.next());
        QCOMPARE(chain.controller.itemId(), episodeId(231));
    }

    void aFailedSeriesListLeavesTheEpisodeAlone()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        chain.server.listingStatus.insert(QStringLiteral("Episodes"), 500);
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_COMPARE(chain.episodeRequests(), 1);
        QTRY_COMPARE(chain.controller.title(), QStringLiteral("Abyss Station"));
        QTest::qWait(100);
        QVERIFY(!chain.controller.hasNext());
        QVERIFY(!chain.controller.hasPrevious());
        // The next item asks again, and a good answer builds the playlist
        chain.server.listingStatus.remove(QStringLiteral("Episodes"));
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_VERIFY(chain.controller.hasNext());
    }

    void repeatOneRepeatsAnEpisodeWithoutAutoplay()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        Kinefin::PlaybackEngine *engine = chain.engine();
        QVERIFY(engine);
        QSignalSpy finished(&chain.controller, &PlayerController::finished);
        chain.controller.setNextEpisodeAutoPlay(false);
        chain.controller.setRepeatMode(2);
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        Q_EMIT chain.client.playbackInfoFailed(episodeId(3), 500, QString());
        Q_EMIT engine->streamEnded(QStringLiteral("eof"));
        QCOMPARE(finished.size(), 0);
        QCOMPARE(chain.controller.itemId(), episodeId(3));
    }

    void repeatPlaysALoneMovieAgainAfterARealEnd()
    {
        Chain chain;
        QVERIFY(chain.ok);
        Kinefin::PlaybackEngine *engine = chain.engine();
        QVERIFY(engine);
        QSignalSpy finished(&chain.controller, &PlayerController::finished);
        QVERIFY(chain.controller.playItem(kMovie, 0));
        // No mpv ran for this item yet: it stands alone from here, and its PlaybackInfo is made to fail
        QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        for (const int mode : {2, 1}) {
            chain.controller.setRepeatMode(mode);
            QVERIFY(chain.controller.playItem(kMovie, 0));
            Q_EMIT chain.client.playbackInfoFailed(kMovie, 500, QString());
            QVERIFY(!chain.controller.loading());
            chain.endsAfter(2100, 100);
            QCOMPARE(finished.size(), 0);
            QCOMPARE(chain.controller.itemId(), kMovie);
            // The item is asked for again
            QVERIFY(chain.controller.loading());
            QVERIFY(chain.controller.errorText().isEmpty());
        }
        // Without repeat the end of a lone movie ends the playback
        chain.controller.setRepeatMode(0);
        Q_EMIT chain.client.playbackInfoFailed(kMovie, 500, QString());
        Q_EMIT engine->streamEnded(QStringLiteral("eof"));
        QCOMPARE(finished.size(), 1);
        QVERIFY(chain.controller.itemId().isEmpty());
    }

    void repeatDoesNotRestartAnEndThatIsNotReal()
    {
        Chain chain;
        QVERIFY(chain.ok);
        Kinefin::PlaybackEngine *engine = chain.engine();
        QVERIFY(engine);
        QSignalSpy finished(&chain.controller, &PlayerController::finished);
        chain.controller.setRepeatMode(2);
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        // An eof far from the duration is a dropped connection, however long it played
        Q_EMIT chain.client.playbackInfoFailed(kMovie, 500, QString());
        chain.endsAfter(2100, 10);
        QCOMPARE(finished.size(), 1);
        QVERIFY(chain.controller.itemId().isEmpty());
        // A stream that ends at once, at its duration, does not loop either
        QVERIFY(chain.controller.playItem(kMovie, 0));
        Q_EMIT chain.client.playbackInfoFailed(kMovie, 500, QString());
        chain.endsAfter(0, 100);
        QCOMPARE(finished.size(), 2);
        QVERIFY(chain.controller.itemId().isEmpty());
    }

    void repeatAllAdvancesThroughTheEpisodesAndWrapsWithoutAutoplay()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        Kinefin::PlaybackEngine *engine = chain.engine();
        QVERIFY(engine);
        QSignalSpy finished(&chain.controller, &PlayerController::finished);
        chain.controller.setNextEpisodeAutoPlay(false);
        chain.controller.setRepeatMode(1);
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        // An explicit repeat means continue: the autoplay preference does not stop it
        for (const int expected : {4, 5, 1}) {
            Q_EMIT chain.client.playbackInfoFailed(chain.controller.itemId(), 500, QString());
            Q_EMIT engine->streamEnded(QStringLiteral("eof"));
            QCOMPARE(finished.size(), 0);
            QCOMPARE(chain.controller.itemId(), episodeId(expected));
        }
        // Without repeat the same preference ends the playback
        chain.controller.setRepeatMode(0);
        Q_EMIT chain.client.playbackInfoFailed(chain.controller.itemId(), 500, QString());
        Q_EMIT engine->streamEnded(QStringLiteral("eof"));
        QCOMPARE(finished.size(), 1);
    }

    void theAspectModeStartsAsTheSettingForEveryItem()
    {
        Chain chain("[video]\naspect=stretch\n");
        QVERIFY(chain.ok);
        QCOMPARE(chain.controller.aspectMode(), QStringLiteral("stretch"));
        chain.controller.setAspectMode(QStringLiteral("zoom"));
        QCOMPARE(chain.controller.aspectMode(), QStringLiteral("zoom"));
        // `custom` is the setting's own value, never a choice, and an unknown name is none
        chain.controller.setAspectMode(QStringLiteral("custom"));
        chain.controller.setAspectMode(QStringLiteral("bogus"));
        QCOMPARE(chain.controller.aspectMode(), QStringLiteral("zoom"));
        QCOMPARE(chain.settings.value(QStringLiteral("video"), QStringLiteral("aspect")).toString(),
                 QStringLiteral("stretch"));
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QCOMPARE(chain.controller.aspectMode(), QStringLiteral("stretch"));
        chain.controller.setAspectMode(QStringLiteral("noscaling"));
        QCOMPARE(chain.controller.aspectMode(), QStringLiteral("noscaling"));
        chain.controller.stop();
        QCOMPARE(chain.controller.aspectMode(), QStringLiteral("stretch"));
    }

    void aCustomAspectSettingReadsAsCustomAndAutoIsStillAChoice()
    {
        Chain chain("[video]\naspect=custom\n");
        QVERIFY(chain.ok);
        QCOMPARE(chain.controller.aspectMode(), QStringLiteral("custom"));
        chain.controller.setAspectMode(QStringLiteral("normal"));
        QCOMPARE(chain.controller.aspectMode(), QStringLiteral("normal"));
    }

    void thereIsNoPlaybackInfoWithoutASource()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.controller.playbackInfo().isEmpty());
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QVERIFY(chain.controller.playbackInfo().isEmpty());
    }

    void shuffleKeepsTheSeriesInOrder()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        chain.controller.setShuffle(true);
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        QVERIFY(chain.controller.next());
        QCOMPARE(chain.controller.itemId(), episodeId(4));
    }

    void stoppingDropsThePlaylist()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        chain.controller.stop();
        QVERIFY(!chain.controller.hasNext());
        QVERIFY(!chain.controller.hasPrevious());
        QVERIFY(chain.controller.title().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPlayerController)
#include "test_player_controller.moc"
