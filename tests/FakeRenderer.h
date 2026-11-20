// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cast/UpnpDescription.h"

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

// A scripted DLNA MediaRenderer over HTTP: a description, an AVTransport SCPD, the AVTransport, ConnectionManager and RenderingControl SOAP actions, and the state a test reads. It also stands in for the media server of the renderer tests: a GET under /media answers the range probe
class FakeRenderer : public QObject {
  public:
    explicit FakeRenderer(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    QByteArray buffer = socket->property("buffer").toByteArray();
                    buffer.append(socket->readAll());
                    socket->setProperty("buffer", buffer);
                    serve(socket);
                });
            }
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_server.serverPort(); }
    QString baseUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(port()); }
    QString descriptionUrl() const { return baseUrl() + QStringLiteral("/description.xml"); }

    // What discovery would have read of this renderer: a controller can be given it without a search
    Kinefin::RendererInfo rendererInfo(const QString &name) const
    {
        Kinefin::RendererInfo info;
        info.udn = QStringLiteral("uuid:test-renderer");
        info.name = name;
        info.model = QStringLiteral("Test TV");
        info.address = QHostAddress::LocalHost;
        info.location = QUrl(descriptionUrl());
        const auto service = [this](const QString &kind) {
            Kinefin::UpnpService entry;
            entry.serviceType = QStringLiteral("urn:schemas-upnp-org:service:%1:1").arg(kind);
            entry.controlUrl = QUrl(baseUrl() + QStringLiteral("/%1/control").arg(kind));
            entry.scpdUrl = QUrl(baseUrl() + QStringLiteral("/%1/scpd.xml").arg(kind));
            return entry;
        };
        info.avTransport = service(QStringLiteral("AVTransport"));
        info.connectionManager = service(QStringLiteral("ConnectionManager"));
        info.renderingControl = service(QStringLiteral("RenderingControl"));
        info.actions = {QStringLiteral("SetAVTransportURI"),
                        QStringLiteral("Play"),
                        QStringLiteral("Pause"),
                        QStringLiteral("Stop"),
                        QStringLiteral("Seek"),
                        QStringLiteral("GetTransportInfo"),
                        QStringLiteral("GetPositionInfo")};
        info.seekModes = {QStringLiteral("REL_TIME")};
        info.sinkMimeTypes = {QStringLiteral("video/x-matroska"), QStringLiteral("video/mp4")};
        return info;
    }

    QByteArray description() const
    {
        return QStringLiteral(
                   "<?xml version=\"1.0\"?>"
                   "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
                   "<device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1"
                   "</deviceType><friendlyName>Fake &amp; Renderer</friendlyName>"
                   "<manufacturer>Kinefin Tests</manufacturer>"
                   "<modelName>Test TV</modelName><UDN>uuid:test-renderer</UDN>"
                   "<serviceList>"
                   "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1"
                   "</serviceType><serviceId>urn:upnp-org:serviceId:AVTransport"
                   "</serviceId><controlURL>/AVTransport/control</controlURL>"
                   "<eventSubURL>/AVTransport/event</eventSubURL>"
                   "<SCPDURL>/AVTransport/scpd.xml</SCPDURL></service>"
                   "<service><serviceType>urn:schemas-upnp-org:service:"
                   "ConnectionManager:1</serviceType><serviceId>urn:upnp-org:"
                   "serviceId:ConnectionManager</serviceId><controlURL>/ConnectionManager/"
                   "control</controlURL><eventSubURL>/ConnectionManager/event"
                   "</eventSubURL><SCPDURL>/ConnectionManager/scpd.xml</SCPDURL></service>"
                   "<service><serviceType>urn:schemas-upnp-org:service:"
                   "RenderingControl:1</serviceType><serviceId>urn:upnp-org:serviceId:"
                   "RenderingControl</serviceId><controlURL>/RenderingControl/control"
                   "</controlURL><eventSubURL>/RenderingControl/event</eventSubURL>"
                   "<SCPDURL>/RenderingControl/scpd.xml</SCPDURL></service>"
                   "</serviceList></device></root>")
            .toUtf8();
    }

    QByteArray scpd() const
    {
        return QStringLiteral("<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:"
                              "service-1-0\"><actionList>"
                              "<action><name>SetAVTransportURI</name></action>"
                              "<action><name>Play</name></action>"
                              "<action><name>Pause</name></action>"
                              "<action><name>Stop</name></action>"
                              "<action><name>Seek</name></action>"
                              "<action><name>GetTransportInfo</name></action>"
                              "<action><name>GetPositionInfo</name></action>"
                              "</actionList><serviceStateTable><stateVariable>"
                              "<name>A_ARG_TYPE_SeekMode</name><allowedValueList>"
                              "<allowedValue>REL_TIME</allowedValue>"
                              "</allowedValueList></stateVariable></serviceStateTable></scpd>")
            .toUtf8();
    }

    QString transport = QStringLiteral("STOPPED");
    int position = 0;
    int duration = 100;
    QString trackUri;
    int volume = 50;
    bool muted = false;
    bool failPolls = false;
    // The answer to a GET under /media: 206 with one byte, or this status
    int mediaStatus = 206;
    int mediaRequests = 0;
    // Actions answered with a SOAP fault (UPnP error `faultCode`); `faultsOnce` lists ones that fault the first time only
    QStringList faults;
    QStringList faultsOnce;
    int faultCode = 701;
    // GetTransportInfo and GetPositionInfo answer after this delay
    int pollDelayMs = 0;
    // Play reaches PLAYING; otherwise the renderer goes TRANSITIONING and then STOPPED, as one that cannot fetch the stream
    bool playStarts = true;
    // When set, GetPositionInfo names this track instead of the one loaded
    QString reportedTrackUri;
    QStringList actions;
    QList<QByteArray> bodies;
    QList<QByteArray> headers;

  private:
    static QByteArray envelope(const QString &service, const QString &action,
                               const QByteArray &inner)
    {
        return QStringLiteral(
                   "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/"
                   "soap/envelope/\"><s:Body><u:%1Response xmlns:u=\"%2\">")
                   .arg(action, service)
                   .toUtf8()
               + inner
               + QStringLiteral("</u:%1Response></s:Body></s:Envelope>").arg(action).toUtf8();
    }

    void serve(QTcpSocket *socket)
    {
        QByteArray buffer = socket->property("buffer").toByteArray();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray head = buffer.left(headerEnd);
        const QList<QByteArray> lines = head.split('\n');
        if (lines.isEmpty())
            return;
        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() < 2)
            return;
        const QByteArray method = requestLine.at(0);
        const QByteArray path = requestLine.at(1);
        qsizetype length = 0;
        QByteArray soapAction;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon < 0)
                continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (name == "content-length")
                length = value.toInt();
            else if (name == "soapaction")
                soapAction = value;
        }
        if (buffer.size() < headerEnd + 4 + length)
            return;
        const QByteArray body = buffer.mid(headerEnd + 4, length);
        buffer.remove(0, headerEnd + 4 + length);
        socket->setProperty("buffer", buffer);
        headers.append(head);

        QByteArray response;
        QByteArray status = "200 OK";
        if (method == "GET" && path == "/description.xml") {
            response = description();
        } else if (method == "GET" && path == "/AVTransport/scpd.xml") {
            response = scpd();
        } else if (method == "GET" && path.startsWith("/media")) {
            ++mediaRequests;
            QByteArray head = "HTTP/1.1 " + QByteArray::number(mediaStatus) + " Media\r\n";
            if (mediaStatus == 206)
                head += "Content-Range: bytes 0-0/1000\r\n";
            socket->write(head + "Content-Length: 1\r\nConnection: close\r\n\r\nx");
            socket->disconnectFromHost();
            return;
        } else if (method == "POST") {
            bodies.append(body);
            const QString action = actionOf(soapAction);
            if (faults.contains(action) || faultsOnce.removeOne(action)) {
                actions.append(action);
                status = "500 Internal Server Error";
                response = fault();
            } else {
                response = soap(soapAction);
            }
            if (pollDelayMs > 0
                && (action == QLatin1StringView("GetTransportInfo")
                    || action == QLatin1StringView("GetPositionInfo"))) {
                QTimer::singleShot(pollDelayMs, socket,
                                   [socket, response] { reply(socket, "200 OK", response); });
                return;
            }
        } else {
            socket->write(
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        reply(socket, status, response);
    }

    static void reply(QTcpSocket *socket, const QByteArray &status, const QByteArray &response)
    {
        socket->write("HTTP/1.1 " + status
                      + "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
                        "Content-Length: "
                      + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n"
                      + response);
        socket->disconnectFromHost();
    }

    static QString actionOf(const QByteArray &header)
    {
        const QByteArray value = header.mid(1, header.size() - 2);
        return QString::fromUtf8(value.mid(value.indexOf('#') + 1));
    }

    QByteArray fault() const
    {
        return QStringLiteral(
                   "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/"
                   "envelope/\"><s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>"
                   "UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:"
                   "control-1-0\"><errorCode>%1</errorCode><errorDescription>Refused"
                   "</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>")
            .arg(faultCode)
            .toUtf8();
    }

    QByteArray soap(const QByteArray &header)
    {
        const QByteArray value = header.mid(1, header.size() - 2);
        const int hash = value.indexOf('#');
        const QString service = QString::fromUtf8(value.left(hash));
        const QString action = QString::fromUtf8(value.mid(hash + 1));
        actions.append(action);
        if (failPolls
            && (action == QLatin1StringView("GetTransportInfo")
                || action == QLatin1StringView("GetPositionInfo")))
            return {};
        if (action == QLatin1StringView("SetAVTransportURI")) {
            const QString uri = QString::fromUtf8(bodies.constLast());
            const int start = uri.indexOf(QStringLiteral("<CurrentURI>"));
            const int end = uri.indexOf(QStringLiteral("</CurrentURI>"));
            if (start >= 0 && end > start) {
                trackUri = uri.mid(start + 12, end - start - 12);
                trackUri.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
            }
            return envelope(service, action, "<InstanceID>0</InstanceID>");
        }
        if (action == QLatin1StringView("Play")) {
            transport = playStarts ? QStringLiteral("PLAYING") : QStringLiteral("TRANSITIONING");
            return envelope(service, action, "<InstanceID>0</InstanceID>");
        }
        if (action == QLatin1StringView("Pause")) {
            transport = QStringLiteral("PAUSED_PLAYBACK");
            return envelope(service, action, "<InstanceID>0</InstanceID>");
        }
        if (action == QLatin1StringView("Stop")) {
            transport = QStringLiteral("STOPPED");
            return envelope(service, action, "<InstanceID>0</InstanceID>");
        }
        if (action == QLatin1StringView("Seek")) {
            const QString body = QString::fromUtf8(bodies.constLast());
            const int start = body.indexOf(QStringLiteral("<Target>"));
            const int end = body.indexOf(QStringLiteral("</Target>"));
            if (start >= 0 && end > start) {
                const QStringList parts =
                    body.mid(start + 8, end - start - 8).split(QLatin1Char(':'));
                position = parts.size() == 3 ? parts.at(0).toInt() * 3600 + parts.at(1).toInt() * 60
                                                   + parts.at(2).toInt()
                                             : parts.constLast().toInt();
            }
            return envelope(service, action, "<InstanceID>0</InstanceID>");
        }
        if (action == QLatin1StringView("GetTransportInfo")) {
            const QByteArray answer =
                envelope(service, action,
                         QStringLiteral("<CurrentTransportState>%1</CurrentTransportState>"
                                        "<CurrentTransportStatus>OK</CurrentTransportStatus>"
                                        "<CurrentSpeed>1</CurrentSpeed>")
                             .arg(transport)
                             .toUtf8());
            // A renderer that could not fetch the stream gives up after one TRANSITIONING
            if (!playStarts && transport == QLatin1StringView("TRANSITIONING"))
                transport = QStringLiteral("STOPPED");
            return answer;
        }
        if (action == QLatin1StringView("GetPositionInfo")) {
            if (failPolls)
                return QByteArray();
            const auto clock = [](int seconds) {
                return QStringLiteral("%1:%2:%3")
                    .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
                    .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
                    .arg(seconds % 60, 2, 10, QLatin1Char('0'));
            };
            const QString time = clock(position);
            const QString total = clock(duration);
            return envelope(
                service, action,
                QStringLiteral("<Track>1</Track><TrackDuration>%1</TrackDuration><TrackMetaData>"
                               "</TrackMetaData><TrackURI>%2</TrackURI><RelTime>%3</RelTime>"
                               "<AbsTime>%3</AbsTime><RelCount>0</RelCount><AbsCount>0</AbsCount>")
                    .arg(total,
                         (reportedTrackUri.isEmpty() ? trackUri : reportedTrackUri).toHtmlEscaped(),
                         time)
                    .toUtf8());
        }
        if (action == QLatin1StringView("GetProtocolInfo"))
            return envelope(
                service, action,
                "<Source></Source><Sink>http-get:*:video/mp4:*,http-get:*:video/x-matroska:*"
                "</Sink>");
        if (action == QLatin1StringView("GetVolume"))
            return envelope(
                service, action,
                QStringLiteral("<CurrentVolume>%1</CurrentVolume>").arg(volume).toUtf8());
        if (action == QLatin1StringView("SetVolume")) {
            const QString body = QString::fromUtf8(bodies.constLast());
            const int start = body.indexOf(QStringLiteral("<DesiredVolume>"));
            const int end = body.indexOf(QStringLiteral("</DesiredVolume>"));
            if (start >= 0 && end > start)
                volume = body.mid(start + 15, end - start - 15).toInt();
            return envelope(service, action, "<InstanceID>0</InstanceID>");
        }
        if (action == QLatin1StringView("GetMute"))
            return envelope(
                service, action,
                QStringLiteral("<CurrentMute>%1</CurrentMute>").arg(muted ? 1 : 0).toUtf8());
        if (action == QLatin1StringView("SetMute"))
            return envelope(service, action, "<InstanceID>0</InstanceID>");
        return QByteArray();
    }

    QTcpServer m_server;
};
