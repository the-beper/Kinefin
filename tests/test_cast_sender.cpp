// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FakeCastDevice.h"

#include "cast/CastSender.h"

#include <QElapsedTimer>
#include <QMetaEnum>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QtTest/QtTest>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

using Kinefin::CastLoad;
using Kinefin::CastSender;
using Failure = Kinefin::CastRenderer::Failure;
using Transport = Kinefin::CastRenderer::Transport;

namespace {
const QString kReceiver = QString(Kinefin::kCastPlatformId);
const QString kTransport = QStringLiteral("transport-1");
const QString kConnectionNs = QString(Kinefin::kCastNsConnection);
const QString kReceiverNs = QString(Kinefin::kCastNsReceiver);
const QString kMediaNs = QString(Kinefin::kCastNsMedia);
const QString kToken = QStringLiteral("SECRET-TOKEN-0123456789");
const QString kLoad = QStringLiteral("LOAD");
const QString kStop = QStringLiteral("STOP");
const QString kClose = QStringLiteral("CLOSE");
const QString kGetStatus = QStringLiteral("GET_STATUS");

using DeviceFound = std::function<void(const std::optional<Kinefin::CastDeviceInfo> &)>;

// A media server on loopback that answers every GET with `status` (a 206 with one byte for the range probe) and keeps the heads it was sent
class FakeMedia : public QObject {
  public:
    explicit FakeMedia(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    ++disconnects;
                    socket->deleteLater();
                });
            }
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_server.serverPort(); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(port()); }

    bool credentialFree() const
    {
        for (const QByteArray &head : heads) {
            const QByteArray lower = head.toLower();
            if (lower.contains("authorization") || lower.contains("cookie")
                || lower.contains(kToken.toLower().toUtf8()))
                return false;
        }
        return true;
    }

    int status = 206;
    // The request is read and never answered
    bool hold = false;
    // The requests for a .vtt path are read and answered only by releaseTracks()
    bool holdTracks = false;
    int requests = 0;
    // The connections that ended, whoever ended them
    int disconnects = 0;
    QList<QByteArray> heads;

  private:
    void read(QTcpSocket *socket)
    {
        QByteArray buffer = socket->property("buffer").toByteArray();
        buffer.append(socket->readAll());
        socket->setProperty("buffer", buffer);
        const qsizetype end = buffer.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        socket->setProperty("buffer", QByteArray());
        ++requests;
        const QByteArray received = buffer.left(end);
        heads.append(received);
        if (hold)
            return;
        if (holdTracks && received.left(received.indexOf("\r\n")).contains(".vtt")) {
            heldTracks.append(socket);
            return;
        }
        answer(socket);
    }

  public:
    // Answers the track requests held so far, as the server does once it extracted the track
    void releaseTracks()
    {
        holdTracks = false;
        for (const QPointer<QTcpSocket> &socket : std::exchange(heldTracks, {})) {
            if (socket)
                answer(socket);
        }
    }

  private:
    QList<QPointer<QTcpSocket>> heldTracks;

    void answer(QTcpSocket *socket)
    {
        QByteArray head = "HTTP/1.1 ";
        head.append(QByteArray::number(status));
        head.append(" Probe\r\n");
        if (status == 206)
            head.append("Content-Range: bytes 0-0/1000\r\n");
        head.append("Content-Length: 1\r\nConnection: close\r\n\r\nx");
        socket->write(head);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
};

struct Spies {
    explicit Spies(CastSender *sender)
        : states(sender, &Kinefin::CastRenderer::stateChanged),
          positions(sender, &Kinefin::CastRenderer::positionChanged),
          volumes(sender, &Kinefin::CastRenderer::volumeChanged),
          ended(sender, &Kinefin::CastRenderer::ended),
          stopped(sender, &Kinefin::CastRenderer::stopped),
          failed(sender, &Kinefin::CastRenderer::failed)
    {
    }

    QSignalSpy states;
    QSignalSpy positions;
    QSignalSpy volumes;
    QSignalSpy ended;
    QSignalSpy stopped;
    QSignalSpy failed;
};

// A fake device, a media server and a sender between them. The device and the server listen when the rig is built; the sender is started by the test once it has set what it needs on `device`, `media` and the options
struct Rig {
    explicit Rig(const CastTestPki &pki) : device(pki)
    {
        device.receiverEnabled = true;
        listening = device.listen() && media.listen();
    }

    Kinefin::CastDeviceInfo info() const
    {
        Kinefin::CastDeviceInfo value;
        value.id = QStringLiteral("0123456789abcdef0123456789abcdef");
        value.name = QStringLiteral("Test device");
        value.address = QHostAddress::LocalHost;
        value.port = device.port();
        return value;
    }

    // Short times and the resolvers answered from the rig: the device is found again at its own address, a host name resolves to `hostAnswer` (nothing by default)
    CastSender::Options options(const CastTestPki &trust)
    {
        CastSender::Options value;
        value.channel.trust = trust.trust();
        value.launchTimeoutMs = 3000;
        value.startTimeoutMs = 5000;
        value.stuckMs = 2000;
        value.statusPollMs = 60'000;
        value.positionTickMs = 50;
        value.reconnectAttempts = 3;
        value.reconnectDelayMs = 50;
        value.stopTimeoutMs = 1500;
        value.resolveDevice =
            [this](const QString &id,
                   std::function<void(const std::optional<Kinefin::CastDeviceInfo> &)> done) {
                ++deviceLookups;
                lastDeviceId = id;
                if (deviceLookupFails) {
                    done(std::nullopt);
                    return;
                }
                done(info());
            };
        value.resolveHost = [this](const QString &host,
                                   std::function<void(const QHostAddress &)> done) {
            ++hostLookups;
            lastHost = host;
            done(hostAnswer);
        };
        return value;
    }

    void start(const CastSender::Options &options)
    {
        sender = std::make_unique<CastSender>(info(), options);
        spy = std::make_unique<Spies>(sender.get());
    }

    QString urlFor(const QString &path) const
    {
        return QStringLiteral("%1%2").arg(media.base(), path);
    }

    QString namedBase() const { return QStringLiteral("http://media.test:%1").arg(media.port()); }

    CastLoad request(bool direct = true) const
    {
        CastLoad value;
        value.url = QUrl(urlFor(direct ? QStringLiteral("/video.mp4")
                                       : QStringLiteral("/transcode.mp4?DeviceId=abc")));
        value.mime = QStringLiteral("video/mp4");
        value.title = QStringLiteral("A title");
        value.serverBase = QUrl(media.base());
        value.direct = direct;
        value.durationMs = 120000;
        return value;
    }

    FakeCastDevice device;
    FakeMedia media;
    bool listening = false;
    int deviceLookups = 0;
    QString lastDeviceId;
    bool deviceLookupFails = false;
    int hostLookups = 0;
    QString lastHost;
    QHostAddress hostAnswer;
    std::unique_ptr<CastSender> sender;
    std::unique_ptr<Spies> spy;
};

bool playing(Rig &rig, const CastLoad &request)
{
    if (!rig.sender->load(request))
        return false;
    return QTest::qWaitFor([&rig] { return rig.sender->transport() == Transport::Playing; });
}

QString describe(const FakeCastDevice::Received &entry)
{
    const Kinefin::CastMessage &message = entry.message;
    const QString ns = message.ns.section(QLatin1Char('.'), -1);
    const QString type =
        message.binary ? QStringLiteral("CHALLENGE") : FakeCastDevice::typeOf(message);
    return QStringLiteral("%1:%2:%3").arg(ns, type, message.destinationId);
}

// The frames of one connection in the order they arrived, the heartbeat left out, as `namespace:TYPE:destination`
QStringList sequence(const FakeCastDevice &device, int connection = 1)
{
    QStringList result;
    const QList<FakeCastDevice::Received> frames = device.onConnection(connection);
    for (const FakeCastDevice::Received &entry : frames) {
        if (entry.message.ns != Kinefin::kCastNsHeartbeat)
            result.append(describe(entry));
    }
    return result;
}

QList<Transport> statesOf(const QSignalSpy &spy)
{
    QList<Transport> states;
    for (const QList<QVariant> &arguments : spy)
        states.append(qvariant_cast<Transport>(arguments.at(0)));
    return states;
}

Failure failureOf(const QList<QVariant> &arguments)
{
    return qvariant_cast<Failure>(arguments.at(0));
}

QJsonObject withoutRequestId(QJsonObject object)
{
    object.remove(QStringLiteral("requestId"));
    return object;
}

QByteArray compact(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject firstFrame(const FakeCastDevice &device, const QString &ns, const QString &type)
{
    const QList<FakeCastDevice::Received> frames = device.frames(ns, type);
    return frames.isEmpty() ? QJsonObject() : FakeCastDevice::jsonOf(frames.constFirst().message);
}

QJsonObject lastFrame(const FakeCastDevice &device, const QString &ns, const QString &type)
{
    const QList<FakeCastDevice::Received> frames = device.frames(ns, type);
    return frames.isEmpty() ? QJsonObject() : FakeCastDevice::jsonOf(frames.constLast().message);
}

quint32 requestIdOf(const QJsonObject &object)
{
    return static_cast<quint32>(object.value(QLatin1String("requestId")).toDouble());
}

bool hasTrack(const QJsonObject &load)
{
    const QJsonObject media = load.value(QLatin1String("media")).toObject();
    return load.contains(QLatin1String("activeTrackIds"))
           || media.contains(QLatin1String("tracks"));
}

QString contentIdOf(const QJsonObject &load)
{
    return load.value(QLatin1String("media"))
        .toObject()
        .value(QLatin1String("contentId"))
        .toString();
}

bool hasPoster(const QJsonObject &load)
{
    return load.value(QLatin1String("media"))
        .toObject()
        .value(QLatin1String("metadata"))
        .toObject()
        .contains(QLatin1String("images"));
}

// A MEDIA_STATUS of the first media session whose only entry has `entryFields` (a JSON fragment); %SID% and %REQ% are filled in by the test
QByteArray mediaStatusJson(const char *entryFields, int requestId = 0)
{
    QByteArray text = "{\"type\":\"MEDIA_STATUS\",\"requestId\":";
    text.append(QByteArray::number(requestId));
    text.append(",\"status\":[{\"mediaSessionId\":%SID%,");
    text.append(entryFields);
    text.append("}]}");
    return text;
}

QByteArray receiverStatusJson(const char *applications)
{
    QByteArray text =
        "{\"type\":\"RECEIVER_STATUS\",\"requestId\":0,\"status\":{\"applications\":[";
    text.append(applications);
    text.append("]}}");
    return text;
}
} // namespace

class TestCastSender : public QObject {
    Q_OBJECT

  private:
    // The device the rig starts with answers like a Default Media Receiver; this runs one probe whose server answers `status`
    void expectProbeFailure(int status, Failure expected)
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.media.status = status;
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), expected);
        // Nothing reached the device and the probe carried no credential
        QCOMPARE(rig.device.connections, 0);
        QCOMPARE(rig.media.requests, 1);
        QVERIFY(rig.media.credentialFree());
        QVERIFY(rig.spy->states.isEmpty());
        QTest::qWait(100);
        QCOMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(rig.device.connections, 0);
    }

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

    void identity()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        const QString expected = QStringLiteral("cast:0123456789abcdef0123456789abcdef");
        QCOMPARE(CastSender::targetId(rig.info()), expected);
        QCOMPARE(rig.sender->id(), expected);
        QVERIFY(rig.sender->canPause());
        QVERIFY(rig.sender->canSeek());
        QVERIFY(!rig.sender->hasVolume());
        QCOMPARE(rig.sender->volume(), -1);
        QVERIFY(!rig.sender->transport().has_value());
    }

    void launchesConnectsLoads()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request();
        request.subtitle = QStringLiteral("S1 E2");
        request.imageUrl = QUrl(rig.urlFor(QStringLiteral("/Items/1/Images/Primary?maxWidth=360")));
        request.track.url =
            QUrl(rig.urlFor(QStringLiteral("/Videos/1/2/Subtitles/3/0/Stream.vtt")));
        request.track.language = QStringLiteral("eng");
        request.track.name = QStringLiteral("English");
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);

        // The challenge first, each destination connected before it is spoken to
        QCOMPARE(sequence(rig.device),
                 (QStringList{QStringLiteral("deviceauth:CHALLENGE:receiver-0"),
                              QStringLiteral("connection:CONNECT:receiver-0"),
                              QStringLiteral("receiver:LAUNCH:receiver-0"),
                              QStringLiteral("connection:CONNECT:transport-1"),
                              QStringLiteral("media:LOAD:transport-1")}));
        QCOMPARE(rig.device.unconnectedFrames, 0);
        QCOMPARE(rig.device.connections, 1);

        const QJsonObject launch = firstFrame(rig.device, kReceiverNs, QStringLiteral("LAUNCH"));
        QVERIFY(requestIdOf(launch) != 0);
        QCOMPARE(compact(withoutRequestId(launch)),
                 compact(QJsonObject{{QStringLiteral("type"), QStringLiteral("LAUNCH")},
                                     {QStringLiteral("appId"), QStringLiteral("CC1AD845")}}));

        const QJsonObject load = rig.device.lastLoad();
        QVERIFY(requestIdOf(load) > requestIdOf(launch));
        QCOMPARE(load.value(QLatin1String("sessionId")).toString(), QStringLiteral("session-1"));
        QVERIFY(load.value(QLatin1String("autoplay")).toBool());
        QCOMPARE(load.value(QLatin1String("currentTime")).toDouble(), 0.0);
        const QJsonArray active = load.value(QLatin1String("activeTrackIds")).toArray();
        QCOMPARE(active.size(), 1);
        QCOMPARE(active.at(0).toInt(), 1);
        const QJsonObject media = load.value(QLatin1String("media")).toObject();
        QCOMPARE(media.value(QLatin1String("contentId")).toString(),
                 request.url.toString(QUrl::FullyEncoded));
        QCOMPARE(media.value(QLatin1String("contentType")).toString(), QStringLiteral("video/mp4"));
        QCOMPARE(media.value(QLatin1String("streamType")).toString(), QStringLiteral("BUFFERED"));
        QCOMPARE(media.value(QLatin1String("duration")).toDouble(), 120.0);
        const QJsonObject metadata = media.value(QLatin1String("metadata")).toObject();
        QCOMPARE(metadata.value(QLatin1String("metadataType")).toInt(), 0);
        QCOMPARE(metadata.value(QLatin1String("title")).toString(), QStringLiteral("A title"));
        QCOMPARE(metadata.value(QLatin1String("subtitle")).toString(), QStringLiteral("S1 E2"));
        const QJsonArray images = metadata.value(QLatin1String("images")).toArray();
        QCOMPARE(images.size(), 1);
        QCOMPARE(images.at(0).toObject().value(QLatin1String("url")).toString(),
                 request.imageUrl.toString(QUrl::FullyEncoded));
        const QJsonArray tracks = media.value(QLatin1String("tracks")).toArray();
        QCOMPARE(tracks.size(), 1);
        const QJsonObject track = tracks.at(0).toObject();
        QCOMPARE(track.value(QLatin1String("trackId")).toInt(), 1);
        QCOMPARE(track.value(QLatin1String("type")).toString(), QStringLiteral("TEXT"));
        QCOMPARE(track.value(QLatin1String("trackContentId")).toString(),
                 request.track.url.toString(QUrl::FullyEncoded));
        QCOMPARE(track.value(QLatin1String("trackContentType")).toString(),
                 QStringLiteral("text/vtt"));
        QCOMPARE(track.value(QLatin1String("subtype")).toString(), QStringLiteral("SUBTITLES"));
        QCOMPARE(track.value(QLatin1String("language")).toString(), QStringLiteral("eng"));
        QCOMPARE(track.value(QLatin1String("name")).toString(), QStringLiteral("English"));

        QCOMPARE(statesOf(rig.spy->states),
                 (QList<Transport>{Transport::Transitioning, Transport::Playing}));
        QVERIFY(rig.spy->failed.isEmpty());
        QVERIFY(rig.spy->stopped.isEmpty());
    }

    void loadWithoutTheOptionalFields()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request();
        request.durationMs = 0;
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        const QJsonObject load = rig.device.lastLoad();
        QVERIFY(!load.contains(QLatin1String("activeTrackIds")));
        const QJsonObject media = load.value(QLatin1String("media")).toObject();
        QVERIFY(!media.contains(QLatin1String("duration")));
        QVERIFY(!media.contains(QLatin1String("tracks")));
        const QJsonObject metadata = media.value(QLatin1String("metadata")).toObject();
        QVERIFY(!metadata.contains(QLatin1String("subtitle")));
        QVERIFY(!metadata.contains(QLatin1String("images")));
    }

    void trackWithoutALanguageGetsUnd()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request();
        request.track.url = QUrl(rig.urlFor(QStringLiteral("/sub.vtt")));
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        const QJsonArray tracks = rig.device.lastLoad()
                                      .value(QLatin1String("media"))
                                      .toObject()
                                      .value(QLatin1String("tracks"))
                                      .toArray();
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.at(0).toObject().value(QLatin1String("language")).toString(),
                 QStringLiteral("und"));
    }

    void directLoadSendsTheStartAsCurrentTime()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request();
        request.startMs = 12500;
        request.durationMs = 600000;
        QVERIFY(rig.sender->load(request));
        // Before anything plays the position is the start
        QCOMPARE(rig.sender->positionMs(), qint64(12500));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QCOMPARE(rig.device.lastLoad().value(QLatin1String("currentTime")).toDouble(), 12.5);
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
        // The first position of the device is the start plus a little
        QVERIFY(rig.sender->positionMs() >= 12800);
        QVERIFY(rig.sender->positionMs() < 12800 + 3000);
    }

    void transcodeLoadStartsAtZero()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        request.startMs = 12500;
        request.durationMs = 600000;
        QVERIFY(rig.sender->load(request));
        QCOMPARE(rig.sender->positionMs(), qint64(12500));
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
        QCOMPARE(rig.device.lastLoad().value(QLatin1String("currentTime")).toDouble(), 0.0);
        // The time of the device counts from the start in the URL
        QVERIFY(rig.sender->positionMs() >= 12800);
        QVERIFY(rig.sender->positionMs() < 12800 + 3000);
        QCOMPARE(rig.media.requests, 0);
    }

    void startNeedsAMovingPosition()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.movingDelayMs = 400;
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.sender->transport(), Transport::Transitioning);
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        // The reply PLAYING at 0 came long before the position moves, and it is not a start
        QTest::qWait(200);
        QCOMPARE(rig.sender->transport(), Transport::Transitioning);
        QVERIFY(!statesOf(rig.spy->states).contains(Transport::Playing));
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
        QCOMPARE(statesOf(rig.spy->states),
                 (QList<Transport>{Transport::Transitioning, Transport::Playing}));
    }

    void stuckAtZeroFails()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.movesAfterLoad = false;
        CastSender::Options options = rig.options(*m_pki);
        options.stuckMs = 300;
        rig.start(options);
        QElapsedTimer clock;
        clock.start();
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::Refused);
        QVERIFY(clock.elapsed() >= 250);
        QVERIFY(!statesOf(rig.spy->states).contains(Transport::Playing));
        // The channel is closed with a CLOSE on both virtual connections
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        QTest::qWait(100);
        QCOMPARE(rig.spy->failed.size(), 1);
    }

    void bufferingAfterTheReplyHoldsTheStuckTimer()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        // A transcode whose first segment takes three times the stuck time
        rig.device.movingDelayMs = 1800;
        CastSender::Options options = rig.options(*m_pki);
        options.stuckMs = 600;
        rig.start(options);
        QVERIFY(rig.sender->load(rig.request(false)));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        // The reply PLAYING at 0 is already on the wire: the device is buffering after it
        rig.device.pushMediaEntry(
            rig.device.mediaEntry(rig.device.mediaSessionId, QStringLiteral("BUFFERING")));
        QTest::qWait(1200);
        QVERIFY(rig.spy->failed.isEmpty());
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
        QTest::qWait(100);
        QVERIFY(rig.spy->failed.isEmpty());
    }

    void playingAtZeroAgainArmsTheStuckTimerAgain()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.movesAfterLoad = false;
        CastSender::Options options = rig.options(*m_pki);
        options.stuckMs = 600;
        rig.start(options);
        QVERIFY(rig.sender->load(rig.request(false)));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        const qint64 session = rig.device.mediaSessionId;
        rig.device.pushMediaEntry(rig.device.mediaEntry(session, QStringLiteral("BUFFERING")));
        QTest::qWait(1200);
        QVERIFY(rig.spy->failed.isEmpty());

        // A PLAYING that is still at the start is a stream that does not move
        rig.device.pushMediaEntry(rig.device.mediaEntry(session, QStringLiteral("PLAYING")));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::Refused);
        QVERIFY(!statesOf(rig.spy->states).contains(Transport::Playing));
    }

    void aDirectLoadTakesItsTextTrackAndMoreTime()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        // The server never answers for the track and the device never answers the LOAD
        rig.media.holdTracks = true;
        rig.device.answerLoad = false;
        CastSender::Options options = rig.options(*m_pki);
        options.startTimeoutMs = 300;
        rig.start(options);
        CastLoad request = rig.request();
        request.track.url =
            QUrl(rig.urlFor(QStringLiteral("/Videos/1/2/Subtitles/3/0/Stream.vtt")));
        QVERIFY(rig.sender->load(request));
        // The server is asked for the track at once, with no credential, beside the range probe
        const QByteArray path("/Videos/1/2/Subtitles/3/0/Stream.vtt");
        QTRY_VERIFY(std::any_of(
            rig.media.heads.cbegin(), rig.media.heads.cend(), [&](const QByteArray &head) {
                return head.startsWith(QByteArray("GET ").append(path).append(" "));
            }));
        QVERIFY(rig.media.credentialFree());
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        // A direct stream can be fetched again, so the device may wait for the track: it is in the LOAD
        QVERIFY(rig.device.lastLoad()
                    .value(QLatin1String("media"))
                    .toObject()
                    .contains(QLatin1String("tracks")));
        // Without a track these options end the load after 300 ms
        QTest::qWait(900);
        QVERIFY(rig.spy->failed.isEmpty());

        QVERIFY(rig.sender->stop());
        QTRY_COMPARE(rig.spy->stopped.size(), 1);
        QVERIFY(rig.spy->failed.isEmpty());
    }

    void aTranscodeTakesItsTextTrackOnlyOnceTheServerAnsweredIt()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        // The server extracts the track for a long time
        rig.media.holdTracks = true;
        rig.start(rig.options(*m_pki));
        QSignalSpy ready(rig.sender.get(), &Kinefin::CastRenderer::textTrackReady);
        CastLoad request = rig.request(false);
        request.track.url =
            QUrl(rig.urlFor(QStringLiteral("/Videos/1/2/Subtitles/3/0/Stream.vtt")));
        QVERIFY(playing(rig, request));
        QCOMPARE(rig.device.count(kMediaNs, kLoad), 1);
        const QJsonObject first = rig.device.lastLoad();
        QVERIFY(!first.value(QLatin1String("media")).toObject().contains(QLatin1String("tracks")));
        QVERIFY(!first.contains(QLatin1String("activeTrackIds")));
        QVERIFY(ready.isEmpty());

        // Answered: the sender asks for a reload, whose LOAD carries the track
        rig.media.releaseTracks();
        QTRY_COMPARE(ready.size(), 1);
        const QUrl track(
            rig.urlFor(QStringLiteral("/Videos/1/2/Subtitles/3/400000000/Stream.vtt")));
        QVERIFY(rig.sender->reload(QUrl(rig.urlFor(QStringLiteral(
                                       "/transcode.mp4?DeviceId=abc&StartTimeTicks=400000000"))),
                                   40000, track));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 2);
        const QJsonArray tracks = rig.device.lastLoad()
                                      .value(QLatin1String("media"))
                                      .toObject()
                                      .value(QLatin1String("tracks"))
                                      .toArray();
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.at(0).toObject().value(QLatin1String("trackContentId")).toString(),
                 track.toString(QUrl::FullyEncoded));
        QCOMPARE(ready.size(), 1);
        QVERIFY(rig.spy->failed.isEmpty());
    }

    void aTranscodeLeavesOutATrackTheServerRefuses()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        // A transcode has no range probe: the one request to the server is the track's
        rig.media.status = 404;
        rig.start(rig.options(*m_pki));
        QSignalSpy ready(rig.sender.get(), &Kinefin::CastRenderer::textTrackReady);
        CastLoad request = rig.request(false);
        request.track.url = QUrl(rig.urlFor(QStringLiteral("/sub.vtt")));
        QVERIFY(playing(rig, request));
        QTRY_COMPARE(rig.media.requests, 1);
        QTest::qWait(200);
        QVERIFY(!rig.device.lastLoad()
                     .value(QLatin1String("media"))
                     .toObject()
                     .contains(QLatin1String("tracks")));
        QVERIFY(ready.isEmpty());
        // A reload leaves it out too
        QVERIFY(rig.sender->reload(
            QUrl(rig.urlFor(QStringLiteral("/transcode.mp4?DeviceId=abc&StartTimeTicks=1"))), 0,
            request.track.url));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 2);
        QVERIFY(!rig.device.lastLoad()
                     .value(QLatin1String("media"))
                     .toObject()
                     .contains(QLatin1String("tracks")));
        QVERIFY(ready.isEmpty());
        QVERIFY(rig.spy->failed.isEmpty());
    }

    void aTranscodeTakesATrackTheServerAnsweredBeforeTheLoad()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QSignalSpy ready(rig.sender.get(), &Kinefin::CastRenderer::textTrackReady);
        CastLoad request = rig.request(false);
        request.track.url = QUrl(rig.urlFor(QStringLiteral("/sub.vtt")));
        // The track is answered while the channel opens and the app starts
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QVERIFY(rig.device.lastLoad()
                    .value(QLatin1String("media"))
                    .toObject()
                    .contains(QLatin1String("tracks")));
        QTest::qWait(100);
        QVERIFY(ready.isEmpty());
    }

    void aFailureCutsTheTrackRequest()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.media.holdTracks = true;
        rig.device.answerLoad = false;
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        request.track.url = QUrl(rig.urlFor(QStringLiteral("/sub.vtt")));
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.media.requests, 1);
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        const qint64 loadId = requestIdOf(rig.device.lastLoad());
        rig.device.sendStatus(QJsonObject{{QStringLiteral("type"), QStringLiteral("LOAD_FAILED")},
                                          {QStringLiteral("requestId"), loadId}});
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::Refused);
        QTRY_COMPARE(rig.media.disconnects, 1);
    }

    void aLoadWithoutATrackAsksTheServerForNothing()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request(false)));
        QTest::qWait(150);
        QCOMPARE(rig.media.requests, 0);
    }

    void aStuckLoadStopsTheAppOnTheDevice()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.movesAfterLoad = false;
        CastSender::Options options = rig.options(*m_pki);
        options.stuckMs = 300;
        rig.start(options);
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::Refused);

        // The media STOP, the STOP of our app and the goodbye reach the device in that order, though the sender waited for no answer
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        const QStringList all = sequence(rig.device);
        QCOMPARE(all.mid(all.size() - 4),
                 (QStringList{QStringLiteral("media:STOP:transport-1"),
                              QStringLiteral("receiver:STOP:receiver-0"),
                              QStringLiteral("connection:CLOSE:receiver-0"),
                              QStringLiteral("connection:CLOSE:transport-1")}));
        const qint64 session = rig.device.mediaSessionId;
        QCOMPARE(compact(withoutRequestId(lastFrame(rig.device, kMediaNs, kStop))),
                 compact(QJsonObject{{QStringLiteral("type"), kStop},
                                     {QStringLiteral("mediaSessionId"), session}}));
        QCOMPARE(compact(withoutRequestId(lastFrame(rig.device, kReceiverNs, kStop))),
                 compact(QJsonObject{{QStringLiteral("type"), kStop},
                                     {QStringLiteral("sessionId"), QStringLiteral("session-1")}}));
        QCOMPARE(rig.device.appStops, 1);
        QTest::qWait(100);
        QCOMPARE(rig.spy->failed.size(), 1);
        QVERIFY(rig.spy->stopped.isEmpty());
    }

    void aRefusedLoadWithoutAMediaSessionStopsOnlyTheApp()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.answerLoad = false;
        CastSender::Options options = rig.options(*m_pki);
        options.startTimeoutMs = 300;
        rig.start(options);
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::Refused);
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        QCOMPARE(rig.device.appStops, 1);
        QCOMPARE(rig.device.count(kMediaNs, kStop), 0);
    }

    void aRefusedLoadLeavesTheAppWhenTheSessionIsNotOurs()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.movesAfterLoad = false;
        CastSender::Options options = rig.options(*m_pki);
        options.stuckMs = 1000;
        rig.start(options);
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        // The last status lists only the ambient app: its STOP could end another sender's session
        rig.device.sendStatus(
            QJsonDocument::fromJson(
                receiverStatusJson(
                    "{\"appId\":\"E8C28D3C\",\"sessionId\":\"a\",\"transportId\":\"b\"}"))
                .object(),
            kReceiverNs, kReceiver);
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::Refused);
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        QCOMPARE(rig.device.count(kMediaNs, kStop), 1);
        QCOMPARE(rig.device.count(kReceiverNs, kStop), 0);
    }

    void noPlayingInTimeFails()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.answerLoad = false;
        CastSender::Options options = rig.options(*m_pki);
        options.startTimeoutMs = 300;
        rig.start(options);
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::Refused);
        QCOMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QVERIFY(rig.spy->states.isEmpty());
        QTest::qWait(100);
        QCOMPARE(rig.spy->failed.size(), 1);
    }

    void loadFailedIsRefused()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.loadFails = true;
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::Refused);
    }

    void statusMapping_data()
    {
        QTest::addColumn<QString>("ns");
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<int>("transport");
        QTest::addColumn<int>("stopped");
        QTest::addColumn<int>("ended");
        QTest::addColumn<int>("failed");

        const QString media = QStringLiteral("media");
        const QString receiver = QStringLiteral("receiver");
        const int playingState = int(Transport::Playing);
        const int none = -1;

        QTest::newRow("buffering")
            << media << mediaStatusJson("\"playerState\":\"BUFFERING\",\"currentTime\":5.0")
            << int(Transport::Transitioning) << 0 << 0 << none;
        QTest::newRow("paused") << media
                                << mediaStatusJson("\"playerState\":\"PAUSED\",\"currentTime\":5.0")
                                << int(Transport::Paused) << 0 << 0 << none;
        QTest::newRow("playing again")
            << media << mediaStatusJson("\"playerState\":\"PLAYING\",\"currentTime\":6.0")
            << playingState << 0 << 0 << none;
        QTest::newRow("idle while loading")
            << media
            << mediaStatusJson(
                   "\"playerState\":\"IDLE\",\"extendedStatus\":{\"playerState\":\"LOADING\"}")
            << int(Transport::Transitioning) << 0 << 0 << none;
        QTest::newRow("idle with no reason is ignored")
            << media << mediaStatusJson("\"playerState\":\"IDLE\"") << playingState << 0 << 0
            << none;
        QTest::newRow("finished long before the end")
            << media << mediaStatusJson("\"playerState\":\"IDLE\",\"idleReason\":\"FINISHED\"")
            << none << 1 << 0 << none;
        QTest::newRow("cancelled by someone else")
            << media << mediaStatusJson("\"playerState\":\"IDLE\",\"idleReason\":\"CANCELLED\"")
            << none << 1 << 0 << none;
        QTest::newRow("interrupted with no load of ours")
            << media << mediaStatusJson("\"playerState\":\"IDLE\",\"idleReason\":\"INTERRUPTED\"")
            << none << 1 << 0 << none;
        QTest::newRow("playback error")
            << media << mediaStatusJson("\"playerState\":\"IDLE\",\"idleReason\":\"ERROR\"") << none
            << 0 << 0 << int(Failure::Refused);
        QTest::newRow("load failed")
            << media << QByteArray("{\"type\":\"LOAD_FAILED\",\"requestId\":%REQ%}") << none << 0
            << 0 << int(Failure::Refused);
        QTest::newRow("load failed of another request")
            << media << QByteArray("{\"type\":\"LOAD_FAILED\",\"requestId\":9999}") << playingState
            << 0 << 0 << none;
        QTest::newRow("invalid request for the load")
            << media
            << QByteArray("{\"type\":\"INVALID_REQUEST\",\"requestId\":%REQ%,\"reason\":\"INVALID_"
                          "PARAMS\"}")
            << none << 0 << 0 << int(Failure::Refused);
        QTest::newRow("invalid request of another")
            << media
            << QByteArray("{\"type\":\"INVALID_REQUEST\",\"requestId\":9999,\"reason\":\"INVALID_"
                          "COMMAND\"}")
            << playingState << 0 << 0 << none;
        QTest::newRow("load cancelled of an older request")
            << media << QByteArray("{\"type\":\"LOAD_CANCELLED\",\"requestId\":9999}")
            << playingState << 0 << 0 << none;
        QTest::newRow("another media session")
            << media
            << QByteArray(
                   "{\"type\":\"MEDIA_STATUS\",\"requestId\":0,\"status\":[{\"mediaSessionId\":77,"
                   "\"playerState\":\"PLAYING\",\"currentTime\":1.0,\"media\":"
                   "{\"contentId\":\"http://elsewhere.invalid/other.mp4\"}}]}")
            << none << 1 << 0 << none;
        QTest::newRow("unknown media message")
            << media << QByteArray("{\"type\":\"SOMETHING_ELSE\"}") << playingState << 0 << 0
            << none;
        QTest::newRow("ambient app only is not a takeover")
            << receiver
            << receiverStatusJson(
                   "{\"appId\":\"E8C28D3C\",\"sessionId\":\"a\",\"transportId\":\"b\"}")
            << playingState << 0 << 0 << none;
        QTest::newRow("our app beside the ambient app")
            << receiver
            << receiverStatusJson(
                   "{\"appId\":\"E8C28D3C\",\"sessionId\":\"a\",\"transportId\":\"b\"},"
                   "{\"appId\":\"CC1AD845\",\"sessionId\":\"session-1\","
                   "\"transportId\":\"transport-1\"}")
            << playingState << 0 << 0 << none;
        QTest::newRow("another app")
            << receiver
            << receiverStatusJson(
                   "{\"appId\":\"233637DE\",\"sessionId\":\"x\",\"transportId\":\"y\"}")
            << none << 1 << 0 << none;
        QTest::newRow("no application") << receiver
                                        << QByteArray("{\"type\":\"RECEIVER_STATUS\",\"requestId\":"
                                                      "0,\"status\":{\"volume\":{\"level\":0.5}}}")
                                        << none << 1 << 0 << none;
        QTest::newRow("launch status is ignored")
            << receiver << QByteArray("{\"type\":\"LAUNCH_STATUS\"}") << playingState << 0 << 0
            << none;
        QTest::newRow("transport closed")
            << QStringLiteral("connection") << QByteArray("{\"type\":\"CLOSE\"}") << none << 1 << 0
            << none;
    }

    void statusMapping()
    {
        QFETCH(QString, ns);
        QFETCH(QByteArray, json);
        QFETCH(int, transport);
        QFETCH(int, stopped);
        QFETCH(int, ended);
        QFETCH(int, failed);

        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request();
        request.durationMs = 600000;
        QVERIFY(playing(rig, request));
        rig.spy->states.clear();
        const quint32 loadId = requestIdOf(rig.device.lastLoad());
        json.replace("%REQ%", QByteArray::number(loadId));
        json.replace("%SID%", QByteArray::number(rig.device.mediaSessionId));

        if (ns == QLatin1String("media"))
            rig.device.pushText(kTransport, kMediaNs, json);
        else if (ns == QLatin1String("receiver"))
            rig.device.pushText(kReceiver, kReceiverNs, json);
        else
            rig.device.pushText(kTransport, kConnectionNs, json);

        const bool terminal = stopped > 0 || ended > 0 || failed >= 0;
        if (terminal) {
            QTRY_VERIFY(rig.spy->stopped.size() + rig.spy->ended.size() + rig.spy->failed.size()
                        > 0);
        } else {
            QTest::qWait(200);
        }
        QCOMPARE(rig.spy->stopped.size(), stopped);
        QCOMPARE(rig.spy->ended.size(), ended);
        QCOMPARE(rig.spy->failed.size(), failed >= 0 ? 1 : 0);
        if (failed >= 0)
            QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure(failed));
        if (transport >= 0)
            QCOMPARE(rig.sender->transport(), Transport(transport));
        // The signal of a row that ends the cast is emitted once
        QTest::qWait(100);
        QCOMPARE(rig.spy->stopped.size(), stopped);
        QCOMPARE(rig.spy->ended.size(), ended);
        QCOMPARE(rig.spy->failed.size(), failed >= 0 ? 1 : 0);
    }

    void endedOnIdleFinished()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        // The device is at the start and the duration is under 10 s away: the end
        CastLoad request = rig.request();
        request.durationMs = 5000;
        QVERIFY(playing(rig, request));
        rig.device.pushMediaEntry(rig.device.mediaEntry(
            rig.device.mediaSessionId, QStringLiteral("IDLE"), QStringLiteral("FINISHED")));
        QTRY_COMPARE(rig.spy->ended.size(), 1);
        QCOMPARE(rig.sender->positionMs(), qint64(5000));
        QVERIFY(rig.spy->stopped.isEmpty());
        QVERIFY(rig.spy->failed.isEmpty());
        // The device is let go with a CLOSE and the app is left as it is
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        QCOMPARE(rig.device.count(kReceiverNs, kStop), 0);
        QTest::qWait(100);
        QCOMPARE(rig.spy->ended.size(), 1);

        // The next item of a queue loads on a new channel
        QVERIFY(playing(rig, rig.request()));
        QCOMPARE(rig.device.connections, 2);
    }

    void earlyIdleFinishedIsAStop()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        // A truncated file: the device says FINISHED two minutes before the end of the item
        QVERIFY(playing(rig, rig.request()));
        rig.device.pushMediaEntry(rig.device.mediaEntry(
            rig.device.mediaSessionId, QStringLiteral("IDLE"), QStringLiteral("FINISHED")));
        QTRY_COMPARE(rig.spy->stopped.size(), 1);
        QVERIFY(rig.spy->ended.isEmpty());
        QVERIFY(rig.spy->failed.isEmpty());
        // The position stays where the device was
        QVERIFY(rig.sender->positionMs() < 10000);
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        QCOMPARE(rig.device.count(kMediaNs, kStop), 0);
        QCOMPARE(rig.device.count(kReceiverNs, kStop), 0);
        QTest::qWait(100);
        QCOMPARE(rig.spy->stopped.size(), 1);
        QVERIFY(rig.spy->ended.isEmpty());
    }

    void finishedBeforeAnythingPlayedIsRefused()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.movesAfterLoad = false;
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        // The answers to the LOAD are on the wire before this: the stream never moved
        rig.device.pushMediaEntry(rig.device.mediaEntry(
            rig.device.mediaSessionId, QStringLiteral("IDLE"), QStringLiteral("FINISHED")));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::Refused);
        QVERIFY(rig.spy->ended.isEmpty());
        QVERIFY(rig.spy->stopped.isEmpty());
        QVERIFY(!statesOf(rig.spy->states).contains(Transport::Playing));
        QTest::qWait(100);
        QCOMPARE(rig.spy->failed.size(), 1);
        QVERIFY(rig.spy->ended.isEmpty());
    }

    void finishedWithAnUnknownDurationEnds()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request();
        request.durationMs = 0;
        QVERIFY(playing(rig, request));
        rig.device.pushMediaEntry(rig.device.mediaEntry(
            rig.device.mediaSessionId, QStringLiteral("IDLE"), QStringLiteral("FINISHED")));
        QTRY_COMPARE(rig.spy->ended.size(), 1);
        QVERIFY(rig.spy->stopped.isEmpty());
        QVERIFY(rig.spy->failed.isEmpty());
        QVERIFY(rig.sender->positionMs() < 10000);
    }

    void anotherSenderTakesOver_data()
    {
        QTest::addColumn<int>("how");
        QTest::newRow("cancelled") << 0;
        QTest::newRow("interrupted") << 1;
        QTest::newRow("interrupted by a foreign session") << 2;
        QTest::newRow("a foreign media session") << 3;
        QTest::newRow("another app") << 4;
        QTest::newRow("no application") << 5;
        QTest::newRow("transport closed") << 6;
    }

    void anotherSenderTakesOver()
    {
        QFETCH(int, how);
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));
        const qint64 session = rig.device.mediaSessionId;
        const QString idle = QStringLiteral("IDLE");
        switch (how) {
        case 0:
            rig.device.pushMediaEntry(
                rig.device.mediaEntry(session, idle, QStringLiteral("CANCELLED")));
            break;
        case 1:
            rig.device.pushMediaEntry(
                rig.device.mediaEntry(session, idle, QStringLiteral("INTERRUPTED"), true));
            break;
        case 2:
            rig.device.pushMediaEntry(
                rig.device.mediaEntry(session + 1, idle, QStringLiteral("INTERRUPTED"), true));
            break;
        case 3:
            rig.device.contentId = QStringLiteral("http://elsewhere.invalid/other.mp4");
            rig.device.pushMediaEntry(rig.device.mediaEntry(session + 1, QStringLiteral("PLAYING"),
                                                            QString(), false, true));
            break;
        case 4:
            rig.device.takeOver();
            break;
        case 5:
            rig.device.appRunning = false;
            rig.device.ambientRunning = false;
            rig.device.pushReceiverStatus();
            break;
        default:
            rig.device.closeTransport();
            break;
        }
        QTRY_COMPARE(rig.spy->stopped.size(), 1);
        QTest::qWait(150);
        QCOMPARE(rig.spy->stopped.size(), 1);
        QVERIFY(rig.spy->failed.isEmpty());
        QVERIFY(rig.spy->ended.isEmpty());
        // It is not our session any more: no STOP of any kind, and the channel is let go
        QCOMPARE(rig.device.count(kMediaNs, kStop), 0);
        QCOMPARE(rig.device.count(kReceiverNs, kStop), 0);
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose, kReceiver), 1);

        // A new load starts over
        QVERIFY(playing(rig, rig.request()));
        QCOMPARE(rig.device.connections, 2);
    }

    void ownReloadIsNoTakeover()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.cancelOldLoadOnReload = true;
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        request.durationMs = 600000;
        request.track.url =
            QUrl(rig.urlFor(QStringLiteral("/Videos/1/2/Subtitles/3/0/Stream.vtt")));
        request.track.language = QStringLiteral("eng");
        request.track.name = QStringLiteral("English");
        QVERIFY(playing(rig, request));
        // A transcode seeks by loading again
        QVERIFY(!rig.sender->seek(30000));

        rig.spy->states.clear();
        const QUrl second(
            rig.urlFor(QStringLiteral("/transcode.mp4?DeviceId=abc&StartTimeTicks=1")));
        const QUrl track(rig.urlFor(QStringLiteral("/Videos/1/2/Subtitles/3/300000/Stream.vtt")));
        QVERIFY(rig.sender->reload(second, 300000, track));
        // The position counts from the new start at once
        QCOMPARE(rig.sender->positionMs(), qint64(300000));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 2);
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
        QVERIFY(rig.sender->positionMs() >= 300300);
        QVERIFY(rig.sender->positionMs() < 300000 + 5000);
        // The interruption of the old session and the cancelled old request were both ignored
        QTest::qWait(150);
        QVERIFY(rig.spy->stopped.isEmpty());
        QVERIFY(rig.spy->failed.isEmpty());
        QVERIFY(rig.spy->ended.isEmpty());
        QCOMPARE(statesOf(rig.spy->states),
                 (QList<Transport>{Transport::Transitioning, Transport::Playing}));

        const QList<FakeCastDevice::Received> loads = rig.device.frames(kMediaNs, kLoad);
        QCOMPARE(loads.size(), 2);
        const QJsonObject first = FakeCastDevice::jsonOf(loads.at(0).message);
        const QJsonObject again = FakeCastDevice::jsonOf(loads.at(1).message);
        QCOMPARE(loads.at(1).message.destinationId, kTransport);
        QVERIFY(requestIdOf(again) > requestIdOf(first));
        QCOMPARE(again.value(QLatin1String("sessionId")).toString(),
                 first.value(QLatin1String("sessionId")).toString());
        QCOMPARE(again.value(QLatin1String("currentTime")).toDouble(), 0.0);
        const QJsonObject media = again.value(QLatin1String("media")).toObject();
        QCOMPARE(media.value(QLatin1String("contentId")).toString(),
                 second.toString(QUrl::FullyEncoded));
        // The text track is the one at the new start, with the language of the first
        const QJsonObject newTrack =
            media.value(QLatin1String("tracks")).toArray().at(0).toObject();
        QCOMPARE(newTrack.value(QLatin1String("trackContentId")).toString(),
                 track.toString(QUrl::FullyEncoded));
        QCOMPARE(newTrack.value(QLatin1String("language")).toString(), QStringLiteral("eng"));
        QCOMPARE(again.value(QLatin1String("activeTrackIds")).toArray().size(), 1);
        // One channel the whole time
        QCOMPARE(rig.device.connections, 1);
    }

    void aSecondReloadReplacesTheFirstWhileLoading()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        request.durationMs = 600000;
        QVERIFY(playing(rig, request));

        // Two seeks in a row: the device never answers the first LOAD, as one that the second replaced
        rig.device.ignoredLoads = 1;
        const QUrl first(rig.urlFor(QStringLiteral("/transcode.mp4?StartTimeTicks=1")));
        const QUrl second(rig.urlFor(QStringLiteral("/transcode.mp4?StartTimeTicks=2")));
        QVERIFY(rig.sender->reload(first, 300000));
        QVERIFY(rig.sender->reload(second, 400000));
        QCOMPARE(rig.sender->positionMs(), qint64(400000));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 3);
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
        // The position counts from the start of the second one
        QVERIFY(rig.sender->positionMs() >= 400300);
        QVERIFY(rig.sender->positionMs() < 400000 + 5000);
        QCOMPARE(rig.device.lastLoad()
                     .value(QLatin1String("media"))
                     .toObject()
                     .value(QLatin1String("contentId"))
                     .toString(),
                 second.toString(QUrl::FullyEncoded));
        // The interruption of the first session is no takeover
        QTest::qWait(150);
        QVERIFY(rig.spy->stopped.isEmpty());
        QVERIFY(rig.spy->failed.isEmpty());
        QVERIFY(rig.spy->ended.isEmpty());
        QCOMPARE(rig.device.connections, 1);
        QVERIFY(rig.sender->pause());
        QTRY_COMPARE(rig.sender->transport(), Transport::Paused);
    }

    void reloadWithoutASubtitleSendsNoTrack()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        request.track.url = QUrl(rig.urlFor(QStringLiteral("/sub.vtt")));
        QVERIFY(playing(rig, request));
        QVERIFY(rig.sender->reload(QUrl(rig.urlFor(QStringLiteral("/transcode.mp4?x=1"))), 60000));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 2);
        const QJsonObject again = rig.device.lastLoad();
        QVERIFY(!again.contains(QLatin1String("activeTrackIds")));
        QVERIFY(!again.value(QLatin1String("media")).toObject().contains(QLatin1String("tracks")));
    }

    void reloadIsRefused()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        const QUrl other(rig.urlFor(QStringLiteral("/transcode.mp4?x=1")));
        // Nothing is loaded yet
        QVERIFY(!rig.sender->reload(other, 1000));

        QVERIFY(playing(rig, rig.request(true)));
        // A direct play seeks with SEEK
        QVERIFY(!rig.sender->reload(other, 1000));
        QVERIFY(rig.sender->stop());
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);

        QVERIFY(playing(rig, rig.request(false)));
        const int loads = rig.device.count(kMediaNs, kLoad);
        QVERIFY(!rig.sender->reload(QUrl(QStringLiteral("http://127.0.0.1:1/x.mp4")), 1000));
        QVERIFY(!rig.sender->reload(
            QUrl(rig.urlFor(QStringLiteral("/x.mp4?ApiKey=%1").arg(kToken))), 1000));
        QVERIFY(!rig.sender->reload(other, -1));
        QVERIFY(!rig.sender->reload(other, 24LL * 3600 * 1000 + 1));
        QVERIFY(!rig.sender->reload(other, 1000, QUrl(QStringLiteral("http://127.0.0.1:1/s.vtt"))));
        QTest::qWait(100);
        QCOMPARE(rig.device.count(kMediaNs, kLoad), loads);
    }

    void controlsNeedAMediaSession()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(!rig.sender->play());
        QVERIFY(!rig.sender->pause());
        QVERIFY(!rig.sender->seek(1000));
        QVERIFY(!rig.sender->stop());
        QVERIFY(!rig.sender->setVolume(10));
        QVERIFY(!rig.sender->setMuted(true));

        // While the app is being launched there is no media session either
        rig.device.answerLaunch = false;
        QVERIFY(rig.sender->load(rig.request(false)));
        QTRY_COMPARE(rig.device.count(kReceiverNs, QStringLiteral("LAUNCH")), 1);
        QVERIFY(!rig.sender->play());
        QVERIFY(!rig.sender->pause());
        QVERIFY(!rig.sender->seek(1000));
        QCOMPARE(rig.device.count(kMediaNs), 0);
    }

    void pauseResumeSeekStop()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));
        const qint64 session = rig.device.mediaSessionId;

        QVERIFY(rig.sender->pause());
        QTRY_COMPARE(rig.device.count(kMediaNs, QStringLiteral("PAUSE")), 1);
        const QList<FakeCastDevice::Received> pauses =
            rig.device.frames(kMediaNs, QStringLiteral("PAUSE"));
        QCOMPARE(pauses.constFirst().message.destinationId, kTransport);
        const QJsonObject pause = FakeCastDevice::jsonOf(pauses.constFirst().message);
        QVERIFY(requestIdOf(pause) != 0);
        QCOMPARE(compact(withoutRequestId(pause)),
                 compact(QJsonObject{{QStringLiteral("type"), QStringLiteral("PAUSE")},
                                     {QStringLiteral("mediaSessionId"), session}}));
        QTRY_COMPARE(rig.sender->transport(), Transport::Paused);
        // A paused position stands still
        const qint64 paused = rig.sender->positionMs();
        QTest::qWait(150);
        QCOMPARE(rig.sender->positionMs(), paused);

        QVERIFY(rig.sender->play());
        QTRY_COMPARE(rig.device.count(kMediaNs, QStringLiteral("PLAY")), 1);
        QCOMPARE(compact(withoutRequestId(lastFrame(rig.device, kMediaNs, QStringLiteral("PLAY")))),
                 compact(QJsonObject{{QStringLiteral("type"), QStringLiteral("PLAY")},
                                     {QStringLiteral("mediaSessionId"), session}}));
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);

        QVERIFY(rig.sender->seek(30000));
        // The position moves at once: the slider does not jump back while the device catches up
        QVERIFY(rig.sender->positionMs() >= 30000);
        QVERIFY(rig.sender->positionMs() < 31500);
        QTRY_COMPARE(rig.device.count(kMediaNs, QStringLiteral("SEEK")), 1);
        QCOMPARE(compact(withoutRequestId(lastFrame(rig.device, kMediaNs, QStringLiteral("SEEK")))),
                 compact(QJsonObject{
                     {QStringLiteral("type"), QStringLiteral("SEEK")},
                     {QStringLiteral("mediaSessionId"), session},
                     {QStringLiteral("currentTime"), 30.0},
                     {QStringLiteral("resumeState"), QStringLiteral("PLAYBACK_START")}}));
        QTest::qWait(150);
        QVERIFY(rig.sender->positionMs() >= 30000);
        QVERIFY(rig.sender->positionMs() < 32000);

        // Past the end it is the end; before the start nothing is sent
        QVERIFY(rig.sender->seek(10'000'000));
        QTRY_COMPARE(rig.device.count(kMediaNs, QStringLiteral("SEEK")), 2);
        QCOMPARE(lastFrame(rig.device, kMediaNs, QStringLiteral("SEEK"))
                     .value(QLatin1String("currentTime"))
                     .toDouble(),
                 120.0);
        QVERIFY(!rig.sender->seek(-1));
        QTest::qWait(50);
        QCOMPARE(rig.device.count(kMediaNs, QStringLiteral("SEEK")), 2);
    }

    void seekRefusedForATranscode()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request(false)));
        QVERIFY(!rig.sender->seek(30000));
        QTest::qWait(50);
        QCOMPARE(rig.device.count(kMediaNs, QStringLiteral("SEEK")), 0);
        // The other controls still work
        QVERIFY(rig.sender->pause());
        QTRY_COMPARE(rig.sender->transport(), Transport::Paused);
    }

    void positionExtrapolates()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));
        const qint64 first = rig.sender->positionMs();
        QTest::qWait(300);
        const qint64 second = rig.sender->positionMs();
        QVERIFY(second - first >= 250);
        QVERIFY(second - first < 2000);

        // The local tick reports a position that only grows, with the item's duration
        QTRY_VERIFY(rig.spy->positions.size() >= 4);
        qint64 previous = -1;
        for (qsizetype i = 0; i < rig.spy->positions.size(); ++i) {
            const qint64 position = rig.spy->positions.at(i).at(0).toLongLong();
            QVERIFY(position >= previous);
            QCOMPARE(rig.spy->positions.at(i).at(1).toLongLong(), qint64(120000));
            previous = position;
        }
        QCOMPARE(rig.sender->durationMs(), qint64(120000));
    }

    void positionIsClampedToTheDuration()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request();
        request.durationMs = 1000;
        QVERIFY(playing(rig, request));
        QTRY_VERIFY(rig.sender->positionMs() >= 1000);
        QTest::qWait(200);
        QCOMPARE(rig.sender->positionMs(), qint64(1000));

        // A runtime that the server names later wins over the first one
        rig.sender->setDurationMs(600000);
        QCOMPARE(rig.sender->durationMs(), qint64(600000));
        QVERIFY(rig.sender->positionMs() > 1000);
    }

    void pollsStatusWhileAMediaSessionExists()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        CastSender::Options options = rig.options(*m_pki);
        options.statusPollMs = 100;
        rig.start(options);
        QCOMPARE(rig.device.count(kMediaNs, kGetStatus), 0);
        QVERIFY(playing(rig, rig.request()));
        QTRY_VERIFY(rig.device.count(kMediaNs, kGetStatus, kTransport) >= 3);
        QCOMPARE(rig.sender->transport(), Transport::Playing);

        QVERIFY(rig.sender->stop());
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        const int polls = rig.device.count(kMediaNs, kGetStatus);
        QTest::qWait(300);
        QCOMPARE(rig.device.count(kMediaNs, kGetStatus), polls);
    }

    void volumeAndMute()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));
        QVERIFY(rig.sender->hasVolume());
        QCOMPARE(rig.sender->volume(), 50);
        QVERIFY(!rig.sender->muted());
        QVERIFY(!rig.sender->setVolume(101));
        QVERIFY(!rig.sender->setVolume(-1));

        QVERIFY(rig.sender->setVolume(70));
        QCOMPARE(rig.sender->volume(), 70);
        QTRY_COMPARE(rig.device.count(kReceiverNs, QStringLiteral("SET_VOLUME")), 1);
        const QList<FakeCastDevice::Received> volumes =
            rig.device.frames(kReceiverNs, QStringLiteral("SET_VOLUME"));
        QCOMPARE(volumes.constFirst().message.destinationId, kReceiver);
        const QJsonObject set = FakeCastDevice::jsonOf(volumes.constFirst().message);
        QVERIFY(requestIdOf(set) != 0);
        QCOMPARE(set.value(QLatin1String("volume")).toObject().keys(),
                 QStringList{QStringLiteral("level")});
        QVERIFY(qFuzzyCompare(
            set.value(QLatin1String("volume")).toObject().value(QLatin1String("level")).toDouble(),
            0.7));
        QTRY_VERIFY(qFuzzyCompare(rig.device.volumeLevel, 0.7));
        QCOMPARE(rig.sender->volume(), 70);

        QVERIFY(rig.sender->setMuted(true));
        QVERIFY(rig.sender->muted());
        QTRY_COMPARE(rig.device.count(kReceiverNs, QStringLiteral("SET_VOLUME")), 2);
        QCOMPARE(compact(withoutRequestId(
                     lastFrame(rig.device, kReceiverNs, QStringLiteral("SET_VOLUME")))),
                 compact(QJsonObject{
                     {QStringLiteral("type"), QStringLiteral("SET_VOLUME")},
                     {QStringLiteral("volume"), QJsonObject{{QStringLiteral("muted"), true}}}}));
        QTRY_VERIFY(rig.device.volumeMuted);
        QVERIFY(rig.spy->volumes.size() >= 2);

        // A change made on the device shows
        rig.device.volumeLevel = 0.2;
        rig.device.volumeMuted = false;
        rig.device.pushReceiverStatus();
        QTRY_COMPARE(rig.sender->volume(), 20);
        QTRY_VERIFY(!rig.sender->muted());
        QVERIFY(rig.spy->stopped.isEmpty());
    }

    void fixedVolumeHasNoControl()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.volumeControlType = QStringLiteral("fixed");
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));
        QVERIFY(!rig.sender->hasVolume());
        QVERIFY(!rig.sender->setVolume(30));
        QVERIFY(!rig.sender->setMuted(true));
        QTest::qWait(50);
        QCOMPARE(rig.device.count(kReceiverNs, QStringLiteral("SET_VOLUME")), 0);
    }

    void stopOrder()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));
        const qint64 session = rig.device.mediaSessionId;

        QVERIFY(rig.sender->stop());
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        // The media STOP, the STOP of our app and the goodbye on both virtual connections, in that order
        const QStringList all = sequence(rig.device);
        QCOMPARE(all.mid(all.size() - 4),
                 (QStringList{QStringLiteral("media:STOP:transport-1"),
                              QStringLiteral("receiver:STOP:receiver-0"),
                              QStringLiteral("connection:CLOSE:receiver-0"),
                              QStringLiteral("connection:CLOSE:transport-1")}));
        QCOMPARE(compact(withoutRequestId(lastFrame(rig.device, kMediaNs, kStop))),
                 compact(QJsonObject{{QStringLiteral("type"), kStop},
                                     {QStringLiteral("mediaSessionId"), session}}));
        const QJsonObject appStop = lastFrame(rig.device, kReceiverNs, kStop);
        QVERIFY(requestIdOf(appStop) != 0);
        QCOMPARE(compact(withoutRequestId(appStop)),
                 compact(QJsonObject{{QStringLiteral("type"), kStop},
                                     {QStringLiteral("sessionId"), QStringLiteral("session-1")}}));
        QCOMPARE(rig.device.appStops, 1);

        // An own stop ends with one stopped(), as a DLNA renderer's does, and with nothing else
        QTest::qWait(150);
        QTRY_COMPARE(rig.spy->stopped.size(), 1);
        QVERIFY(rig.spy->failed.isEmpty());
        QVERIFY(rig.spy->ended.isEmpty());
        QVERIFY(!rig.sender->stop());
        QVERIFY(!rig.sender->pause());

        // A new load starts over on a new channel
        QVERIFY(playing(rig, rig.request()));
        QCOMPARE(rig.device.connections, 2);
        QCOMPARE(rig.device.count(kReceiverNs, QStringLiteral("LAUNCH")), 2);
    }

    void stopWaitsForTheMediaStopThenGivesUp()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        CastSender::Options options = rig.options(*m_pki);
        options.stopTimeoutMs = 300;
        rig.start(options);
        QVERIFY(playing(rig, rig.request()));
        rig.device.answerMedia = false;
        QVERIFY(rig.sender->stop());
        QTRY_COMPARE(rig.device.count(kReceiverNs, kStop), 1);
        const qint64 mediaStop = rig.device.frames(kMediaNs, kStop).constFirst().atMs;
        const qint64 appStop = rig.device.frames(kReceiverNs, kStop).constFirst().atMs;
        QVERIFY(appStop - mediaStop >= 250);
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
    }

    void aLostChannelWhileStoppingStillAnnouncesTheStop()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        CastSender::Options options = rig.options(*m_pki);
        // Longer than the QTRY below: the stopped() must not wait for it
        options.stopTimeoutMs = 20'000;
        rig.start(options);
        QVERIFY(playing(rig, rig.request()));
        rig.device.answerMedia = false;
        QVERIFY(rig.sender->stop());
        QTRY_COMPARE(rig.device.count(kMediaNs, kStop), 1);
        rig.device.dropConnection();
        QTRY_COMPARE(rig.spy->stopped.size(), 1);
        QVERIFY(rig.spy->failed.isEmpty());
        QVERIFY(rig.spy->ended.isEmpty());
        QTest::qWait(150);
        QCOMPARE(rig.spy->stopped.size(), 1);
        QCOMPARE(rig.deviceLookups, 0);
        QCOMPARE(rig.device.connections, 1);
        QVERIFY(!rig.sender->stop());
    }

    void noAppStopWhenTheSessionIsNotOurs()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));
        // The last status lists only the ambient app: not a takeover, and not our session either
        rig.device.sendStatus(
            QJsonDocument::fromJson(
                receiverStatusJson(
                    "{\"appId\":\"E8C28D3C\",\"sessionId\":\"a\",\"transportId\":\"b\"}"))
                .object(),
            kReceiverNs, kReceiver);
        QTest::qWait(150);
        QVERIFY(rig.spy->stopped.isEmpty());

        QVERIFY(rig.sender->stop());
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        QCOMPARE(rig.device.count(kMediaNs, kStop), 1);
        QCOMPARE(rig.device.count(kReceiverNs, kStop), 0);
        QCOMPARE(rig.device.appStops, 0);
        QTest::qWait(100);
        QTRY_COMPARE(rig.spy->stopped.size(), 1);
    }

    void aLoadRightAfterAStopDropsTheLateStopped()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));
        QVERIFY(rig.sender->stop());
        QVERIFY(rig.sender->load(rig.request()));
        // The stopped() of the old stop would end the new cast: it is dropped
        QTest::qWait(300);
        QVERIFY(rig.spy->stopped.isEmpty());
        QVERIFY(rig.spy->failed.isEmpty());
    }

    void stopDuringLaunchJustCancels()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.answerLaunch = false;
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.device.count(kReceiverNs, QStringLiteral("LAUNCH")), 1);
        QVERIFY(rig.sender->stop());
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose, kReceiver), 1);
        QTest::qWait(100);
        QCOMPARE(rig.device.count(kReceiverNs, kStop), 0);
        QCOMPARE(rig.device.count(kMediaNs), 0);
        QTRY_COMPARE(rig.spy->stopped.size(), 1);
        QVERIFY(rig.spy->failed.isEmpty());

        // The launch timer is gone with the cancel
        QTest::qWait(100);
        QVERIFY(rig.spy->failed.isEmpty());
        rig.device.answerLaunch = true;
        QVERIFY(playing(rig, rig.request()));
    }

    void stopWhileProbingJustCancels()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.media.hold = true;
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.media.requests, 1);
        QVERIFY(rig.sender->stop());
        QTest::qWait(150);
        QCOMPARE(rig.device.connections, 0);
        QTRY_COMPARE(rig.spy->stopped.size(), 1);
        QVERIFY(rig.spy->failed.isEmpty());
    }

    void launchErrorRetriedOnceOnCancelled()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.launchErrors = 1;
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));
        const QList<FakeCastDevice::Received> launches =
            rig.device.frames(kReceiverNs, QStringLiteral("LAUNCH"));
        QCOMPARE(launches.size(), 2);
        QVERIFY(requestIdOf(FakeCastDevice::jsonOf(launches.at(1).message))
                > requestIdOf(FakeCastDevice::jsonOf(launches.at(0).message)));
        QCOMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QVERIFY(rig.spy->failed.isEmpty());
    }

    void launchErrorTwiceFails()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.launchErrors = 2;
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::NotResponding);
        QCOMPARE(rig.device.count(kReceiverNs, QStringLiteral("LAUNCH")), 2);
        QCOMPARE(rig.device.count(kMediaNs, kLoad), 0);
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose, kReceiver), 1);
        QCOMPARE(rig.device.count(kReceiverNs, kStop), 0);
    }

    void launchErrorOtherReasonFails()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.launchErrors = 1;
        rig.device.launchErrorReason = QStringLiteral("NOT_FOUND");
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::NotResponding);
        QCOMPARE(rig.device.count(kReceiverNs, QStringLiteral("LAUNCH")), 1);
    }

    void launchTimeoutFails()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.answerLaunch = false;
        CastSender::Options options = rig.options(*m_pki);
        options.launchTimeoutMs = 300;
        rig.start(options);
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::NotResponding);
        QCOMPARE(rig.device.count(kReceiverNs, QStringLiteral("LAUNCH")), 1);
        QCOMPARE(rig.device.count(kMediaNs), 0);
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose, kReceiver), 1);
        // No answer to the LAUNCH: there is no app of ours to stop
        QCOMPARE(rig.device.count(kReceiverNs, kStop), 0);
    }

    void aStatusWithoutATransportIsWaitedOut()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.launchStatusWithoutTransport = true;
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));
        QCOMPARE(rig.device.count(kReceiverNs, QStringLiteral("LAUNCH")), 1);
        QCOMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QVERIFY(rig.spy->failed.isEmpty());
    }

    void authFailureIsUntrusted()
    {
        // The device presents the chain of another PKI than the one the sender trusts
        Rig rig(*m_otherPki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::Untrusted);
        // Only the challenge reached it
        QCOMPARE(rig.device.receivedCount(), 1);
        QCOMPARE(sequence(rig.device),
                 QStringList{QStringLiteral("deviceauth:CHALLENGE:receiver-0")});
        QTest::qWait(100);
        QCOMPARE(rig.device.receivedCount(), 1);
        QCOMPARE(rig.spy->failed.size(), 1);
        QVERIFY(rig.spy->states.isEmpty());
    }

    void connectFailureIsNotResponding()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.stopListening();
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request(false)));
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::NotResponding);
        QCOMPARE(rig.device.connections, 0);
    }

    void probe401IsServerRefused()
    {
        expectProbeFailure(401, Failure::ServerRefused);
        expectProbeFailure(403, Failure::ServerRefused);
    }

    void probe500IsRefused()
    {
        expectProbeFailure(500, Failure::Refused);
        expectProbeFailure(404, Failure::Refused);
        // A server that ignores the range answers 200
        expectProbeFailure(200, Failure::Refused);
    }

    void probeOnlyForDirectPlay()
    {
        Rig direct(*m_pki);
        QVERIFY(direct.listening);
        direct.start(direct.options(*m_pki));
        QVERIFY(playing(direct, direct.request(true)));
        QCOMPARE(direct.media.requests, 1);
        QVERIFY(direct.media.heads.constFirst().contains("Range: bytes=0-0"));
        QVERIFY(direct.media.credentialFree());

        // A transcode is not probed: the request would start a job
        Rig transcode(*m_pki);
        QVERIFY(transcode.listening);
        transcode.start(transcode.options(*m_pki));
        QVERIFY(playing(transcode, transcode.request(false)));
        QTest::qWait(100);
        QCOMPARE(transcode.media.requests, 0);
    }

    void nothingReceivedCarriesTheToken()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));

        // A URL that names a token in its query is refused whatever the key's case, and nothing is sent anywhere
        const QStringList keys{QStringLiteral("ApiKey"), QStringLiteral("api_key"),
                               QStringLiteral("APIKEY"), QStringLiteral("X-Emby-Token"),
                               QStringLiteral("token")};
        for (const QString &key : keys) {
            const QString query = QStringLiteral("/video.mp4?%1=%2").arg(key, kToken);
            CastLoad request = rig.request();
            request.url = QUrl(rig.urlFor(query));
            QVERIFY2(!rig.sender->load(request), qPrintable(key));
        }
        QTest::qWait(100);
        QCOMPARE(rig.device.connections, 0);
        QCOMPARE(rig.device.receivedCount(), 0);
        QCOMPARE(rig.media.requests, 0);

        // A load of token-free URLs goes through, and neither the device nor the server ever hears the token
        CastLoad request = rig.request();
        request.track.url = QUrl(rig.urlFor(QStringLiteral("/sub.vtt")));
        request.imageUrl = QUrl(rig.urlFor(QStringLiteral("/image.jpg?maxWidth=360")));
        QVERIFY(playing(rig, request));
        QVERIFY(rig.sender->pause());
        QVERIFY(rig.sender->setVolume(40));
        QVERIFY(rig.sender->stop());
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        QVERIFY(rig.device.receivedCount() > 5);
        const QByteArray token = kToken.toUtf8();
        for (const FakeCastDevice::Received &entry : std::as_const(rig.device.received))
            QVERIFY(!entry.message.payload.contains(token));
        QVERIFY(rig.media.credentialFree());
    }

    void optionalUrlsThatNameATokenAreLeftOut()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        request.track.url = QUrl(rig.urlFor(QStringLiteral("/sub.vtt?ApiKey=%1").arg(kToken)));
        request.imageUrl =
            QUrl(rig.urlFor(QStringLiteral("/image.jpg?X-Emby-Token=%1").arg(kToken)));
        // The video loads without them, and nothing that was sent names the token
        QVERIFY(playing(rig, request));
        const QJsonObject load = rig.device.lastLoad();
        QVERIFY(!hasTrack(load));
        QVERIFY(!hasPoster(load));
        QVERIFY(rig.sender->stop());
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        QTest::qWait(100);
        const QByteArray token = kToken.toUtf8();
        for (const FakeCastDevice::Received &entry : std::as_const(rig.device.received))
            QVERIFY(!entry.message.payload.contains(token));
        // The track that was left out is not fetched either
        QCOMPARE(rig.media.requests, 0);
        QVERIFY(rig.media.credentialFree());
    }

    void anOversizedImageUrlIsDropped()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        const QString padding(1100, QLatin1Char('a'));
        request.imageUrl = QUrl(rig.urlFor(QStringLiteral("/image.jpg?q=%1").arg(padding)));
        request.track.url = QUrl(rig.urlFor(QStringLiteral("/sub.vtt")));
        // The poster is optional: the video and the track load without it
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        const QJsonObject load = rig.device.lastLoad();
        QVERIFY(!hasPoster(load));
        QVERIFY(hasTrack(load));
        QCOMPARE(contentIdOf(load), request.url.toString(QUrl::FullyEncoded));
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
        QVERIFY(rig.spy->failed.isEmpty());
    }

    void anOversizedTrackUrlIsDropped()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        const QString padding(1100, QLatin1Char('a'));
        request.track.url = QUrl(rig.urlFor(QStringLiteral("/sub.vtt?q=%1").arg(padding)));
        request.track.language = QStringLiteral("eng");
        request.imageUrl = QUrl(rig.urlFor(QStringLiteral("/image.jpg?maxWidth=360")));
        // The text track is optional: the video and the poster load without it
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        const QJsonObject load = rig.device.lastLoad();
        QVERIFY(!hasTrack(load));
        QVERIFY(hasPoster(load));
        QCOMPARE(contentIdOf(load), request.url.toString(QUrl::FullyEncoded));
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
        QVERIFY(rig.spy->failed.isEmpty());
        // Nothing asks the server for the track that was left out
        QCOMPARE(rig.media.requests, 0);
    }

    void contentIdOverOneKilobyteRefused()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        // The URL is the contentId as the device gets it: 1024 bytes pass, one more does not
        const QString head = rig.urlFor(QStringLiteral("/transcode.mp4?q="));
        const qsizetype room = 1024 - head.toUtf8().size();
        const QString tooLong =
            QStringLiteral("%1%2").arg(head, QString(room + 1, QLatin1Char('a')));
        const QString justFits = QStringLiteral("%1%2").arg(head, QString(room, QLatin1Char('a')));
        QCOMPARE(tooLong.toUtf8().size(), qsizetype(1025));
        QCOMPARE(justFits.toUtf8().size(), qsizetype(1024));
        CastLoad request = rig.request(false);
        request.url = QUrl(tooLong);
        QVERIFY(!rig.sender->load(request));
        QTest::qWait(100);
        QCOMPARE(rig.device.connections, 0);

        request = rig.request(false);
        request.url = QUrl(justFits);
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
    }

    void refusesAnUrlWithUserInfoAndOtherSchemesAndOffBase_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("user info") << QStringLiteral("http://user:secret@%HOSTPORT%/v.mp4");
        QTest::newRow("password only") << QStringLiteral("http://:secret@%HOSTPORT%/v.mp4");
        QTest::newRow("ftp") << QStringLiteral("ftp://%HOSTPORT%/v.mp4");
        QTest::newRow("file") << QStringLiteral("file:///etc/passwd");
        QTest::newRow("javascript") << QStringLiteral("javascript:alert(1)");
        QTest::newRow("scheme-relative") << QStringLiteral("//%HOSTPORT%/v.mp4");
        QTest::newRow("relative") << QStringLiteral("/v.mp4");
        QTest::newRow("empty") << QString();
        QTest::newRow("another host") << QStringLiteral("http://192.168.77.7:%PORT%/v.mp4");
        QTest::newRow("another port") << QStringLiteral("http://127.0.0.1:1/v.mp4");
        QTest::newRow("https for an http server") << QStringLiteral("https://%HOSTPORT%/v.mp4");
    }

    void refusesAnUrlWithUserInfoAndOtherSchemesAndOffBase()
    {
        QFETCH(QString, url);
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        url.replace(QLatin1String("%HOSTPORT%"),
                    QStringLiteral("127.0.0.1:%1").arg(rig.media.port()));
        url.replace(QLatin1String("%PORT%"), QString::number(rig.media.port()));

        CastLoad request = rig.request(false);
        request.url = QUrl(url);
        QVERIFY(!rig.sender->load(request));
        QTest::qWait(100);
        QCOMPARE(rig.device.connections, 0);
        QCOMPARE(rig.media.requests, 0);
        QVERIFY(rig.spy->failed.isEmpty());

        // The text track and the image follow the same rules but are optional: the video loads without them, and the server is not asked for the track
        if (!url.isEmpty()) {
            request = rig.request(false);
            request.track.url = QUrl(url);
            request.imageUrl = QUrl(url);
            QVERIFY(rig.sender->load(request));
            QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
            QVERIFY(!hasTrack(rig.device.lastLoad()));
            QVERIFY(!hasPoster(rig.device.lastLoad()));
            QCOMPARE(rig.media.requests, 0);
        }
    }

    void startAndLoadRules()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        request.startMs = -1;
        QVERIFY(!rig.sender->load(request));
        request.startMs = 24LL * 3600 * 1000 + 1;
        QVERIFY(!rig.sender->load(request));
        QCOMPARE(rig.device.connections, 0);

        request.startMs = 24LL * 3600 * 1000;
        QVERIFY(rig.sender->load(request));
        // A load that is already running is refused until a stop
        QVERIFY(!rig.sender->load(rig.request(false)));
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
        QVERIFY(!rig.sender->load(rig.request(false)));
        QVERIFY(rig.sender->stop());
        QVERIFY(rig.sender->load(rig.request(false)));
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
    }

    void lanHostnameSentAsAddress()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.hostAnswer = QHostAddress::LocalHost;
        CastSender::Options options = rig.options(*m_pki);
        options.mediaAddressPolicy = [](const QHostAddress &address) {
            return address.isLoopback();
        };
        rig.start(options);
        CastLoad request = rig.request();
        request.serverBase = QUrl(rig.namedBase());
        request.url = QUrl(QStringLiteral("%1/stream.mp4").arg(rig.namedBase()));
        request.imageUrl = QUrl(QStringLiteral("%1/img.jpg?maxWidth=360").arg(rig.namedBase()));
        request.track.url = QUrl(QStringLiteral("%1/sub.vtt").arg(rig.namedBase()));
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QCOMPARE(rig.hostLookups, 1);
        QCOMPARE(rig.lastHost, QStringLiteral("media.test"));

        // The device gets the address, with the port and the path; the probe went to it as well
        const QString base = rig.media.base();
        const QJsonObject media = rig.device.lastLoad().value(QLatin1String("media")).toObject();
        QCOMPARE(media.value(QLatin1String("contentId")).toString(),
                 QStringLiteral("%1/stream.mp4").arg(base));
        QCOMPARE(media.value(QLatin1String("metadata"))
                     .toObject()
                     .value(QLatin1String("images"))
                     .toArray()
                     .at(0)
                     .toObject()
                     .value(QLatin1String("url"))
                     .toString(),
                 QStringLiteral("%1/img.jpg?maxWidth=360").arg(base));
        QCOMPARE(media.value(QLatin1String("tracks"))
                     .toArray()
                     .at(0)
                     .toObject()
                     .value(QLatin1String("trackContentId"))
                     .toString(),
                 QStringLiteral("%1/sub.vtt").arg(base));
        // The probe and the request that warms the text track both went to the address
        QTRY_COMPARE(rig.media.requests, 2);
        QTRY_COMPARE(rig.sender->transport(), Transport::Playing);
    }

    void reloadKeepsTheAddress()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.hostAnswer = QHostAddress::LocalHost;
        CastSender::Options options = rig.options(*m_pki);
        options.mediaAddressPolicy = [](const QHostAddress &address) {
            return address.isLoopback();
        };
        rig.start(options);
        CastLoad request = rig.request(false);
        request.serverBase = QUrl(rig.namedBase());
        request.url = QUrl(QStringLiteral("%1/stream.mp4?a=1").arg(rig.namedBase()));
        QVERIFY(playing(rig, request));
        const QUrl again(QStringLiteral("%1/stream.mp4?a=1&b=2").arg(rig.namedBase()));
        QVERIFY(rig.sender->reload(again, 60000));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 2);
        QCOMPARE(rig.device.lastLoad()
                     .value(QLatin1String("media"))
                     .toObject()
                     .value(QLatin1String("contentId"))
                     .toString(),
                 QStringLiteral("%1/stream.mp4?a=1&b=2").arg(rig.media.base()));
        // The name is resolved once per load
        QCOMPARE(rig.hostLookups, 1);
    }

    void publicHostnameStays_data()
    {
        QTest::addColumn<QString>("address");
        QTest::newRow("a public address") << QStringLiteral("8.8.8.8");
        QTest::newRow("loopback") << QStringLiteral("127.0.0.1");
        QTest::newRow("no answer") << QString();
    }

    void publicHostnameStays()
    {
        QFETCH(QString, address);
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.hostAnswer = QHostAddress(address);
        // The default policy: private addresses only
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        request.serverBase = QUrl(rig.namedBase());
        request.url = QUrl(QStringLiteral("%1/stream.mp4").arg(rig.namedBase()));
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QCOMPARE(rig.hostLookups, 1);
        QCOMPARE(rig.device.lastLoad()
                     .value(QLatin1String("media"))
                     .toObject()
                     .value(QLatin1String("contentId"))
                     .toString(),
                 QStringLiteral("%1/stream.mp4").arg(rig.namedBase()));
    }

    void privateAddressWithTheDefaultPolicyIsSent()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.hostAnswer = QHostAddress(QStringLiteral("192.168.1.50"));
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        request.serverBase = QUrl(rig.namedBase());
        request.url = QUrl(QStringLiteral("%1/stream.mp4").arg(rig.namedBase()));
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QCOMPARE(rig.device.lastLoad()
                     .value(QLatin1String("media"))
                     .toObject()
                     .value(QLatin1String("contentId"))
                     .toString(),
                 QStringLiteral("http://192.168.1.50:%1/stream.mp4").arg(rig.media.port()));
    }

    void httpsHostnameStays()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.hostAnswer = QHostAddress::LocalHost;
        rig.start(rig.options(*m_pki));
        CastLoad request = rig.request(false);
        request.serverBase = QUrl(QStringLiteral("https://media.test"));
        request.url = QUrl(QStringLiteral("https://media.test/stream.mp4"));
        QVERIFY(rig.sender->load(request));
        QTRY_COMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QCOMPARE(rig.hostLookups, 0);
        QCOMPARE(rig.device.lastLoad()
                     .value(QLatin1String("media"))
                     .toObject()
                     .value(QLatin1String("contentId"))
                     .toString(),
                 QStringLiteral("https://media.test/stream.mp4"));
    }

    void reconnectsToTheSameDevice()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.start(rig.options(*m_pki));
        QVERIFY(playing(rig, rig.request()));

        rig.device.dropConnection();
        QTRY_COMPARE(rig.device.connections, 2);
        QTRY_VERIFY(rig.device.count(kMediaNs, kGetStatus, kTransport) >= 1);
        QCOMPARE(rig.deviceLookups, 1);
        QCOMPARE(rig.lastDeviceId, rig.info().id);

        // The second connection authenticates again, joins the running session and asks for its status: no LAUNCH, no LOAD
        QCOMPARE(sequence(rig.device, 2),
                 (QStringList{QStringLiteral("deviceauth:CHALLENGE:receiver-0"),
                              QStringLiteral("connection:CONNECT:receiver-0"),
                              QStringLiteral("receiver:GET_STATUS:receiver-0"),
                              QStringLiteral("connection:CONNECT:transport-1"),
                              QStringLiteral("media:GET_STATUS:transport-1")}));

        // The cast carries on: positions are reported again and the controls work
        QTest::qWait(100);
        const qsizetype ticks = rig.spy->positions.size();
        QTRY_VERIFY(rig.spy->positions.size() > ticks);
        QCOMPARE(rig.sender->transport(), Transport::Playing);
        QVERIFY(rig.sender->pause());
        QTRY_COMPARE(rig.sender->transport(), Transport::Paused);
        QTest::qWait(200);
        QVERIFY(rig.spy->stopped.isEmpty());
        QVERIFY(rig.spy->failed.isEmpty());
        QVERIFY(rig.spy->ended.isEmpty());
        QCOMPARE(rig.device.count(kMediaNs, kLoad), 1);
        QCOMPARE(rig.device.count(kReceiverNs, QStringLiteral("LAUNCH")), 1);
    }

    void aDifferentLeafOnReconnectStops()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        CastSender::Options options = rig.options(*m_pki);
        // Both devices are genuine: only the leaf tells them apart
        options.channel.trust =
            Kinefin::CastTrust::fromDer({m_pki->rootDer(), m_otherPki->rootDer()});
        rig.start(options);
        QVERIFY(playing(rig, rig.request()));

        rig.device.setIdentity(*m_otherPki);
        rig.device.dropConnection();
        QTRY_COMPARE(rig.spy->stopped.size(), 1);
        QCOMPARE(rig.device.connections, 2);
        // Nothing but the challenge was sent to what answered
        QCOMPARE(sequence(rig.device, 2),
                 QStringList{QStringLiteral("deviceauth:CHALLENGE:receiver-0")});
        QTest::qWait(150);
        QCOMPARE(rig.spy->stopped.size(), 1);
        QVERIFY(rig.spy->failed.isEmpty());
        QCOMPARE(rig.device.connections, 2);
    }

    void aSpoofedReconnectAnswerDoesNotMoveTheDevice()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        // Another device at another address, genuine as far as the authentication goes
        FakeCastDevice impostor(*m_otherPki);
        impostor.receiverEnabled = true;
        QVERIFY(impostor.listen(QHostAddress(QStringLiteral("127.0.0.2"))));
        CastSender::Options options = rig.options(*m_pki);
        options.channel.trust =
            Kinefin::CastTrust::fromDer({m_pki->rootDer(), m_otherPki->rootDer()});
        options.resolveDevice = [&rig, &impostor](const QString &, DeviceFound done) {
            Kinefin::CastDeviceInfo found = rig.info();
            found.address = QHostAddress(QStringLiteral("127.0.0.2"));
            found.port = impostor.port();
            done(found);
        };
        rig.start(options);
        QVERIFY(playing(rig, rig.request()));

        rig.device.dropConnection();
        QTRY_COMPARE(rig.spy->stopped.size(), 1);
        QTRY_COMPARE(impostor.connections, 1);
        // Nothing but the challenge was sent to it, and it was not remembered as the device
        QCOMPARE(sequence(impostor),
                 QStringList{QStringLiteral("deviceauth:CHALLENGE:receiver-0")});
        QCOMPARE(rig.sender->info().address, QHostAddress(QHostAddress::LocalHost));
        QCOMPARE(rig.sender->info().port, rig.device.port());
        QTest::qWait(150);
        QVERIFY(rig.spy->failed.isEmpty());
        QCOMPARE(rig.spy->stopped.size(), 1);

        // The next load goes to the device that was picked
        QVERIFY(playing(rig, rig.request()));
        QCOMPARE(rig.device.connections, 2);
        QCOMPARE(impostor.connections, 1);
    }

    void reconnectGivesUpAfterTheAttempts_data()
    {
        QTest::addColumn<bool>("deviceFound");
        QTest::newRow("the device is not found") << false;
        QTest::newRow("the device refuses connections") << true;
    }

    void reconnectGivesUpAfterTheAttempts()
    {
        QFETCH(bool, deviceFound);
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        CastSender::Options options = rig.options(*m_pki);
        options.reconnectAttempts = 2;
        rig.start(options);
        QVERIFY(playing(rig, rig.request()));

        rig.deviceLookupFails = !deviceFound;
        if (deviceFound)
            rig.device.stopListening();
        rig.device.dropConnection();
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::NotResponding);
        QCOMPARE(rig.deviceLookups, 2);
        QTest::qWait(200);
        QCOMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(rig.deviceLookups, 2);
        QVERIFY(rig.spy->stopped.isEmpty());
    }

    void lossBeforeAMediaSessionFailsWithoutReconnect()
    {
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        rig.device.answerLaunch = false;
        rig.start(rig.options(*m_pki));
        QVERIFY(rig.sender->load(rig.request()));
        QTRY_COMPARE(rig.device.count(kReceiverNs, QStringLiteral("LAUNCH")), 1);
        rig.device.dropConnection();
        QTRY_COMPARE(rig.spy->failed.size(), 1);
        QCOMPARE(failureOf(rig.spy->failed.constFirst()), Failure::NotResponding);
        QTest::qWait(200);
        QCOMPARE(rig.device.connections, 1);
        QCOMPARE(rig.deviceLookups, 0);
    }

    void destructionIsSilent_data()
    {
        QTest::addColumn<int>("moment");
        QTest::newRow("while playing") << 0;
        QTest::newRow("while probing") << 1;
        QTest::newRow("while launching") << 2;
        QTest::newRow("while reconnecting") << 3;
    }

    void destructionIsSilent()
    {
        QFETCH(int, moment);
        Rig rig(*m_pki);
        QVERIFY(rig.listening);
        CastSender::Options options = rig.options(*m_pki);
        // Long enough that the reconnect is still waiting when the sender goes
        options.reconnectDelayMs = 5000;
        options.reconnectAttempts = 3;
        if (moment == 1)
            rig.media.hold = true;
        if (moment == 2)
            rig.device.answerLaunch = false;
        rig.start(options);
        if (moment == 0 || moment == 3) {
            QVERIFY(playing(rig, rig.request()));
        } else {
            QVERIFY(rig.sender->load(rig.request()));
            if (moment == 1)
                QTRY_COMPARE(rig.media.requests, 1);
            else
                QTRY_COMPARE(rig.device.count(kReceiverNs, QStringLiteral("LAUNCH")), 1);
        }
        if (moment == 3) {
            rig.deviceLookupFails = true;
            rig.device.dropConnection();
            QTRY_COMPARE(rig.deviceLookups, 1);
        }

        const qsizetype states = rig.spy->states.size();
        rig.sender.reset();
        QTest::qWait(250);
        QCOMPARE(rig.spy->failed.size(), 0);
        QCOMPARE(rig.spy->stopped.size(), 0);
        QCOMPARE(rig.spy->ended.size(), 0);
        QCOMPARE(rig.spy->states.size(), states);
    }

    // Opt-in against a real device: KINEFIN_CAST_LIVE_HOST is its address and KINEFIN_CAST_LIVE_URL a token-free http MP4 of at least 60 s with Range support that it can reach; KINEFIN_CAST_LIVE_MIME (video/mp4), KINEFIN_CAST_LIVE_DURATION_S (60) and KINEFIN_CAST_LIVE_MODEL (Google Nest Hub) are optional. It plays, pauses, resumes, seeks and stops; the URL is never logged
    void liveDevice()
    {
        if (qEnvironmentVariableIsEmpty("KINEFIN_CAST_LIVE_HOST")
            || qEnvironmentVariableIsEmpty("KINEFIN_CAST_LIVE_URL"))
            QSKIP("no KINEFIN_CAST_LIVE_HOST and KINEFIN_CAST_LIVE_URL, skipping the live check");
        const QHostAddress host(qEnvironmentVariable("KINEFIN_CAST_LIVE_HOST"));
        const QUrl url(qEnvironmentVariable("KINEFIN_CAST_LIVE_URL"));
        QVERIFY2(!host.isNull(), "KINEFIN_CAST_LIVE_HOST is not an address");
        QVERIFY2(url.isValid(), "KINEFIN_CAST_LIVE_URL is not a URL");
        bool durationSet = false;
        int durationS = qEnvironmentVariableIntValue("KINEFIN_CAST_LIVE_DURATION_S", &durationSet);
        if (!durationSet || durationS <= 0)
            durationS = 60;

        Kinefin::CastDeviceInfo info;
        info.id = QString(32, QLatin1Char('0'));
        info.name = QStringLiteral("Live device");
        info.model =
            qEnvironmentVariable("KINEFIN_CAST_LIVE_MODEL", QStringLiteral("Google Nest Hub"));
        info.address = host;
        info.port = 8009;
        CastSender::Options options;
        options.mediaAddressPolicy = Kinefin::isAllowedRendererAddress;
        CastSender sender(info, options);
        Spies spy(&sender);

        QUrl origin;
        origin.setScheme(url.scheme());
        origin.setHost(url.host());
        origin.setPort(url.port());
        CastLoad request;
        request.url = url;
        request.mime = qEnvironmentVariable("KINEFIN_CAST_LIVE_MIME", QStringLiteral("video/mp4"));
        request.title = QStringLiteral("Kinefin live check");
        request.durationMs = static_cast<qint64>(durationS) * 1000;
        request.serverBase = origin;
        request.direct = true;
        QVERIFY2(sender.load(request), "the sender refused the URL");

        const auto settled = [&] {
            return sender.transport() == Transport::Playing || !spy.failed.isEmpty();
        };
        QTRY_VERIFY_WITH_TIMEOUT(settled(), 40'000);
        if (!spy.failed.isEmpty()) {
            const int failure = static_cast<int>(failureOf(spy.failed.constFirst()));
            QFAIL(QMetaEnum::fromType<Failure>().valueToKey(failure));
        }
        const qint64 started = sender.positionMs();
        QTRY_VERIFY_WITH_TIMEOUT(sender.positionMs() >= started + 2000, 15'000);

        QVERIFY(sender.pause());
        QTRY_COMPARE_WITH_TIMEOUT(sender.transport(), Transport::Paused, 10'000);
        QVERIFY(sender.play());
        QTRY_COMPARE_WITH_TIMEOUT(sender.transport(), Transport::Playing, 10'000);

        QVERIFY(sender.seek(30'000));
        // The device answers the SEEK with its own position, which replaces the local one
        QTest::qWait(1500);
        QVERIFY2(qAbs(sender.positionMs() - 30'000) <= 3000, "the position is not near the target");

        QVERIFY(sender.stop());
        QTRY_COMPARE_WITH_TIMEOUT(spy.stopped.size(), 1, 10'000);
        QTest::qWait(300);
        QCOMPARE(spy.stopped.size(), 1);
        QVERIFY(spy.failed.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCastSender)
#include "test_cast_sender.moc"
