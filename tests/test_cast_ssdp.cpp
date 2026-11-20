// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/Ssdp.h"

#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QUdpSocket>
#include <QtTest>

using namespace Kinefin;

namespace {
const QString kRendererType = QStringLiteral("urn:schemas-upnp-org:device:MediaRenderer:1");

bool allowLoopback(const QHostAddress &address)
{
    return address.isLoopback();
}

QByteArray header(const char *name, const QString &value)
{
    return QByteArray(name) + ": " + value.toLatin1() + "\r\n";
}

QByteArray response(const QString &location,
                    const QString &usn = QStringLiteral("uuid:abc-123::") + kRendererType,
                    const QString &st = kRendererType,
                    const QString &cacheControl = QStringLiteral("max-age=1800"))
{
    QByteArray out = "HTTP/1.1 200 OK\r\n";
    if (!cacheControl.isEmpty())
        out.append(header("CACHE-CONTROL", cacheControl));
    out.append("EXT:\r\n");
    if (!location.isEmpty())
        out.append(header("LOCATION", location));
    if (!st.isEmpty())
        out.append(header("ST", st));
    if (!usn.isEmpty())
        out.append(header("USN", usn));
    out.append("\r\n");
    return out;
}

const QHostAddress kSender(QStringLiteral("192.168.1.20"));
const QString kLocation = QStringLiteral("http://192.168.1.20:49152/desc.xml");

// A unicast socket that answers every datagram it receives with the given replies, standing in for renderers
class FakeRenderer {
  public:
    explicit FakeRenderer(const QHostAddress &address = QHostAddress::LocalHost)
        : m_address(address)
    {
        QObject::connect(&m_socket, &QUdpSocket::readyRead, &m_socket, [this] {
            while (m_socket.hasPendingDatagrams()) {
                const QNetworkDatagram datagram = m_socket.receiveDatagram();
                received.append(datagram.data());
                lastSenderPort = datagram.senderPort();
                for (const QByteArray &reply : std::as_const(replies))
                    m_socket.writeDatagram(reply, datagram.senderAddress(), datagram.senderPort());
            }
        });
    }
    bool listen() { return m_socket.bind(m_address, 0); }
    quint16 port() const { return m_socket.localPort(); }
    void sendTo(quint16 targetPort, const QByteArray &data)
    {
        m_socket.writeDatagram(data, QHostAddress::LocalHost, targetPort);
    }

    QList<QByteArray> replies;
    QList<QByteArray> received;
    quint16 lastSenderPort = 0;

  private:
    QHostAddress m_address;
    QUdpSocket m_socket;
};

SsdpSearch::Options loopbackOptions(quint16 port)
{
    SsdpSearch::Options options;
    options.target = QHostAddress::LocalHost;
    options.port = port;
    options.windowMs = 500;
    options.repeatMs = 100;
    options.senderPolicy = allowLoopback;
    return options;
}

QByteArray loopbackResponse(const QString &udn, const QString &host = QStringLiteral("127.0.0.1"),
                            const QString &st = kRendererType)
{
    return response(QStringLiteral("http://%1:49152/desc.xml").arg(host),
                    udn + QStringLiteral("::") + kRendererType, st);
}
} // namespace

class TestCastSsdp : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void parsesAValidResponse()
    {
        const auto parsed = parseSsdpResponse(response(kLocation), kSender);
        QVERIFY(parsed);
        QCOMPARE(parsed->udn, QStringLiteral("uuid:abc-123"));
        QCOMPARE(parsed->searchTarget, kRendererType);
        QCOMPARE(parsed->location, QUrl(kLocation));
        QCOMPARE(parsed->sender, kSender);
        QCOMPARE(parsed->maxAgeSeconds, 1800);
    }

    void headerNamesAreCaseInsensitiveAndTheFirstWins()
    {
        const QByteArray datagram =
            "HTTP/1.1 200 OK\r\nlocation: " + kLocation.toLatin1()
            + "\r\nLocation: http://192.168.1.21/other.xml\r\nst: " + kRendererType.toLatin1()
            + "\r\nUsn: uuid:abc::" + kRendererType.toLatin1() + "\r\n\r\n";
        const auto parsed = parseSsdpResponse(datagram, kSender);
        QVERIFY(parsed);
        QCOMPARE(parsed->location, QUrl(kLocation));
    }

    void aBodyAfterTheHeadersIsIgnored()
    {
        QVERIFY(parseSsdpResponse(response(kLocation) + "<root>ignored</root>", kSender));
    }

    void requiresLocationStAndUsn()
    {
        QVERIFY(!parseSsdpResponse(response(QString()), kSender));
        QVERIFY(!parseSsdpResponse(response(kLocation, QStringLiteral("uuid:abc::x"), QString()),
                                   kSender));
        QVERIFY(!parseSsdpResponse(response(kLocation, QString()), kSender));
    }

    void locationMustBeTheSendersOrigin()
    {
        const QStringList bad{QStringLiteral("http://192.168.1.99:49152/desc.xml"),
                              QStringLiteral("http://tv.local:49152/desc.xml"),
                              QStringLiteral("https://192.168.1.20:49152/desc.xml"),
                              QStringLiteral("http://user:pw@192.168.1.20:49152/desc.xml"),
                              QStringLiteral("http://192.168.1.20:0/desc.xml"),
                              QStringLiteral("http://8.8.8.8/desc.xml"),
                              QStringLiteral("/desc.xml"),
                              QStringLiteral("ftp://192.168.1.20/desc.xml"),
                              QStringLiteral("http://[::1]/desc.xml"),
                              QStringLiteral("http://192.168.1.20:49152/a b")};
        for (const QString &location : bad)
            QVERIFY2(!parseSsdpResponse(response(location), kSender), qPrintable(location));
        QVERIFY(
            parseSsdpResponse(response(QStringLiteral("http://192.168.1.20/desc.xml")), kSender));
        QVERIFY(parseSsdpResponse(response(QStringLiteral("HTTP://192.168.1.20:80/d")), kSender));
        const QHostAddress v6(QStringLiteral("fe80::1"));
        QVERIFY(parseSsdpResponse(response(QStringLiteral("http://[fe80::1]:49152/d.xml")), v6));
        QVERIFY(!parseSsdpResponse(response(QStringLiteral("http://[fe80::2]:49152/d.xml")), v6));
        const QString longLocation =
            QStringLiteral("http://192.168.1.20/") + QString(600, QLatin1Char('a'));
        QVERIFY(!parseSsdpResponse(response(longLocation), kSender));
    }

    void rejectsSendersOutsideThePolicy()
    {
        for (const char *ip :
             {"8.8.8.8", "127.0.0.1", "224.0.0.251", "169.254.169.254", "0.0.0.0"}) {
            const QHostAddress sender{QString::fromLatin1(ip)};
            const QByteArray datagram =
                response(QStringLiteral("http://%1/d.xml").arg(QString::fromLatin1(ip)));
            QVERIFY2(!parseSsdpResponse(datagram, sender), ip);
        }
        // The policy is a parameter, for the tests that talk over loopback
        QVERIFY(parseSsdpResponse(response(QStringLiteral("http://127.0.0.1/d.xml")),
                                  QHostAddress::LocalHost, allowLoopback));
    }

    void addressPolicy()
    {
        for (const char *ip : {"10.0.0.1", "10.255.255.255", "172.16.0.1", "172.31.255.255",
                               "192.168.1.1", "169.254.1.1", "169.254.169.253", "fe80::1",
                               "fd12:3456::1", "fc00::1", "::ffff:192.168.1.5"}) {
            QVERIFY2(isAllowedRendererAddress(QHostAddress(QString::fromLatin1(ip))), ip);
        }
        for (const char *ip :
             {"127.0.0.1", "0.0.0.0", "8.8.8.8", "172.15.255.255", "172.32.0.1", "169.254.169.254",
              "224.0.0.251", "239.255.255.250", "255.255.255.255", "192.169.0.1", "::1",
              "::", "ff02::1", "2001:db8::1", "fd00:ec2::254", "::ffff:8.8.8.8", "::ffff:127.0.0.1",
              "::ffff:169.254.169.254"}) {
            QVERIFY2(!isAllowedRendererAddress(QHostAddress(QString::fromLatin1(ip))), ip);
        }
        QVERIFY(!isAllowedRendererAddress(QHostAddress()));
    }

    void datagramSizeAndShape()
    {
        // Exactly the limit passes, one byte more does not
        QByteArray padded = response(kLocation);
        padded.chop(2);
        padded += "X-PAD: ";
        padded += QByteArray(kMaxSsdpDatagramBytes - padded.size() - 2, 'a');
        padded += "\r\n";
        QCOMPARE(padded.size(), int(kMaxSsdpDatagramBytes));
        QVERIFY(parseSsdpResponse(padded, kSender));
        QVERIFY(!parseSsdpResponse(padded + "a", kSender));

        QVERIFY(!parseSsdpResponse(QByteArray(), kSender));
        QVERIFY(!parseSsdpResponse("HTTP/1.1 404 Not Found\r\n\r\n", kSender));
        QVERIFY(!parseSsdpResponse("HTTP/1.0 200 OK\r\n\r\n", kSender));
        QVERIFY(!parseSsdpResponse("HTTP/1.1 2000 OK\r\n\r\n", kSender));
        QVERIFY(!parseSsdpResponse("NOTIFY * HTTP/1.1\r\n\r\n", kSender));
        // A bare LF, a bare CR and a NUL drop the datagram
        QByteArray lf = response(kLocation);
        lf.replace("EXT:", "EXT:\n");
        QVERIFY(!parseSsdpResponse(lf, kSender));
        QByteArray cr = response(kLocation);
        cr.replace("EXT:", "EXT:\rx");
        QVERIFY(!parseSsdpResponse(cr, kSender));
        QByteArray nul = response(kLocation);
        nul.replace("EXT:", QByteArray("EXT:\0", 5));
        QVERIFY(!parseSsdpResponse(nul, kSender));
    }

    void maxAge()
    {
        const auto age = [](const QString &cacheControl) {
            return parseSsdpResponse(
                response(kLocation, QStringLiteral("uuid:abc::x"), kRendererType, cacheControl),
                kSender);
        };
        QCOMPARE(age(QStringLiteral("max-age=60"))->maxAgeSeconds, 60);
        QCOMPARE(age(QStringLiteral("max-age = 120"))->maxAgeSeconds, 120);
        QCOMPARE(age(QStringLiteral("no-cache, MAX-AGE=90"))->maxAgeSeconds, 90);
        QCOMPARE(age(QStringLiteral("max-age=99999"))->maxAgeSeconds, kSsdpMaxAgeCapSeconds);
        QCOMPARE(age(QString())->maxAgeSeconds, kSsdpMaxAgeCapSeconds);
        QCOMPARE(age(QStringLiteral("no-cache"))->maxAgeSeconds, kSsdpMaxAgeCapSeconds);
        QVERIFY(!age(QStringLiteral("max-age=0")));
    }

    void udnFromTheUsn()
    {
        const auto udnOf = [](const QString &usn) -> QString {
            const auto parsed = parseSsdpResponse(response(kLocation, usn), kSender);
            return parsed ? parsed->udn : QString();
        };
        QCOMPARE(udnOf(QStringLiteral("uuid:ABC-123::upnp:rootdevice")),
                 QStringLiteral("uuid:abc-123"));
        QCOMPARE(udnOf(QStringLiteral("uuid:RINCON_000E58:1400")),
                 QStringLiteral("uuid:rincon_000e58:1400"));
        QCOMPARE(udnOf(QStringLiteral("uuid:abc")), QStringLiteral("uuid:abc"));
        QVERIFY(udnOf(QStringLiteral("abc::x")).isEmpty());
        QVERIFY(udnOf(QStringLiteral("uuid:::x")).isEmpty());
        QVERIFY(udnOf(QStringLiteral("uuid:a b::x")).isEmpty());
        QVERIFY(udnOf(QStringLiteral("uuid:a<b>::x")).isEmpty());
        QVERIFY(!udnOf(QStringLiteral("uuid:") + QString(128, QLatin1Char('a'))).isEmpty());
        QVERIFY(udnOf(QStringLiteral("uuid:") + QString(129, QLatin1Char('a'))).isEmpty());
        QVERIFY(!normalizeUdn(u"  UUID:abc ").value_or(QString()).isEmpty());
    }

    void searchRequestShape()
    {
        const QByteArray request = ssdpSearchRequest();
        QVERIFY(request.startsWith("M-SEARCH * HTTP/1.1\r\n"));
        QVERIFY(request.contains("HOST: 239.255.255.250:1900\r\n"));
        QVERIFY(request.contains("MAN: \"ssdp:discover\"\r\n"));
        QVERIFY(request.contains("MX: 2\r\n"));
        QVERIFY(request.contains("ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"));
        QVERIFY(request.endsWith("\r\n\r\n"));
    }

    void searchReportsEachDeviceOnceAndSendsTwice()
    {
        FakeRenderer renderer;
        QVERIFY(renderer.listen());
        renderer.replies = {loopbackResponse(QStringLiteral("uuid:one")),
                            loopbackResponse(QStringLiteral("uuid:two"))};
        SsdpSearch search(loopbackOptions(renderer.port()));
        QSignalSpy found(&search, &SsdpSearch::deviceFound);
        QSignalSpy finished(&search, &SsdpSearch::finished);
        QVERIFY(search.start());
        QVERIFY(search.running());
        QVERIFY(finished.wait(3000));
        QVERIFY(!search.running());
        // Two M-SEARCH sends answered twice each: still two devices
        QCOMPARE(renderer.received.size(), 2);
        QVERIFY(
            renderer.received.at(0).contains("ST: urn:schemas-upnp-org:device:MediaRenderer:1"));
        QCOMPARE(found.size(), 2);
        QCOMPARE(found.at(0).at(0).value<SsdpResponse>().udn, QStringLiteral("uuid:one"));
        QCOMPARE(found.at(1).at(0).value<SsdpResponse>().udn, QStringLiteral("uuid:two"));
    }

    void searchIgnoresAnotherAddressForTheSameUdn()
    {
        FakeRenderer renderer;
        FakeRenderer other{QHostAddress(QStringLiteral("127.0.0.2"))};
        QVERIFY(renderer.listen());
        if (!other.listen())
            QSKIP("127.0.0.2 cannot be bound here");
        renderer.replies = {loopbackResponse(QStringLiteral("uuid:one"))};
        SsdpSearch search(loopbackOptions(renderer.port()));
        QSignalSpy found(&search, &SsdpSearch::deviceFound);
        QSignalSpy finished(&search, &SsdpSearch::finished);
        QVERIFY(search.start());
        QTRY_VERIFY(!renderer.received.isEmpty());
        other.sendTo(renderer.lastSenderPort,
                     loopbackResponse(QStringLiteral("uuid:one"), QStringLiteral("127.0.0.2")));
        QVERIFY(finished.wait(3000));
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).at(0).value<SsdpResponse>().sender,
                 QHostAddress(QHostAddress::LocalHost));
    }

    void searchStopsAtTheDeviceLimit()
    {
        FakeRenderer renderer;
        QVERIFY(renderer.listen());
        for (int i = 0; i < kMaxSsdpDevices + 8; ++i)
            renderer.replies.append(loopbackResponse(QStringLiteral("uuid:dev-%1").arg(i)));
        SsdpSearch search(loopbackOptions(renderer.port()));
        QSignalSpy found(&search, &SsdpSearch::deviceFound);
        QSignalSpy finished(&search, &SsdpSearch::finished);
        QVERIFY(search.start());
        QVERIFY(finished.wait(3000));
        QCOMPARE(found.size(), kMaxSsdpDevices);
    }

    void searchDropsOtherTargetsAndOversizedDatagrams()
    {
        FakeRenderer renderer;
        QVERIFY(renderer.listen());
        QByteArray oversized = loopbackResponse(QStringLiteral("uuid:big"));
        oversized.chop(2);
        oversized.append("X-PAD: ");
        oversized.append(QByteArray(kMaxSsdpDatagramBytes, 'a'));
        oversized.append("\r\n\r\n");
        renderer.replies = {loopbackResponse(QStringLiteral("uuid:root"),
                                             QStringLiteral("127.0.0.1"),
                                             QStringLiteral("upnp:rootdevice")),
                            oversized, loopbackResponse(QStringLiteral("uuid:ok"))};
        SsdpSearch search(loopbackOptions(renderer.port()));
        QSignalSpy found(&search, &SsdpSearch::deviceFound);
        QSignalSpy finished(&search, &SsdpSearch::finished);
        QVERIFY(search.start());
        QVERIFY(finished.wait(3000));
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).at(0).value<SsdpResponse>().udn, QStringLiteral("uuid:ok"));
    }

    // A spoofed SSDP sender off the local links is not believed: only an address in an interface's subnet passes
    void onlyASenderOnALocalSubnetIsBelieved()
    {
        QNetworkAddressEntry lan;
        lan.setIp(QHostAddress(QStringLiteral("192.168.1.10")));
        lan.setPrefixLength(24);
        QNetworkAddressEntry v6;
        v6.setIp(QHostAddress(QStringLiteral("fd12:3456:789a:1::10")));
        v6.setPrefixLength(64);
        const QList<QNetworkAddressEntry> entries{lan, v6};
        QVERIFY(Kinefin::isInAnySubnet(QHostAddress(QStringLiteral("192.168.1.77")), entries));
        QVERIFY(
            Kinefin::isInAnySubnet(QHostAddress(QStringLiteral("::ffff:192.168.1.77")), entries));
        QVERIFY(!Kinefin::isInAnySubnet(QHostAddress(QStringLiteral("192.168.2.77")), entries));
        QVERIFY(!Kinefin::isInAnySubnet(QHostAddress(QStringLiteral("172.17.0.2")), entries));
        QVERIFY(
            Kinefin::isInAnySubnet(QHostAddress(QStringLiteral("fd12:3456:789a:1::99")), entries));
        QVERIFY(
            !Kinefin::isInAnySubnet(QHostAddress(QStringLiteral("fd12:3456:789a:2::99")), entries));
        QVERIFY(!Kinefin::isInAnySubnet(QHostAddress(QStringLiteral("192.168.1.77")), {}));
        // The default policy adds the subnet to the address rules: a loopback or public sender never passes
        QVERIFY(!Kinefin::isLocalRendererAddress(QHostAddress(QStringLiteral("127.0.0.1"))));
        QVERIFY(!Kinefin::isLocalRendererAddress(QHostAddress(QStringLiteral("8.8.8.8"))));
    }

    void searchAppliesTheDefaultAddressPolicy()
    {
        FakeRenderer renderer;
        QVERIFY(renderer.listen());
        renderer.replies = {loopbackResponse(QStringLiteral("uuid:one"))};
        SsdpSearch::Options options = loopbackOptions(renderer.port());
        options.senderPolicy = isAllowedRendererAddress;
        options.windowMs = 300;
        SsdpSearch search(options);
        QSignalSpy found(&search, &SsdpSearch::deviceFound);
        QSignalSpy finished(&search, &SsdpSearch::finished);
        QVERIFY(search.start());
        QVERIFY(finished.wait(3000));
        QVERIFY(!renderer.received.isEmpty());
        QCOMPARE(found.size(), 0);
    }

    void aLateReplyAndAStoppedSearchReportNothing()
    {
        FakeRenderer renderer;
        QVERIFY(renderer.listen());
        SsdpSearch::Options options = loopbackOptions(renderer.port());
        options.windowMs = 200;
        SsdpSearch search(options);
        QSignalSpy found(&search, &SsdpSearch::deviceFound);
        QSignalSpy finished(&search, &SsdpSearch::finished);
        QVERIFY(search.start());
        QTRY_VERIFY(!renderer.received.isEmpty());
        const quint16 searchPort = renderer.lastSenderPort;
        QVERIFY(finished.wait(3000));
        renderer.sendTo(searchPort, loopbackResponse(QStringLiteral("uuid:late")));
        QTest::qWait(100);
        QCOMPARE(found.size(), 0);

        QVERIFY(search.start());
        search.stop();
        QVERIFY(!search.running());
        QTest::qWait(300);
        QCOMPARE(finished.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestCastSsdp)
#include "test_cast_ssdp.moc"
