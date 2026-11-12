// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FakeJellyfin.h"
#include "player/PlaybackReporter.h"

#include <chrono>
#include <optional>

using Kinefin::JellyfinClient;
using Kinefin::PlaybackReporter;
using Kinefin::PlaybackSession;

namespace {
// What the report log says about one entry: "<last route segment>:<session>:<paused|playing|->"
QString describe(const FakeJellyfin::ReportLog &entry)
{
    const QJsonObject body = QJsonDocument::fromJson(entry.body).object();
    QString paused = QStringLiteral("-");
    if (body.contains(QStringLiteral("IsPaused")))
        paused = body.value(QStringLiteral("IsPaused")).toBool() ? QStringLiteral("paused")
                                                                 : QStringLiteral("playing");
    const QString route = QString::fromLatin1(entry.route.mid(entry.route.lastIndexOf('/') + 1));
    return route + QLatin1Char(':') + body.value(QStringLiteral("PlaySessionId")).toString()
           + QLatin1Char(':') + paused;
}

int countOf(const FakeJellyfin &server, const QString &routeSuffix)
{
    int count = 0;
    for (const auto &entry : server.reportLog) {
        if (entry.route.endsWith(routeSuffix.toLatin1()))
            ++count;
    }
    return count;
}

const PlaybackSession::Identity kIdentity{"c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9",
                                          "c1b2a3d4e5f6071829304a5b6c7d8e9f", "s1"};

// A logged-in client, one session, one reporter: the whole chain the player will use
struct Chain {
    explicit Chain(std::chrono::milliseconds interval = std::chrono::milliseconds(50))
        : reporter(
              session, client,
              [this] {
                  return PlaybackReporter::PlayerState{.positionTicks = position, .canSeek = true};
              },
              interval)
    {
        ok = server.listen() && login(client, server);
    }
    FakeJellyfin server;
    JellyfinClient client;
    PlaybackSession session;
    std::optional<qint64> position = 5000;
    PlaybackReporter reporter;
    bool ok = false;
};
} // namespace

class TestPlaybackReporter : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void startOnlyWhenTheStreamOpened()
    {
        Chain chain(std::chrono::seconds(30));
        QVERIFY(chain.ok);
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Transcoded));
        QTest::qWait(100);
        QCOMPARE(chain.server.reportLog.size(), 0); // opening is not playing
        QVERIFY(chain.session.opened());
        QTRY_COMPARE(chain.server.reportLog.size(), 1);
        const QJsonObject body =
            QJsonDocument::fromJson(chain.server.reportLog.at(0).body).object();
        QVERIFY(chain.server.reportLog.at(0).route.endsWith("/Sessions/Playing"));
        QCOMPARE(body.value(QStringLiteral("ItemId")).toString(),
                 QString::fromStdString(kIdentity.itemId));
        QCOMPARE(body.value(QStringLiteral("PlayMethod")).toString(), QStringLiteral("Transcode"));
        QCOMPARE(body.value(QStringLiteral("PositionTicks")).toVariant().toLongLong(), 5000);
    }

    void pauseAndResumeReportAtOnce()
    {
        Chain chain(std::chrono::seconds(30)); // the timer cannot be what sends these
        QVERIFY(chain.ok);
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QTRY_COMPARE(chain.server.reportLog.size(), 1);
        chain.position = 7000;
        QVERIFY(chain.session.pause());
        QTRY_COMPARE(chain.server.reportLog.size(), 2);
        QCOMPARE(describe(chain.server.reportLog.at(1)), QStringLiteral("Progress:s1:paused"));
        QCOMPARE(QJsonDocument::fromJson(chain.server.reportLog.at(1).body)
                     .object()
                     .value(QStringLiteral("PositionTicks"))
                     .toVariant()
                     .toLongLong(),
                 7000);
        QVERIFY(chain.session.resume());
        QTRY_COMPARE(chain.server.reportLog.size(), 3);
        QCOMPARE(describe(chain.server.reportLog.at(2)), QStringLiteral("Progress:s1:playing"));
        // Buffering is not a report
        QVERIFY(chain.session.buffering());
        QVERIFY(chain.session.bufferRecovered());
        QTest::qWait(100);
        QCOMPARE(chain.server.reportLog.size(), 3);
    }

    void heartbeatContinuesWhilePlayingAndPaused()
    {
        Chain chain(std::chrono::milliseconds(40));
        QVERIFY(chain.ok);
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QTRY_VERIFY(countOf(chain.server, QStringLiteral("/Progress")) >= 2);
        QVERIFY(chain.session.pause());
        const int atPause = countOf(chain.server, QStringLiteral("/Progress"));
        // Paused sessions keep reporting, or the server would end them after five minutes
        QTRY_VERIFY(countOf(chain.server, QStringLiteral("/Progress")) >= atPause + 2);
        QCOMPARE(describe(chain.server.reportLog.last()), QStringLiteral("Progress:s1:paused"));
    }

    void reportNowRestartsTheTimer()
    {
        Chain chain(std::chrono::milliseconds(600));
        QVERIFY(chain.ok);
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QTRY_COMPARE(chain.server.reportLog.size(), 1);
        QTest::qWait(400);
        chain.reporter.reportNow(); // a seek: at t=400
        QTRY_COMPARE(countOf(chain.server, QStringLiteral("/Progress")), 1);
        QTest::qWait(400); // t=800: the timer that started at t=0 would have fired at 600
        QCOMPARE(countOf(chain.server, QStringLiteral("/Progress")), 1);
        QTRY_COMPARE_WITH_TIMEOUT(countOf(chain.server, QStringLiteral("/Progress")), 2,
                                  1500); // t=1000
    }

    void stopSendsOneStoppedAndSilencesTheTimer()
    {
        Chain chain(std::chrono::milliseconds(40));
        QVERIFY(chain.ok);
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QTRY_VERIFY(countOf(chain.server, QStringLiteral("/Progress")) >= 1);
        chain.position = 9000;
        QVERIFY(chain.session.stop());
        QTRY_COMPARE(countOf(chain.server, QStringLiteral("/Stopped")), 1);
        QVERIFY(chain.session.stopped());
        const int total = chain.server.reportLog.size();
        QTest::qWait(200);
        QCOMPARE(chain.server.reportLog.size(), total);
        const QJsonObject body =
            QJsonDocument::fromJson(chain.server.reportLog.last().body).object();
        QCOMPARE(body.value(QStringLiteral("PositionTicks")).toVariant().toLongLong(), 9000);
        QCOMPARE(body.value(QStringLiteral("Failed")).toBool(), false);
    }

    void aFailureWhileStoppingSendsNoSecondStopped()
    {
        Chain chain(std::chrono::seconds(30));
        QVERIFY(chain.ok);
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QVERIFY(chain.session.stop());
        QVERIFY(!chain.session.failed());
        QVERIFY(chain.session.stop() == false);
        QVERIFY(chain.session.stopped());
        QTest::qWait(150);
        QCOMPARE(countOf(chain.server, QStringLiteral("/Stopped")), 1);
    }

    void anUnknownPositionNeverReportsZero()
    {
        Chain chain(std::chrono::seconds(30));
        QVERIFY(chain.ok);
        chain.position.reset();
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QTRY_COMPARE(countOf(chain.server, QStringLiteral("/Playing")), 1); // Start always goes out
        chain.reporter.reportNow();
        QVERIFY(chain.session.pause());
        QTest::qWait(150);
        QCOMPARE(countOf(chain.server, QStringLiteral("/Progress")), 0); // nothing known: skipped
        chain.position = 8000;
        chain.reporter.reportNow();
        QTRY_COMPARE(countOf(chain.server, QStringLiteral("/Progress")), 1);
        chain.position.reset(); // the player lost its position: the last known one is repeated
        QVERIFY(chain.session.resume());
        QTRY_COMPARE(countOf(chain.server, QStringLiteral("/Progress")), 2);
        QVERIFY(chain.session.stop());
        QTRY_COMPARE(countOf(chain.server, QStringLiteral("/Stopped")), 1);
        for (int i = 1; i < chain.server.reportLog.size(); ++i) {
            const QJsonObject body =
                QJsonDocument::fromJson(chain.server.reportLog.at(i).body).object();
            QCOMPARE(body.value(QStringLiteral("PositionTicks")).toVariant().toLongLong(), 8000);
        }
    }

    void failureAfterOpeningReportsFailedOnStop()
    {
        Chain chain(std::chrono::milliseconds(40));
        QVERIFY(chain.ok);
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QTRY_VERIFY(chain.server.reportLog.size() >= 1);
        QVERIFY(chain.session.failed());
        QTest::qWait(60); // let a report that was already queued go out
        const int atFailure = chain.server.reportLog.size();
        QTest::qWait(200);
        QCOMPARE(chain.server.reportLog.size(), atFailure); // progress stopped with the failure
        QVERIFY(chain.session.stop());
        QTRY_COMPARE(countOf(chain.server, QStringLiteral("/Stopped")), 1);
        QVERIFY(QJsonDocument::fromJson(chain.server.reportLog.last().body)
                    .object()
                    .value(QStringLiteral("Failed"))
                    .toBool());
        QVERIFY(chain.session.stopped());
        // The next playback is not marked as failed
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QVERIFY(chain.session.stop());
        QTRY_COMPARE(countOf(chain.server, QStringLiteral("/Stopped")), 2);
        QVERIFY(!QJsonDocument::fromJson(chain.server.reportLog.last().body)
                     .object()
                     .value(QStringLiteral("Failed"))
                     .toBool());
    }

    void aFailedOpeningNeverReports()
    {
        Chain chain(std::chrono::milliseconds(40));
        QVERIFY(chain.ok);
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.failed());
        QVERIFY(chain.session.stop());
        QVERIFY(chain.session.stopped());
        // And a cancellation before the first frame
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.stop());
        QVERIFY(chain.session.stopped());
        QTest::qWait(200);
        QCOMPARE(chain.server.reportLog.size(), 0);
    }

    void offlineAndCastRoutesNeverReport()
    {
        for (const auto route : {PlaybackSession::Route::Offline, PlaybackSession::Route::Cast}) {
            Chain chain(std::chrono::milliseconds(40));
            QVERIFY(chain.ok);
            QVERIFY(chain.session.open(kIdentity, route));
            QVERIFY(chain.session.opened());
            QVERIFY(chain.session.pause());
            QVERIFY(chain.session.resume());
            QVERIFY(chain.session.stop());
            QVERIFY(chain.session.stopped());
            QTest::qWait(150);
            QCOMPARE(chain.server.reportLog.size(), 0);
        }
    }

    void theOldStopPrecedesTheNewStart()
    {
        Chain chain(std::chrono::seconds(30));
        QVERIFY(chain.ok);
        chain.server.holdReports = true;
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QTRY_COMPARE(chain.server.reportLog.size(), 1); // Start of s1, unanswered
        QVERIFY(chain.session.stop());
        QVERIFY(chain.session.stopped());
        auto second = kIdentity;
        second.playSessionId = "s2";
        QVERIFY(chain.session.open(second, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        chain.server.holdReports = false;
        chain.server.releaseHeld();
        QTRY_COMPARE(chain.server.reportLog.size(), 3);
        QStringList sequence;
        for (const auto &entry : chain.server.reportLog) {
            const QString text = describe(entry);
            sequence << text.section(QLatin1Char(':'), 0, 1);
        }
        QCOMPARE(sequence, (QStringList{QStringLiteral("Playing:s1"), QStringLiteral("Stopped:s1"),
                                        QStringLiteral("Playing:s2")}));
    }

    void serverErrorsNeverChangePlayback()
    {
        Chain chain(std::chrono::milliseconds(40));
        QVERIFY(chain.ok);
        chain.server.reportStatus = 500;
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QTRY_VERIFY(countOf(chain.server, QStringLiteral("/Progress")) >= 2);
        QCOMPARE(chain.session.state(), PlaybackSession::State::Playing);
        QVERIFY(chain.client.isAuthenticated());
    }

    void anExpiredSessionEndsTheReportsButNotPlayback()
    {
        Chain chain(std::chrono::milliseconds(40));
        QVERIFY(chain.ok);
        chain.server.reportStatus = 401;
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QTRY_VERIFY(!chain.client.isAuthenticated());
        const int atExpiry = chain.server.reportLog.size();
        QTest::qWait(200);
        QCOMPARE(chain.server.reportLog.size(), atExpiry);
        // Stopping the session is the shell's call: the reporter does not touch it
        QCOMPARE(chain.session.state(), PlaybackSession::State::Playing);
    }

    void clearedCredentialsEndTheHeartbeat()
    {
        Chain chain(std::chrono::milliseconds(40));
        QVERIFY(chain.ok);
        QVERIFY(chain.session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(chain.session.opened());
        QTRY_VERIFY(countOf(chain.server, QStringLiteral("/Progress")) >= 2);
        // A user switch drops the session's credentials without expiring them; the next user signs in while the stream is still playing
        QSignalSpy loggedIn(&chain.client, &JellyfinClient::loginSucceeded);
        chain.client.forgetSession();
        QVERIFY(chain.client.authenticate(QStringLiteral("fixture-user"),
                                          QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QTest::qWait(60);
        const int atSwitch = countOf(chain.server, QStringLiteral("/Progress"));
        QTest::qWait(250);
        QCOMPARE(countOf(chain.server, QStringLiteral("/Progress")), atSwitch);
        QCOMPARE(chain.session.state(), PlaybackSession::State::Playing);
    }

    void theObserverIsReleasedWithTheReporterAndOthersSurvive()
    {
        PlaybackSession session;
        int transitions = 0;
        session.addObserver([&](auto, auto) { ++transitions; });
        {
            FakeJellyfin server;
            JellyfinClient client;
            PlaybackReporter reporter(session, client, {});
        }
        // No dangling observer: transitions after the reporter is gone are harmless, and the other listener is still called
        QVERIFY(session.open(kIdentity, PlaybackSession::Route::Direct));
        QVERIFY(session.opened());
        QVERIFY(session.stop());
        QVERIFY(session.stopped());
        QCOMPARE(transitions, 4);
    }
};
QTEST_GUILESS_MAIN(TestPlaybackReporter)
#include "test_playback_reporter.moc"
