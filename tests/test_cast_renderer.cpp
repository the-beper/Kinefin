// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FakeRenderer.h"

#include "cast/CastController.h"
#include "cast/CastTargetsModel.h"
#include "cast/DlnaRenderer.h"

#include <QSignalSpy>
#include <QUdpSocket>
#include <QtTest/QtTest>

using Kinefin::CastController;
using Kinefin::CastTargetsModel;
using Kinefin::DlnaRenderer;

namespace {
Kinefin::RendererInfo rendererInfo(const FakeRenderer &fake)
{
    Kinefin::RendererInfo info;
    info.udn = QStringLiteral("uuid:test-renderer");
    info.name = QStringLiteral("Fake & Renderer");
    info.model = QStringLiteral("Test TV");
    info.address = QHostAddress::LocalHost;
    info.location = QUrl(fake.descriptionUrl());
    info.avTransport.serviceType = QStringLiteral("urn:schemas-upnp-org:service:AVTransport:1");
    info.avTransport.controlUrl = QUrl(fake.baseUrl() + QStringLiteral("/AVTransport/control"));
    info.avTransport.scpdUrl = QUrl(fake.baseUrl() + QStringLiteral("/AVTransport/scpd.xml"));
    info.connectionManager.serviceType =
        QStringLiteral("urn:schemas-upnp-org:service:ConnectionManager:1");
    info.connectionManager.controlUrl =
        QUrl(fake.baseUrl() + QStringLiteral("/ConnectionManager/control"));
    info.renderingControl.serviceType =
        QStringLiteral("urn:schemas-upnp-org:service:RenderingControl:1");
    info.renderingControl.controlUrl =
        QUrl(fake.baseUrl() + QStringLiteral("/RenderingControl/control"));
    info.actions = {QStringLiteral("SetAVTransportURI"),
                    QStringLiteral("Play"),
                    QStringLiteral("Pause"),
                    QStringLiteral("Stop"),
                    QStringLiteral("Seek"),
                    QStringLiteral("GetTransportInfo"),
                    QStringLiteral("GetPositionInfo")};
    info.seekModes = {QStringLiteral("REL_TIME")};
    return info;
}

QString mediaUrl(const FakeRenderer &fake)
{
    return fake.baseUrl() + QStringLiteral("/media.mp4");
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
                 QStringLiteral("Test TV, 127.0.0.1"));
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
};

QTEST_GUILESS_MAIN(TestCastRenderer)
#include "test_cast_renderer.moc"
