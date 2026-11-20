// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastText.h"
#include "cast/UpnpDescription.h"
#include "cast/XmlTree.h"

#include <QtTest>

using namespace Kinefin;

namespace {
const QUrl kLocation(QStringLiteral("http://192.168.1.20:49152/dir/desc.xml"));
const QHostAddress kAddress(QStringLiteral("192.168.1.20"));

QString service(const QString &name, const QString &scpd, const QString &control,
                const QString &event, const QString &version = QStringLiteral("1"))
{
    return QStringLiteral("<service><serviceType>urn:schemas-upnp-org:service:%1:%5</serviceType>"
                          "<serviceId>urn:upnp-org:serviceId:%1</serviceId><SCPDURL>%2</SCPDURL>"
                          "<controlURL>%3</controlURL><eventSubURL>%4</eventSubURL></service>")
        .arg(name, scpd, control, event, version);
}

QString defaultServices()
{
    return service(QStringLiteral("AVTransport"), QStringLiteral("/AVT/scpd.xml"),
                   QStringLiteral("/AVT/ctl"), QStringLiteral("/AVT/evt"))
           + service(QStringLiteral("ConnectionManager"), QStringLiteral("/CM/scpd.xml"),
                     QStringLiteral("/CM/ctl"), QStringLiteral("/CM/evt"))
           + service(QStringLiteral("RenderingControl"), QStringLiteral("/RC/scpd.xml"),
                     QStringLiteral("/RC/ctl"), QStringLiteral("/RC/evt"));
}

QString device(const QString &friendlyName, const QString &services,
               const QString &udn = QStringLiteral("uuid:ABC-123"),
               const QString &type = QStringLiteral("urn:schemas-upnp-org:device:MediaRenderer:1"),
               const QString &inner = {})
{
    return QStringLiteral(
               "<device><deviceType>%1</deviceType><friendlyName>%2</friendlyName>"
               "<manufacturer>Acme</manufacturer><modelName>Model X</modelName><UDN>%3</UDN>"
               "<serviceList>%4</serviceList>%5</device>")
        .arg(type, friendlyName, udn, services, inner);
}

QByteArray document(const QString &deviceXml, const QString &urlBase = {},
                    const QString &prolog = {})
{
    const QString base =
        urlBase.isEmpty() ? QString() : QStringLiteral("<URLBase>%1</URLBase>").arg(urlBase);
    const QString root =
        QStringLiteral("<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
                       "<specVersion><major>1</major><minor>0</minor></specVersion>%1%2</root>")
            .arg(base, deviceXml);
    const QString text = QStringLiteral("<?xml version=\"1.0\"?>") + prolog + root;
    return text.toUtf8();
}

QByteArray standard(const QString &name = QStringLiteral("Living room TV"))
{
    return document(device(name, defaultServices()));
}

std::optional<RendererInfo> parse(const QByteArray &xml, QString *error = nullptr,
                                  const QString &expected = {})
{
    return parseDeviceDescription(xml, kLocation, kAddress, expected, error);
}

QByteArray scpd(const QStringList &actions, const QStringList &seekModes,
                const QString &extraActions = {})
{
    QString list;
    for (const QString &name : actions)
        list += QStringLiteral("<action><name>%1</name></action>").arg(name);
    QString modes;
    for (const QString &mode : seekModes)
        modes += QStringLiteral("<allowedValue>%1</allowedValue>").arg(mode);
    return QStringLiteral(
               "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
               "<actionList>%1%2</actionList><serviceStateTable>"
               "<stateVariable sendEvents=\"no\"><name>TransportState</name></stateVariable>"
               "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekMode</name>"
               "<allowedValueList>%3</allowedValueList></stateVariable></serviceStateTable></scpd>")
        .arg(list, extraActions, modes)
        .toUtf8();
}

const QStringList kRequired{QStringLiteral("SetAVTransportURI"), QStringLiteral("Play"),
                            QStringLiteral("Stop"), QStringLiteral("GetTransportInfo"),
                            QStringLiteral("GetPositionInfo")};
} // namespace

class TestCastDescription : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void parsesAFullDescription()
    {
        QString error;
        const auto info = parse(standard(), &error);
        QVERIFY2(info, qPrintable(error));
        QCOMPARE(info->udn, QStringLiteral("uuid:abc-123"));
        QCOMPARE(info->name, QStringLiteral("Living room TV"));
        QCOMPARE(info->manufacturer, QStringLiteral("Acme"));
        QCOMPARE(info->model, QStringLiteral("Model X"));
        QCOMPARE(info->address, kAddress);
        QCOMPARE(info->location, kLocation);
        QCOMPARE(info->avTransport.serviceType,
                 QStringLiteral("urn:schemas-upnp-org:service:AVTransport:1"));
        QCOMPARE(info->avTransport.controlUrl,
                 QUrl(QStringLiteral("http://192.168.1.20:49152/AVT/ctl")));
        QCOMPARE(info->avTransport.scpdUrl,
                 QUrl(QStringLiteral("http://192.168.1.20:49152/AVT/scpd.xml")));
        QCOMPARE(info->avTransport.eventSubUrl,
                 QUrl(QStringLiteral("http://192.168.1.20:49152/AVT/evt")));
        QCOMPARE(info->connectionManager.controlUrl,
                 QUrl(QStringLiteral("http://192.168.1.20:49152/CM/ctl")));
        QVERIFY(info->hasVolume());
        QCOMPARE(info->renderingControl.controlUrl,
                 QUrl(QStringLiteral("http://192.168.1.20:49152/RC/ctl")));
    }

    void relativeUrlsFollowUrlBaseOrTheLocation()
    {
        const QString services =
            service(QStringLiteral("AVTransport"), QStringLiteral("scpd.xml"),
                    QStringLiteral("ctl"), QString())
            + service(QStringLiteral("ConnectionManager"), QStringLiteral("cm.xml"),
                      QStringLiteral("../cmctl"), QString());
        const auto plain = parse(document(device(QStringLiteral("TV"), services)));
        QVERIFY(plain);
        QCOMPARE(plain->avTransport.controlUrl,
                 QUrl(QStringLiteral("http://192.168.1.20:49152/dir/ctl")));
        QCOMPARE(plain->connectionManager.controlUrl,
                 QUrl(QStringLiteral("http://192.168.1.20:49152/cmctl")));
        QVERIFY(plain->avTransport.eventSubUrl.isEmpty());
        QVERIFY(!plain->hasVolume());

        const auto based = parse(document(device(QStringLiteral("TV"), services),
                                          QStringLiteral("http://192.168.1.20:49152/base/")));
        QVERIFY(based);
        QCOMPARE(based->avTransport.controlUrl,
                 QUrl(QStringLiteral("http://192.168.1.20:49152/base/ctl")));

        const QString absolute =
            service(QStringLiteral("AVTransport"),
                    QStringLiteral("http://192.168.1.20:49152/a/s.xml"),
                    QStringLiteral("http://192.168.1.20:49152/a/c"), QString())
            + service(QStringLiteral("ConnectionManager"), QStringLiteral("/cm.xml"),
                      QStringLiteral("/cm"), QString());
        const auto abs = parse(document(device(QStringLiteral("TV"), absolute)));
        QVERIFY(abs);
        QCOMPARE(abs->avTransport.controlUrl,
                 QUrl(QStringLiteral("http://192.168.1.20:49152/a/c")));
    }

    void anOffOriginServiceIsDropped_data()
    {
        QTest::addColumn<QString>("control");
        QTest::newRow("other host") << QStringLiteral("http://192.168.1.99:49152/ctl");
        QTest::newRow("other port") << QStringLiteral("http://192.168.1.20:80/ctl");
        QTest::newRow("https") << QStringLiteral("https://192.168.1.20:49152/ctl");
        QTest::newRow("network path") << QStringLiteral("//evil.example/ctl");
        QTest::newRow("hostname") << QStringLiteral("http://tv.local:49152/ctl");
        QTest::newRow("credentials") << QStringLiteral("http://u:p@192.168.1.20:49152/ctl");
        QTest::newRow("metadata address") << QStringLiteral("http://169.254.169.254/latest");
        QTest::newRow("too long") << QString(QStringLiteral("/") + QString(600, QLatin1Char('a')));
    }
    void anOffOriginServiceIsDropped()
    {
        QFETCH(QString, control);
        const QString services =
            service(QStringLiteral("AVTransport"), QStringLiteral("/s.xml"), control, QString())
            + service(QStringLiteral("ConnectionManager"), QStringLiteral("/cm.xml"),
                      QStringLiteral("/cm"), QString());
        QString error;
        QVERIFY(!parse(document(device(QStringLiteral("TV"), services)), &error));
        QVERIFY(!error.isEmpty());
    }

    void offOriginEventOrScpdUrlDropsThatService()
    {
        const QString cm = service(QStringLiteral("ConnectionManager"), QStringLiteral("/cm.xml"),
                                   QStringLiteral("/cm"), QString());
        QVERIFY(!parse(
            document(device(QStringLiteral("TV"),
                            service(QStringLiteral("AVTransport"), QStringLiteral("/s.xml"),
                                    QStringLiteral("/c"), QStringLiteral("http://192.168.1.99/evt"))
                                + cm))));
        QVERIFY(!parse(document(
            device(QStringLiteral("TV"), service(QStringLiteral("AVTransport"),
                                                 QStringLiteral("http://192.168.1.99/s.xml"),
                                                 QStringLiteral("/c"), QString())
                                             + cm))));
        // Only the optional service is lost
        const QString badVolume =
            service(QStringLiteral("RenderingControl"), QStringLiteral("/r.xml"),
                    QStringLiteral("http://192.168.1.99/rc"), QString());
        const auto info = parse(document(
            device(QStringLiteral("TV"),
                   defaultServices().left(defaultServices().indexOf(QStringLiteral(
                       "<service><serviceType>urn:schemas-upnp-org:service:RenderingControl")))
                       + badVolume)));
        QVERIFY(info);
        QVERIFY(!info->hasVolume());
    }

    void anOffOriginUrlBaseDropsEverything()
    {
        QString error;
        QVERIFY(!parse(document(device(QStringLiteral("TV"), defaultServices()),
                                QStringLiteral("http://192.168.1.99:49152/")),
                       &error));
        QVERIFY(!parse(document(device(QStringLiteral("TV"), defaultServices()),
                                QStringLiteral("http://u:p@192.168.1.20:49152/")),
                       &error));
        QVERIFY(!parse(document(device(QStringLiteral("TV"), defaultServices()),
                                QStringLiteral("https://192.168.1.20:49152/")),
                       &error));
    }

    void theLocationMustBeTheAddressOfTheDevice()
    {
        QString error;
        QVERIFY(!parseDeviceDescription(standard(), kLocation,
                                        QHostAddress(QStringLiteral("192.168.1.21")), {}, &error));
        QVERIFY(!parseDeviceDescription(
            standard(), QUrl(QStringLiteral("https://192.168.1.20/d.xml")), kAddress, {}, &error));
        QVERIFY(!parseDeviceDescription(standard(), QUrl(QStringLiteral("http://tv.local/d.xml")),
                                        kAddress, {}, &error));
    }

    void aDeviceNeedsAvTransportAndConnectionManager()
    {
        const QString avt = service(QStringLiteral("AVTransport"), QStringLiteral("/s.xml"),
                                    QStringLiteral("/c"), QString());
        const QString cm = service(QStringLiteral("ConnectionManager"), QStringLiteral("/cm.xml"),
                                   QStringLiteral("/cm"), QString());
        QVERIFY(parse(document(device(QStringLiteral("TV"), avt + cm))));
        QVERIFY(!parse(document(device(QStringLiteral("TV"), avt))));
        QVERIFY(!parse(document(device(QStringLiteral("TV"), cm))));
        QVERIFY(!parse(document(device(QStringLiteral("TV"), {}))));
        // A service without a control URL or an SCPD does not count
        QVERIFY(!parse(document(
            device(QStringLiteral("TV"), service(QStringLiteral("AVTransport"),
                                                 QStringLiteral("/s.xml"), QString(), QString())
                                             + cm))));
        QVERIFY(!parse(
            document(device(QStringLiteral("TV"), service(QStringLiteral("AVTransport"), QString(),
                                                          QStringLiteral("/c"), QString())
                                                      + cm))));
        // A newer version of the service is the same service
        QVERIFY(parse(document(device(
            QStringLiteral("TV"), service(QStringLiteral("AVTransport"), QStringLiteral("/s.xml"),
                                          QStringLiteral("/c"), QString(), QStringLiteral("3"))
                                      + cm))));
    }

    void onlyAMediaRendererWithAValidUdnIsListed()
    {
        const QString services = defaultServices();
        QVERIFY(
            !parse(document(device(QStringLiteral("S"), services, QStringLiteral("uuid:abc"),
                                   QStringLiteral("urn:schemas-upnp-org:device:MediaServer:1")))));
        QVERIFY(!parse(document(device(QStringLiteral("S"), services, QString()))));
        QVERIFY(
            !parse(document(device(QStringLiteral("S"), services, QStringLiteral("not-a-uuid")))));
        QVERIFY(!parse(
            document(device(QStringLiteral("S"), services, QStringLiteral("uuid:has space")))));
        QVERIFY(!parse(QByteArray("<html><body/></html>")));
        QVERIFY(!parse(document(QString())));
    }

    void anEmbeddedRendererIsPickedByItsUdn()
    {
        const QString renderer = device(QStringLiteral("Embedded"), defaultServices(),
                                        QStringLiteral("uuid:renderer-1"));
        const QString other =
            device(QStringLiteral("Other"), defaultServices(), QStringLiteral("uuid:renderer-2"));
        const QString server =
            device(QStringLiteral("Server"),
                   service(QStringLiteral("ContentDirectory"), QStringLiteral("/s"),
                           QStringLiteral("/c"), QString()),
                   QStringLiteral("uuid:server"),
                   QStringLiteral("urn:schemas-upnp-org:device:MediaServer:1"),
                   QStringLiteral("<deviceList>%1%2</deviceList>").arg(renderer, other));
        const QByteArray xml = document(server);
        const auto picked = parse(xml, nullptr, QStringLiteral("UUID:renderer-2"));
        QVERIFY(picked);
        QCOMPARE(picked->name, QStringLiteral("Other"));
        const auto first = parse(xml);
        QVERIFY(first);
        QCOMPARE(first->udn, QStringLiteral("uuid:renderer-1"));
        QVERIFY(!parse(xml, nullptr, QStringLiteral("uuid:missing")));
    }

    void rendererTextIsSanitized()
    {
        const auto info = parse(
            document(device(QStringLiteral("&lt;b&gt;TV&lt;/b&gt;&#x202E;\t\n  &amp;co&#x200B;")
                                + QString(100, QLatin1Char('x')),
                            defaultServices())));
        QVERIFY(info);
        QVERIFY(info->name.startsWith(QStringLiteral("<b>TV</b> &co")));
        QCOMPARE(info->name.size(), kMaxDisplayChars);
        QVERIFY(!info->name.contains(QChar(0x202E)));
        QVERIFY(!info->name.contains(QChar(0x200B)));

        // A name longer than any list entry is cut, not dropped
        const auto longName =
            parse(document(device(QString(300, QLatin1Char('n')), defaultServices())));
        QVERIFY(longName);
        QCOMPARE(longName->name, QString(kMaxDisplayChars, QLatin1Char('n')));

        const auto noName = parse(document(device(QString(), defaultServices())));
        QVERIFY(noName);
        QCOMPARE(noName->name, QStringLiteral("Model X"));
        const QString noModel =
            QString::fromUtf8(document(device(QString(), defaultServices())))
                .replace(QStringLiteral("<modelName>Model X</modelName>"), QString());
        const auto addressName = parse(noModel.toUtf8());
        QVERIFY(addressName);
        QCOMPARE(addressName->name, QStringLiteral("192.168.1.20"));
    }

    void sanitizeDisplayTextRules()
    {
        QCOMPARE(sanitizeDisplayText(u"  a \t b\n\nc  "), QStringLiteral("a b c"));
        const QString controls = QString(QChar(1)) + QStringLiteral("x") + QChar(0x7F)
                                 + QChar(0x200F) + QChar(0xE000) + QChar(0x2028);
        QCOMPARE(sanitizeDisplayText(controls), QStringLiteral("x"));
        QCOMPARE(sanitizeDisplayText(u"abcdef", 3), QStringLiteral("abc"));
        QCOMPARE(sanitizeDisplayText(u"ab cd", 3), QStringLiteral("ab"));
        // A code point outside the BMP is one character
        const QString emoji = QString::fromUcs4(U"\U0001F600\U0001F600\U0001F600");
        QCOMPARE(sanitizeDisplayText(emoji, 2).size(), 4);
        const QString tooLong(70, QChar(u'x'));
        QCOMPARE(sanitizeDisplayText(tooLong).size(), kMaxDisplayChars);
        // A lone surrogate is dropped
        const QString lone = QString(QChar(0xD800)) + QStringLiteral("ok");
        QCOMPARE(sanitizeDisplayText(lone), QStringLiteral("ok"));
        QVERIFY(sanitizeDisplayText(u"").isEmpty());
        QCOMPARE(xmlEscape(u"a&b<c>\"d'e"), QStringLiteral("a&amp;b&lt;c&gt;&quot;d&apos;e"));
        const QString illegal = QString(QChar(1)) + QStringLiteral("x") + QChar(0xFFFE);
        QCOMPARE(xmlEscape(illegal), QStringLiteral("x"));
    }

    void aDoctypeAborts_data()
    {
        QTest::addColumn<QString>("prolog");
        QTest::newRow("internal entity") << QStringLiteral("<!DOCTYPE root [<!ENTITY x \"y\">]>");
        QTest::newRow("external entity")
            << QStringLiteral("<!DOCTYPE root [<!ENTITY e SYSTEM \"file:///etc/passwd\">]>");
        QTest::newRow("external subset")
            << QStringLiteral("<!DOCTYPE root SYSTEM \"http://192.168.1.99/x.dtd\">");
        QTest::newRow("entity bomb")
            << QStringLiteral("<!DOCTYPE root [<!ENTITY a \"aaaaaaaaaa\"><!ENTITY b "
                              "\"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">]>");
    }
    void aDoctypeAborts()
    {
        QFETCH(QString, prolog);
        QString error;
        QVERIFY(!parse(document(device(QStringLiteral("&e;&b;"), defaultServices()), {}, prolog),
                       &error));
        QCOMPARE(error, QStringLiteral("DTD not allowed"));
        QVERIFY(!parseXml(document(QString(), {}, prolog), kMaxDescriptionBytes));
        QString scpdError;
        RendererInfo info;
        QVERIFY(!parseAvTransportScpd(prolog.toUtf8() + scpd(kRequired, {}), info, &scpdError));
        QCOMPARE(scpdError, QStringLiteral("DTD not allowed"));
    }

    void undeclaredEntitiesAndBrokenXmlAreRefused()
    {
        QString error;
        QVERIFY(!parse(document(device(QStringLiteral("&nope;"), defaultServices())), &error));
        QVERIFY(!parse(standard().left(standard().size() - 8), &error));
        QVERIFY(!parse(standard() + "<extra/>", &error));
        QVERIFY(!parse(QByteArray(), &error));
    }

    void sizeLimit()
    {
        QByteArray atLimit = standard();
        const QByteArray tail = "<!--";
        atLimit.chop(7);
        const int pad = kMaxDescriptionBytes - atLimit.size() - 7 - int(tail.size()) - 3;
        atLimit.append(tail);
        atLimit.append(QByteArray(pad, 'x'));
        atLimit.append("-->");
        atLimit.append("</root>");
        QCOMPARE(atLimit.size(), kMaxDescriptionBytes);
        QString error;
        QVERIFY2(parse(atLimit, &error), qPrintable(error));
        atLimit.insert(atLimit.size() - 7, "x");
        QVERIFY(!parse(atLimit, &error));
        QCOMPARE(error, QStringLiteral("document too large"));
    }

    void depthAndNodeLimits()
    {
        const auto nested = [](int depth) -> QByteArray {
            return QByteArray("<a>").repeated(depth) + QByteArray("</a>").repeated(depth);
        };
        QVERIFY(parseXml(nested(kMaxXmlDepth), 1 << 16));
        QString error;
        QVERIFY(!parseXml(nested(kMaxXmlDepth + 1), 1 << 16, &error));
        QCOMPARE(error, QStringLiteral("nesting too deep"));
        QVERIFY(parseXml("<r>" + QByteArray("<a/>").repeated(kMaxXmlNodes - 1) + "</r>", 1 << 20));
        QVERIFY(
            !parseXml("<r>" + QByteArray("<a/>").repeated(kMaxXmlNodes) + "</r>", 1 << 20, &error));
        QCOMPARE(error, QStringLiteral("too many elements"));
        QVERIFY(!parseXml("<a/><b/>", 100));
    }

    void serviceListsAreCapped()
    {
        QString junk;
        for (int i = 0; i < 254; ++i)
            junk += service(QStringLiteral("Junk"), QStringLiteral("/j"), QStringLiteral("/j"),
                            QString());
        const QString avt = service(QStringLiteral("AVTransport"), QStringLiteral("/s.xml"),
                                    QStringLiteral("/c"), QString());
        const QString cm = service(QStringLiteral("ConnectionManager"), QStringLiteral("/cm.xml"),
                                   QStringLiteral("/cm"), QString());
        QVERIFY(parse(document(device(QStringLiteral("TV"), junk + avt + cm))));
        QVERIFY(!parse(document(
            device(QStringLiteral("TV"), junk + QStringLiteral("<service/>") + avt + cm))));
    }

    void scpdGivesActionsAndSeekModes()
    {
        RendererInfo info;
        QString error;
        QVERIFY2(parseAvTransportScpd(
                     scpd(kRequired + QStringList{QStringLiteral("Pause"), QStringLiteral("Seek")},
                          {QStringLiteral("rel_time"), QStringLiteral("TRACK_NR")}),
                     info, &error),
                 qPrintable(error));
        QVERIFY(info.hasAction(QStringLiteral("Pause")));
        QVERIFY(info.canPause());
        QVERIFY(info.canSeekRelTime());
        QCOMPARE(info.seekModes,
                 (QStringList{QStringLiteral("REL_TIME"), QStringLiteral("TRACK_NR")}));

        RendererInfo bare;
        QVERIFY(parseAvTransportScpd(
            scpd(kRequired + QStringList{QStringLiteral("Seek")}, {QStringLiteral("TRACK_NR")}),
            bare));
        QVERIFY(!bare.canPause());
        QVERIFY(!bare.canSeekRelTime());

        RendererInfo noList;
        QVERIFY(parseAvTransportScpd(scpd(kRequired + QStringList{QStringLiteral("Seek")}, {}),
                                     noList));
        QVERIFY(noList.seekModes.isEmpty());
        QVERIFY(!noList.canSeekRelTime());

        // The mode without the Seek action is not a seek
        RendererInfo noSeek;
        QVERIFY(parseAvTransportScpd(scpd(kRequired, {QStringLiteral("REL_TIME")}), noSeek));
        QVERIFY(!noSeek.canSeekRelTime());
    }

    void scpdRefusals()
    {
        RendererInfo info;
        QString error;
        QStringList missingPlay = kRequired;
        missingPlay.removeAll(QStringLiteral("Play"));
        QVERIFY(!parseAvTransportScpd(scpd(missingPlay, {}), info, &error));
        QVERIFY(!parseAvTransportScpd("<root/>", info, &error));
        QVERIFY(!parseAvTransportScpd("<scpd><actionList>", info, &error));
        QVERIFY(!parseAvTransportScpd(QByteArray(kMaxDescriptionBytes + 1, ' '), info, &error));
        QCOMPARE(error, QStringLiteral("document too large"));
        // The required actions after the 256th entry are not read
        QStringList many;
        for (int i = 0; i < kMaxListEntries; ++i)
            many.append(QStringLiteral("Junk%1").arg(i));
        QVERIFY(!parseAvTransportScpd(scpd(many + kRequired, {}), info, &error));
        // An action name longer than 256 characters is skipped
        QStringList longName = kRequired;
        longName.replace(1, QString(kMaxListEntryChars + 1, QLatin1Char('P')));
        QVERIFY(!parseAvTransportScpd(scpd(longName, {}), info, &error));
        // Nothing was stored by the refusals
        QVERIFY(info.actions.isEmpty());
    }

    void sinkProtocolInfo()
    {
        const QString sink =
            QStringLiteral("http-get:*:video/mp4:*,"
                           "http-get:*:video/x-matroska:DLNA.ORG_PN=X;DLNA.ORG_OP=01,"
                           "rtsp-rtp-udp:*:video/mpeg:*,"
                           "http-get:*:audio/mpeg:*, "
                           "HTTP-GET:*:VIDEO/WEBM;codecs=vp9:*,"
                           "http-get:*:*:*,junk,,http-get:*:not a mime:*,"
                           "http-get:*:video/mp2t:*");
        QCOMPARE(parseSinkProtocolInfo(sink),
                 (QStringList{QStringLiteral("video/mp4"), QStringLiteral("video/x-matroska"),
                              QStringLiteral("audio/mpeg"), QStringLiteral("video/webm"),
                              QStringLiteral("video/mp2t")}));
        QVERIFY(parseSinkProtocolInfo(QString()).isEmpty());

        QStringList entries;
        for (int i = 0; i < kMaxListEntries; ++i)
            entries.append(QStringLiteral("http-get:*:video/x%1:*").arg(i));
        entries.append(QStringLiteral("http-get:*:video/late:*"));
        const QStringList capped = parseSinkProtocolInfo(entries.join(QLatin1Char(',')));
        QCOMPARE(capped.size(), kMaxListEntries);
        QVERIFY(!capped.contains(QStringLiteral("video/late")));
        const QString longEntry = QStringLiteral("http-get:*:video/")
                                  + QString(kMaxListEntryChars, QLatin1Char('a'))
                                  + QStringLiteral(":*");
        QVERIFY(parseSinkProtocolInfo(longEntry).isEmpty());
    }

    void sameOrigin()
    {
        const QUrl base(QStringLiteral("http://192.168.1.20:49152/a"));
        QVERIFY(isSameOrigin(base, QUrl(QStringLiteral("http://192.168.1.20:49152/b/c?d=e"))));
        QVERIFY(isSameOrigin(QUrl(QStringLiteral("http://192.168.1.20/a")),
                             QUrl(QStringLiteral("http://192.168.1.20:80/b"))));
        QVERIFY(!isSameOrigin(base, QUrl(QStringLiteral("http://192.168.1.20:49153/a"))));
        QVERIFY(!isSameOrigin(base, QUrl(QStringLiteral("http://192.168.1.21:49152/a"))));
        QVERIFY(!isSameOrigin(base, QUrl(QStringLiteral("https://192.168.1.20:49152/a"))));
        QVERIFY(!isSameOrigin(base, QUrl(QStringLiteral("http://tv.local:49152/a"))));
        QVERIFY(!isSameOrigin(QUrl(), QUrl()));
        QVERIFY(isSameOrigin(QUrl(QStringLiteral("http://[fe80::1]:80/a")),
                             QUrl(QStringLiteral("http://[fe80::1]/b"))));
    }
};

QTEST_GUILESS_MAIN(TestCastDescription)
#include "test_cast_description.moc"
