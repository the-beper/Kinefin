// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The player controller without a renderer: the engine is not attached, so an item stays loading and nothing is negotiated, but the item's own request, the playlist of a series and the end-of-file decision all run against the fake server; the tests of a repeat after a real end start the engine on its own (vo=null, ao=null) to give it the position and the duration of a played file
#include "FakeJellyfin.h"
#include "WavClip.h"

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
using Kinefin::TrackChoice;
using Kinefin::TrackFact;

namespace {
const QString kMovie = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");
const QString kOtherMovie = QStringLiteral("0f1e2d3c4b5a69788796a5b4c3d2e1f0");
const QString kThirdMovie = QStringLiteral("1f2e3d4c5b6a79887766a5b4c3d2e1f0");
const QString kSeries = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
constexpr qint64 kSecond = 10'000'000;
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

// A media segment as the client hands it on, in seconds
QJsonObject segment(const QString &type, qint64 start, qint64 end)
{
    return {{QStringLiteral("type"), type},
            {QStringLiteral("start"), start * kSecond},
            {QStringLiteral("end"), end * kSecond}};
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
    // Without a profile id (the default, an invalid one) the controller keeps its memory files off the disk
    explicit Chain(const QByteArray &conf = {}, const QString &profileId = {})
        : queue(&client), settings(settingsFile(dir, conf)),
          controller(client, queue, settings, profileId)
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
    // mpv reports where the item is (`position` of `duration`, in seconds), as it would while it plays
    void playsAt(double position, double duration = 1800)
    {
        auto *mpv = engine()->controller();
        Q_EMIT mpv->propertyChanged(QStringLiteral("duration"), duration);
        Q_EMIT mpv->propertyChanged(QStringLiteral("time-pos"), position);
    }
    // The third of five episodes, its series listed, mpv running without a file and the item's own source refused: the position and the duration come from playsAt; `segments` arrive while it opens
    bool episodeThreeOpened(const QJsonArray &segments = {})
    {
        serveEpisodeThree();
        if (!controller.playItem(episodeId(3), 0)
            || !engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")})
            || !QTest::qWaitFor([this] { return controller.hasNext(); }))
            return false;
        if (!segments.isEmpty())
            Q_EMIT client.mediaSegmentsReady(episodeId(3), segments);
        Q_EMIT client.playbackInfoFailed(episodeId(3), 500, QString());
        return !controller.loading();
    }
    // Plays a generated clip of `seconds` as the current item, in place of the stream the server would give
    bool opensClip(const QString &itemId, int seconds)
    {
        Kinefin::PlaybackSource source;
        source.itemId = itemId;
        source.mediaSourceId = QStringLiteral("c1b2a3d4e5f6071829304a5b6c7d8e9f");
        source.playSessionId = QStringLiteral("s1");
        source.method = Kinefin::PlaybackSource::Method::Direct;
        source.url = QUrl(TestWav::writeWavClip(dir, seconds));
        return !source.url.isEmpty() && engine()->open(source)
               && QTest::qWaitFor([this] { return controller.canSeek(); }, 10000);
    }
    QString upNextTitle() const
    {
        return controller.upNext().value(QStringLiteral("title")).toString();
    }
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
    // The profile tests keep their files here
    void initTestCase()
    {
        QVERIFY(m_data.isValid());
        qputenv("XDG_DATA_HOME", m_data.path().toUtf8());
    }

    void rememberedTracksFollowTheirPrecedence()
    {
        using Kinefin::rememberedTracks;
        const QList<TrackFact> audio{{1, QStringLiteral("eng"), false},
                                     {2, QStringLiteral("jpn"), false}};
        const QList<TrackFact> subtitles{{3, QStringLiteral("eng"), false},
                                         {4, QStringLiteral("eng"), true}};
        Kinefin::SeriesChoice series;
        series.audio = TrackChoice{QStringLiteral("jpn"), false, 1};
        series.subtitle = TrackChoice{QStringLiteral("en"), true, 0};
        const QJsonObject item{{QStringLiteral("audio"), 1}, {QStringLiteral("subtitle"), 0}};
        // The series' choices, found by language
        auto tracks = rememberedTracks({}, series, audio, subtitles, true, true);
        QCOMPARE(tracks.audio, 2);
        QCOMPARE(tracks.subtitle, 4);
        QVERIFY(!tracks.subtitleOff);
        // The item's own choice wins, subtitles off included
        tracks = rememberedTracks(item, series, audio, subtitles, true, true);
        QCOMPARE(tracks.audio, 1);
        QVERIFY(tracks.subtitleOff);
        // An id this source lacks leaves it to the series
        tracks = rememberedTracks(
            QJsonObject{{QStringLiteral("audio"), 9}, {QStringLiteral("subtitle"), 9}}, series,
            audio, subtitles, true, true);
        QCOMPARE(tracks.audio, 2);
        QCOMPARE(tracks.subtitle, 4);
        // A kind the user does not remember takes neither the item's nor the series' choice
        tracks = rememberedTracks(item, series, audio, subtitles, false, true);
        QCOMPARE(tracks.audio, 0);
        QVERIFY(tracks.subtitleOff);
        tracks = rememberedTracks(item, series, audio, subtitles, true, false);
        QCOMPARE(tracks.audio, 1);
        QCOMPARE(tracks.subtitle, 0);
        QVERIFY(!tracks.subtitleOff);
        // Subtitles off for the whole series
        series.subtitle.reset();
        series.subtitleOff = true;
        QVERIFY(rememberedTracks({}, series, audio, subtitles, true, true).subtitleOff);
    }

    void seriesSpeedAndAspectComeBackOnTheNextEpisode()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.episodeThreeOpened());
        QVERIFY(chain.opensClip(episodeId(3), 30));
        QVERIFY(!chain.controller.hasSeriesChoices());
        chain.controller.setRate(1.5);
        chain.controller.setAspectMode(QStringLiteral("zoom"));
        QVERIFY(chain.controller.hasSeriesChoices());
        auto *store = chain.controller.findChild<Kinefin::SeriesPreferences *>();
        QVERIFY(store);
        QSignalSpy writes(store, &Kinefin::SeriesPreferences::changed);
        // The next episode starts with them; restoring them writes nothing. Its source is asked once the first file stopped, with the settings' speed applied again, and never answered
        chain.server.holdPlaybackInfo = true;
        const int sourceRequests = chain.server.playbackInfoRequests;
        QVERIFY(chain.controller.next());
        QTRY_VERIFY(chain.server.playbackInfoRequests > sourceRequests);
        QTRY_VERIFY(!chain.controller.title().isEmpty());
        QTRY_COMPARE(chain.controller.rate(), 1.0);
        QCOMPARE(chain.controller.aspectMode(), QStringLiteral("normal"));
        Q_EMIT chain.client.playbackInfoFailed(episodeId(4), 500, QString());
        QVERIFY(!chain.controller.active());
        QVERIFY(chain.opensClip(episodeId(4), 30));
        QTRY_COMPARE(chain.controller.rate(), 1.5);
        QCOMPARE(chain.controller.aspectMode(), QStringLiteral("zoom"));
        QVERIFY(chain.controller.hasSeriesChoices());
        QCOMPARE(writes.size(), 0);
        // Choosing the default speed and the setting's aspect removes them, and with them the series' entry
        chain.controller.setRate(1.0);
        QVERIFY(chain.controller.hasSeriesChoices());
        chain.controller.setAspectMode(QStringLiteral("normal"));
        QVERIFY(!chain.controller.hasSeriesChoices());
        // Forget Series Choices takes what was chosen again
        chain.controller.setRate(2.0);
        QVERIFY(chain.controller.hasSeriesChoices());
        chain.controller.forgetSeriesChoices();
        QVERIFY(!chain.controller.hasSeriesChoices());
        QTRY_COMPARE(chain.controller.rate(), 2.0);
    }

    void aMovieKeepsNoSeriesChoice()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.trickplayMetadataBody = movieBody(kMovie, QStringLiteral("Big Movie"));
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QVERIFY(chain.engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        QTRY_COMPARE(chain.controller.title(), QStringLiteral("Big Movie"));
        Q_EMIT chain.client.playbackInfoFailed(kMovie, 500, QString());
        QVERIFY(chain.opensClip(kMovie, 30));
        chain.controller.setRate(1.5);
        QVERIFY(!chain.controller.hasSeriesChoices());
    }

    void forgettingAUserTakesTheirSeriesChoicesAndTrackMemory()
    {
        Chain chain({}, QStringLiteral("forgetting"));
        QVERIFY(chain.ok);
        QVERIFY(chain.episodeThreeOpened());
        QVERIFY(chain.opensClip(episodeId(3), 30));
        chain.controller.setRate(1.5);
        QTRY_VERIFY(!chain.controller.audioTracks().isEmpty());
        chain.controller.selectAudioTrack(chain.controller.audioTracks()
                                              .constFirst()
                                              .toMap()
                                              .value(QStringLiteral("id"))
                                              .toInt());
        const QString server = chain.client.serverId().toLower();
        const QString user = chain.client.userId().toLower();
        const QString data = m_data.filePath(QStringLiteral("kinefin/profiles/forgetting"));
        const QString seriesFile = data + QStringLiteral("/series/") + server + QLatin1Char('_')
                                   + user + QStringLiteral(".json");
        const QString memoryKey =
            server + QLatin1Char('/') + user + QLatin1Char('/') + episodeId(3);
        const auto remembered = [&data] {
            QFile file(data + QStringLiteral("/storage.json"));
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll())
                                                        .object()
                                                        .value(QStringLiteral("tracks"))
                                                        .toObject()
                                                  : QJsonObject();
        };
        // The series' file waits a second; the track memory is written at once
        QTRY_VERIFY(QFile::exists(seriesFile));
        QVERIFY(remembered().contains(memoryKey));
        chain.controller.forgetStoredUser(chain.client.serverId(), chain.client.userId());
        QVERIFY(!QFile::exists(seriesFile));
        QVERIFY(!remembered().contains(memoryKey));
        QVERIFY(!chain.controller.hasSeriesChoices());
    }

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

    void theTitleAndTheChaptersAreClearedWhenTheAccountLeaves()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.serveEpisodeThree();
        QJsonObject episode = episodeObject(episodeId(3), QStringLiteral("Weightless"), 2, 6);
        episode.insert(
            QStringLiteral("Chapters"),
            QJsonArray{QJsonObject{{QStringLiteral("Name"), QStringLiteral("Cold Open")},
                                   {QStringLiteral("StartPositionTicks"), 0}},
                       QJsonObject{{QStringLiteral("Name"), QStringLiteral("Act One")},
                                   {QStringLiteral("StartPositionTicks"), 600'000'000}}});
        chain.server.trickplayMetadataBody = toBody(episode);
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        QTRY_VERIFY(chain.controller.hasNext());
        QVERIFY(!chain.controller.title().isEmpty());
        QVERIFY(!chain.controller.subtitle().isEmpty());
        QCOMPARE(chain.controller.chapters().size(), 2);
        chain.controller.onAccountLeaving();
        QVERIFY(chain.controller.title().isEmpty());
        QVERIFY(chain.controller.subtitle().isEmpty());
        QVERIFY(chain.controller.chapters().isEmpty());
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

    void chaptersComeSortedFromThePlayedItem()
    {
        Chain chain;
        QVERIFY(chain.ok);
        constexpr qint64 second = 10'000'000;
        const auto chapter = [](const QJsonValue &name, qint64 seconds) {
            return QJsonObject{{QStringLiteral("Name"), name},
                               {QStringLiteral("StartPositionTicks"), seconds * second}};
        };
        QJsonObject movie =
            QJsonDocument::fromJson(movieBody(kMovie, QStringLiteral("Big Movie"))).object();
        // Out of order, a second chapter on one start, an entry that is no object, two without a name, and one whose name holds a line break and a right-to-left override
        const QString broken = QStringLiteral("Mid") + QChar(0x202E) + QStringLiteral("\ndle");
        movie.insert(QStringLiteral("Chapters"),
                     QJsonArray{chapter(broken, 600), chapter(QStringLiteral("Start"), 0),
                                chapter(QStringLiteral("Again"), 600),
                                QStringLiteral("not a chapter"),
                                chapter(QStringLiteral("  "), 1200), chapter(QJsonValue(), 1800)});
        chain.server.trickplayMetadataBody = toBody(movie);
        QSignalSpy changed(&chain.controller, &PlayerController::chaptersChanged);
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTRY_COMPARE(chain.controller.chapters().size(), 4);
        QVERIFY(chain.server.trickplayMetadataRoute.contains("fields=Trickplay,Chapters"));
        const QStringList names{QStringLiteral("Start"), QStringLiteral("Mid dle"),
                                QStringLiteral("Chapter 3"), QStringLiteral("Chapter 4")};
        const QStringList times{QStringLiteral("00:00"), QStringLiteral("10:00"),
                                QStringLiteral("20:00"), QStringLiteral("30:00")};
        for (int i = 0; i < 4; ++i) {
            const QVariantMap entry = chain.controller.chapters().at(i).toMap();
            QCOMPARE(entry.value(QStringLiteral("name")).toString(), names.at(i));
            QCOMPARE(entry.value(QStringLiteral("time")).toString(), times.at(i));
            QCOMPARE(entry.value(QStringLiteral("startTicks")).toLongLong(),
                     QList<qint64>({0, 600, 1200, 1800}).at(i) * second);
        }
        QCOMPARE(chain.controller.chapterAt(0), 0);
        QCOMPARE(chain.controller.chapterAt(1199 * second), 1);
        QCOMPARE(chain.controller.chapterAt(1200 * second), 2);
        QCOMPARE(chain.controller.chapterAt(5000 * second), 3);
        QCOMPARE(chain.controller.currentChapter(), 0);
        // The next item starts without them, and a lone chapter counts as none
        movie = QJsonDocument::fromJson(movieBody(kOtherMovie, QStringLiteral("Other"))).object();
        movie.insert(QStringLiteral("Chapters"), QJsonArray{chapter(QStringLiteral("Only"), 0)});
        chain.server.trickplayMetadataBody = toBody(movie);
        changed.clear();
        QVERIFY(chain.controller.playItem(kOtherMovie, 0));
        QVERIFY(chain.controller.chapters().isEmpty());
        QCOMPARE(changed.size(), 1);
        QTRY_COMPARE(chain.controller.title(), QStringLiteral("Other"));
        QVERIFY(chain.controller.chapters().isEmpty());
        QCOMPARE(chain.controller.chapterAt(0), -1);
        QCOMPARE(chain.controller.currentChapter(), -1);
        QCOMPARE(changed.size(), 1);
    }

    void chaptersFollowTheDurationMpvReports()
    {
        Chain chain;
        QVERIFY(chain.ok);
        constexpr qint64 second = 10'000'000;
        Kinefin::PlaybackEngine *engine = chain.engine();
        QVERIFY(engine);
        QJsonObject movie =
            QJsonDocument::fromJson(movieBody(kMovie, QStringLiteral("Big Movie"))).object();
        QJsonArray chapters;
        for (const qint64 start : {0, 50, 100, 150})
            chapters.append(QJsonObject{{QStringLiteral("Name"), QStringLiteral("C%1").arg(start)},
                                        {QStringLiteral("StartPositionTicks"), start * second}});
        movie.insert(QStringLiteral("Chapters"), chapters);
        chain.server.trickplayMetadataBody = toBody(movie);
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        QTRY_COMPARE(chain.controller.chapters().size(), 4);
        auto *mpv = engine->controller();
        // A chapter at or past the end of the file is not one to go to
        Q_EMIT mpv->propertyChanged(QStringLiteral("duration"), 100.0);
        QCOMPARE(chain.controller.chapters().size(), 2);
        // A video of an hour or more names its chapters' times with the hours
        Q_EMIT mpv->propertyChanged(QStringLiteral("duration"), 7200.0);
        QCOMPARE(chain.controller.chapters().size(), 4);
        QCOMPARE(chain.controller.chapters().at(1).toMap().value(QStringLiteral("time")).toString(),
                 QStringLiteral("00:00:50"));
        // The current chapter is notified on entering a chapter, not with every position
        QSignalSpy current(&chain.controller, &PlayerController::currentChapterChanged);
        for (const double position : {10.0, 20.0, 60.0, 70.0, 99.0})
            Q_EMIT mpv->propertyChanged(QStringLiteral("time-pos"), position);
        QCOMPARE(current.size(), 1);
        QCOMPARE(chain.controller.currentChapter(), 1);
        // Within the duration only one is left, which counts as none
        Q_EMIT mpv->propertyChanged(QStringLiteral("duration"), 40.0);
        QVERIFY(chain.controller.chapters().isEmpty());
    }

    void chapterStepsLandOnChapterStarts()
    {
        constexpr qint64 second = 10'000'000;
        QList<Kinefin::ChapterSummary> chapters;
        for (const qint64 start : {10, 100, 200})
            chapters.append({QStringLiteral("C%1").arg(start), start * second});
        using Kinefin::chapterStepTarget;
        // Before the first chapter: forward to it, back to the start of the video
        QCOMPARE(chapterStepTarget(chapters, 5 * second, 1), 10 * second);
        QCOMPARE(chapterStepTarget(chapters, 5 * second, -1), 0);
        // Forward: the next chapter, nowhere from the last
        QCOMPARE(chapterStepTarget(chapters, 150 * second, 1), 200 * second);
        QCOMPARE(chapterStepTarget(chapters, 250 * second, 1), -1);
        // Back: the start of the current chapter more than 3 s into it, else the previous chapter
        QCOMPARE(chapterStepTarget(chapters, 150 * second, -1), 100 * second);
        QCOMPARE(chapterStepTarget(chapters, 103 * second + 1, -1), 100 * second);
        QCOMPARE(chapterStepTarget(chapters, 103 * second, -1), 10 * second);
        QCOMPARE(chapterStepTarget(chapters, 11 * second, -1), 0);
        // A seek that landed a frame before the start it asked for counts as there
        const qint64 justBefore = 100 * second - second / 25;
        QCOMPARE(chapterStepTarget(chapters, justBefore, 1), 200 * second);
        QCOMPARE(chapterStepTarget(chapters, justBefore, -1), 10 * second);
        QCOMPARE(chapterStepTarget({}, 50 * second, 1), -1);
        QCOMPARE(chapterStepTarget(chapters, 50 * second, 0), -1);
        QCOMPARE(Kinefin::chapterIndexAt(chapters, 9 * second), -1);
        QCOMPARE(Kinefin::chapterIndexAt(chapters, 100 * second), 1);
    }

    void upNextWindowShapes()
    {
        using Kinefin::upNextWindow;
        const qint64 hour = 3600 * kSecond;
        // An Outro that runs to the end: the card at its start, the next item 10 s later
        auto window =
            upNextWindow(hour, {segment(QStringLiteral("Outro"), 3500, 3595).toVariantMap()});
        QVERIFY(window);
        QCOMPARE(window->showAt, 3500 * kSecond);
        QCOMPARE(window->advanceAt, 3510 * kSecond);
        // One that starts in the last 10 s advances at the end
        window = upNextWindow(hour, {segment(QStringLiteral("Outro"), 3595, 3600).toVariantMap()});
        QCOMPARE(window->advanceAt, hour);
        // An Outro followed by a scene, an Intro or no segment: the last 30 s, advancing at the end
        for (const QVariantList &segments :
             {QVariantList{segment(QStringLiteral("Outro"), 3000, 3100).toVariantMap()},
              QVariantList{segment(QStringLiteral("Intro"), 3580, 3600).toVariantMap()},
              QVariantList{}}) {
            window = upNextWindow(hour, segments);
            QVERIFY(window);
            QCOMPARE(window->showAt, 3570 * kSecond);
            QCOMPARE(window->advanceAt, hour);
        }
        // None under 10 minutes or without a duration
        QVERIFY(!upNextWindow(599 * kSecond, {}));
        QVERIFY(upNextWindow(600 * kSecond, {}));
        QVERIFY(!upNextWindow(0, {}));
    }

    void upNextOffersWhatTheEndWouldStart()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.episodeThreeOpened());
        QSignalSpy changed(&chain.controller, &PlayerController::upNextChanged);
        // Half an hour without an Outro: the card is up for the last 30 s
        chain.playsAt(1769);
        QVERIFY(!chain.controller.upNextShown());
        chain.playsAt(1775);
        QVERIFY(chain.controller.upNextShown());
        QCOMPARE(chain.upNextTitle(), QStringLiteral("Episode 4"));
        QVERIFY(chain.controller.upNext().value(QStringLiteral("subtitle")).toString().isEmpty());
        const QString image =
            chain.controller.upNext().value(QStringLiteral("imageUrl")).toUrl().toString();
        QVERIFY2(image.contains(QStringLiteral("/Items/") + episodeId(4)
                                + QStringLiteral("/Images/Primary")),
                 qPrintable(image));
        QCOMPARE(chain.controller.upNextSeconds(), 25);
        // The count moves once a second, not with every position
        changed.clear();
        chain.playsAt(1775.5);
        chain.playsAt(1775.9);
        QCOMPARE(changed.size(), 0);
        chain.playsAt(1776.1);
        QCOMPARE(chain.controller.upNextSeconds(), 24);
        QCOMPARE(changed.size(), 1);
        // Autoplay off: an episode played alone stops at its end, unless a repeat mode says otherwise; Repeat One replays it
        chain.controller.setNextEpisodeAutoPlay(false);
        QVERIFY(!chain.controller.upNextShown());
        chain.controller.setRepeatMode(1);
        QVERIFY(chain.controller.upNextShown());
        chain.controller.setRepeatMode(2);
        QVERIFY(!chain.controller.upNextShown());
        chain.controller.setRepeatMode(0);
        chain.controller.setNextEpisodeAutoPlay(true);
        QVERIFY(chain.controller.upNextShown());
        // Under 10 minutes there is no card
        chain.playsAt(590, 599);
        QVERIFY(!chain.controller.upNextShown());
        // Back before the last 30 s it goes
        chain.playsAt(1700);
        QVERIFY(!chain.controller.upNextShown());
    }

    void upNextOnTheLastEpisodeWrapsOnlyUnderRepeatAll()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.generatedEpisodeTotal = 5;
        chain.server.trickplayMetadataBody =
            toBody(episodeObject(episodeId(5), QStringLiteral("Episode 5"), 1, 5));
        QVERIFY(chain.controller.playItem(episodeId(5), 0));
        QVERIFY(chain.engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        QTRY_VERIFY(chain.controller.hasPrevious());
        Q_EMIT chain.client.playbackInfoFailed(episodeId(5), 500, QString());
        chain.playsAt(1790);
        QVERIFY(!chain.controller.upNextShown());
        chain.controller.setRepeatMode(1);
        QVERIFY(chain.controller.upNextShown());
        QCOMPARE(chain.upNextTitle(), QStringLiteral("Episode 1"));
    }

    void upNextCancelEndsThePlaybackAtTheEnd()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.episodeThreeOpened());
        QSignalSpy finished(&chain.controller, &PlayerController::finished);
        chain.playsAt(1780);
        QVERIFY(chain.controller.upNextShown());
        chain.controller.cancelUpNext();
        QVERIFY(!chain.controller.upNextShown());
        // It stays down while the item plays on, and a repeat mode does not replay the item either
        chain.playsAt(1790);
        chain.controller.setRepeatMode(1);
        QVERIFY(!chain.controller.upNextShown());
        Q_EMIT chain.engine()->streamEnded(QStringLiteral("eof"));
        QCOMPARE(finished.size(), 1);
        QVERIFY(chain.controller.itemId().isEmpty());
    }

    void upNextPlayNowAndTheOutroStartTheNextEpisode()
    {
        Chain chain;
        QVERIFY(chain.ok);
        const QJsonArray outro{segment(QStringLiteral("Outro"), 1700, 1795)};
        QVERIFY(chain.episodeThreeOpened(outro));
        chain.playsAt(1650);
        QVERIFY(!chain.controller.upNextShown());
        // From the Outro's start the card counts the 10 s to the next item
        chain.playsAt(1705);
        QVERIFY(chain.controller.upNextShown());
        QCOMPARE(chain.controller.upNextSeconds(), 5);
        chain.controller.playUpNext();
        QCOMPARE(chain.controller.itemId(), episodeId(4));
        QVERIFY(!chain.controller.upNextShown());
        QVERIFY(chain.controller.loading());
        // On that one, playing into the advance starts the episode after it
        Q_EMIT chain.client.mediaSegmentsReady(episodeId(4), outro);
        Q_EMIT chain.client.playbackInfoFailed(episodeId(4), 500, QString());
        chain.playsAt(1650);
        chain.playsAt(1705);
        QVERIFY(chain.controller.upNextShown());
        QCOMPARE(chain.upNextTitle(), QStringLiteral("Episode 5"));
        chain.playsAt(1710.2);
        QCOMPARE(chain.controller.itemId(), episodeId(5));
    }

    void upNextReachedWithoutTheCardLeavesTheEndToMoveOn()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.episodeThreeOpened({segment(QStringLiteral("Outro"), 1700, 1795)}));
        // Past the early advance without the card ever up (a resume there): nothing starts early, and the end moves on as usual
        chain.playsAt(1720);
        QVERIFY(!chain.controller.upNextShown());
        QCOMPARE(chain.controller.itemId(), episodeId(3));
        Q_EMIT chain.engine()->streamEnded(QStringLiteral("eof"));
        QCOMPARE(chain.controller.itemId(), episodeId(4));
    }

    void aRepeatOneChosenAfterCancelStillReplays()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.episodeThreeOpened());
        QSignalSpy finished(&chain.controller, &PlayerController::finished);
        chain.playsAt(1780);
        chain.controller.cancelUpNext();
        chain.controller.setRepeatMode(2);
        Q_EMIT chain.engine()->streamEnded(QStringLiteral("eof"));
        QCOMPARE(finished.size(), 0);
        QCOMPARE(chain.controller.itemId(), episodeId(3));
        QVERIFY(chain.controller.loading());
    }

    void upNextUnderShuffleStartsTheItemItOffers()
    {
        Chain chain;
        QVERIFY(chain.ok);
        const QHash<QString, QString> names{{kMovie, QStringLiteral("Arcturus")},
                                            {kOtherMovie, QStringLiteral("Boreal")},
                                            {kThirdMovie, QStringLiteral("Cygnus")}};
        for (const QString &id : {kMovie, kOtherMovie, kThirdMovie}) {
            chain.server.itemsById.insert(id, movieBody(id, names.value(id)));
            QSignalSpy added(&chain.queue, &Kinefin::PlayQueue::added);
            QVERIFY(chain.queue.add(id, Kinefin::PlayQueue::Last));
            QTRY_COMPARE(added.size(), 1);
        }
        QCOMPARE(chain.queue.count(), 3);
        chain.controller.setShuffle(true);
        QVERIFY(chain.engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        QVERIFY(chain.controller.playQueue(0));
        // Each end starts the item the card named, not a new draw: eight rounds leave a lucky draw no chance
        for (int round = 0; round < 8; ++round) {
            const QString current = chain.controller.itemId();
            Q_EMIT chain.client.playbackInfoFailed(current, 500, QString());
            chain.playsAt(1000);
            chain.playsAt(1775);
            QVERIFY(chain.controller.upNextShown());
            const QString offered = names.key(chain.upNextTitle());
            QVERIFY(!offered.isEmpty());
            QVERIFY(offered != current);
            // Next while the card is up is its Play Now
            if (round % 2)
                QVERIFY(chain.controller.next());
            else
                Q_EMIT chain.engine()->streamEnded(QStringLiteral("eof"));
            QCOMPARE(chain.controller.itemId(), offered);
        }
        // Taking the offered item out of the queue offers the one left
        const QString current = chain.controller.itemId();
        Q_EMIT chain.client.playbackInfoFailed(current, 500, QString());
        chain.playsAt(1000);
        chain.playsAt(1775);
        const QString offered = names.key(chain.upNextTitle());
        QVERIFY(chain.queue.remove(chain.queue.itemIds().indexOf(offered)));
        QVERIFY(chain.controller.upNextShown());
        const QString left = names.key(chain.upNextTitle());
        QVERIFY(!left.isEmpty() && left != offered && left != current);
    }

    void skipIntrosSkipsEachIntroOnceAndNeverOneSeekedInto()
    {
        Chain chain("[video]\nskip_intros=true\n");
        QVERIFY(chain.ok);
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QVERIFY(chain.engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        Q_EMIT chain.client.mediaSegmentsReady(
            kMovie, QJsonArray{segment(QStringLiteral("Intro"), 0, 5),
                               segment(QStringLiteral("Intro"), 15, 20)});
        Q_EMIT chain.client.playbackInfoFailed(kMovie, 500, QString());
        QVERIFY(chain.opensClip(kMovie, 30));
        // The first Intro goes as soon as a seek can be made
        QTRY_VERIFY(chain.controller.positionTicks() >= 5 * kSecond);
        // A seek into the second one keeps it
        chain.controller.seek(16 * kSecond);
        QTRY_VERIFY(chain.controller.positionTicks() >= 16 * kSecond);
        QTest::qWait(300);
        QVERIFY(chain.controller.positionTicks() < 19 * kSecond);
        // Back into the first one: skipped once already
        chain.controller.seek(1 * kSecond);
        QTRY_VERIFY(chain.controller.positionTicks() < 4 * kSecond);
        QTest::qWait(300);
        QVERIFY(chain.controller.positionTicks() < 4 * kSecond);
    }

    void skipIntrosLeavesWhatTheUserWentTo()
    {
        Chain chain("[video]\nskip_intros=true\n");
        QVERIFY(chain.ok);
        // Resumed at 8 s: the Intro before it stays, the one after it goes when playback reaches it
        QVERIFY(chain.controller.playItem(kMovie, 8 * kSecond));
        QVERIFY(chain.engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        Q_EMIT chain.client.mediaSegmentsReady(kMovie,
                                               QJsonArray{segment(QStringLiteral("Intro"), 0, 5),
                                                          segment(QStringLiteral("Intro"), 9, 12)});
        Q_EMIT chain.client.playbackInfoFailed(kMovie, 500, QString());
        QVERIFY(chain.opensClip(kMovie, 30));
        QTRY_VERIFY(chain.controller.positionTicks() >= 12 * kSecond);
        chain.controller.seek(1 * kSecond);
        QTRY_VERIFY(chain.controller.positionTicks() < 4 * kSecond);
        QTest::qWait(300);
        QVERIFY(chain.controller.positionTicks() < 4 * kSecond);
    }

    void skipIntrosKeepsAnIntroSeekedIntoBeforeTheSegmentsCame()
    {
        Chain chain("[video]\nskip_intros=true\n");
        QVERIFY(chain.ok);
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QVERIFY(chain.engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        Q_EMIT chain.client.playbackInfoFailed(kMovie, 500, QString());
        QVERIFY(chain.opensClip(kMovie, 30));
        chain.controller.seek(16 * kSecond);
        QTRY_VERIFY(chain.controller.positionTicks() >= 16 * kSecond);
        Q_EMIT chain.client.mediaSegmentsReady(
            kMovie, QJsonArray{segment(QStringLiteral("Intro"), 15, 20)});
        QTest::qWait(300);
        QVERIFY(chain.controller.positionTicks() < 19 * kSecond);
        QCOMPARE(chain.controller.skipLabel(), QStringLiteral("Skip intro"));
    }

    void introsStayWithoutTheSetting()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QVERIFY(chain.engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        Q_EMIT chain.client.mediaSegmentsReady(kMovie,
                                               QJsonArray{segment(QStringLiteral("Intro"), 0, 5)});
        Q_EMIT chain.client.playbackInfoFailed(kMovie, 500, QString());
        QVERIFY(chain.opensClip(kMovie, 30));
        QTRY_VERIFY(chain.controller.positionTicks() > 0);
        QTest::qWait(500);
        QVERIFY(chain.controller.positionTicks() < 4 * kSecond);
        QCOMPARE(chain.controller.skipLabel(), QStringLiteral("Skip intro"));
    }

    void upNextSeeksArmAndCancelTheCard()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.episodeThreeOpened({segment(QStringLiteral("Outro"), 580, 600)}));
        // Ten minutes of a real file, its Outro in the last 20 s: the card from 580 s, the next episode at 590 s
        QVERIFY(chain.opensClip(episodeId(3), 600));
        QTRY_VERIFY(chain.controller.durationTicks() > 595 * kSecond);
        chain.controller.seek(582 * kSecond);
        QTRY_VERIFY(chain.controller.upNextShown());
        // A seek past the advance is a Cancel
        chain.controller.seek(595 * kSecond);
        QTRY_VERIFY(chain.controller.positionTicks() >= 595 * kSecond);
        QVERIFY(!chain.controller.upNextShown());
        QCOMPARE(chain.controller.itemId(), episodeId(3));
        chain.controller.seek(582 * kSecond);
        QTRY_VERIFY(chain.controller.positionTicks() < 590 * kSecond);
        QVERIFY(!chain.controller.upNextShown());
        // Back before the card arms it again
        chain.controller.seek(100 * kSecond);
        QTRY_VERIFY(chain.controller.positionTicks() < 200 * kSecond);
        chain.controller.seek(582 * kSecond);
        QTRY_VERIFY(chain.controller.upNextShown());
        // Play Now ends the item in its credits: the stop is reported at its end, so the server counts it played
        chain.server.reportLog.clear();
        chain.controller.playUpNext();
        QCOMPARE(chain.controller.itemId(), episodeId(4));
        QTRY_VERIFY(
            std::any_of(chain.server.reportLog.cbegin(), chain.server.reportLog.cend(),
                        [](const auto &report) { return report.route.endsWith("/Stopped"); }));
        for (const auto &report : std::as_const(chain.server.reportLog)) {
            if (!report.route.endsWith("/Stopped"))
                continue;
            const QJsonObject body = QJsonDocument::fromJson(report.body).object();
            QCOMPARE(body.value(QStringLiteral("ItemId")).toString(), episodeId(3));
            QVERIFY2(body.value(QStringLiteral("PositionTicks")).toInteger() > 595 * kSecond,
                     report.body.constData());
        }
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

    void playQueueStartsAtTheGivenPositionAndRefusesOneOutOfRange()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.itemsById.insert(kMovie, movieBody(kMovie, QStringLiteral("Arcturus")));
        QSignalSpy added(&chain.queue, &Kinefin::PlayQueue::added);
        QVERIFY(chain.queue.add(kMovie, Kinefin::PlayQueue::Last));
        QTRY_COMPARE(added.size(), 1);
        QVERIFY(!chain.controller.playQueue(0, -1));
        QVERIFY(!chain.controller.playQueue(0, 24LL * 3600 * kSecond + 1));
        QVERIFY(!chain.controller.playQueue(1, 0));
        QVERIFY(chain.controller.itemId().isEmpty());
        QVERIFY(chain.engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        QVERIFY(chain.controller.playQueue(0, 90 * kSecond));
        QCOMPARE(chain.controller.itemId(), kMovie);
        QTRY_VERIFY(chain.server.playbackInfoRequests >= 1);
        QCOMPARE(QJsonDocument::fromJson(chain.server.lastPlaybackInfoBody)
                     .object()
                     .value(QStringLiteral("StartTimeTicks"))
                     .toVariant()
                     .toLongLong(),
                 90 * kSecond);
        QVERIFY(chain.controller.playQueue(0));
    }

    void streamIndexesSelectTracksOnlyInADirectPlay()
    {
        Chain chain;
        QVERIFY(chain.ok);
        // Nothing plays
        QVERIFY(!chain.controller.selectAudioStreamIndex(0));
        QVERIFY(!chain.controller.selectSubtitleStreamIndex(0));
        QVERIFY(!chain.controller.selectSubtitleStreamIndex(-1));

        QVERIFY(playsClip(chain, Kinefin::PlaybackSource::Method::Direct));
        const int audioIndex = streamIndexOf(chain, QStringLiteral("audio"));
        QVERIFY(audioIndex >= 0);
        QVERIFY(chain.controller.selectAudioStreamIndex(audioIndex));
        QVERIFY(!chain.controller.selectAudioStreamIndex(audioIndex + 1));
        // The clip has no subtitle track: the audio track's index is not a subtitle's
        QVERIFY(!chain.controller.selectSubtitleStreamIndex(audioIndex));
        QVERIFY(chain.controller.selectSubtitleStreamIndex(-1));
        for (const int index : {-2, -1, 1025, 100000})
            QVERIFY(!chain.controller.selectAudioStreamIndex(index));
        for (const int index : {-2, 1025, 100000})
            QVERIFY(!chain.controller.selectSubtitleStreamIndex(index));
    }

    void streamIndexesAreRefusedInATranscode()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(playsClip(chain, Kinefin::PlaybackSource::Method::Transcode));
        const int audioIndex = streamIndexOf(chain, QStringLiteral("audio"));
        QVERIFY(audioIndex >= 0);
        QVERIFY(!chain.controller.selectAudioStreamIndex(audioIndex));
        QVERIFY(!chain.controller.selectSubtitleStreamIndex(audioIndex));
        QVERIFY(!chain.controller.selectSubtitleStreamIndex(-1));
    }

    void playsALocalFile()
    {
        Chain chain;
        QVERIFY(chain.ok);
        const QString clip = QUrl(TestWav::writeWavClip(chain.dir, 2)).toLocalFile();
        QVERIFY(!clip.isEmpty());
        QVERIFY(chain.engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        QSignalSpy local(&chain.controller, &PlayerController::localPlaybackChanged);
        QVERIFY(chain.controller.playLocalFile(clip, kMovie, QStringLiteral("Local Movie"),
                                               QStringLiteral("Local Subtitle")));
        QVERIFY(chain.controller.localPlayback());
        QCOMPARE(chain.controller.title(), QStringLiteral("Local Movie"));
        QCOMPARE(chain.controller.subtitle(), QStringLiteral("Local Subtitle"));
        QCOMPARE(chain.server.playbackInfoRequests, 0);
        QCOMPARE(local.count(), 1);
        QTRY_VERIFY(chain.controller.canSeek());
        QVERIFY(chain.server.reportLog.isEmpty());
        chain.controller.stop();
        QTRY_VERIFY(!chain.controller.localPlayback());
        QVERIFY(chain.server.reportLog.isEmpty());
    }

  private:
    // The item plays a generated clip, handed over as the source the server would have chosen with `method`
    static bool playsClip(Chain &chain, Kinefin::PlaybackSource::Method method)
    {
        if (!chain.controller.playItem(kMovie, 0)
            || !chain.engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}))
            return false;
        Kinefin::PlaybackSource source;
        source.itemId = kMovie;
        source.mediaSourceId = QStringLiteral("c1b2a3d4e5f6071829304a5b6c7d8e9f");
        source.playSessionId = QStringLiteral("s1");
        source.method = method;
        source.url = QUrl(TestWav::writeWavClip(chain.dir, 30));
        if (source.url.isEmpty())
            return false;
        Q_EMIT chain.client.playbackInfoReady(source);
        return QTest::qWaitFor([&chain] { return chain.controller.canSeek(); }, 10000);
    }
    // The `ff-index` mpv gives the first track of a type, -1 when there is none
    static int streamIndexOf(Chain &chain, const QString &type)
    {
        const QVariantList tracks =
            chain.engine()->mpvProperty(QStringLiteral("track-list")).toList();
        for (const QVariant &entry : tracks) {
            const QVariantMap map = entry.toMap();
            if (map.value(QStringLiteral("type")).toString() == type
                && map.value(QStringLiteral("ff-index")).isValid())
                return map.value(QStringLiteral("ff-index")).toInt();
        }
        return -1;
    }
    QTemporaryDir m_data;
};

QTEST_GUILESS_MAIN(TestPlayerController)
#include "test_player_controller.moc"
