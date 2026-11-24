// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "CastTestPki.h"
#include "FakeCastDevice.h"

#include "cast/CastController.h"
#include "cast/CastProfile.h"
#include "cast/CastTargetsModel.h"
#include "cast/RendererProfile.h"

#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QUdpSocket>
#include <QUrl>
#include <QtTest/QtTest>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

using Kinefin::CastController;
using Kinefin::CastDeviceInfo;
using Kinefin::CastLoad;
using Kinefin::CastSender;
using Kinefin::CastTargetsModel;
using Kinefin::MdnsSearch;
using Kinefin::SsdpSearch;
using Failure = Kinefin::CastRenderer::Failure;

namespace {
const QString kMediaNs = QString(Kinefin::kCastNsMedia);
const QString kLoad = QStringLiteral("LOAD");
const QString kBase = QStringLiteral("http://127.0.0.1:8096");
const QString kDeviceId = QStringLiteral("00112233445566778899aabbccddeeff");
const QString kName = QStringLiteral("Living room");
const QString kModel = QStringLiteral("Google Nest Hub");
using DeviceLookup = std::function<void(const std::optional<CastDeviceInfo> &)>;

QString idOf(int n)
{
    return QStringLiteral("%1").arg(n, 32, 16, QLatin1Char('0'));
}

QString castTarget(const QString &id)
{
    return QStringLiteral("cast:") + id;
}

CastDeviceInfo castInfo(const QString &id, const QString &name, const QString &model = kModel)
{
    CastDeviceInfo info;
    info.id = id;
    info.name = name;
    info.model = model;
    info.address = QHostAddress::LocalHost;
    return info;
}

Kinefin::RendererInfo dlnaInfo()
{
    Kinefin::RendererInfo info;
    info.udn = QStringLiteral("uuid:test-renderer");
    info.name = QStringLiteral("Living room TV");
    info.model = QStringLiteral("Test TV");
    info.address = QHostAddress::LocalHost;
    info.sinkMimeTypes = {QStringLiteral("video/x-matroska"), QStringLiteral("video/mp4")};
    return info;
}

QByteArray be16(quint16 value)
{
    QByteArray out;
    out.append(static_cast<char>(value >> 8));
    out.append(static_cast<char>(value & 0xFF));
    return out;
}

QByteArray be32(quint32 value)
{
    QByteArray out = be16(static_cast<quint16>(value >> 16));
    out.append(be16(static_cast<quint16>(value & 0xFFFF)));
    return out;
}

QByteArray wireName(const QString &dotted)
{
    QByteArray out;
    for (const QString &label : dotted.split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
        const QByteArray bytes = label.toUtf8();
        out.append(static_cast<char>(bytes.size()));
        out.append(bytes);
    }
    out.append('\0');
    return out;
}

QByteArray record(const QString &owner, quint16 type, quint16 recordClass, const QByteArray &data)
{
    QByteArray out = wireName(owner);
    out.append(be16(type));
    out.append(be16(recordClass));
    out.append(be32(120));
    out.append(be16(static_cast<quint16>(data.size())));
    out.append(data);
    return out;
}

void addTxtString(QByteArray &txt, const QString &string)
{
    const QByteArray bytes = string.toUtf8();
    txt.append(static_cast<char>(bytes.size()));
    txt.append(bytes);
}

// What a Cast device answers: the PTR as an answer, the SRV, TXT and A records as additional ones, the A record naming 127.0.0.1
QByteArray castAnswer(const QString &id, const QString &name, const QString &model)
{
    const QString instance = QStringLiteral("Chromecast-%1._googlecast._tcp.local").arg(id);
    const QString host = QStringLiteral("host-%1.local").arg(id.left(8));
    const quint32 loopback = QHostAddress(QHostAddress::LocalHost).toIPv4Address();
    QByteArray srv = be16(0);
    srv.append(be16(0));
    srv.append(be16(8009));
    srv.append(wireName(host));
    QByteArray txt;
    addTxtString(txt, QStringLiteral("id=%1").arg(id));
    addTxtString(txt, QStringLiteral("ve=05"));
    addTxtString(txt, QStringLiteral("md=%1").arg(model));
    addTxtString(txt, QStringLiteral("fn=%1").arg(name));
    addTxtString(txt, QStringLiteral("ca=231941"));
    addTxtString(txt, QStringLiteral("st=0"));
    QByteArray out = be16(0);
    out.append(be16(0x8400));
    out.append(be16(0));
    out.append(be16(1));
    out.append(be16(0));
    out.append(be16(3));
    out.append(record(QStringLiteral("_googlecast._tcp.local"), 12, 0x0001, wireName(instance)));
    out.append(record(instance, 33, 0x8001, srv));
    out.append(record(instance, 16, 0x8001, txt));
    out.append(record(host, 1, 0x8001, be32(loopback)));
    return out;
}

// A unicast socket that answers every datagram it receives with the given replies, standing in for the devices
class FakeResponder {
  public:
    FakeResponder()
    {
        QObject::connect(&m_socket, &QUdpSocket::readyRead, &m_socket, [this] {
            while (m_socket.hasPendingDatagrams()) {
                const QNetworkDatagram datagram = m_socket.receiveDatagram();
                received.append(datagram.data());
                for (const QByteArray &reply : std::as_const(replies))
                    m_socket.writeDatagram(reply, datagram.senderAddress(), datagram.senderPort());
            }
        });
    }
    bool listen() { return m_socket.bind(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_socket.localPort(); }

    QList<QByteArray> replies;
    QList<QByteArray> received;

  private:
    QUdpSocket m_socket;
};

bool allowLoopback(const QHostAddress &address)
{
    return address.isLoopback();
}

SsdpSearch::Options ssdpOptions(quint16 port, int windowMs = 400)
{
    SsdpSearch::Options options;
    options.target = QHostAddress::LocalHost;
    options.port = port;
    options.windowMs = windowMs;
    options.repeatMs = 100;
    options.senderPolicy = allowLoopback;
    return options;
}

MdnsSearch::Options mdnsOptions(quint16 port, int windowMs = 400)
{
    MdnsSearch::Options options;
    options.target = QHostAddress::LocalHost;
    options.port = port;
    options.bindPort = 0;
    options.senderPort = 0;
    options.windowMs = windowMs;
    options.repeatMs = 100;
    options.senderPolicy = allowLoopback;
    return options;
}

void addWindowOrders()
{
    QTest::addColumn<int>("ssdpMs");
    QTest::addColumn<int>("mdnsMs");
    QTest::newRow("the SSDP window ends first") << 150 << 600;
    QTest::newRow("the mDNS window ends first") << 600 << 150;
}

QString roleOf(const CastController &controller, int row, int role)
{
    return controller.targets()->data(controller.targets()->index(row), role).toString();
}

CastLoad transcodeRequest()
{
    CastLoad request;
    request.url = QUrl(kBase + QStringLiteral("/transcode.mp4?DeviceId=abc"));
    request.mime = QStringLiteral("video/mp4");
    request.title = QStringLiteral("A title");
    request.serverBase = QUrl(kBase);
    request.direct = false;
    request.durationMs = 600000;
    return request;
}

// A lost connection must not start a real mDNS search
void deviceNotFound(const QString &, DeviceLookup done)
{
    done(std::nullopt);
}

// A fake device on loopback and the options of a sender that talks to it; the device presents the chain of `pki`
struct CastRig {
    explicit CastRig(const CastTestPki &pki) : device(pki)
    {
        device.receiverEnabled = true;
        listening = device.listen();
    }

    CastDeviceInfo info() const
    {
        CastDeviceInfo value = castInfo(kDeviceId, QStringLiteral("Test device"));
        value.port = device.port();
        return value;
    }

    CastSender::Options options(const CastTestPki &trust) const
    {
        CastSender::Options value;
        value.channel.trust = trust.trust();
        value.launchTimeoutMs = 3000;
        value.startTimeoutMs = 5000;
        value.stuckMs = 2000;
        value.statusPollMs = 60'000;
        value.positionTickMs = 50;
        value.reconnectAttempts = 1;
        value.reconnectDelayMs = 50;
        value.stopTimeoutMs = 1500;
        value.resolveDevice = deviceNotFound;
        return value;
    }

    FakeCastDevice device;
    bool listening = false;
};
} // namespace

class TestCastController : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<CastTestPki> m_pki;
    std::unique_ptr<CastTestPki> m_otherPki;

  private Q_SLOTS:
    // Generating the keys takes longer than any test, so the PKIs are built once
    void initTestCase()
    {
        m_pki = std::make_unique<CastTestPki>();
        m_otherPki = std::make_unique<CastTestPki>();
        QVERIFY(m_pki->isValid());
        QVERIFY(m_otherPki->isValid());
    }

    void discoveryListsACastDevice()
    {
        FakeResponder ssdp;
        FakeResponder mdns;
        QVERIFY(ssdp.listen());
        QVERIFY(mdns.listen());
        mdns.replies = {castAnswer(kDeviceId, kName, kModel)};
        CastController controller(ssdpOptions(ssdp.port(), 1500), mdnsOptions(mdns.port(), 1500));
        controller.search();
        QVERIFY(controller.searching());
        QTRY_COMPARE(controller.targets()->rowCount(), 1);
        QCOMPARE(roleOf(controller, 0, CastTargetsModel::UdnRole), castTarget(kDeviceId));
        QCOMPARE(roleOf(controller, 0, CastTargetsModel::NameRole), kName);
        QCOMPARE(roleOf(controller, 0, CastTargetsModel::DetailRole),
                 QStringLiteral("Google Cast, Google Nest Hub, 127.0.0.1"));
        QCOMPARE(roleOf(controller, 0, CastTargetsModel::KindRole), QStringLiteral("cast"));
        QVERIFY(controller.hasTarget(castTarget(kDeviceId)));
        QVERIFY(mdns.received.contains(Kinefin::mdnsQuery()));

        // The answers of the repeated queries and the end of the search leave one row
        QTRY_VERIFY(!controller.searching());
        QCOMPARE(controller.targets()->rowCount(), 1);
        QVERIFY(controller.error().isEmpty());
    }

    void aDeviceWithoutAModelIsListedAsGoogleCast()
    {
        CastController controller(SsdpSearch::Options{});
        controller.addCastDevice(castInfo(kDeviceId, kName, QString()));
        QCOMPARE(controller.targets()->rowCount(), 1);
        QCOMPARE(roleOf(controller, 0, CastTargetsModel::DetailRole),
                 QStringLiteral("Google Cast, 127.0.0.1"));
    }

    void aDeviceThatStopsAnsweringIsDroppedWhenBothSearchesEnd_data() { addWindowOrders(); }

    void aDeviceThatStopsAnsweringIsDroppedWhenBothSearchesEnd()
    {
        QFETCH(int, ssdpMs);
        QFETCH(int, mdnsMs);
        FakeResponder ssdp;
        FakeResponder mdns;
        QVERIFY(ssdp.listen());
        QVERIFY(mdns.listen());
        CastController controller(ssdpOptions(ssdp.port(), ssdpMs),
                                  mdnsOptions(mdns.port(), mdnsMs));
        const QString id = castTarget(kDeviceId);
        controller.addCastDevice(castInfo(kDeviceId, kName));
        QVERIFY(controller.hasTarget(id));
        QCOMPARE(controller.targets()->rowCount(), 1);

        QList<bool> listed;
        connect(&controller, &CastController::searchingChanged, this,
                [&listed, &controller, id] { listed.append(controller.hasTarget(id)); });
        controller.search();
        QTRY_VERIFY(!controller.searching());
        // Listed as the search starts and as its first window ends, gone as the last one ends
        QCOMPARE(listed, (QList<bool>{true, true, false}));
        QCOMPARE(controller.targets()->rowCount(), 0);
        QVERIFY(controller.targets()->indexOf(id) < 0);
        QVERIFY(!controller.hasTarget(id));
    }

    void theActiveDeviceStaysWhenItDoesNotAnswer()
    {
        CastRig rig(*m_pki);
        QVERIFY(rig.listening);
        FakeResponder ssdp;
        FakeResponder mdns;
        QVERIFY(ssdp.listen());
        QVERIFY(mdns.listen());
        CastController controller(ssdpOptions(ssdp.port(), 200), mdnsOptions(mdns.port(), 200));
        const QString playing = castTarget(kDeviceId);
        const QString silent = castTarget(idOf(2));
        controller.addCastDevice(rig.info(), rig.options(*m_pki));
        controller.addCastDevice(castInfo(idOf(2), QStringLiteral("Silent")));
        QCOMPARE(controller.targets()->rowCount(), 2);
        QVERIFY(controller.load(playing, transcodeRequest()));
        QTRY_VERIFY(controller.isPlaying());

        controller.search();
        QTRY_VERIFY(!controller.searching());
        QCOMPARE(controller.targets()->rowCount(), 1);
        QVERIFY(controller.targets()->indexOf(playing) >= 0);
        QVERIFY(controller.hasTarget(playing));
        QVERIFY(!controller.hasTarget(silent));
        QVERIFY(controller.active());
        QCOMPARE(controller.activeUdn(), playing);

        // The device that stayed is still the one the controls reach
        QVERIFY(controller.pause());
        QTRY_VERIFY(controller.isPaused());
    }

    void searchingIsTrueWhileEitherSearchRuns_data() { addWindowOrders(); }

    void searchingIsTrueWhileEitherSearchRuns()
    {
        QFETCH(int, ssdpMs);
        QFETCH(int, mdnsMs);
        FakeResponder ssdp;
        FakeResponder mdns;
        QVERIFY(ssdp.listen());
        QVERIFY(mdns.listen());
        CastController controller(ssdpOptions(ssdp.port(), ssdpMs),
                                  mdnsOptions(mdns.port(), mdnsMs));
        QList<bool> states;
        connect(&controller, &CastController::searchingChanged, this,
                [&states, &controller] { states.append(controller.searching()); });
        QVERIFY(!controller.searching());
        controller.search();
        QVERIFY(controller.searching());
        QTRY_VERIFY(!controller.searching());
        // The start, the end of the first window with the other still running, the end of the last
        QCOMPARE(states, (QList<bool>{true, true, false}));

        controller.search();
        QVERIFY(controller.searching());
        controller.stopSearch();
        QVERIFY(!controller.searching());
    }

    void theSsdpOnlyControllerNeverSearchesMdns()
    {
        FakeResponder responder;
        QVERIFY(responder.listen());
        {
            CastController controller(ssdpOptions(responder.port(), 300));
            controller.search();
            QVERIFY(controller.searching());
            QTRY_VERIFY(!controller.searching());
            QVERIFY(controller.error().isEmpty());
        }
        QTRY_VERIFY(!responder.received.isEmpty());
        for (const QByteArray &datagram : std::as_const(responder.received))
            QCOMPARE(datagram, Kinefin::ssdpSearchRequest());

        // The same port hears the mDNS query of a controller that has the search
        FakeResponder both;
        QVERIFY(both.listen());
        CastController full(ssdpOptions(both.port(), 300), mdnsOptions(both.port(), 300));
        full.search();
        QTRY_VERIFY(!full.searching());
        QTRY_VERIFY(both.received.contains(Kinefin::mdnsQuery()));
        QVERIFY(both.received.contains(Kinefin::ssdpSearchRequest()));
    }

    void hasTargetDeviceProfileAndMimeForBothKinds_data()
    {
        QTest::addColumn<bool>("burnIn");
        QTest::addColumn<bool>("transcodeOnly");
        QTest::newRow("plain") << false << false;
        QTest::newRow("burn in") << true << false;
        QTest::newRow("transcode only") << false << true;
        QTest::newRow("burn in, transcode only") << true << true;
    }

    void hasTargetDeviceProfileAndMimeForBothKinds()
    {
        QFETCH(bool, burnIn);
        QFETCH(bool, transcodeOnly);
        CastController controller(SsdpSearch::Options{});
        const CastDeviceInfo device = castInfo(kDeviceId, kName);
        const Kinefin::RendererInfo renderer = dlnaInfo();
        controller.addCastDevice(device);
        controller.addRenderer(renderer);
        const QString cast = castTarget(kDeviceId);
        const QString unknown = castTarget(idOf(9));

        QVERIFY(controller.hasTarget(cast));
        QVERIFY(controller.hasTarget(renderer.udn));
        QVERIFY(!controller.hasTarget(unknown));
        QCOMPARE(controller.targets()->rowCount(), 2);

        const QJsonObject castProfile = controller.deviceProfile(cast, burnIn, transcodeOnly);
        QVERIFY(!castProfile.isEmpty());
        QCOMPARE(castProfile, Kinefin::castDeviceProfile(device.model, burnIn, transcodeOnly));
        const QJsonObject dlnaProfile =
            controller.deviceProfile(renderer.udn, burnIn, transcodeOnly);
        QVERIFY(!dlnaProfile.isEmpty());
        QCOMPARE(dlnaProfile,
                 Kinefin::rendererDeviceProfile(renderer.sinkMimeTypes, burnIn, transcodeOnly));
        QVERIFY(castProfile != dlnaProfile);
        QVERIFY(controller.deviceProfile(unknown, burnIn, transcodeOnly).isEmpty());

        const QString mkv = QStringLiteral("mkv");
        const QString avi = QStringLiteral("avi");
        QCOMPARE(controller.mimeFor(cast, mkv, false), Kinefin::castMime(mkv, false));
        QCOMPARE(controller.mimeFor(cast, mkv, false), QStringLiteral("video/x-matroska"));
        QCOMPARE(controller.mimeFor(cast, mkv, true), Kinefin::castMime(mkv, true));
        QCOMPARE(controller.mimeFor(cast, mkv, true), QStringLiteral("video/x-matroska"));
        QCOMPARE(controller.mimeFor(cast, avi, false), Kinefin::castMime(avi, false));
        QVERIFY(controller.mimeFor(cast, avi, false).isEmpty());

        QCOMPARE(controller.mimeFor(renderer.udn, mkv, false),
                 Kinefin::rendererMime(mkv, renderer.sinkMimeTypes));
        QVERIFY(!controller.mimeFor(renderer.udn, mkv, false).isEmpty());
        QCOMPARE(controller.mimeFor(renderer.udn, mkv, true),
                 Kinefin::rendererMime(QStringLiteral("ts"), renderer.sinkMimeTypes));
        QCOMPARE(controller.mimeFor(renderer.udn, avi, false),
                 Kinefin::rendererMime(avi, renderer.sinkMimeTypes));

        QVERIFY(controller.mimeFor(unknown, mkv, false).isEmpty());
        QVERIFY(controller.mimeFor(unknown, mkv, true).isEmpty());
    }

    void takesTextTrackOnlyForCast()
    {
        CastController controller(SsdpSearch::Options{});
        controller.addCastDevice(castInfo(kDeviceId, kName));
        controller.addRenderer(dlnaInfo());
        QVERIFY(controller.takesTextTrack(castTarget(kDeviceId)));
        QVERIFY(!controller.takesTextTrack(dlnaInfo().udn));
        QVERIFY(!controller.takesTextTrack(castTarget(idOf(9))));
    }

    void nameForACastDevice()
    {
        CastController controller(SsdpSearch::Options{});
        controller.addCastDevice(castInfo(idOf(1), QStringLiteral("  Living room  ")));
        controller.addCastDevice(castInfo(idOf(2), QStringLiteral("   ")));
        controller.addCastDevice(castInfo(idOf(3), QString()));
        QCOMPARE(controller.nameFor(castTarget(idOf(1))), kName);
        QCOMPARE(controller.nameFor(castTarget(idOf(2))), QStringLiteral("127.0.0.1"));
        QCOMPARE(controller.nameFor(castTarget(idOf(3))), QStringLiteral("127.0.0.1"));
        QVERIFY(controller.nameFor(castTarget(idOf(9))).isEmpty());
        // The row of a device with no name shows its address
        const int row = controller.targets()->indexOf(castTarget(idOf(3)));
        QVERIFY(row >= 0);
        QCOMPARE(roleOf(controller, row, CastTargetsModel::NameRole), QStringLiteral("127.0.0.1"));
    }

    void loadReloadStopOnACastDevice()
    {
        CastRig rig(*m_pki);
        QVERIFY(rig.listening);
        CastController controller(SsdpSearch::Options{});
        controller.addCastDevice(rig.info(), rig.options(*m_pki));
        const QString id = castTarget(kDeviceId);
        QSignalSpy playingSpy(&controller, &CastController::playing);
        QSignalSpy stoppedSpy(&controller, &CastController::stopped);
        QSignalSpy endedSpy(&controller, &CastController::ended);
        QSignalSpy failedSpy(&controller, &CastController::failed);

        QVERIFY(!controller.active());
        QVERIFY(controller.load(id, transcodeRequest()));
        QVERIFY(controller.active());
        QCOMPARE(controller.activeUdn(), id);
        QCOMPARE(controller.activeName(), QStringLiteral("Test device"));
        QTRY_VERIFY(controller.isPlaying());
        QVERIFY(!playingSpy.isEmpty());
        QVERIFY(controller.canPause());
        QVERIFY(controller.canSeek());
        QCOMPARE(controller.durationMs().value_or(0), qint64(600000));
        QCOMPARE(rig.device.count(kMediaNs, kLoad), 1);

        // A transcode seeks by loading again: the second LOAD carries the text track of the new start
        const qsizetype playsBefore = playingSpy.size();
        const QUrl second(kBase + QStringLiteral("/transcode.mp4?DeviceId=abc&StartTimeTicks=1"));
        const QUrl track(kBase + QStringLiteral("/Videos/1/2/Subtitles/3/300000/Stream.vtt"));
        QVERIFY(controller.reload(second, 300000, track));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 2);
        const QJsonObject media = rig.device.lastLoad().value(QLatin1String("media")).toObject();
        QCOMPARE(media.value(QLatin1String("contentId")).toString(),
                 second.toString(QUrl::FullyEncoded));
        const QJsonObject newTrack =
            media.value(QLatin1String("tracks")).toArray().at(0).toObject();
        QCOMPARE(newTrack.value(QLatin1String("trackContentId")).toString(),
                 track.toString(QUrl::FullyEncoded));
        QTRY_VERIFY(controller.isPlaying());
        QTRY_VERIFY(playingSpy.size() > playsBefore);
        QTRY_VERIFY(controller.positionMs().value_or(0) >= 300000);
        QVERIFY(controller.active());
        QCOMPARE(rig.device.connections, 1);

        QVERIFY(controller.stop());
        QTRY_COMPARE(stoppedSpy.size(), 1);
        QVERIFY(!controller.active());
        QVERIFY(controller.activeUdn().isEmpty());
        QVERIFY(!controller.isPlaying());
        QVERIFY(!controller.stop());
        QVERIFY(!controller.pause());
        QVERIFY(controller.hasTarget(id));
        QTest::qWait(150);
        QCOMPARE(stoppedSpy.size(), 1);
        QVERIFY(failedSpy.isEmpty());
        QVERIFY(endedSpy.isEmpty());
        QTRY_COMPARE(rig.device.appStops, 1);
    }

    void aWrongPkiFailsAsUntrusted()
    {
        // The device presents the chain of another PKI than the one the sender trusts
        CastRig rig(*m_otherPki);
        QVERIFY(rig.listening);
        CastController controller(SsdpSearch::Options{});
        controller.addCastDevice(rig.info(), rig.options(*m_pki));
        QSignalSpy stoppedSpy(&controller, &CastController::stopped);
        QSignalSpy failedSpy(&controller, &CastController::failed);

        QVERIFY(controller.load(castTarget(kDeviceId), transcodeRequest()));
        QVERIFY(controller.active());
        QTRY_COMPARE(failedSpy.size(), 1);
        QCOMPARE(qvariant_cast<Failure>(failedSpy.constFirst().at(0)), Failure::Untrusted);
        QVERIFY(!controller.active());
        QVERIFY(!controller.isPlaying());
        QVERIFY(controller.hasTarget(castTarget(kDeviceId)));
        QTest::qWait(100);
        QCOMPARE(failedSpy.size(), 1);
        QVERIFY(stoppedSpy.isEmpty());
    }

    void loadOfAnUnknownIdFails()
    {
        CastController controller(SsdpSearch::Options{});
        const QString unknown = castTarget(kDeviceId);
        QVERIFY(!controller.load(unknown, transcodeRequest()));
        QVERIFY(!controller.load(unknown, QUrl(kBase + QStringLiteral("/a.mp4")),
                                 QStringLiteral("video/mp4"), QStringLiteral("A title"), 0, 1000,
                                 QUrl(kBase), true));
        QVERIFY(!controller.active());
        QVERIFY(controller.activeUdn().isEmpty());
        QVERIFY(!controller.pause());
        QVERIFY(!controller.resume());
        QVERIFY(!controller.seek(1000));
        QVERIFY(!controller.stop());
        QVERIFY(!controller.reload(QUrl(kBase + QStringLiteral("/b.mp4")), 0));
    }

    void aLoadTheDeviceRefusesActivatesNothing()
    {
        CastController controller(SsdpSearch::Options{});
        controller.addCastDevice(castInfo(kDeviceId, kName));
        CastLoad request = transcodeRequest();
        request.url = QUrl(QStringLiteral("http://192.0.2.1/x.mp4"));
        QVERIFY(!controller.load(castTarget(kDeviceId), request));
        QVERIFY(!controller.active());
        QVERIFY(controller.activeName().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCastController)
#include "test_cast_controller.moc"
