// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FakeCastDevice.h"

#include "cast/CastChannel.h"

#include <QSignalSpy>
#include <QSslSocket>
#include <QTcpServer>
#include <QtTest/QtTest>

#include <algorithm>
#include <memory>

using Kinefin::CastChannel;
using Mode = FakeCastDevice::AuthMode;
using Reason = CastChannel::FailReason;
using State = CastChannel::State;

namespace {
const QString kReceiver = QString(Kinefin::kCastPlatformId);
const QString kSender = QString(Kinefin::kCastSenderId);
const QString kTransport = QStringLiteral("transport-1");
const QString kAuthNs = QString(Kinefin::kCastNsDeviceAuth);
const QString kConnectionNs = QString(Kinefin::kCastNsConnection);
const QString kHeartbeatNs = QString(Kinefin::kCastNsHeartbeat);
const QString kReceiverNs = QString(Kinefin::kCastNsReceiver);
const QString kMediaNs = QString(Kinefin::kCastNsMedia);
const QString kPing = QStringLiteral("PING");
const QString kPong = QStringLiteral("PONG");
const QString kConnect = QStringLiteral("CONNECT");
const QString kClose = QStringLiteral("CLOSE");
const QString kGetStatus = QStringLiteral("GET_STATUS");

QJsonObject typed(const QString &type)
{
    return QJsonObject{{QStringLiteral("type"), type}};
}

QJsonObject reply(const QString &type, qint64 requestId)
{
    QJsonObject object = typed(type);
    object.insert(QStringLiteral("requestId"), requestId);
    return object;
}

Reason reasonOf(const QList<QVariant> &arguments)
{
    return qvariant_cast<Reason>(arguments.at(0));
}

QString detailOf(const QList<QVariant> &arguments)
{
    return arguments.at(1).toString();
}

void verifyOnlyTheChallenge(const FakeCastDevice &device)
{
    QCOMPARE(device.receivedCount(), 1);
    QCOMPARE(device.received.constFirst().message.ns, kAuthNs);
}

// A fake device and a channel to it, with the spies most tests read. The device listens before the channel is built
struct Rig {
    Rig(const CastTestPki &pki, const CastChannel::Options &options, Mode mode = Mode::Answer)
        : device(pki), channel(QHostAddress::LocalHost, listenOn(device, mode), options)
    {
    }

    // True once the device passed. failed() may come from inside open(), so the spy is read first
    bool open()
    {
        channel.open();
        return opened.size() == 1 || opened.wait(5000);
    }

    FakeCastDevice device;
    CastChannel channel;
    QSignalSpy opened{&channel, &CastChannel::opened};
    QSignalSpy failed{&channel, &CastChannel::failed};
    QSignalSpy messages{&channel, &CastChannel::messageReceived};

  private:
    static quint16 listenOn(FakeCastDevice &device, Mode mode)
    {
        device.authMode = mode;
        return device.listen() ? device.port() : 0;
    }
};
} // namespace

class TestCastChannel : public QObject {
    Q_OBJECT

  private:
    CastChannel::Options options()
    {
        CastChannel::Options value;
        value.trust = m_pki->trust();
        return value;
    }

    // Generating the keys takes longer than any test, so the PKIs are built once
    std::unique_ptr<CastTestPki> m_pki;
    std::unique_ptr<CastTestPki> m_otherPki;

  private Q_SLOTS:
    void initTestCase()
    {
        m_pki = std::make_unique<CastTestPki>();
        m_otherPki = std::make_unique<CastTestPki>();
        QVERIFY(m_pki->isValid());
        QVERIFY(m_otherPki->isValid());
    }

    void authenticatesFirst()
    {
        Rig rig(*m_pki, options());
        QSignalSpy states(&rig.channel, &CastChannel::stateChanged);
        QCOMPARE(rig.channel.state(), State::Idle);
        QVERIFY(rig.open());
        QCOMPARE(rig.channel.state(), State::Open);
        QVERIFY(rig.failed.isEmpty());

        // The challenge is the first frame and the only one before the channel opened
        QCOMPARE(rig.device.connections, 1);
        verifyOnlyTheChallenge(rig.device);
        const Kinefin::CastMessage &challenge = rig.device.received.constFirst().message;
        QCOMPARE(challenge.sourceId, kSender);
        QCOMPARE(challenge.destinationId, kReceiver);
        QVERIFY(challenge.binary);
        QCOMPARE(rig.device.lastNonce.size(), 16);
        QCOMPARE(challenge.payload, Kinefin::encodeAuthChallenge(rig.device.lastNonce));

        QList<State> seen;
        for (const QList<QVariant> &arguments : states)
            seen.append(qvariant_cast<State>(arguments.at(0)));
        QCOMPARE(seen, (QList<State>{State::Connecting, State::Authenticating, State::Open}));

        rig.channel.connectTo(kReceiver);
        QTRY_COMPARE(rig.device.receivedCount(), 2);
        const Kinefin::CastMessage &connectFrame = rig.device.received.at(1).message;
        QCOMPARE(connectFrame.ns, kConnectionNs);
        QCOMPARE(connectFrame.sourceId, kSender);
        QCOMPARE(connectFrame.destinationId, kReceiver);
        QVERIFY(!connectFrame.binary);
        QCOMPARE(connectFrame.payload,
                 QByteArrayLiteral(R"({"type":"CONNECT","userAgent":"Kinefin"})"));
    }

    void failsClosedSendingOnlyTheChallenge()
    {
        Rig rig(*m_pki, options(), Mode::Tampered);
        rig.channel.open();
        QTRY_COMPARE(rig.failed.size(), 1);
        QCOMPARE(reasonOf(rig.failed.constFirst()), Reason::AuthRefused);
        QCOMPARE(detailOf(rig.failed.constFirst()), QStringLiteral("BadSignature"));
        QCOMPARE(rig.channel.state(), State::Failed);
        QVERIFY(rig.opened.isEmpty());
        QVERIFY(!rig.channel.send(kReceiver, kReceiverNs, typed(kGetStatus)));
        QCOMPARE(rig.channel.request(kReceiver, kReceiverNs, typed(kGetStatus)), quint32(0));

        QTest::qWait(150);
        QCOMPARE(rig.failed.size(), 1);
        verifyOnlyTheChallenge(rig.device);
    }

    void authErrorRefused()
    {
        Rig rig(*m_pki, options(), Mode::Error);
        rig.channel.open();
        QTRY_COMPARE(rig.failed.size(), 1);
        QCOMPARE(reasonOf(rig.failed.constFirst()), Reason::AuthRefused);
        QCOMPARE(detailOf(rig.failed.constFirst()), QStringLiteral("DeviceError"));
        QCOMPARE(rig.channel.state(), State::Failed);
        verifyOnlyTheChallenge(rig.device);
    }

    void authTimeout()
    {
        CastChannel::Options quick = options();
        quick.authTimeoutMs = 200;
        Rig rig(*m_pki, quick, Mode::Manual);
        rig.channel.open();
        QTRY_COMPARE(rig.failed.size(), 1);
        QCOMPARE(reasonOf(rig.failed.constFirst()), Reason::AuthTimeout);
        QCOMPARE(rig.channel.state(), State::Failed);
        QCOMPARE(rig.device.challenges, 1);
        verifyOnlyTheChallenge(rig.device);
    }

    void unknownRootRefused()
    {
        CastChannel::Options other = options();
        other.trust = m_otherPki->trust();
        Rig rig(*m_pki, other);
        rig.channel.open();
        QTRY_COMPARE(rig.failed.size(), 1);
        QCOMPARE(reasonOf(rig.failed.constFirst()), Reason::AuthRefused);
        QCOMPARE(detailOf(rig.failed.constFirst()), QStringLiteral("BadChain"));
        QVERIFY(rig.opened.isEmpty());
        verifyOnlyTheChallenge(rig.device);
    }

    void framesBeforeTheAnswerAreDropped()
    {
        Rig rig(*m_pki, options(), Mode::Manual);
        rig.channel.open();
        QTRY_COMPARE(rig.device.challenges, 1);
        QCOMPARE(rig.channel.state(), State::Authenticating);

        // Frames are processed in order, so once the channel opened all of these were seen and dropped
        rig.device.push(kReceiver, kReceiverNs, typed(QStringLiteral("RECEIVER_STATUS")));
        rig.device.push(kTransport, kMediaNs, typed(QStringLiteral("MEDIA_STATUS")));
        rig.device.push(kReceiver, kConnectionNs, typed(kClose));
        rig.device.sendPing();
        rig.device.answerAuth();
        QVERIFY(rig.opened.wait(5000));
        QVERIFY(rig.messages.isEmpty());
        QVERIFY(rig.failed.isEmpty());
        // No PONG either: nothing is sent before the device passed
        verifyOnlyTheChallenge(rig.device);
    }

    void preAuthFramesCountTowardTheFloodCap()
    {
        CastChannel::Options tight = options();
        tight.floodFrames = 5;
        Rig rig(*m_pki, tight, Mode::Manual);
        rig.channel.open();
        QTRY_COMPARE(rig.device.challenges, 1);
        rig.device.flood(10);
        QTRY_COMPARE(rig.failed.size(), 1);
        QCOMPARE(reasonOf(rig.failed.constFirst()), Reason::Flood);
        verifyOnlyTheChallenge(rig.device);
    }

    void answersPings()
    {
        Rig rig(*m_pki, options());
        QVERIFY(rig.open());
        rig.device.sendPing();
        rig.device.sendPing(kTransport);
        QTRY_COMPARE(rig.device.count(kHeartbeatNs, kPong), 2);
        const QList<FakeCastDevice::Received> pongs = rig.device.frames(kHeartbeatNs, kPong);
        for (const FakeCastDevice::Received &pong : pongs) {
            QCOMPARE(pong.message.sourceId, kSender);
            QVERIFY(!pong.message.binary);
            QCOMPARE(pong.message.payload, QByteArrayLiteral(R"({"type":"PONG"})"));
        }
        QCOMPARE(pongs.at(0).message.destinationId, kReceiver);
        QCOMPARE(pongs.at(1).message.destinationId, kTransport);
        QVERIFY(rig.messages.isEmpty());
    }

    void sendsPingsToEveryVirtualConnection()
    {
        CastChannel::Options fast = options();
        fast.pingIntervalMs = 100;
        Rig rig(*m_pki, fast);
        QVERIFY(rig.open());
        rig.channel.connectTo(kReceiver);
        rig.channel.connectTo(kTransport);
        QTRY_COMPARE(rig.device.count(kConnectionNs, kConnect), 2);
        QTRY_VERIFY(rig.device.count(kHeartbeatNs, kPing, kTransport) >= 3);
        const int toReceiver = rig.device.count(kHeartbeatNs, kPing, kReceiver);
        const int toTransport = rig.device.count(kHeartbeatNs, kPing, kTransport);
        // receiver-0 is pinged once a round although it has a virtual connection too
        QVERIFY(qAbs(toReceiver - toTransport) <= 1);
        const FakeCastDevice::Received ping =
            rig.device.frames(kHeartbeatNs, kPing, kTransport).constFirst();
        QCOMPARE(ping.message.sourceId, kSender);
        QCOMPARE(ping.message.payload, QByteArrayLiteral(R"({"type":"PING"})"));

        // After closeTo the transport hears nothing more, receiver-0 keeps being pinged
        rig.channel.closeTo(kTransport);
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose, kTransport), 1);
        const int transportBefore = rig.device.count(kHeartbeatNs, kPing, kTransport);
        const int receiverBefore = rig.device.count(kHeartbeatNs, kPing, kReceiver);
        QTRY_VERIFY(rig.device.count(kHeartbeatNs, kPing, kReceiver) >= receiverBefore + 2);
        QCOMPARE(rig.device.count(kHeartbeatNs, kPing, kTransport), transportBefore);
        QVERIFY(rig.failed.isEmpty());
    }

    void heartbeatLossFails()
    {
        CastChannel::Options silent = options();
        silent.pingIntervalMs = 100;
        silent.livenessMs = 300;
        Rig rig(*m_pki, silent, Mode::AnswerThenSilent);
        QVERIFY(rig.open());
        QTRY_COMPARE(rig.failed.size(), 1);
        QCOMPARE(reasonOf(rig.failed.constFirst()), Reason::Lost);
        QCOMPARE(detailOf(rig.failed.constFirst()), QStringLiteral("silent"));
        QCOMPARE(rig.channel.state(), State::Failed);
        QVERIFY(rig.device.count(kHeartbeatNs, kPing) >= 1);
    }

    void heartbeatKeepsTheChannelAlive()
    {
        CastChannel::Options alive = options();
        alive.pingIntervalMs = 100;
        alive.livenessMs = 300;
        Rig rig(*m_pki, alive);
        QVERIFY(rig.open());
        // Three times the liveness window: only the PONGs of the device keep the channel up
        QTest::qWait(900);
        QCOMPARE(rig.channel.state(), State::Open);
        QVERIFY(rig.failed.isEmpty());
        QVERIFY(rig.messages.isEmpty());
    }

    void requestAssignsAndMatchesIds()
    {
        Rig rig(*m_pki, options());
        QVERIFY(rig.open());
        QSignalSpy timedOut(&rig.channel, &CastChannel::requestTimedOut);
        const QJsonObject body = typed(kGetStatus);
        const quint32 first = rig.channel.request(kReceiver, kReceiverNs, body, 1000);
        const quint32 second = rig.channel.request(kReceiver, kReceiverNs, body, 1000);
        const quint32 third = rig.channel.request(kReceiver, kReceiverNs, body, 1000);
        QCOMPARE(first, quint32(1));
        QCOMPARE(second, quint32(2));
        QCOMPARE(third, quint32(3));
        QVERIFY(rig.channel.send(kReceiver, kReceiverNs, body));

        QTRY_COMPARE(rig.device.count(kReceiverNs, kGetStatus), 4);
        const QList<FakeCastDevice::Received> sent = rig.device.frames(kReceiverNs);
        const QString requestId = QStringLiteral("requestId");
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(FakeCastDevice::jsonOf(sent.at(i).message).value(requestId).toInt(), i + 1);
            QCOMPARE(sent.at(i).message.destinationId, kReceiver);
        }
        QVERIFY(!FakeCastDevice::jsonOf(sent.at(3).message).contains(requestId));

        // A reply ends the wait of its id only; 0 (a spontaneous status) and a fraction are no reply. Each one is still delivered
        rig.device.push(kReceiver, kReceiverNs, reply(QStringLiteral("RECEIVER_STATUS"), 1));
        rig.device.push(kReceiver, kReceiverNs, typed(QStringLiteral("MEDIA_STATUS")));
        QJsonObject fraction = typed(QStringLiteral("MEDIA_STATUS"));
        fraction.insert(requestId, 2.5);
        rig.device.push(kReceiver, kReceiverNs, fraction);
        rig.device.push(kReceiver, kReceiverNs, reply(QStringLiteral("MEDIA_STATUS"), 0));
        QTRY_COMPARE(rig.messages.size(), 4);
        QCOMPARE(qvariant_cast<QJsonObject>(rig.messages.at(0).at(2)).value(requestId).toInt(), 1);

        QTRY_COMPARE(timedOut.size(), 2);
        QList<quint32> expired{timedOut.at(0).at(0).toUInt(), timedOut.at(1).at(0).toUInt()};
        std::sort(expired.begin(), expired.end());
        // The id 1 would have expired before the others had it stayed pending
        QCOMPARE(expired, (QList<quint32>{2, 3}));
    }

    void requestTimesOut()
    {
        Rig rig(*m_pki, options());
        QVERIFY(rig.open());
        QSignalSpy timedOut(&rig.channel, &CastChannel::requestTimedOut);
        const quint32 id = rig.channel.request(kReceiver, kReceiverNs, typed(kGetStatus), 150);
        QVERIFY(id != 0);
        QTRY_COMPARE(timedOut.size(), 1);
        QCOMPARE(timedOut.constFirst().constFirst().toUInt(), id);
        QTest::qWait(400);
        QCOMPARE(timedOut.size(), 1);

        // A late reply is still delivered and does not time anything out again
        rig.device.push(kReceiver, kReceiverNs, reply(QStringLiteral("RECEIVER_STATUS"), id));
        QTRY_COMPARE(rig.messages.size(), 1);
        QCOMPARE(timedOut.size(), 1);
        QCOMPARE(rig.channel.state(), State::Open);
    }

    void requestUsesTheDefaultTimeout()
    {
        CastChannel::Options quick = options();
        quick.requestTimeoutMs = 150;
        Rig rig(*m_pki, quick);
        QVERIFY(rig.open());
        QSignalSpy timedOut(&rig.channel, &CastChannel::requestTimedOut);
        const quint32 id = rig.channel.request(kReceiver, kReceiverNs, typed(kGetStatus));
        QVERIFY(id != 0);
        QTRY_COMPARE(timedOut.size(), 1);
        QCOMPARE(timedOut.constFirst().constFirst().toUInt(), id);
    }

    void requestCapOfPending()
    {
        CastChannel::Options capped = options();
        capped.maxPendingRequests = 3;
        Rig rig(*m_pki, capped);
        QVERIFY(rig.open());
        const QJsonObject body = typed(kGetStatus);
        for (int i = 0; i < 3; ++i)
            QVERIFY(rig.channel.request(kReceiver, kReceiverNs, body) != 0);
        QCOMPARE(rig.channel.request(kReceiver, kReceiverNs, body), quint32(0));

        // A reply frees a slot
        rig.device.push(kReceiver, kReceiverNs, reply(QStringLiteral("RECEIVER_STATUS"), 2));
        QTRY_COMPARE(rig.messages.size(), 1);
        QVERIFY(rig.channel.request(kReceiver, kReceiverNs, body) != 0);
        QCOMPARE(rig.channel.request(kReceiver, kReceiverNs, body), quint32(0));

        // The refused requests sent nothing
        QTRY_COMPARE(rig.device.count(kReceiverNs, kGetStatus), 4);
        QTest::qWait(100);
        QCOMPARE(rig.device.count(kReceiverNs, kGetStatus), 4);
    }

    void floodCloses()
    {
        CastChannel::Options tight = options();
        tight.floodFrames = 20;
        Rig rig(*m_pki, tight);
        QVERIFY(rig.open());
        rig.device.flood(50);
        QTRY_COMPARE(rig.failed.size(), 1);
        QCOMPARE(reasonOf(rig.failed.constFirst()), Reason::Flood);
        QCOMPARE(rig.channel.state(), State::Failed);
        QTest::qWait(100);
        QCOMPARE(rig.failed.size(), 1);
    }

    void oversizedLengthHeaderFails_data()
    {
        QTest::addColumn<quint32>("length");
        QTest::newRow("one over the cap") << quint32(Kinefin::kMaxCastFrameBytes + 1);
        QTest::newRow("four gigabytes") << quint32(0xFFFFFFFF);
        QTest::newRow("zero") << quint32(0);
    }

    void oversizedLengthHeaderFails()
    {
        QFETCH(quint32, length);
        Rig rig(*m_pki, options());
        QVERIFY(rig.open());
        rig.device.sendLengthHeader(length);
        QTRY_COMPARE(rig.failed.size(), 1);
        QCOMPARE(reasonOf(rig.failed.constFirst()), Reason::Protocol);
        QCOMPARE(rig.channel.state(), State::Failed);
    }

    void malformedFrameFails_data()
    {
        QTest::addColumn<QByteArray>("frame");
        // A valid protocol_version and nothing else
        QTest::newRow("missing fields") << QByteArray::fromHex("000000020800");
        QTest::newRow("cut short varint") << QByteArray::fromHex("00000003ffffff");
    }

    void malformedFrameFails()
    {
        QFETCH(QByteArray, frame);
        Rig rig(*m_pki, options());
        QVERIFY(rig.open());
        rig.device.pushRaw(frame);
        QTRY_COMPARE(rig.failed.size(), 1);
        QCOMPARE(reasonOf(rig.failed.constFirst()), Reason::Protocol);
        QCOMPARE(rig.channel.state(), State::Failed);
    }

    void messageReceivedForJson()
    {
        Rig rig(*m_pki, options());
        QVERIFY(rig.open());
        // None of these is delivered: heartbeat frames, payloads that are not a JSON object and a BINARY frame
        rig.device.push(kReceiver, kHeartbeatNs, typed(kPong));
        rig.device.sendPing();
        rig.device.pushText(kReceiver, kReceiverNs, QByteArrayLiteral("not json"));
        rig.device.pushText(kReceiver, kReceiverNs, QByteArrayLiteral("[1,2]"));
        rig.device.pushText(kReceiver, kReceiverNs, QByteArrayLiteral("\"a string\""));
        rig.device.pushBinary(kReceiver, kMediaNs, QByteArrayLiteral(R"({"type":"BINARY"})"));
        // Delivered: an object, on any namespace but the heartbeat
        QJsonObject status = typed(QStringLiteral("RECEIVER_STATUS"));
        status.insert(QStringLiteral("status"), QJsonObject{{QStringLiteral("level"), 0.5}});
        rig.device.push(kReceiver, kReceiverNs, status);
        rig.device.push(kTransport, kMediaNs, typed(QStringLiteral("MEDIA_STATUS")));
        const QString multizone = QStringLiteral("urn:x-cast:com.google.cast.multizone");
        rig.device.push(kReceiver, multizone, typed(QStringLiteral("MULTIZONE_STATUS")));

        QTRY_COMPARE(rig.messages.size(), 3);
        QCOMPARE(rig.messages.at(0).at(0).toString(), kReceiver);
        QCOMPARE(rig.messages.at(0).at(1).toString(), kReceiverNs);
        QCOMPARE(qvariant_cast<QJsonObject>(rig.messages.at(0).at(2)), status);
        QCOMPARE(rig.messages.at(1).at(0).toString(), kTransport);
        QCOMPARE(rig.messages.at(1).at(1).toString(), kMediaNs);
        QCOMPARE(rig.messages.at(2).at(1).toString(), multizone);
        QCOMPARE(rig.channel.state(), State::Open);
        QVERIFY(rig.failed.isEmpty());
        // The PING was answered
        QTRY_COMPARE(rig.device.count(kHeartbeatNs, kPong), 1);
    }

    void virtualConnectionClosedSignal()
    {
        CastChannel::Options fast = options();
        fast.pingIntervalMs = 100;
        Rig rig(*m_pki, fast);
        QVERIFY(rig.open());
        QSignalSpy closed(&rig.channel, &CastChannel::virtualConnectionClosed);
        rig.channel.connectTo(kTransport);
        QTRY_VERIFY(rig.device.count(kHeartbeatNs, kPing, kTransport) >= 1);

        rig.device.push(kTransport, kConnectionNs, typed(kClose));
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(closed.constFirst().constFirst().toString(), kTransport);

        // The connection is forgotten: no heartbeat to it and no CLOSE of ours in answer
        QTest::qWait(250);
        const int pings = rig.device.count(kHeartbeatNs, kPing, kTransport);
        QTest::qWait(350);
        QCOMPARE(rig.device.count(kHeartbeatNs, kPing, kTransport), pings);
        rig.channel.closeTo(kTransport);
        QTest::qWait(100);
        QCOMPARE(rig.device.count(kConnectionNs, kClose), 0);
        QCOMPARE(rig.channel.state(), State::Open);
    }

    void closeSendsCloseToEveryConnection()
    {
        Rig rig(*m_pki, options());
        QVERIFY(rig.open());
        rig.channel.connectTo(kReceiver);
        rig.channel.connectTo(kTransport);
        rig.channel.close();
        QCOMPARE(rig.channel.state(), State::Closed);
        QTRY_COMPARE(rig.device.count(kConnectionNs, kClose), 2);
        QCOMPARE(rig.device.count(kConnectionNs, kClose, kReceiver), 1);
        QCOMPARE(rig.device.count(kConnectionNs, kClose, kTransport), 1);
        const FakeCastDevice::Received closeFrame =
            rig.device.frames(kConnectionNs, kClose).constFirst();
        QCOMPARE(closeFrame.message.sourceId, kSender);
        QCOMPARE(closeFrame.message.payload, QByteArrayLiteral(R"({"type":"CLOSE"})"));

        QVERIFY(!rig.channel.send(kReceiver, kReceiverNs, typed(kGetStatus)));
        QCOMPARE(rig.channel.request(kReceiver, kReceiverNs, typed(kGetStatus)), quint32(0));
        rig.channel.close();
        QTest::qWait(100);
        QVERIFY(rig.failed.isEmpty());
        QCOMPARE(rig.channel.state(), State::Closed);
    }

    void deviceClosingTheConnectionIsLost_data()
    {
        QTest::addColumn<bool>("afterAuth");
        QTest::newRow("while authenticating") << false;
        QTest::newRow("while open") << true;
    }

    void deviceClosingTheConnectionIsLost()
    {
        QFETCH(bool, afterAuth);
        Rig rig(*m_pki, options(), afterAuth ? Mode::Answer : Mode::Manual);
        if (afterAuth) {
            QVERIFY(rig.open());
        } else {
            rig.channel.open();
            QTRY_COMPARE(rig.device.challenges, 1);
        }
        rig.device.closeConnection();
        QTRY_COMPARE(rig.failed.size(), 1);
        QCOMPARE(reasonOf(rig.failed.constFirst()), Reason::Lost);
        QCOMPARE(rig.channel.state(), State::Failed);
    }

    void connectFailsWhenNothingListens()
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();

        CastChannel channel(QHostAddress::LocalHost, port, options());
        QSignalSpy opened(&channel, &CastChannel::opened);
        QSignalSpy failed(&channel, &CastChannel::failed);
        channel.open();
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(reasonOf(failed.constFirst()), Reason::ConnectFailed);
        QVERIFY(!detailOf(failed.constFirst()).isEmpty());
        QCOMPARE(channel.state(), State::Failed);
        QVERIFY(opened.isEmpty());
    }

    void connectTimesOutOnASilentServer()
    {
        // The kernel completes the TCP handshake, and nothing ever answers the ClientHello
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
        CastChannel::Options quick = options();
        quick.connectTimeoutMs = 200;
        CastChannel channel(QHostAddress::LocalHost, silent.serverPort(), quick);
        QSignalSpy failed(&channel, &CastChannel::failed);
        channel.open();
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(reasonOf(failed.constFirst()), Reason::ConnectFailed);
        QCOMPARE(detailOf(failed.constFirst()), QStringLiteral("timeout"));
    }

    void defaultTlsConfigurationUntouched()
    {
        const QSslSocket::PeerVerifyMode before =
            QSslConfiguration::defaultConfiguration().peerVerifyMode();
        QVERIFY(before != QSslSocket::VerifyNone);

        Rig rig(*m_pki, options());
        QVERIFY(rig.open());
        QCOMPARE(QSslConfiguration::defaultConfiguration().peerVerifyMode(), before);

        // A socket made afterwards still verifies: it refuses the device's certificate
        QSslSocket other;
        QVERIFY(other.sslConfiguration().peerVerifyMode() != QSslSocket::VerifyNone);
        QSignalSpy sslErrors(&other, &QSslSocket::sslErrors);
        QSignalSpy encrypted(&other, &QSslSocket::encrypted);
        other.connectToHostEncrypted(QStringLiteral("127.0.0.1"), rig.device.port());
        QTRY_COMPARE(sslErrors.size(), 1);
        QVERIFY(encrypted.isEmpty());
        other.abort();
    }

    void neverSendsBeforeOpen()
    {
        Rig rig(*m_pki, options(), Mode::Manual);
        const QJsonObject body = typed(kGetStatus);
        QVERIFY(!rig.channel.send(kReceiver, kReceiverNs, body));
        QCOMPARE(rig.channel.request(kReceiver, kReceiverNs, body), quint32(0));
        rig.channel.connectTo(kReceiver);
        rig.channel.closeTo(kReceiver);

        rig.channel.open();
        QTRY_COMPARE(rig.device.challenges, 1);
        QCOMPARE(rig.channel.state(), State::Authenticating);
        QVERIFY(!rig.channel.send(kReceiver, kReceiverNs, body));
        QCOMPARE(rig.channel.request(kReceiver, kReceiverNs, body), quint32(0));
        rig.channel.connectTo(kReceiver);
        rig.channel.closeTo(kReceiver);
        QTest::qWait(50);

        rig.device.answerAuth();
        QVERIFY(rig.opened.wait(5000));
        QTest::qWait(50);
        verifyOnlyTheChallenge(rig.device);
    }

    void destructionIsSilent()
    {
        FakeCastDevice device(*m_pki);
        QVERIFY(device.listen());
        auto channel =
            std::make_unique<CastChannel>(QHostAddress::LocalHost, device.port(), options());
        QSignalSpy opened(channel.get(), &CastChannel::opened);
        QSignalSpy states(channel.get(), &CastChannel::stateChanged);
        QSignalSpy failed(channel.get(), &CastChannel::failed);
        channel->open();
        QVERIFY(opened.wait(5000));
        const qsizetype changes = states.size();

        channel.reset();
        QTest::qWait(150);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(states.size(), changes);
        QCOMPARE(device.connections, 1);
    }

    // Opt-in against a real device: KINEFIN_CAST_LIVE_HOST is the address of a Google Cast device. It passes the device authentication with the shipped roots and answers a GET_STATUS; nothing is cast
    void liveDevice()
    {
        const QHostAddress host(qEnvironmentVariable("KINEFIN_CAST_LIVE_HOST"));
        if (host.isNull())
            QSKIP("no KINEFIN_CAST_LIVE_HOST, skipping the live check");
        CastChannel channel(host, 8009);
        QSignalSpy opened(&channel, &CastChannel::opened);
        QSignalSpy failed(&channel, &CastChannel::failed);
        QSignalSpy messages(&channel, &CastChannel::messageReceived);
        channel.open();
        QTRY_VERIFY_WITH_TIMEOUT(opened.count() == 1 || failed.count() == 1, 15'000);
        if (failed.count() == 1)
            QFAIL(qPrintable(failed.first().at(1).toString()));
        channel.connectTo(QString::fromLatin1(Kinefin::kCastPlatformId));
        QVERIFY(channel.request(QString::fromLatin1(Kinefin::kCastPlatformId),
                                QString::fromLatin1(Kinefin::kCastNsReceiver),
                                {{QStringLiteral("type"), QStringLiteral("GET_STATUS")}})
                != 0);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(messages.cbegin(), messages.cend(),
                                             [](const QList<QVariant> &arguments) {
                                                 return arguments.at(2).toJsonObject().value(
                                                            QStringLiteral("type"))
                                                        == QStringLiteral("RECEIVER_STATUS");
                                             }),
                                 10'000);
        channel.close();
        QCOMPARE(channel.state(), State::Closed);
    }
};

QTEST_GUILESS_MAIN(TestCastChannel)
#include "test_cast_channel.moc"
