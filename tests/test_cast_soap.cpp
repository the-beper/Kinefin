// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/Didl.h"
#include "cast/Soap.h"
#include "cast/XmlTree.h"

#include <QtTest>

#include <limits>

using namespace Kinefin;

namespace {
const QString kAvTransport = QStringLiteral("urn:schemas-upnp-org:service:AVTransport:1");

QByteArray envelope(const QString &payload,
                    const QString &ns = QStringLiteral("http://schemas.xmlsoap.org/soap/envelope/"))
{
    return QStringLiteral("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"%1\" "
                          "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/"
                          "\"><s:Body>%2</s:Body></s:Envelope>")
        .arg(ns, payload)
        .toUtf8();
}

QString responseElement(const QString &action, const QString &inner)
{
    return QStringLiteral("<u:%1Response xmlns:u=\"%2\">%3</u:%1Response>")
        .arg(action, kAvTransport, inner);
}

QByteArray fault(const QString &code, const QString &description)
{
    return envelope(
        QStringLiteral(
            "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
            "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>%1</errorCode>"
            "<errorDescription>%2</errorDescription></UPnPError></detail></s:Fault>")
            .arg(code, description));
}
} // namespace

class TestCastSoap : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void goldenEnvelope()
    {
        const QByteArray body =
            soapRequestBody(kAvTransport, QStringLiteral("Play"),
                            {{QStringLiteral("InstanceID"), QStringLiteral("0")},
                             {QStringLiteral("Speed"), QStringLiteral("1")}});
        QCOMPARE(body,
                 QByteArray(
                     "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                     "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                     "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                     "<s:Body><u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                     "<InstanceID>0</InstanceID><Speed>1</Speed></u:Play></s:Body></s:Envelope>"));
        const QByteArray noArguments = soapRequestBody(kAvTransport, QStringLiteral("Stop"), {});
        QVERIFY(noArguments.contains(
            "<u:Stop xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"></u:Stop>"));
        QCOMPARE(soapActionHeader(kAvTransport, QStringLiteral("Play")),
                 QByteArray("\"urn:schemas-upnp-org:service:AVTransport:1#Play\""));
        // A service type or an action that could break out of the header line gives no header at all
        QVERIFY(
            soapActionHeader(kAvTransport + QStringLiteral("\"\r\nX-A: b"), QStringLiteral("Play"))
                .isEmpty());
        QVERIFY(soapActionHeader(kAvTransport, QStringLiteral("Play\r\nX-A: b")).isEmpty());
        QCOMPARE(QString(kSoapContentType), QStringLiteral("text/xml; charset=\"utf-8\""));
    }

    void argumentOrderIsKeptAndValuesAreEscaped()
    {
        const QByteArray body =
            soapRequestBody(kAvTransport, QStringLiteral("SetAVTransportURI"),
                            {{QStringLiteral("InstanceID"), QStringLiteral("0")},
                             {QStringLiteral("CurrentURI"), QStringLiteral("http://h/s?a=1&b=<2>")},
                             {QStringLiteral("CurrentURIMetaData"), QStringLiteral("x%1%2\x01y")}});
        QVERIFY(body.contains(
            "<InstanceID>0</InstanceID><CurrentURI>http://h/s?a=1&amp;b=&lt;2&gt;</CurrentURI>"
            "<CurrentURIMetaData>x%1%2y</CurrentURIMetaData>"));
        const auto tree = parseXml(body, 1 << 16);
        QVERIFY(tree);
        const XmlNode *action = tree->child(u"Body")->child(u"SetAVTransportURI");
        QVERIFY(action);
        QCOMPARE(action->child(u"CurrentURI")->text, QStringLiteral("http://h/s?a=1&b=<2>"));
    }

    void invalidNamesGiveNoEnvelope()
    {
        QVERIFY(soapRequestBody(kAvTransport, QStringLiteral("Play X"), {}).isEmpty());
        QVERIFY(soapRequestBody(kAvTransport, QStringLiteral("1Play"), {}).isEmpty());
        QVERIFY(soapRequestBody(QStringLiteral("urn:x\"><evil/>"), QStringLiteral("Play"), {})
                    .isEmpty());
        QVERIFY(soapRequestBody(kAvTransport, QStringLiteral("Play"),
                                {{QStringLiteral("a b"), QString()}})
                    .isEmpty());
        QVERIFY(soapRequestBody(kAvTransport, QStringLiteral("Play"),
                                {{QStringLiteral("a><b"), QString()}})
                    .isEmpty());
        QVERIFY(soapRequestBody(kAvTransport, QStringLiteral("Play"), {{QString(), QString()}})
                    .isEmpty());
    }

    void didlTitleIsEscapedTwice()
    {
        const QString title = QStringLiteral("A & <B> \"q\" %1\x01");
        const QString url =
            QStringLiteral("http://192.168.1.5:8096/Videos/abc/stream?static=true&mediaSourceId=x");
        const QString didl = didlItem(title, url, QStringLiteral("video/mp4"));
        QVERIFY(didl.contains(
            QStringLiteral("<dc:title>A &amp; &lt;B&gt; &quot;q&quot; %1</dc:title>")));
        QVERIFY(didl.contains(
            QStringLiteral("<res protocolInfo=\"http-get:*:video/mp4:*\">%1</res>")
                .arg(QStringLiteral(
                    "http://192.168.1.5:8096/Videos/abc/stream?static=true&amp;mediaSourceId=x"))));
        QVERIFY(didl.contains(QStringLiteral("<upnp:class>object.item.videoItem</upnp:class>")));
        QVERIFY(didl.startsWith(
            QStringLiteral("<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\"")));

        const QByteArray body =
            soapRequestBody(kAvTransport, QStringLiteral("SetAVTransportURI"),
                            {{QStringLiteral("InstanceID"), QStringLiteral("0")},
                             {QStringLiteral("CurrentURI"), url},
                             {QStringLiteral("CurrentURIMetaData"), didl}});
        QVERIFY(body.contains("A &amp;amp; &amp;lt;B&amp;gt; &amp;quot;q&amp;quot; %1"));

        // The envelope carries the DIDL as text, and the DIDL carries the title as text
        const auto envelopeTree = parseXml(body, 1 << 16);
        QVERIFY(envelopeTree);
        const XmlNode *metadata =
            envelopeTree->child(u"Body")->child(u"SetAVTransportURI")->child(u"CurrentURIMetaData");
        QVERIFY(metadata);
        QCOMPARE(metadata->text, didl);
        const auto didlTree = parseXml(metadata->text.toUtf8(), 1 << 16);
        QVERIFY(didlTree);
        const XmlNode *item = didlTree->child(u"item");
        QVERIFY(item);
        QCOMPARE(item->child(u"title")->text, QStringLiteral("A & <B> \"q\" %1"));
        QCOMPARE(item->child(u"res")->text, url);
    }

    void didlRefusesABadMimeAndCutsTheTitle()
    {
        QVERIFY(didlItem(QStringLiteral("t"), QStringLiteral("http://h/"),
                         QStringLiteral("video/mp4\"><x"))
                    .isEmpty());
        QVERIFY(didlItem(QStringLiteral("t"), QStringLiteral("http://h/"), QStringLiteral("video"))
                    .isEmpty());
        QVERIFY(didlProtocolInfo(QStringLiteral("video/vnd.dlna.mpeg-tts"))
                == QStringLiteral("http-get:*:video/vnd.dlna.mpeg-tts:*"));
        const QString longTitle(kMaxDidlTitleChars + 50, QLatin1Char('t'));
        const QString didl =
            didlItem(longTitle, QStringLiteral("http://h/"), QStringLiteral("video/mp4"));
        QVERIFY(didl.contains(QStringLiteral("<dc:title>%1</dc:title>")
                                  .arg(QString(kMaxDidlTitleChars, QLatin1Char('t')))));
        QVERIFY(didlItem(QString(), QStringLiteral("http://h/"), QStringLiteral("video/mp4"))
                    .contains(QStringLiteral("<dc:title></dc:title>")));
    }

    void parsesAResponseAndReadsOnlyTheExpectedElements()
    {
        const QByteArray body = envelope(responseElement(
            QStringLiteral("GetTransportInfo"),
            QStringLiteral(
                "<CurrentTransportState> PLAYING </CurrentTransportState><Unexpected>x</Unexpected>"
                "<CurrentTransportState>STOPPED</CurrentTransportState><CurrentSpeed>1</"
                "CurrentSpeed>")));
        const SoapResult result = parseSoapResponse(
            body, QStringLiteral("GetTransportInfo"),
            {QStringLiteral("CurrentTransportState"), QStringLiteral("CurrentTransportStatus")});
        QCOMPARE(result.status, SoapStatus::Ok);
        QCOMPARE(result.values.size(), 1);
        QCOMPARE(result.values.value(QStringLiteral("CurrentTransportState")),
                 QStringLiteral("PLAYING"));
        QVERIFY(!result.values.contains(QStringLiteral("Unexpected")));
        QVERIFY(!result.values.contains(QStringLiteral("CurrentTransportStatus")));
    }

    void anEmptyResponseElementIsOk()
    {
        const SoapResult result =
            parseSoapResponse(envelope(responseElement(QStringLiteral("Play"), QString())),
                              QStringLiteral("Play"), {});
        QCOMPARE(result.status, SoapStatus::Ok);
        QVERIFY(result.values.isEmpty());
    }

    void upnpErrorFault()
    {
        const SoapResult result =
            parseSoapResponse(fault(QStringLiteral("714"), QStringLiteral("Illegal MIME-type")),
                              QStringLiteral("SetAVTransportURI"), {});
        QCOMPARE(result.status, SoapStatus::Fault);
        QCOMPARE(result.errorCode, 714);
        QCOMPARE(result.errorDescription, QStringLiteral("Illegal MIME-type"));

        QCOMPARE(
            parseSoapResponse(fault(QStringLiteral("abc"), QString()), QStringLiteral("Play"), {})
                .errorCode,
            0);
        QCOMPARE(parseSoapResponse(fault(QStringLiteral("99999999999"), QString()),
                                   QStringLiteral("Play"), {})
                     .errorCode,
                 0);
        QCOMPARE(
            parseSoapResponse(fault(QStringLiteral("-5"), QString()), QStringLiteral("Play"), {})
                .errorCode,
            0);
        const SoapResult longDescription =
            parseSoapResponse(fault(QStringLiteral("501"),
                                    QString(500, QLatin1Char('d')) + QStringLiteral("&#x202E;")),
                              QStringLiteral("Play"), {});
        QCOMPARE(longDescription.errorCode, 501);
        QCOMPARE(longDescription.errorDescription.size(), kMaxFaultDescriptionChars);

        const SoapResult bare = parseSoapResponse(
            envelope(QStringLiteral("<s:Fault><faultcode>s:Server</faultcode></s:Fault>")),
            QStringLiteral("Play"), {});
        QCOMPARE(bare.status, SoapStatus::Fault);
        QCOMPARE(bare.errorCode, 0);
        QVERIFY(bare.errorDescription.isEmpty());
    }

    void refusesWhatIsNotAResponseToTheAction()
    {
        const auto status = [](const QByteArray &body,
                               const QString &action = QStringLiteral("Play")) {
            return parseSoapResponse(body, action, {}).status;
        };
        QCOMPARE(status(QByteArray()), SoapStatus::Invalid);
        QCOMPARE(status("not xml"), SoapStatus::Invalid);
        QCOMPARE(status(envelope(responseElement(QStringLiteral("Pause"), QString()))),
                 SoapStatus::Invalid);
        QCOMPARE(status(envelope(responseElement(QStringLiteral("Play"), QString()),
                                 QStringLiteral("urn:other"))),
                 SoapStatus::Invalid);
        QCOMPARE(status(envelope(QString())), SoapStatus::Invalid);
        QCOMPARE(status("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"/>"),
                 SoapStatus::Invalid);
        QCOMPARE(status("<html/>"), SoapStatus::Invalid);
        // A response of one action is not the answer to another
        QCOMPARE(status(envelope(responseElement(QStringLiteral("Play"), QString())),
                        QStringLiteral("Stop")),
                 SoapStatus::Invalid);
    }

    void aDoctypeIsRefused()
    {
        const QByteArray body =
            QByteArray("<?xml version=\"1.0\"?><!DOCTYPE s [<!ENTITY x \"y\">]>")
            + envelope(responseElement(QStringLiteral("Play"), QStringLiteral("<A>&x;</A>")))
                  .mid(QByteArray("<?xml version=\"1.0\"?>").size());
        QCOMPARE(parseSoapResponse(body, QStringLiteral("Play"), {QStringLiteral("A")}).status,
                 SoapStatus::Invalid);
    }

    void responseSizeLimits()
    {
        const auto sized = [](const QString &action, int valueChars) {
            return envelope(responseElement(
                action,
                QStringLiteral("<Sink>%1</Sink>").arg(QString(valueChars, QLatin1Char('a')))));
        };
        QCOMPARE(soapResponseLimit(QStringLiteral("GetPositionInfo")), kMaxSoapResponseBytes);
        QCOMPARE(soapResponseLimit(QStringLiteral("GetProtocolInfo")),
                 kMaxProtocolInfoResponseBytes);

        QCOMPARE(parseSoapResponse(sized(QStringLiteral("GetPositionInfo"), 1000),
                                   QStringLiteral("GetPositionInfo"), {QStringLiteral("Sink")})
                     .status,
                 SoapStatus::Ok);
        QCOMPARE(parseSoapResponse(sized(QStringLiteral("GetPositionInfo"), 20000),
                                   QStringLiteral("GetPositionInfo"), {QStringLiteral("Sink")})
                     .status,
                 SoapStatus::Invalid);
        const SoapResult sink =
            parseSoapResponse(sized(QStringLiteral("GetProtocolInfo"), 20000),
                              QStringLiteral("GetProtocolInfo"), {QStringLiteral("Sink")});
        QCOMPARE(sink.status, SoapStatus::Ok);
        QCOMPARE(sink.values.value(QStringLiteral("Sink")).size(), 20000);
        QCOMPARE(parseSoapResponse(sized(QStringLiteral("GetProtocolInfo"), 70000),
                                   QStringLiteral("GetProtocolInfo"), {QStringLiteral("Sink")})
                     .status,
                 SoapStatus::Invalid);
    }

    void upnpTimeFormat()
    {
        QCOMPARE(upnpTime(0), QStringLiteral("00:00:00"));
        QCOMPARE(upnpTime(3723500), QStringLiteral("01:02:03"));
        QCOMPARE(upnpTime(59999), QStringLiteral("00:00:59"));
        QCOMPARE(upnpTime(-5000), QStringLiteral("00:00:00"));
        QCOMPARE(upnpTime(100LL * 3600 * 1000), QStringLiteral("100:00:00"));
        QCOMPARE(upnpTime(std::numeric_limits<qint64>::max()), QStringLiteral("999999:59:59"));
    }

    void parseUpnpTimeCases_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<qint64>("milliseconds");
        QTest::newRow("fraction") << QStringLiteral("1:02:03.5") << true << qint64(3723500);
        QTest::newRow("padded") << QStringLiteral("01:02:03") << true << qint64(3723000);
        QTest::newRow("zero") << QStringLiteral("00:00:00") << true << qint64(0);
        QTest::newRow("long fraction")
            << QStringLiteral("01:02:03.123456") << true << qint64(3723123);
        QTest::newRow("short fraction") << QStringLiteral("0:00:00.05") << true << qint64(50);
        QTest::newRow("nine digits") << QStringLiteral("0:00:00.999999999") << true << qint64(999);
        QTest::newRow("single digits") << QStringLiteral("1:2:3") << true << qint64(3723000);
        QTest::newRow("whitespace") << QStringLiteral(" 0:00:01 ") << true << qint64(1000);
        QTest::newRow("plus") << QStringLiteral("+0:00:05") << true << qint64(5000);
        QTest::newRow("negative") << QStringLiteral("-0:00:05.5") << true << qint64(-5500);
        QTest::newRow("six hour digits")
            << QStringLiteral("999999:59:59") << true << qint64(3599999999000);
        QTest::newRow("not implemented") << QStringLiteral("NOT_IMPLEMENTED") << false << qint64(0);
        QTest::newRow("empty") << QString() << false << qint64(0);
        QTest::newRow("overflow") << QStringLiteral("1000000:00:00") << false << qint64(0);
        QTest::newRow("huge") << QStringLiteral("99999999999999999999:00:00") << false << qint64(0);
        QTest::newRow("minutes") << QStringLiteral("1:60:00") << false << qint64(0);
        QTest::newRow("seconds") << QStringLiteral("1:00:60") << false << qint64(0);
        QTest::newRow("two parts") << QStringLiteral("1:02") << false << qint64(0);
        QTest::newRow("four parts") << QStringLiteral("1:02:03:04") << false << qint64(0);
        QTest::newRow("empty fraction") << QStringLiteral("1:02:03.") << false << qint64(0);
        QTest::newRow("long fraction digits")
            << QStringLiteral("1:02:03.1234567890") << false << qint64(0);
        QTest::newRow("letters") << QStringLiteral("a:02:03") << false << qint64(0);
        QTest::newRow("fraction as ratio") << QStringLiteral("0:00:00.1/2") << false << qint64(0);
        QTest::newRow("sign only") << QStringLiteral("-") << false << qint64(0);
        QTest::newRow("unicode digits")
            << QString::fromUtf8("\xd9\xa1:02:03") << false << qint64(0);
    }
    void parseUpnpTimeCases()
    {
        QFETCH(QString, text);
        QFETCH(bool, valid);
        QFETCH(qint64, milliseconds);
        const auto parsed = parseUpnpTime(text);
        QCOMPARE(bool(parsed), valid);
        if (valid)
            QCOMPARE(*parsed, milliseconds);
    }

    void upnpTimeRoundTrips()
    {
        for (const qint64 ms : {0LL, 1000LL, 61000LL, 3723000LL, 86400000LL, 360000000LL}) {
            const auto parsed = parseUpnpTime(upnpTime(ms));
            QVERIFY(parsed);
            QCOMPARE(*parsed, ms);
        }
    }
};

QTEST_GUILESS_MAIN(TestCastSoap)
#include "test_cast_soap.moc"
