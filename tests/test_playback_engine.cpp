// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Offline engine tests: the mpv mapping runs on a real mpv (offscreen, vo=null/ao=null) over a generated WAV clip (16-bit mono PCM, a sine). No network, no window, no media files in the repo; the only deliberately broken case is a path that does not exist. The live reproduction of INT-02..06 stays in test_jellyfin_live.cpp, on the real Jellyfin-streamed media URLs
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "player/PlaybackEngine.h"
#include "player/PlaybackSession.h"
#include "player/PlayerSettings.h"
#include "settings/ClientSettings.h"

#include <MpvController>

#include <QDataStream>
#include <QFile>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWindow>
#include <QtTest/QtTest>
#include <cmath>

using Kinefin::ClientSettings;
using Kinefin::JellyfinClient;
using Kinefin::PlaybackEngine;
using Kinefin::PlaybackSession;
using Kinefin::PlaybackSource;
using Kinefin::PlayerSettings;

namespace {
// The fixture is one fixed WAV: PCM data for a sine is what the demuxer reports as the duration, position and seekability; the container's mpv build has no lavfi stream protocol (verified offline 2026-11), so generated media stands in for the synthetic clip cases
constexpr int kWavSampleRate = 8000;

QString writeWavClip(const QTemporaryDir &dir, int seconds)
{
    QFile file(dir.filePath(QStringLiteral("clip%1.wav").arg(seconds)));
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    const int frames = seconds * kWavSampleRate;
    QByteArray pcm(int(frames * sizeof(qint16)), Qt::Uninitialized);
    auto *samples = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        samples[i] =
            qint16(std::sin(2 * M_PI * 440.0 * double(i) / double(kWavSampleRate)) * 8000.0);
    }
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    auto ascii = [&stream](const char *text) { stream.writeRawData(text, 4); };
    // RIFF size = whole rest of the file = data + 36
    const quint32 riffSize = quint32(frames * sizeof(qint16)) + 36;
    ascii("RIFF");
    stream << riffSize;
    ascii("WAVE");
    ascii("fmt ");
    stream << quint32(16)                              // fmt chunk size
           << quint16(1)                               // PCM
           << quint16(1)                               // mono
           << quint32(kWavSampleRate)                  // rate
           << quint32(kWavSampleRate * sizeof(qint16)) // byte rate
           << quint16(sizeof(qint16))                  // block align
           << quint16(16);                             // bits
    ascii("data");
    stream << quint32(frames * sizeof(qint16));
    if (file.write(header) == header.size() && file.write(pcm) == pcm.size()) {
        return QUrl::fromLocalFile(file.fileName()).toString(QUrl::FullyEncoded);
    }
    return {};
}

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
        : server(), client(), session(), source(directSource(writeWavClip(dir, seconds))),
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

QVariantList headerEntries(PlaybackEngine &engine)
{
    return engine.controller()->getProperty(QStringLiteral("http-header-fields")).toList();
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

  private:
    QTemporaryDir dir;
};

QTEST_MAIN(TestPlaybackEngine)
#include "test_playback_engine.moc"
