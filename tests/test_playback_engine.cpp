// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Offline engine tests: the mpv mapping runs on a real mpv (offscreen, vo=null/ao=null) over a generated WAV clip (16-bit mono PCM, a sine). No network, no window, no media files in the repo; the only deliberately broken case is a path that does not exist. The live reproduction of INT-02..06 stays in test_jellyfin_live.cpp, on the real Jellyfin-streamed media URLs
#include "FakeJellyfin.h"
#include "WavClip.h"

#include "api/JellyfinClient.h"
#include "models/PlayQueue.h"
#include "player/PlaybackEngine.h"
#include "player/PlaybackSession.h"
#include "player/PlayerController.h"
#include "player/PlayerSettings.h"
#include "settings/ClientSettings.h"

#include <MpvController>

#include <QFile>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWindow>
#include <QtTest/QtTest>

using Kinefin::ClientSettings;
using Kinefin::JellyfinClient;
using Kinefin::PlaybackEngine;
using Kinefin::PlaybackSession;
using Kinefin::PlaybackSource;
using Kinefin::PlayerSettings;

namespace {
PlaybackSource directSource(const QString &url)
{
    PlaybackSource source;
    source.itemId = QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
    source.mediaSourceId = QStringLiteral("c1b2a3d4e5f6071829304a5b6c7d8e9f");
    source.playSessionId = QStringLiteral("s1");
    source.method = PlaybackSource::Method::Direct;
    source.url = QUrl(url);
    return source;
}

// The whole chain a real play goes through: an authenticated fake-server client, one session and one engine. The `ok` figure follows the reporter test's Chain pattern: prepared in a helper constructor, asserted by the test itself
struct Chain {
    // Offscreen basics always apply; `extraOptions` is for case-specific tweaks
    Chain(const QTemporaryDir &dir, int seconds, const QStringList &extraOptions = {})
        : server(), client(), session(), source(directSource(TestWav::writeWavClip(dir, seconds))),
          engine(client, session)
    {
        ok = !source.url.isEmpty() && server.listen() && login(client, server)
             && engine.init(QStringList{QStringLiteral("vo=null"), QStringLiteral("ao=null")}
                            + extraOptions);
    }
    Chain(const Chain &) = delete;
    Chain &operator=(const Chain &) = delete;

    FakeJellyfin server;
    JellyfinClient client;
    PlaybackSession session;
    PlaybackSource source;
    PlaybackEngine engine;
    bool ok = false;
};

// mpv answers a flag as a boolean and a yes/no choice as a string
bool isYes(const QVariant &value)
{
    return value.typeId() == QMetaType::Bool ? value.toBool()
                                             : value.toString() == QLatin1String("yes");
}

// `video.aspect` is hidden from the UI, so only the file can hold a value other than the default
QString settingsFile(const QTemporaryDir &dir, const QString &name, const QByteArray &body)
{
    QFile file(dir.filePath(name));
    return file.open(QIODevice::WriteOnly) && file.write(body) == body.size() ? file.fileName()
                                                                              : QString();
}

QVariantList headerEntries(PlaybackEngine &engine)
{
    return engine.controller()->getProperty(QStringLiteral("http-header-fields")).toList();
}

// A tiny moving picture mpv reads natively (YUV4MPEG2, 4:2:0, 64x36 at 10 fps), for the video track cases the WAV clip cannot give
QString writeY4mClip(const QTemporaryDir &dir, int seconds)
{
    constexpr int width = 64;
    constexpr int height = 36;
    QFile file(dir.filePath(QStringLiteral("clip.y4m")));
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(QByteArrayLiteral("YUV4MPEG2 W64 H36 F10:1 Ip A1:1 C420jpeg\n"));
    for (int frame = 0; frame < seconds * 10; ++frame) {
        file.write(QByteArrayLiteral("FRAME\n"));
        QByteArray luma(width * height, char(0));
        for (int i = 0; i < luma.size(); ++i)
            luma[i] = char(40 + (i % width + frame * 4) % 200);
        file.write(luma);
        file.write(QByteArray(width * height / 2, char(128)));
    }
    return file.error() == QFileDevice::NoError ? QUrl::fromLocalFile(file.fileName()).toString()
                                                : QString();
}

bool videoOff(const QVariant &vid)
{
    return (vid.typeId() == QMetaType::Bool && !vid.toBool())
           || vid.toString() == QLatin1String("no");
}
} // namespace

class TestPlaybackEngine : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void initRefusesASecondCall()
    {
        JellyfinClient client;
        PlaybackSession session;
        PlaybackEngine engine(client, session);
        QVERIFY(engine.init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        QVERIFY(!engine.init({}));
    }

    void uninitialisedOrBusyRefusals()
    {
        // Without init, every player call is a refusal that touches nothing
        JellyfinClient client;
        PlaybackSession session;
        PlaybackEngine engine(client, session);
        QCOMPARE(session.state(), PlaybackSession::State::Idle);
        engine.pause();
        engine.resume();
        engine.stop();
        QCOMPARE(session.state(), PlaybackSession::State::Idle);
        // A missing file still passes loadfile's synchronous acceptance: the failure surfaces through the session machine (the open call itself had nothing to refuse yet — until mpv's own answer; refusing URLs is MpvQt-agnostic). Here without init, the whole open is refused before any mpv call
        QVERIFY(!engine.open(directSource(QStringLiteral("file:///no/such/clip.wav"))));

        // A live-but-busy session refuses a second load, already in Opening
        Chain chain(dir, 30);
        QVERIFY(chain.ok);
        QVERIFY(chain.engine.open(chain.source));
        QCOMPARE(chain.session.state(), PlaybackSession::State::Opening);
        QVERIFY(!chain.engine.open(chain.source));
        chain.engine.stop();
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Idle);
    }

    void clipPlaysPausesAndStops()
    {
        Chain chain(dir, 30);
        QVERIFY(chain.ok);
        QVERIFY(chain.engine.open(chain.source));
        QCOMPARE(chain.session.state(), PlaybackSession::State::Opening);
        QVERIFY(!chain.engine.knownPositionTicks()); // nothing from mpv yet: not 0
        // The stream starts: opened() runs on fileLoaded, and the session keeps the identity while it is Playing
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        QCOMPARE(QString::fromStdString(chain.session.identity().playSessionId),
                 chain.source.playSessionId);
        QTRY_VERIFY(chain.engine.positionTicks() > 0);
        QVERIFY(chain.engine.knownPositionTicks());
        QTRY_COMPARE(chain.engine.durationTicks(), qint64(30 * 10000000));
        QTRY_VERIFY(chain.engine.canSeek());

        chain.engine.pause();
        QCOMPARE(chain.session.state(), PlaybackSession::State::Paused);
        const qint64 atPause = chain.engine.positionTicks();
        QTest::qWait(300);
        // One in-flight mpv tick may still land; nothing past that
        QVERIFY(chain.engine.positionTicks() <= atPause + 2 * 10000000);

        chain.engine.resume();
        QCOMPARE(chain.session.state(), PlaybackSession::State::Playing);
        QTRY_VERIFY(chain.engine.positionTicks() > atPause);

        QSignalSpy ended(&chain.engine, &PlaybackEngine::streamEnded);
        chain.engine.stop();
        QCOMPARE(chain.session.state(), PlaybackSession::State::Stopping);
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Idle);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.at(0).at(0).toString(), QStringLiteral("stop"));
        // The identity is cleared once the session lands in Idle
        QVERIFY(chain.session.identity().itemId.empty());
    }

    void clipEndsNaturally()
    {
        // Realtime two-second clip: eof arrives inside the QTRY window and the reporting cadence is what interests the live gate. (mpv's untimed option only exists before init; the engine sets options after mpvqt's init, so it would be refused: keep realtime)
        Chain chain(dir, 2);
        QVERIFY(chain.ok);
        QSignalSpy ended(&chain.engine, &PlaybackEngine::streamEnded);
        QVERIFY(chain.engine.open(chain.source));
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        // The clip is two seconds long: eof arrives, the session takes the ordinary one-stop path and lands in Idle without an error
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(ended.at(0).at(0).toString(), QStringLiteral("eof"));
        QCOMPARE(chain.session.state(), PlaybackSession::State::Idle);
        // The item's end reports the duration, not a time-pos a few ms short of it
        QCOMPARE(chain.engine.positionTicks(), chain.engine.durationTicks());
    }

    void stopDuringOpeningNeverLeaksIntoTheNextOpen()
    {
        Chain chain(dir, 30);
        QVERIFY(chain.ok);
        for (int round = 0; round < 10; ++round) {
            QVERIFY(chain.engine.open(chain.source));
            chain.engine.stop();
            QCOMPARE(chain.session.state(), PlaybackSession::State::Stopping);
            // The load is still ending: a next item is refused instead of meeting a stale mpv event
            QVERIFY(!chain.engine.open(chain.source));
            QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Idle);
        }
        QVERIFY(chain.engine.open(chain.source));
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        QTest::qWait(300);
        QCOMPARE(chain.session.state(), PlaybackSession::State::Playing);
    }

    void stopWithoutEndFileSettlesTheSession()
    {
        Chain chain(dir, 30);
        QVERIFY(chain.ok);
        QVERIFY(chain.engine.open(chain.source));
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        // mpv's end-file never reaches the engine: the watchdog releases the session
        chain.engine.controller()->disconnect(&chain.engine);
        chain.engine.stop();
        QCOMPARE(chain.session.state(), PlaybackSession::State::Stopping);
        QTRY_COMPARE_WITH_TIMEOUT(chain.session.state(), PlaybackSession::State::Idle, 6000);
    }

    void prematureEofKeepsItsPosition()
    {
        Chain chain(dir, 30);
        QVERIFY(chain.ok);
        QVERIFY(chain.engine.open(chain.source));
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        QTRY_COMPARE(chain.engine.durationTicks(), qint64(30 * 10000000));
        QTRY_VERIFY(chain.engine.positionTicks() > 0);
        // A dropped connection ends the file with eof far from the end: the position stays where it was
        QMetaObject::invokeMethod(chain.engine.controller(), "endFile", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("eof")));
        QCOMPARE(chain.session.state(), PlaybackSession::State::Idle);
        QVERIFY(chain.engine.positionTicks() < 20 * 10000000);
    }

    void stopNotDrivenByTheEngineIsNotAFailure()
    {
        Chain chain(dir, 30);
        QVERIFY(chain.ok);
        bool failed = false;
        chain.session.addObserver([&failed](PlaybackSession::State, PlaybackSession::State to) {
            failed = failed || to == PlaybackSession::State::Failed;
        });
        QVERIFY(chain.engine.open(chain.source));
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        chain.engine.controller()->command({QStringLiteral("stop")});
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Idle);
        QVERIFY(!failed);
    }

    void failedOpenLandsBackInIdle()
    {
        Chain chain(dir, 30);
        QVERIFY(chain.ok);
        QSignalSpy ended(&chain.engine, &PlaybackEngine::streamEnded);
        // The load is accepted by mpv and the missing file is what fails: the session passes through Failed and settles to Idle with no phantom server report (an opening failure never runs the best-effort remote stop)
        QVERIFY(chain.engine.open(directSource(QStringLiteral("file:///no/such/clip.wav"))));
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Idle);
        QVERIFY(chain.session.identity().itemId.empty());
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(ended.at(0).at(0).toString(), QStringLiteral("error"));
    }

    void streamHeadersCarryTheTokenEntry()
    {
        Chain chain(dir, 6);
        QVERIFY(chain.ok);
        QVERIFY(chain.engine.open(chain.source));
        // The entry mpv keeps is exactly `Authorization: <streamAuthorization()>`: one list entry, token included, URL untouched. The node-array path must have survived the commas and quotes of the MediaBrowser header
        QCOMPARE(headerEntries(chain.engine).size(), 1);
        const QString expectedEntry = QString::fromUtf8(QByteArrayLiteral("Authorization: ")
                                                        + chain.client.streamAuthorization());
        QCOMPARE(headerEntries(chain.engine).at(0).toString(), expectedEntry);
        // mpv reports the plain local path once the load has been processed. The read must sit inside the QTRY itself: capturing it beforehand would freeze the pre-load (unavailable) result
        QTRY_COMPARE(chain.engine.controller()->getProperty(QStringLiteral("path")).toString(),
                     QUrl(chain.source.url).toLocalFile());
    }

    void logoutLeavesNoStaleHeader()
    {
        Chain chain(dir, 5);
        QVERIFY(chain.ok);
        QVERIFY(chain.engine.open(chain.source));
        QCOMPARE(headerEntries(chain.engine).size(), 1);
        chain.engine.stop();
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Idle);
        QSignalSpy loggedOut(&chain.client, &JellyfinClient::credentialsCleared);
        chain.client.disconnectFromServer();
        QTRY_COMPARE(loggedOut.size(), 1);

        // A stale entry is preseeded; the next open on a client without credentials must drop it rather than let it ride along
        chain.engine.controller()->setProperty(
            QStringLiteral("http-header-fields"),
            QStringList{QStringLiteral("Authorization: MediaBrowser Token=\"STALE\"")});
        QCOMPARE(headerEntries(chain.engine).size(), 1);
        QVERIFY(chain.engine.open(chain.source));
        QCOMPARE(headerEntries(chain.engine).size(), 0);
        // The header assertion above is the point; the reload may already have reached Playing by now, so anything but a reset session is fine
        QVERIFY(chain.session.state() != PlaybackSession::State::Idle);
    }

    void rateAndSeekOnAClip()
    {
        Chain chain(dir, 8);
        QVERIFY(chain.ok);
        QVERIFY(chain.engine.open(chain.source));
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        QCOMPARE(chain.engine.playbackRate(), 1.0);
        chain.engine.setRate(2.0);
        QTRY_VERIFY(chain.engine.playbackRate() > 1.9);
        // An absolute seek into the clip: the position takes the new time
        chain.engine.seekTo(60'000'000); // six seconds into an eight-second clip
        QTRY_VERIFY(chain.engine.positionTicks() > 50'000'000); // five seconds
        QTest::qWait(200);
        QVERIFY(chain.engine.positionTicks() > 50'000'000);
    }

    void theDisplayRateIsTheVideoWindowsScreens()
    {
        JellyfinClient client;
        PlaybackSession session;
        PlaybackEngine engine(client, session);
        QVERIFY(engine.init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        ClientSettings settings(dir.filePath(QStringLiteral("display-rate.conf")));
        PlayerSettings mpv(settings, engine);
        QWindow window;
        QVERIFY(window.screen());
        mpv.setWindow(&window);
        const qreal rate = window.screen()->refreshRate();
        QCOMPARE(mpv.displayRate(), rate);
        mpv.apply();
        if (rate > 20.0 && rate < 500.0)
            QCOMPARE(engine.mpvProperty(QStringLiteral("display-fps-override")).toDouble(), rate);
        // A rate change sets only what the rate decides: the speed the user chose while playing stays
        engine.setMpvProperty(QStringLiteral("speed"), 1.5);
        engine.setMpvProperty(QStringLiteral("display-fps-override"), 1.0);
        Q_EMIT window.screen()->refreshRateChanged(rate);
        QCOMPARE(engine.mpvProperty(QStringLiteral("speed")).toDouble(), 1.5);
        QCOMPARE(engine.mpvProperty(QStringLiteral("display-fps-override")).toDouble(),
                 rate > 20.0 && rate < 500.0 ? rate : 0.0);
        // A window that goes away leaves the primary screen
        mpv.setWindow(nullptr);
        QCOMPARE(mpv.displayRate(), QGuiApplication::primaryScreen()->refreshRate());
    }

    void theCertificateBundleIsTheFirstReadableCandidate()
    {
        QTemporaryDir bundles;
        QVERIFY(bundles.isValid());
        const QString second = bundles.filePath(QStringLiteral("second.crt"));
        const QString third = bundles.filePath(QStringLiteral("third.crt"));
        for (const QString &path : {second, third}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        const QString missing = bundles.filePath(QStringLiteral("missing.crt"));
        // A directory is not a bundle
        QCOMPARE(PlayerSettings::detectCertBundle({bundles.path(), missing, second, third}),
                 second);
        QCOMPARE(PlayerSettings::detectCertBundle({missing, third}), third);
        QVERIFY(PlayerSettings::detectCertBundle({missing}).isEmpty());
        QVERIFY(PlayerSettings::detectCertBundle({}).isEmpty());
        QVERIFY(PlayerSettings::certBundleCandidates().contains(
            QStringLiteral("/etc/ssl/certs/ca-certificates.crt")));
    }

    void tlsFollowsTheCertificateSettingsAndMpvConfWinsLast()
    {
        JellyfinClient client;
        PlaybackSession session;
        PlaybackEngine engine(client, session);
        QVERIFY(engine.init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        ClientSettings settings(dir.filePath(QStringLiteral("tls.conf")));
        PlayerSettings mpv(settings, engine);
        const QString detected =
            PlayerSettings::detectCertBundle(PlayerSettings::certBundleCandidates());
        const auto verify = [&engine] {
            return engine.mpvProperty(QStringLiteral("tls-verify")).toBool();
        };
        const auto bundle = [&engine] {
            return engine.mpvProperty(QStringLiteral("tls-ca-file")).toString();
        };
        // At start: verified, with the detected bundle (the host may have none)
        mpv.apply();
        QVERIFY(verify());
        QCOMPARE(bundle(), detected);
        // The effective value follows at once, from the setting and from the command line alike
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("ignoreSSLErrors"), true));
        QVERIFY(!verify());
        QVERIFY(
            settings.setValue(QStringLiteral("main"), QStringLiteral("ignoreSSLErrors"), false));
        QVERIFY(verify());
        settings.setCertificateErrorsIgnoredByCommandLine(true);
        QVERIFY(!verify());
        // Turned off during a session the bundle stays until the next start: an empty string would be a path to mpv, not "unset"
        if (!detected.isEmpty()) {
            QVERIFY(settings.setValue(QStringLiteral("main"),
                                      QStringLiteral("autodetectCertBundle"), false));
            QCOMPARE(bundle(), detected);
            QVERIFY(settings.setValue(QStringLiteral("main"),
                                      QStringLiteral("autodetectCertBundle"), true));
            QCOMPARE(bundle(), detected);
        }
        // The user's mpv.conf is applied after both
        ClientSettings custom(
            settingsFile(dir, QStringLiteral("tls-custom.conf"),
                         "[other]\nother_conf=tls-verify=no\\ntls-ca-file=/x.pem\n"));
        PlayerSettings customMpv(custom, engine);
        customMpv.apply();
        QVERIFY(!verify());
        QCOMPARE(bundle(), QStringLiteral("/x.pem"));
    }

    void aspectChoiceAppliesAtOnceAndSurvivesEveryApply()
    {
        JellyfinClient client;
        PlaybackSession session;
        PlaybackEngine engine(client, session);
        QVERIFY(engine.init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        ClientSettings settings(
            settingsFile(dir, QStringLiteral("aspect.conf"), "[video]\naspect=zoom\n"));
        PlayerSettings mpv(settings, engine);
        QWindow window;
        mpv.setWindow(&window);
        mpv.apply();
        QCOMPARE(mpv.aspectMode(), QStringLiteral("zoom"));
        QCOMPARE(engine.mpvProperty(QStringLiteral("panscan")).toDouble(), 1.0);

        // A choice replaces the setting at once, with the speed the user set left alone and the setting unwritten
        engine.setMpvProperty(QStringLiteral("speed"), 1.5);
        mpv.setAspectOverride(QStringLiteral("stretch"));
        QCOMPARE(mpv.aspectMode(), QStringLiteral("stretch"));
        QCOMPARE(engine.mpvProperty(QStringLiteral("panscan")).toDouble(), 0.0);
        QVERIFY(!isYes(engine.mpvProperty(QStringLiteral("keepaspect"))));
        QCOMPARE(engine.mpvProperty(QStringLiteral("speed")).toDouble(), 1.5);
        QCOMPARE(settings.value(QStringLiteral("video"), QStringLiteral("aspect")).toString(),
                 QStringLiteral("zoom"));

        // A full apply and a screen rate change keep it
        mpv.apply();
        QCOMPARE(mpv.aspectMode(), QStringLiteral("stretch"));
        QVERIFY(!isYes(engine.mpvProperty(QStringLiteral("keepaspect"))));
        QCOMPARE(engine.mpvProperty(QStringLiteral("panscan")).toDouble(), 0.0);
        engine.setMpvProperty(QStringLiteral("speed"), 1.5);
        Q_EMIT window.screen()->refreshRateChanged(window.screen()->refreshRate());
        QVERIFY(!isYes(engine.mpvProperty(QStringLiteral("keepaspect"))));
        QCOMPARE(engine.mpvProperty(QStringLiteral("speed")).toDouble(), 1.5);

        // Another choice clears the modifiers of the previous one
        mpv.setAspectOverride(QStringLiteral("noscaling"));
        QVERIFY(isYes(engine.mpvProperty(QStringLiteral("keepaspect"))));
        QVERIFY(isYes(engine.mpvProperty(QStringLiteral("video-unscaled"))));
        mpv.setAspectOverride({});
        QCOMPARE(mpv.aspectMode(), QStringLiteral("zoom"));

        // The setting is back, with the speed untouched
        QCOMPARE(engine.mpvProperty(QStringLiteral("panscan")).toDouble(), 1.0);
        QVERIFY(!isYes(engine.mpvProperty(QStringLiteral("video-unscaled"))));
        QCOMPARE(engine.mpvProperty(QStringLiteral("speed")).toDouble(), 1.5);
    }

    void aspectOfACustomSettingReadsAsCustomAndLeavesMpvAlone()
    {
        JellyfinClient client;
        PlaybackSession session;
        PlaybackEngine engine(client, session);
        QVERIFY(engine.init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        ClientSettings settings(
            settingsFile(dir, QStringLiteral("aspect-custom.conf"), "[video]\naspect=custom\n"));
        PlayerSettings mpv(settings, engine);
        QCOMPARE(mpv.aspectMode(), QStringLiteral("custom"));
        // Custom does not touch mpv's own aspect properties, not even once a file is loaded
        engine.setMpvProperty(QStringLiteral("panscan"), 0.5);
        mpv.apply();
        mpv.applyLoadedAspect();
        QCOMPARE(engine.mpvProperty(QStringLiteral("panscan")).toDouble(), 0.5);
        // A choice made for one playback is undone when it ends
        mpv.setAspectOverride(QStringLiteral("zoom"));
        QCOMPARE(engine.mpvProperty(QStringLiteral("panscan")).toDouble(), 1.0);
        mpv.setAspectOverride({});
        QCOMPARE(mpv.aspectMode(), QStringLiteral("custom"));
        QCOMPARE(engine.mpvProperty(QStringLiteral("panscan")).toDouble(), 0.0);
        // Auto is a choice of its own against a custom setting
        engine.setMpvProperty(QStringLiteral("panscan"), 0.5);
        mpv.setAspectOverride(QStringLiteral("normal"));
        QCOMPARE(mpv.aspectMode(), QStringLiteral("normal"));
        QCOMPARE(engine.mpvProperty(QStringLiteral("panscan")).toDouble(), 0.0);
        // A value that is no mode at all is no choice
        mpv.setAspectOverride({});
        mpv.setAspectOverride(QStringLiteral("bogus"));
        QCOMPARE(mpv.aspectMode(), QStringLiteral("custom"));
    }

    void theLoadedFileReevaluatesOnlyTheModeThatNeedsThePicture()
    {
        JellyfinClient client;
        PlaybackSession session;
        PlaybackEngine engine(client, session);
        QVERIFY(engine.init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        ClientSettings zoom(
            settingsFile(dir, QStringLiteral("aspect-zoom.conf"), "[video]\naspect=zoom\n"));
        PlayerSettings zoomed(zoom, engine);
        zoomed.apply();
        engine.setMpvProperty(QStringLiteral("panscan"), 0.5);
        zoomed.applyLoadedAspect();
        QCOMPARE(engine.mpvProperty(QStringLiteral("panscan")).toDouble(), 0.5);

        ClientSettings wide(settingsFile(dir, QStringLiteral("aspect-wide.conf"),
                                         "[video]\naspect=force_16_9_if_4_3\n"));
        PlayerSettings widescreen(wide, engine);
        widescreen.apply();
        // This mode is evaluated again: the modifiers go back to their defaults, and there is no picture here to widen
        engine.setMpvProperty(QStringLiteral("panscan"), 0.5);
        widescreen.applyLoadedAspect();
        QCOMPARE(engine.mpvProperty(QStringLiteral("panscan")).toDouble(), 0.0);
    }

    void controllerReportsThePlayingSourceAndForgetsItAtTheEnd()
    {
        FakeJellyfin server;
        JellyfinClient client;
        QVERIFY(server.listen() && login(client, server));
        Kinefin::PlayQueue queue(&client);
        ClientSettings settings(dir.filePath(QStringLiteral("controller.conf")));
        Kinefin::PlayerController controller(client, queue, settings, QString());
        auto *engine = controller.findChild<PlaybackEngine *>();
        QVERIFY(engine);
        QVERIFY(controller.playItem(QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9"), 0));
        // No window renders here: the engine stands alone, after the item asked for its source
        QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        QVERIFY(controller.playbackInfo().isEmpty());

        PlaybackSource source = directSource(TestWav::writeWavClip(dir, 8));
        source.method = PlaybackSource::Method::Transcode;
        source.media.streamType = QStringLiteral("HLS");
        source.media.protocol = QStringLiteral("https");
        source.media.targetVideoCodec = QStringLiteral("hevc");
        source.media.targetAudioCodec = QStringLiteral("aac");
        source.media.transcodeReasons = {QStringLiteral("AudioCodecNotSupported")};
        source.media.source.container = QStringLiteral("mkv");
        source.media.source.size = 4'000'000'000LL;
        source.media.source.bitrate = 19'000'000;
        Kinefin::StreamSummary video;
        video.type = QStringLiteral("Video");
        video.codec = QStringLiteral("hevc");
        video.profile = QStringLiteral("Main 10");
        video.level = 153;
        video.bitrate = 18'000'000;
        video.frameRate = 23.976;
        video.bitDepth = 10;
        video.pixelFormat = QStringLiteral("yuv420p10le");
        video.videoRangeType = QStringLiteral("HDR10");
        video.videoRange = QStringLiteral("HDR");
        video.colorSpace = QStringLiteral("bt2020nc");
        video.colorPrimaries = QStringLiteral("bt2020");
        video.colorTransfer = QStringLiteral("smpte2084");
        Kinefin::StreamSummary audio;
        audio.index = 1;
        audio.type = QStringLiteral("Audio");
        audio.codec = QStringLiteral("dts");
        audio.channels = 6;
        audio.bitrate = 1'500'000;
        audio.sampleRate = 48000;
        audio.isDefault = true;
        source.media.source.streams = {video, audio};
        source.itemId = QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
        Q_EMIT client.playbackInfoReady(source);

        const QVariantMap info = controller.playbackInfo();
        QCOMPARE(info.value(QStringLiteral("playMethod")).toString(), QStringLiteral("Transcode"));
        QCOMPARE(info.value(QStringLiteral("streamType")).toString(), QStringLiteral("HLS"));
        QCOMPARE(info.value(QStringLiteral("protocol")).toString(), QStringLiteral("https"));
        QCOMPARE(info.value(QStringLiteral("targetVideoCodec")).toString(), QStringLiteral("hevc"));
        QCOMPARE(info.value(QStringLiteral("targetAudioCodec")).toString(), QStringLiteral("aac"));
        QCOMPARE(info.value(QStringLiteral("transcodeReasons")).toStringList(),
                 (QStringList{QStringLiteral("AudioCodecNotSupported")}));
        QCOMPARE(info.value(QStringLiteral("container")).toString(), QStringLiteral("mkv"));
        QCOMPARE(info.value(QStringLiteral("sizeBytes")).toLongLong(), 4'000'000'000LL);
        QCOMPARE(info.value(QStringLiteral("bitrate")).toLongLong(), 19'000'000LL);
        QCOMPARE(info.value(QStringLiteral("videoCodec")).toString(), QStringLiteral("hevc"));
        QCOMPARE(info.value(QStringLiteral("videoProfile")).toString(), QStringLiteral("Main 10"));
        QCOMPARE(info.value(QStringLiteral("videoLevel")).toDouble(), 153.0);
        QCOMPARE(info.value(QStringLiteral("videoBitrate")).toLongLong(), 18'000'000LL);
        QCOMPARE(info.value(QStringLiteral("videoFps")).toDouble(), 23.976);
        QCOMPARE(info.value(QStringLiteral("videoBitDepth")).toInt(), 10);
        QCOMPARE(info.value(QStringLiteral("videoPixelFormat")).toString(),
                 QStringLiteral("yuv420p10le"));
        QCOMPARE(info.value(QStringLiteral("videoRange")).toString(), QStringLiteral("HDR10"));
        QCOMPARE(info.value(QStringLiteral("colorSpace")).toString(), QStringLiteral("bt2020nc"));
        QCOMPARE(info.value(QStringLiteral("colorPrimaries")).toString(), QStringLiteral("bt2020"));
        QCOMPARE(info.value(QStringLiteral("colorTransfer")).toString(),
                 QStringLiteral("smpte2084"));
        QCOMPARE(info.value(QStringLiteral("audioCodec")).toString(), QStringLiteral("dts"));
        QCOMPARE(info.value(QStringLiteral("audioChannels")).toInt(), 6);
        QCOMPARE(info.value(QStringLiteral("audioBitrate")).toLongLong(), 1'500'000LL);
        QCOMPARE(info.value(QStringLiteral("audioSampleRate")).toInt(), 48000);
        // An audio-only clip has no picture size, no frame counters and no hardware decoder: unknown is absent, never 0
        for (const char *key :
             {"videoWidth", "videoHeight", "droppedFrames", "decoderDroppedFrames", "hwdec"})
            QVERIFY2(!info.contains(QString::fromLatin1(key)), key);
        // Neither the URL nor the file name is in the snapshot
        for (auto it = info.cbegin(); it != info.cend(); ++it) {
            const QString text = it.value().toString() + it.value().toStringList().join(u' ');
            QVERIFY2(!text.contains(QStringLiteral("clip8")), qPrintable(it.key()));
            QVERIFY2(!text.contains(QStringLiteral("file:")), qPrintable(it.key()));
        }

        // The choice of an aspect is this playback's alone
        QCOMPARE(controller.aspectMode(), QStringLiteral("normal"));
        controller.setAspectMode(QStringLiteral("zoom"));
        QCOMPARE(controller.aspectMode(), QStringLiteral("zoom"));
        QCOMPARE(engine->mpvProperty(QStringLiteral("panscan")).toDouble(), 1.0);
        controller.setAspectMode(QStringLiteral("custom"));
        QCOMPARE(controller.aspectMode(), QStringLiteral("zoom"));
        QVERIFY(settings.value(QStringLiteral("video"), QStringLiteral("aspect")).toString()
                != QLatin1String("zoom"));

        controller.stop();
        QTRY_VERIFY(controller.playbackInfo().isEmpty());
        QCOMPARE(controller.aspectMode(), QStringLiteral("normal"));
        QCOMPARE(engine->mpvProperty(QStringLiteral("panscan")).toDouble(), 0.0);
    }

    void aRestartOfTheSameItemKeepsTheAspectChoiceAndTheRate()
    {
        FakeJellyfin server;
        JellyfinClient client;
        QVERIFY(server.listen() && login(client, server));
        Kinefin::PlayQueue queue(&client);
        ClientSettings settings(dir.filePath(QStringLiteral("restart.conf")));
        Kinefin::PlayerController controller(client, queue, settings, QString());
        auto *engine = controller.findChild<PlaybackEngine *>();
        QVERIFY(engine);
        const QString item = QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
        QVERIFY(controller.playItem(item, 0));
        QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
        controller.setAspectMode(QStringLiteral("zoom"));
        engine->setRate(1.5);
        QTRY_COMPARE(engine->playbackRate(), 1.5);
        // The same item again keeps both, although the settings are applied again before it opens
        QVERIFY(controller.playItem(item, 0));
        QCOMPARE(controller.aspectMode(), QStringLiteral("zoom"));
        QCOMPARE(engine->mpvProperty(QStringLiteral("panscan")).toDouble(), 1.0);
        QCOMPARE(engine->mpvProperty(QStringLiteral("speed")).toDouble(), 1.5);
        // Another item starts from the settings
        QVERIFY(controller.playItem(QStringLiteral("d5ed5d4e2e5447b58e9f2b6f7bd5c96a"), 0));
        QCOMPARE(controller.aspectMode(), QStringLiteral("normal"));
        QCOMPARE(engine->mpvProperty(QStringLiteral("panscan")).toDouble(), 0.0);
        QCOMPARE(engine->mpvProperty(QStringLiteral("speed")).toDouble(), 1.0);
    }

    void directPlayShowsTheAudioTrackMpvPlaysAndTheFactsItReports()
    {
        FakeJellyfin server;
        JellyfinClient client;
        QVERIFY(server.listen() && login(client, server));
        Kinefin::PlayQueue queue(&client);
        ClientSettings settings(dir.filePath(QStringLiteral("direct.conf")));
        Kinefin::PlayerController controller(client, queue, settings, QString());
        auto *engine = controller.findChild<PlaybackEngine *>();
        QVERIFY(engine);
        QVERIFY(controller.playItem(QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9"), 0));
        QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));

        PlaybackSource source = directSource(TestWav::writeWavClip(dir, 8));
        Kinefin::StreamSummary defaultTrack;
        defaultTrack.index = 1;
        defaultTrack.type = QStringLiteral("Audio");
        defaultTrack.codec = QStringLiteral("dts");
        defaultTrack.isDefault = true;
        // The clip's only track is the stream with index 0
        Kinefin::StreamSummary playing;
        playing.index = 0;
        playing.type = QStringLiteral("Audio");
        playing.codec = QStringLiteral("pcm_s16le");
        source.media.source.streams = {defaultTrack, playing};
        Q_EMIT client.playbackInfoReady(source);

        QCOMPARE(controller.playbackInfo().value(QStringLiteral("playMethod")).toString(),
                 QStringLiteral("DirectPlay"));
        // The track mpv plays replaces the default one as soon as mpv names it
        QTRY_COMPARE(controller.playbackInfo().value(QStringLiteral("audioCodec")).toString(),
                     QStringLiteral("pcm_s16le"));

        // The facts mpv reports come from the engine's cache, and an unavailable property takes its fact away; emitted here since the clip has no picture
        auto *mpv = engine->controller();
        QVERIFY(mpv);
        Q_EMIT mpv->propertyChanged(QStringLiteral("dwidth"), qlonglong(1920));
        Q_EMIT mpv->propertyChanged(QStringLiteral("dheight"), qlonglong(1080));
        Q_EMIT mpv->propertyChanged(QStringLiteral("frame-drop-count"), qlonglong(0));
        Q_EMIT mpv->propertyChanged(QStringLiteral("decoder-frame-drop-count"), qlonglong(3));
        Q_EMIT mpv->propertyChanged(QStringLiteral("hwdec-current"), QStringLiteral("vaapi"));
        QVariantMap info = controller.playbackInfo();
        QCOMPARE(info.value(QStringLiteral("videoWidth")).toInt(), 1920);
        QCOMPARE(info.value(QStringLiteral("videoHeight")).toInt(), 1080);
        QVERIFY(info.contains(QStringLiteral("droppedFrames")));
        QCOMPARE(info.value(QStringLiteral("droppedFrames")).toInt(), 0);
        QCOMPARE(info.value(QStringLiteral("decoderDroppedFrames")).toInt(), 3);
        QCOMPARE(info.value(QStringLiteral("hwdec")).toString(), QStringLiteral("vaapi"));
        Q_EMIT mpv->propertyChanged(QStringLiteral("hwdec-current"), QVariant());
        Q_EMIT mpv->propertyChanged(QStringLiteral("dwidth"), QVariant());
        info = controller.playbackInfo();
        QVERIFY(!info.contains(QStringLiteral("hwdec")));
        QVERIFY(!info.contains(QStringLiteral("videoWidth")));
        QCOMPARE(info.value(QStringLiteral("videoHeight")).toInt(), 1080);

        controller.stop();
        QTRY_VERIFY(controller.playbackInfo().isEmpty());
    }

    // mpv does not tell paused-for-cache again after a pause: a resume while the cache still holds the playback is a stall again
    void resumeDuringACacheStallBuffersAgain()
    {
        QTemporaryDir dir;
        Chain chain(dir, 30);
        QVERIFY(chain.ok);
        QVERIFY(chain.engine.open(chain.source));
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        Q_EMIT chain.engine.controller()->propertyChanged(QStringLiteral("paused-for-cache"), true);
        QCOMPARE(chain.session.state(), PlaybackSession::State::Buffering);
        chain.engine.pause();
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Paused);
        chain.engine.resume();
        QCOMPARE(chain.session.state(), PlaybackSession::State::Buffering);
        Q_EMIT chain.engine.controller()->propertyChanged(QStringLiteral("paused-for-cache"),
                                                          false);
        QCOMPARE(chain.session.state(), PlaybackSession::State::Playing);
        // Without a stall the resume plays
        chain.engine.pause();
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Paused);
        chain.engine.resume();
        QCOMPARE(chain.session.state(), PlaybackSession::State::Playing);
    }

    // The video item moved to another window and mpv turned the video off with the old render context: the track comes back, the playback goes on
    void restoreVideoSelectsTheTrackTheFileWasShowing()
    {
        QTemporaryDir dir;
        Chain chain(dir, 1);
        QVERIFY(chain.ok);
        chain.source.url = QUrl(writeY4mClip(dir, 4));
        QVERIFY(chain.source.url.isValid());
        QVERIFY(chain.engine.open(chain.source));
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        QTRY_COMPARE(chain.engine.mpvProperty(QStringLiteral("vid")).toLongLong(), qint64(1));
        chain.engine.setMpvProperty(QStringLiteral("vid"), QStringLiteral("no"));
        QTRY_VERIFY(videoOff(chain.engine.mpvProperty(QStringLiteral("vid"))));
        chain.engine.restoreVideo();
        QTRY_COMPARE(chain.engine.mpvProperty(QStringLiteral("vid")).toLongLong(), qint64(1));
        QCOMPARE(chain.session.state(), PlaybackSession::State::Playing);
    }

    // A file whose video was never on (vid=no from a raw mpv.conf) keeps it off: the renderer did not turn it off
    void restoreVideoLeavesAVideoTheUserTurnedOffAlone()
    {
        QTemporaryDir dir;
        // An audio track keeps the file playing with its video off
        const QString audio = QUrl(TestWav::writeWavClip(dir, 5)).toLocalFile();
        Chain chain(dir, 1, {QStringLiteral("vid=no"), QStringLiteral("audio-files=") + audio});
        QVERIFY(chain.ok);
        chain.source.url = QUrl(writeY4mClip(dir, 4));
        QVERIFY(chain.engine.open(chain.source));
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        chain.engine.restoreVideo();
        QTest::qWait(300);
        QVERIFY(videoOff(chain.engine.mpvProperty(QStringLiteral("vid"))));
    }

    // The renderer changed while the file opened, before any track was selected: the choice is made once the file is loaded
    void restoreVideoDuringTheOpeningWaitsForTheLoad()
    {
        QTemporaryDir dir;
        // An audio track keeps the file playing with its video off
        const QString audio = QUrl(TestWav::writeWavClip(dir, 5)).toLocalFile();
        Chain chain(dir, 1, {QStringLiteral("vid=no"), QStringLiteral("audio-files=") + audio});
        QVERIFY(chain.ok);
        chain.source.url = QUrl(writeY4mClip(dir, 4));
        QVERIFY(chain.engine.open(chain.source));
        QCOMPARE(chain.session.state(), PlaybackSession::State::Opening);
        chain.engine.restoreVideo();
        QTRY_COMPARE(chain.session.state(), PlaybackSession::State::Playing);
        QTRY_COMPARE(chain.engine.mpvProperty(QStringLiteral("vid")).toLongLong(), qint64(1));
    }

    void videoTrackToRestoreFollowsThePreviousThenTheDefaultThenTheFirst()
    {
        const auto track = [](const char *type, int id, bool albumArt = false,
                              bool isDefault = false) {
            return QVariant(QVariantMap{{QStringLiteral("type"), QString::fromLatin1(type)},
                                        {QStringLiteral("id"), id},
                                        {QStringLiteral("albumart"), albumArt},
                                        {QStringLiteral("default"), isDefault}});
        };
        using Kinefin::videoTrackToRestore;
        // No video track at all
        QVERIFY(!videoTrackToRestore({}, std::nullopt));
        QVERIFY(!videoTrackToRestore({track("audio", 1), track("sub", 2)}, 1));
        // One video track, with and without a previous choice (the previous id is of another type here)
        const QVariantList one{track("audio", 1), track("video", 2)};
        QCOMPARE(videoTrackToRestore(one, std::nullopt), std::optional<qint64>(2));
        QCOMPARE(videoTrackToRestore(one, 1), std::optional<qint64>(2));
        // An album-art cover is never the fallback
        const QVariantList cover{track("video", 1, true), track("video", 2), track("audio", 3)};
        QCOMPARE(videoTrackToRestore(cover, std::nullopt), std::optional<qint64>(2));
        QVERIFY(!videoTrackToRestore({track("video", 1, true), track("audio", 2)}, std::nullopt));
        // The default track beats an earlier one, and the previous track beats both when it is still there
        const QVariantList two{track("video", 1), track("video", 2, false, true),
                               track("video", 3)};
        QCOMPARE(videoTrackToRestore(two, std::nullopt), std::optional<qint64>(2));
        QCOMPARE(videoTrackToRestore(two, 3), std::optional<qint64>(3));
        QCOMPARE(videoTrackToRestore(two, 9), std::optional<qint64>(2));
    }

  private:
    QTemporaryDir dir;
};

QTEST_MAIN(TestPlaybackEngine)
#include "test_playback_engine.moc"
