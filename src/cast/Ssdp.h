// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QNetworkAddressEntry>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringView>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>

#include <optional>

namespace Kinefin {

// An SSDP datagram is read only up to this size (design: Security, SSDP datagram 4 KiB); a larger one is dropped
inline constexpr int kMaxSsdpDatagramBytes = 4096;
// Devices one search reports (design: Security, 32 devices)
inline constexpr int kMaxSsdpDevices = 32;
// Datagrams one search window reads before it drops the rest; bounds a flood from a hostile host on the network
inline constexpr int kMaxSsdpDatagramsPerSearch = 256;
// The longest max-age honoured, in seconds (design: Discovery and descriptions, at most 1800 s), and what a response without one gets
inline constexpr int kSsdpMaxAgeCapSeconds = 1800;
// M-SEARCH parameters (design: Components, MX 2, sent at 0 s and 1 s, TTL 2, a 5 s window)
inline constexpr int kSsdpMx = 2;
inline constexpr int kSsdpMulticastTtl = 2;
inline constexpr int kSsdpRepeatMs = 1000;
inline constexpr int kSsdpWindowMs = 5000;
// The only search target: what the picker lists
inline constexpr QLatin1StringView kMediaRendererSearchTarget{
    "urn:schemas-upnp-org:device:MediaRenderer:1"};

using AddressPolicy = bool (*)(const QHostAddress &);

// The address policy of renderers and of everything they answer (design: Security, Sender address): RFC 1918, 169.254.0.0/16 except the cloud metadata address 169.254.169.254, IPv6 unique local (fc00::/7, except the AWS metadata address fd00:ec2::254) and link-local (fe80::/10). IPv4-mapped IPv6 is judged as the IPv4 address. Never loopback, multicast, unspecified or public
bool isAllowedRendererAddress(const QHostAddress &address);
// The address lies in the subnet of one of these interface addresses (an SSDP sender is spoofable: only a neighbour on a local link is believed)
bool isInAnySubnet(const QHostAddress &address, const QList<QNetworkAddressEntry> &entries);
// isAllowedRendererAddress, and on the subnet of one of this host's interfaces: the default of a search
bool isLocalRendererAddress(const QHostAddress &address);

// The address without scope id, an IPv4-mapped IPv6 address as plain IPv4: what two addresses are compared by
QHostAddress normalizedAddress(const QHostAddress &address);

// The host of a URL without the brackets of an IPv6 literal
QString hostWithoutBrackets(const QUrl &url);

// `uuid:<id>` lower-cased, from a USN or a `UDN` element; the id is 1 to 128 characters of letters, digits and `-_.:`. Anything else is nullopt
std::optional<QString> normalizeUdn(QStringView text);

// True for an `http` URL with no credentials and a usable port whose host is the IP literal `address` (IPv6 scope ignored): the origin a renderer's LOCATION must have
bool isValidLocation(const QUrl &url, const QHostAddress &address);

// A LOCATION header value as a URL, nullopt when it is longer than 512 characters, does not parse strictly or fails isValidLocation
std::optional<QUrl> parseRendererLocation(const QString &text, const QHostAddress &address);

struct SsdpResponse {
    // `uuid:` plus the id, lower-cased
    QString udn;
    QString usn;
    QString searchTarget;
    QUrl location;
    QHostAddress sender;
    int maxAgeSeconds = kSsdpMaxAgeCapSeconds;
};

// An M-SEARCH for kMediaRendererSearchTarget
QByteArray ssdpSearchRequest();

// A unicast reply to an M-SEARCH. Kept only when it is at most kMaxSsdpDatagramBytes, its sender passes `policy`, the status line is `HTTP/1.1 200`, LOCATION, ST and USN are present, the USN names a valid UDN, LOCATION passes parseRendererLocation for the sender and CACHE-CONTROL carries a positive max-age or none. Headers only: a body is ignored, a bare CR or LF or a NUL anywhere drops the datagram. max-age is capped at kSsdpMaxAgeCapSeconds
std::optional<SsdpResponse> parseSsdpResponse(const QByteArray &datagram,
                                              const QHostAddress &sender,
                                              AddressPolicy policy = isAllowedRendererAddress);

// One search for MediaRenderer devices: sends the M-SEARCH at 0 and kSsdpRepeatMs on every up, running, multicast-capable, non point-to-point IPv4 interface, collects the replies for the window, then reports finished(). Each UDN is reported once; a UDN seen from another address is ignored and logged once. Used only while the picker is open
class SsdpSearch : public QObject {
    Q_OBJECT

  public:
    // The defaults are the production values; the tests aim target and port at a unicast socket and shorten the times
    struct Options {
        QHostAddress target = QHostAddress(QStringLiteral("239.255.255.250"));
        quint16 port = 1900;
        int windowMs = kSsdpWindowMs;
        int repeatMs = kSsdpRepeatMs;
        AddressPolicy senderPolicy = isLocalRendererAddress;
    };

    explicit SsdpSearch(QObject *parent = nullptr);
    explicit SsdpSearch(const Options &options, QObject *parent = nullptr);
    ~SsdpSearch() override;

    bool running() const { return m_running; }

    // Starts a search and forgets the devices of the previous one. False, with one warning, when the socket cannot be bound
    bool start();
    // Ends the search without finished()
    void stop();

  Q_SIGNALS:
    void deviceFound(const Kinefin::SsdpResponse &response);
    void finished();

  private:
    void sendQuery();
    void readPending();
    void handleDatagram(const QByteArray &data, const QHostAddress &sender);
    void endWindow();

    Options m_options;
    QUdpSocket m_socket;
    QTimer m_repeatTimer;
    QTimer m_windowTimer;
    QHash<QString, QHostAddress> m_seen;
    QSet<QString> m_conflicting;
    int m_datagrams = 0;
    bool m_running = false;
    bool m_limitLogged = false;
};

} // namespace Kinefin
