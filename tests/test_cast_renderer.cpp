// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FakeRenderer.h"

#include "cast/CastController.h"
#include "cast/CastTargetsModel.h"
#include "cast/DlnaRenderer.h"

#include <QSignalSpy>
#include <QTimer>
#include <QUdpSocket>
#include <QtTest/QtTest>

using Kinefin::CastController;
using Kinefin::CastTargetsModel;
using Kinefin::DlnaRenderer;

namespace {
Kinefin::RendererInfo rendererInfo(const FakeRenderer &fake)
{
    return fake.rendererInfo(QStringLiteral("Fake & Renderer"));
}

QString mediaUrl(const FakeRenderer &fake)
{
    return fake.baseUrl() + QStringLiteral("/media.mp4");
}

bool loadDirect(DlnaRenderer &renderer, const FakeRenderer &fake)
{
    return renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                         QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true);
}

bool noCredentials(const FakeRenderer &fake)
{
    for (const QByteArray &head : fake.headers) {
        if (head.toLower().contains("authorization"))
            return false;
    }
    return true;
}
} // namespace

class TestCastRenderer : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void loadPlaysPollsAndStops()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy stopped(&renderer, &DlnaRenderer::stopped);
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        QVERIFY(fake.actions.contains(QStringLiteral("SetAVTransportURI")));
        QVERIFY(fake.actions.contains(QStringLiteral("Play")));
        QVERIFY(!fake.bodies.isEmpty());
        QVERIFY(fake.bodies.constFirst().contains("A title"));
        // A direct play first asked the stream once, with a range and no credential
        QCOMPARE(fake.mediaRequests, 1);
        QVERIFY(fake.headers.constFirst().contains("Range: bytes=0-0"));
        QVERIFY(noCredentials(fake));

        QTRY_VERIFY(renderer.positionMs() >= 0);
        QVERIFY(renderer.pause());
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Paused);
        QVERIFY(renderer.seek(30000));
        QTRY_VERIFY(fake.actions.contains(QStringLiteral("Seek")));
        QVERIFY(renderer.setVolume(70));
        QTRY_COMPARE(renderer.volume(), 70);
        QVERIFY(renderer.stop());
        QTRY_COMPARE(stopped.size(), 1);
        QVERIFY(!renderer.polling());
        QVERIFY(noCredentials(fake));
    }

    void aTranscodeCountsFromItsStartAndSeeksByReloading()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        fake.position = 10;
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp2t"),
                              QStringLiteral("A title"), 5000, 100000, QUrl(fake.baseUrl()),
                              false));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        // No probe and no Seek: the transcode URL itself starts at 5 s, and the renderer counts from there
        QCOMPARE(fake.mediaRequests, 0);
        QVERIFY(!fake.actions.contains(QStringLiteral("Seek")));
        QTRY_COMPARE(renderer.positionMs(), 15000);
        QVERIFY(renderer.canSeek());
        QVERIFY(!renderer.seek(10000));
        fake.position = 0;
        QVERIFY(renderer.reload(QUrl(mediaUrl(fake) + QStringLiteral("?StartTimeTicks=600000000")),
                                60000));
        QTRY_COMPARE(fake.actions.count(QStringLiteral("SetAVTransportURI")), 2);
        QTRY_COMPARE(renderer.positionMs(), 60000);
    }

    void theItemRuntimeWinsOverTheRendererDuration()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        fake.duration = 20;
        fake.position = 30;
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp2t"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), false));
        QTRY_COMPARE(renderer.positionMs(), 30000);
        QCOMPARE(renderer.durationMs(), 100000);
        // A position past the runtime is held at it
        fake.position = 99;
        renderer.setDurationMs(60000);
        QTRY_COMPARE(renderer.positionMs(), 60000);
    }

    void aResumeTheRendererIgnoredBeforePlayIsSoughtAgain_data()
    {
        QTest::addColumn<int>("startMs");
        QTest::addColumn<bool>("ignoresEarlySeek");
        QTest::addColumn<int>("seeks");
        QTest::addColumn<int>("rendererSeconds");
        QTest::newRow("ignored at 8 s") << 8000 << true << 2 << 8;
        QTest::newRow("honoured at 8 s") << 8000 << false << 1 << 8;
        QTest::newRow("ignored below 2 s") << 1500 << true << 1 << 0;
    }
    void aResumeTheRendererIgnoredBeforePlayIsSoughtAgain()
    {
        QFETCH(int, startMs);
        QFETCH(bool, ignoresEarlySeek);
        QFETCH(int, seeks);
        QFETCH(int, rendererSeconds);
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        fake.ignoreSeekWhileStopped = ignoresEarlySeek;
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), startMs, 100000, QUrl(fake.baseUrl()),
                              true));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        // The second position poll comes after the check of the first one and its Seek
        QTRY_VERIFY_WITH_TIMEOUT(fake.actions.count(QStringLiteral("GetPositionInfo")) >= 2, 10000);
        QCOMPARE(fake.actions.count(QStringLiteral("Seek")), seeks);
        QCOMPARE(fake.position, rendererSeconds);
        QCOMPARE(renderer.positionMs(), rendererSeconds * 1000);
    }

    void aLowerCaseStateStillPlays()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy stopped(&renderer, &DlnaRenderer::stopped);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        fake.reportedTransport = QStringLiteral("playing");
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        fake.reportedTransport = QStringLiteral("paused_playback");
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Paused);
        QCOMPARE(stopped.size(), 0);
        QCOMPARE(failed.size(), 0);
        QVERIFY(renderer.polling());
    }

    void aVendorStateDoesNotEndTheSession()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy stopped(&renderer, &DlnaRenderer::stopped);
        QSignalSpy ended(&renderer, &DlnaRenderer::ended);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        fake.reportedTransport = QStringLiteral("X_VENDOR_BUFFERING");
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Transitioning);
        const int polls = int(fake.actions.count(QStringLiteral("GetTransportInfo")));
        QTRY_VERIFY(fake.actions.count(QStringLiteral("GetTransportInfo")) >= polls + 2);
        QCOMPARE(stopped.size(), 0);
        QCOMPARE(ended.size(), 0);
        QCOMPARE(failed.size(), 0);
        QVERIFY(renderer.polling());
        fake.reportedTransport = QStringLiteral("PLAYING");
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        // The renderer's own STOPPED still ends the session, in any case
        fake.reportedTransport = QStringLiteral("stopped");
        QTRY_COMPARE(stopped.size(), 1);
        QVERIFY(!renderer.polling());
    }

    void aForeignTrackStopsTheCast()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy stopped(&renderer, &DlnaRenderer::stopped);
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        fake.reportedTrackUri = fake.baseUrl() + QStringLiteral("/other.mp4");
        QTRY_COMPARE(stopped.size(), 1);
        QVERIFY(!renderer.polling());
    }

    void itsOwnTrackEchoedDifferentlyIsNotForeign()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy stopped(&renderer, &DlnaRenderer::stopped);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        QVERIFY(renderer.load(QUrl(mediaUrl(fake) + QStringLiteral("?Static=true&a=b%20c")),
                              QStringLiteral("video/mp4"), QStringLiteral("A title"), 0, 100000,
                              QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        // Another case in the host, the query dropped, or nothing a URL at all: still this cast
        fake.reportedTrackUri = QStringLiteral("HTTP://127.0.0.1:%1/media.mp4").arg(fake.port());
        const int polls = int(fake.actions.count(QStringLiteral("GetPositionInfo")));
        QTRY_VERIFY(fake.actions.count(QStringLiteral("GetPositionInfo")) >= polls + 2);
        fake.reportedTrackUri = QStringLiteral("NOT_IMPLEMENTED");
        QTRY_VERIFY(fake.actions.count(QStringLiteral("GetPositionInfo")) >= polls + 4);
        QCOMPARE(stopped.size(), 0);
        QCOMPARE(failed.size(), 0);
        QVERIFY(renderer.polling());
    }

    void aRefusedCommandKeepsTheCast()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        fake.faults = {QStringLiteral("Pause"), QStringLiteral("Seek"),
                       QStringLiteral("SetVolume")};
        QVERIFY(renderer.pause());
        QVERIFY(renderer.seek(10000));
        QVERIFY(renderer.setVolume(30));
        QTRY_COMPARE(fake.actions.count(QStringLiteral("SetVolume")), 1);
        const int polls = int(fake.actions.count(QStringLiteral("GetTransportInfo")));
        QTRY_VERIFY(fake.actions.count(QStringLiteral("GetTransportInfo")) > polls);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        QVERIFY(renderer.polling());
    }

    void aRefusedLoadIsARefusal()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        fake.faults = {QStringLiteral("SetAVTransportURI")};
        fake.faultCode = 714;
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().constFirst().value<DlnaRenderer::Failure>(),
                 DlnaRenderer::Failure::Refused);
        QVERIFY(!renderer.polling());
        QVERIFY(!fake.actions.contains(QStringLiteral("Play")));
    }

    void aStopBeforeItEverPlayedIsARefusal()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        QSignalSpy stopped(&renderer, &DlnaRenderer::stopped);
        fake.playStarts = false;
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().constFirst().value<DlnaRenderer::Failure>(),
                 DlnaRenderer::Failure::Refused);
        QCOMPARE(stopped.size(), 0);
    }

    void aStreamTheServerGuardsNeverReachesTheRenderer()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        fake.mediaStatus = 401;
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().constFirst().value<DlnaRenderer::Failure>(),
                 DlnaRenderer::Failure::ServerRefused);
        QCOMPARE(fake.mediaRequests, 1);
        QVERIFY(fake.actions.isEmpty());
    }

    void aStreamOffTheServerOrWithCredentialsIsRefused()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        const QUrl base(fake.baseUrl());
        QVERIFY(!renderer.load(QUrl(QStringLiteral("http://192.0.2.1/media.mp4")),
                               QStringLiteral("video/mp4"), QString(), 0, 0, base, true));
        QVERIFY(!renderer.load(
            QUrl(QStringLiteral("http://user:secret@127.0.0.1:%1/media.mp4").arg(fake.port())),
            QStringLiteral("video/mp4"), QString(), 0, 0, base, true));
        QVERIFY(!renderer.load(QUrl(QStringLiteral("file:///etc/passwd")),
                               QStringLiteral("video/mp4"), QString(), 0, 0, base, true));
        QCOMPARE(fake.mediaRequests, 0);
        QVERIFY(fake.actions.isEmpty());
    }

    void aStopThenALoadKeepsTheNewTrack()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy stopped(&renderer, &DlnaRenderer::stopped);
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        // The Stop of the old item is still on its way when the next one loads: its answer must not end the new one
        QVERIFY(renderer.stop());
        QVERIFY(renderer.load(QUrl(mediaUrl(fake) + QStringLiteral("2")),
                              QStringLiteral("video/mp4"), QStringLiteral("Next"), 0, 100000,
                              QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(fake.actions.count(QStringLiteral("SetAVTransportURI")), 2);
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        const int polls = int(fake.actions.count(QStringLiteral("GetPositionInfo")));
        QTRY_VERIFY(fake.actions.count(QStringLiteral("GetPositionInfo")) >= polls + 2);
        QCOMPARE(stopped.size(), 0);
        QVERIFY(renderer.polling());
    }

    void slowPollsDoNotPileUp()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        // Each poll takes as long as two ticks of the poll timer: a backlog would flush in a burst once the renderer is quick again
        fake.pollDelayMs = 1200;
        QTest::qWait(4000);
        fake.pollDelayMs = 0;
        const int before = int(fake.actions.count(QStringLiteral("GetTransportInfo")));
        QTest::qWait(500);
        QVERIFY2(fake.actions.count(QStringLiteral("GetTransportInfo")) <= before + 1,
                 qPrintable(QString::number(fake.actions.count(QStringLiteral("GetTransportInfo"))
                                            - before)));
    }

    void threeFailedPollsReportTheDevice()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), true));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        fake.failPolls = true;
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().constFirst().value<DlnaRenderer::Failure>(),
                 DlnaRenderer::Failure::NotResponding);
        QVERIFY(!renderer.polling());
    }

    void aRendererStuckAtZeroIsRefused()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        renderer.setStallTimeoutMs(600);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        QVERIFY(loadDirect(renderer, fake));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QCOMPARE(failed.constFirst().constFirst().value<DlnaRenderer::Failure>(),
                 DlnaRenderer::Failure::Refused);
        QVERIFY(!renderer.polling());
        QTRY_VERIFY(fake.actions.contains(QStringLiteral("Stop")));
    }

    void aTranscodeIsNeverStallFailed()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        renderer.setStallTimeoutMs(600);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        QVERIFY(renderer.load(QUrl(mediaUrl(fake)), QStringLiteral("video/mp4"),
                              QStringLiteral("A title"), 0, 100000, QUrl(fake.baseUrl()), false));
        QTRY_VERIFY(renderer.transport() == DlnaRenderer::Transport::Playing);
        QTest::qWait(2500);
        QCOMPARE(failed.size(), 0);
    }

    void aMovingRendererIsNotStalled()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        renderer.setStallTimeoutMs(2500);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        QTimer playhead;
        playhead.setInterval(200);
        connect(&playhead, &QTimer::timeout, this, [&fake] { ++fake.position; });
        QVERIFY(loadDirect(renderer, fake));
        playhead.start();
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        // Longer than the stall timeout plus a poll: a watchdog that missed the movement would have failed the load by now
        QTest::qWait(4500);
        QCOMPARE(failed.size(), 0);
        QVERIFY(renderer.polling());
        QVERIFY(renderer.positionMs() > 0);
    }

    void aPausedRendererIsNotStalled()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        renderer.setStallTimeoutMs(2500);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        // The first position poll arms the stall timer and the next poll reports the pause
        fake.position = 7;
        QVERIFY(loadDirect(renderer, fake));
        QTRY_COMPARE(renderer.positionMs(), 7000);
        QVERIFY(renderer.pause());
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Paused);
        QTest::qWait(3000);
        QCOMPARE(failed.size(), 0);
        QVERIFY(renderer.polling());
    }

    void aPlayAfterAPauseArmsTheWatchdogAgain()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        renderer.setStallTimeoutMs(2500);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        fake.position = 7;
        QVERIFY(loadDirect(renderer, fake));
        QTRY_COMPARE(renderer.positionMs(), 7000);
        QVERIFY(renderer.pause());
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Paused);
        QVERIFY(renderer.play());
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QCOMPARE(failed.constFirst().constFirst().value<DlnaRenderer::Failure>(),
                 DlnaRenderer::Failure::Refused);
    }

    void aRendererWithoutRelTimeIsNotStalled()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        renderer.setStallTimeoutMs(600);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        fake.reportedRelTime = QStringLiteral("NOT_IMPLEMENTED");
        QVERIFY(loadDirect(renderer, fake));
        QTRY_COMPARE(renderer.transport(), DlnaRenderer::Transport::Playing);
        QTest::qWait(3000);
        QVERIFY(fake.actions.count(QStringLiteral("GetPositionInfo")) >= 2);
        QCOMPARE(failed.size(), 0);
        QVERIFY(renderer.polling());
    }

    void aStallAfterProgressIsNotFailed()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        renderer.setStallTimeoutMs(2500);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        fake.position = 7;
        QVERIFY(loadDirect(renderer, fake));
        QTRY_COMPARE(renderer.positionMs(), 7000);
        // The renderer moves once and then freezes at 20 s
        fake.position = 20;
        QTRY_COMPARE(renderer.positionMs(), 20000);
        QTest::qWait(4000);
        QCOMPARE(failed.size(), 0);
        QVERIFY(renderer.polling());
        QCOMPARE(renderer.positionMs(), 20000);
    }

    void aSeekCountsAsProgress()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        renderer.setStallTimeoutMs(2500);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        fake.position = 7;
        QVERIFY(loadDirect(renderer, fake));
        QTRY_COMPARE(renderer.positionMs(), 7000);
        QVERIFY(renderer.seek(30000));
        QTRY_COMPARE(fake.position, 30);
        QTest::qWait(4000);
        QCOMPARE(failed.size(), 0);
        QVERIFY(renderer.polling());
        QCOMPARE(fake.position, 30);
    }

    void aStalledLoadThatWasStoppedDoesNotFailLater()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        DlnaRenderer renderer(rendererInfo(fake));
        renderer.setStallTimeoutMs(2500);
        QSignalSpy failed(&renderer, &DlnaRenderer::failed);
        QSignalSpy stopped(&renderer, &DlnaRenderer::stopped);
        fake.position = 7;
        QVERIFY(loadDirect(renderer, fake));
        QTRY_COMPARE(renderer.positionMs(), 7000);
        QVERIFY(renderer.stop());
        QTRY_COMPARE(stopped.size(), 1);
        QTest::qWait(4000);
        QCOMPARE(failed.size(), 0);
        QVERIFY(!renderer.polling());
    }

    void discoveryListsTheRendererAndLoadsIt()
    {
        FakeRenderer fake;
        QVERIFY(fake.listen());
        QUdpSocket responder;
        QVERIFY(responder.bind(QHostAddress::LocalHost, 0));
        connect(&responder, &QUdpSocket::readyRead, this, [&responder, &fake] {
            while (responder.hasPendingDatagrams()) {
                QByteArray datagram;
                datagram.resize(int(responder.pendingDatagramSize()));
                QHostAddress sender;
                quint16 senderPort = 0;
                responder.readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
                if (!datagram.contains("M-SEARCH"))
                    continue;
                const QByteArray reply =
                    "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\n"
                    "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                    "USN: uuid:test-renderer::urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                    "LOCATION: "
                    + fake.descriptionUrl().toUtf8() + "\r\n\r\n";
                responder.writeDatagram(reply, sender, senderPort);
            }
        });
        Kinefin::SsdpSearch::Options options;
        options.target = QHostAddress::LocalHost;
        options.port = responder.localPort();
        options.windowMs = 500;
        options.repeatMs = 100;
        options.senderPolicy = [](const QHostAddress &) { return true; };
        CastController controller(options);
        controller.search();
        QTRY_COMPARE(controller.targets()->rowCount(), 1);
        QCOMPARE(controller.targets()
                     ->data(controller.targets()->index(0), CastTargetsModel::NameRole)
                     .toString(),
                 QStringLiteral("Fake & Renderer"));
        QCOMPARE(controller.targets()
                     ->data(controller.targets()->index(0), CastTargetsModel::DetailRole)
                     .toString(),
                 QStringLiteral("DLNA, Test TV, 127.0.0.1"));
        QVERIFY(controller.targets()->indexOf(QStringLiteral("uuid:test-renderer")) >= 0);
        QVERIFY(controller.load(QStringLiteral("uuid:test-renderer"), QUrl(mediaUrl(fake)),
                                QStringLiteral("video/mp4"), QStringLiteral("A title"), 0, 100000,
                                QUrl(fake.baseUrl()), true));
        QTRY_VERIFY(controller.isPlaying());
        QVERIFY(controller.active());
        QCOMPARE(controller.activeName(), QStringLiteral("Fake & Renderer"));
        QVERIFY(noCredentials(fake));

        // The device that plays stays listed when it does not answer the next search; any other silent one goes
        Kinefin::RendererInfo other = rendererInfo(fake);
        other.udn = QStringLiteral("uuid:other-renderer");
        controller.addRenderer(other);
        QCOMPARE(controller.targets()->rowCount(), 2);
        responder.close();
        controller.search();
        QTRY_VERIFY(!controller.searching());
        QCOMPARE(controller.targets()->rowCount(), 1);
        QVERIFY(controller.targets()->indexOf(QStringLiteral("uuid:test-renderer")) >= 0);
    }

    // Opt-in against a real renderer: KINEFIN_DLNA_LIVE_HOST is its address and KINEFIN_DLNA_LIVE_URL a token-free http media URL of at least 60 seconds with Range support (KINEFIN_DLNA_LIVE_MIME defaults to video/mp4, KINEFIN_DLNA_LIVE_DURATION_S to 60). The production search finds it, then it plays, pauses, resumes, seeks and stops
    void liveRenderer()
    {
        const QString host = qEnvironmentVariable("KINEFIN_DLNA_LIVE_HOST");
        const QUrl url(qEnvironmentVariable("KINEFIN_DLNA_LIVE_URL"));
        if (host.isEmpty() || !url.isValid() || url.isEmpty())
            QSKIP("no KINEFIN_DLNA_LIVE_HOST and KINEFIN_DLNA_LIVE_URL, skipping the live check");
        const QString mime =
            qEnvironmentVariable("KINEFIN_DLNA_LIVE_MIME", QStringLiteral("video/mp4"));
        const qint64 durationMs =
            qEnvironmentVariable("KINEFIN_DLNA_LIVE_DURATION_S", QStringLiteral("60")).toInt()
            * 1000;
        CastController controller;
        controller.search();
        QString id;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                CastTargetsModel *targets = controller.targets();
                for (int row = 0; row < targets->rowCount(); ++row) {
                    const QModelIndex index = targets->index(row);
                    if (targets->data(index, CastTargetsModel::DetailRole).toString().contains(host)
                        && targets->data(index, CastTargetsModel::KindRole).toString()
                               == QLatin1String("dlna")) {
                        id = targets->data(index, CastTargetsModel::UdnRole).toString();
                        return true;
                    }
                }
                return false;
            }(),
            15'000);
        QSignalSpy playing(&controller, &CastController::playing);
        QSignalSpy failed(&controller, &CastController::failed);
        QSignalSpy stopped(&controller, &CastController::stopped);
        QVERIFY(controller.load(id, url, mime, QStringLiteral("Kinefin live check"), 0, durationMs,
                                QUrl(url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery)), true));
        QTRY_VERIFY_WITH_TIMEOUT(playing.count() >= 1 || failed.count() >= 1, 40'000);
        QCOMPARE(failed.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(controller.positionMs().value_or(0) >= 3000, 20'000);
        QVERIFY(controller.pause());
        QTRY_VERIFY_WITH_TIMEOUT(controller.isPaused(), 10'000);
        QVERIFY(controller.resume());
        QTRY_VERIFY_WITH_TIMEOUT(controller.isPlaying(), 10'000);
        QVERIFY(controller.seek(30'000));
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(controller.positionMs().value_or(0) - 30'000) < 6000,
                                 15'000);
        QVERIFY(controller.stop());
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 10'000);
        QCOMPARE(failed.count(), 0);
    }
};

QTEST_GUILESS_MAIN(TestCastRenderer)
#include "test_cast_renderer.moc"
