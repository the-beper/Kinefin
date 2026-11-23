// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/Ssdp.h"

#include "cast/CastNetwork.h"

#include <QLoggingCategory>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRegularExpression>

#include <algorithm>

namespace Kinefin {
namespace {
Q_LOGGING_CATEGORY(ssdpLog, "kinefin.cast.ssdp")

constexpr int kMaxLocationChars = 512;
constexpr int kMaxHeaderValueChars = 256;
constexpr int kMaxUdnIdChars = 128;

std::optional<quint32> ipv4Of(const QHostAddress &address)
{
    if (address.protocol() == QAbstractSocket::IPv4Protocol)
        return address.toIPv4Address();
    if (address.protocol() != QAbstractSocket::IPv6Protocol)
        return std::nullopt;
    const Q_IPV6ADDR a = address.toIPv6Address();
    for (int i = 0; i < 10; ++i) {
        if (a[i] != 0)
            return std::nullopt;
    }
    if (a[10] != 0xFF || a[11] != 0xFF)
        return std::nullopt;
    return (quint32(a[12]) << 24) | (quint32(a[13]) << 16) | (quint32(a[14]) << 8) | quint32(a[15]);
}

bool isAllowedIpv4(quint32 v)
{
    const quint32 a = v >> 24;
    const quint32 b = (v >> 16) & 0xFF;
    if (a == 10)
        return true;
    if (a == 172 && b >= 16 && b <= 31)
        return true;
    if (a == 192 && b == 168)
        return true;
    if (a == 169 && b == 254)
        return v != 0xA9FEA9FE;
    return false;
}

bool isAllowedIpv6(const QHostAddress &address)
{
    if (address.isLoopback() || address.isMulticast() || address.isNull())
        return false;
    const Q_IPV6ADDR a = address.toIPv6Address();
    const bool uniqueLocal = (a[0] & 0xFE) == 0xFC;
    const bool linkLocal = a[0] == 0xFE && (a[1] & 0xC0) == 0x80;
    if (!uniqueLocal && !linkLocal)
        return false;
    // fd00:ec2::254, the AWS metadata address
    static const QHostAddress awsMetadata(QStringLiteral("fd00:ec2::254"));
    return address != awsMetadata;
}

// No readable max-age directive means the cap; a directive of zero is refused
std::optional<int> parseMaxAge(const QString &cacheControl)
{
    static const QRegularExpression maxAge(
        QStringLiteral("(?:^|,)\\s*max-age\\s*=\\s*(\\d{1,9})\\s*(?:,|$)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = maxAge.match(cacheControl);
    if (!match.hasMatch())
        return kSsdpMaxAgeCapSeconds;
    const int value = match.captured(1).toInt();
    if (value <= 0)
        return std::nullopt;
    return qMin(value, kSsdpMaxAgeCapSeconds);
}
} // namespace

QString hostWithoutBrackets(const QUrl &url)
{
    QString host = url.host();
    if (host.size() >= 2 && host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.mid(1, host.size() - 2);
    return host;
}

QHostAddress normalizedAddress(const QHostAddress &address)
{
    if (const auto v4 = ipv4Of(address))
        return QHostAddress(*v4);
    QHostAddress copy = address;
    copy.setScopeId(QString());
    return copy;
}

bool isAllowedRendererAddress(const QHostAddress &address)
{
    if (address.isNull())
        return false;
    const QHostAddress normalized = normalizedAddress(address);
    if (normalized.protocol() == QAbstractSocket::IPv4Protocol)
        return isAllowedIpv4(normalized.toIPv4Address());
    if (normalized.protocol() == QAbstractSocket::IPv6Protocol)
        return isAllowedIpv6(normalized);
    return false;
}

bool isInAnySubnet(const QHostAddress &address, const QList<QNetworkAddressEntry> &entries)
{
    const QHostAddress plain = normalizedAddress(address);
    return std::any_of(
        entries.cbegin(), entries.cend(), [&plain](const QNetworkAddressEntry &entry) {
            return entry.prefixLength() > 0
                   && plain.isInSubnet(normalizedAddress(entry.ip()), entry.prefixLength());
        });
}

bool isLocalRendererAddress(const QHostAddress &address)
{
    if (!isAllowedRendererAddress(address))
        return false;
    QList<QNetworkAddressEntry> entries;
    for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
        if (interface.flags().testFlag(QNetworkInterface::IsUp))
            entries << interface.addressEntries();
    }
    return isInAnySubnet(address, entries);
}

std::optional<QString> normalizeUdn(QStringView text)
{
    const QStringView trimmed = text.trimmed();
    static constexpr QStringView prefix = u"uuid:";
    if (!trimmed.startsWith(prefix, Qt::CaseInsensitive))
        return std::nullopt;
    const QStringView id = trimmed.sliced(prefix.size());
    if (id.isEmpty() || id.size() > kMaxUdnIdChars)
        return std::nullopt;
    for (const QChar c : id) {
        const char16_t u = c.unicode();
        const bool ok = (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'z')
                        || (u >= u'A' && u <= u'Z') || u == u'-' || u == u'_' || u == u'.'
                        || u == u':';
        if (!ok)
            return std::nullopt;
    }
    return trimmed.toString().toLower();
}

bool isValidLocation(const QUrl &url, const QHostAddress &address)
{
    if (!url.isValid() || url.scheme() != QLatin1StringView("http"))
        return false;
    if (!url.userInfo().isEmpty() || url.host().isEmpty())
        return false;
    if (url.port(80) < 1)
        return false;
    QHostAddress host;
    if (!host.setAddress(hostWithoutBrackets(url)))
        return false;
    return normalizedAddress(host) == normalizedAddress(address);
}

std::optional<QUrl> parseRendererLocation(const QString &text, const QHostAddress &address)
{
    if (text.isEmpty() || text.size() > kMaxLocationChars)
        return std::nullopt;
    const QUrl url(text, QUrl::StrictMode);
    if (!isValidLocation(url, address))
        return std::nullopt;
    return url;
}

QByteArray ssdpSearchRequest()
{
    return "M-SEARCH * HTTP/1.1\r\n"
           "HOST: 239.255.255.250:1900\r\n"
           "MAN: \"ssdp:discover\"\r\n"
           "MX: "
           + QByteArray::number(kSsdpMx) + "\r\nST: "
           + QByteArray(kMediaRendererSearchTarget.data(), kMediaRendererSearchTarget.size())
           + "\r\nUSER-AGENT: Linux UPnP/1.1 Kinefin\r\n\r\n";
}

std::optional<SsdpResponse> parseSsdpResponse(const QByteArray &datagram,
                                              const QHostAddress &sender, AddressPolicy policy)
{
    if (datagram.isEmpty() || datagram.size() > kMaxSsdpDatagramBytes)
        return std::nullopt;
    if (!policy || !policy(sender))
        return std::nullopt;
    if (datagram.contains('\0'))
        return std::nullopt;

    QHash<QByteArray, QByteArray> headers;
    bool statusSeen = false;
    qsizetype pos = 0;
    while (true) {
        const qsizetype end = datagram.indexOf("\r\n", pos);
        const QByteArray line = end < 0 ? datagram.mid(pos) : datagram.mid(pos, end - pos);
        if (line.contains('\r') || line.contains('\n'))
            return std::nullopt;
        if (!statusSeen) {
            const bool ok =
                line.startsWith("HTTP/1.1 200") && (line.size() == 12 || line.at(12) == ' ');
            if (!ok)
                return std::nullopt;
            statusSeen = true;
        } else if (line.isEmpty()) {
            break;
        } else {
            const qsizetype colon = line.indexOf(':');
            if (colon > 0) {
                const QByteArray name = line.left(colon).trimmed().toLower();
                if (!headers.contains(name))
                    headers.insert(name, line.mid(colon + 1).trimmed());
            }
        }
        if (end < 0)
            break;
        pos = end + 2;
    }

    const auto value = [&headers](const char *name) {
        return QString::fromLatin1(headers.value(name));
    };
    const QString location = value("location");
    const QString st = value("st");
    const QString usn = value("usn");
    if (location.isEmpty() || st.isEmpty() || usn.isEmpty())
        return std::nullopt;
    if (st.size() > kMaxHeaderValueChars || usn.size() > kMaxHeaderValueChars)
        return std::nullopt;

    const qsizetype separator = usn.indexOf(QLatin1StringView("::"));
    const auto udn =
        normalizeUdn(separator < 0 ? QStringView(usn) : QStringView(usn).first(separator));
    if (!udn)
        return std::nullopt;
    const auto url = parseRendererLocation(location, sender);
    if (!url)
        return std::nullopt;
    const auto maxAge = parseMaxAge(value("cache-control"));
    if (!maxAge)
        return std::nullopt;

    SsdpResponse response;
    response.udn = *udn;
    response.usn = usn;
    response.searchTarget = st;
    response.location = *url;
    response.sender = sender;
    response.maxAgeSeconds = *maxAge;
    return response;
}

SsdpSearch::SsdpSearch(QObject *parent) : SsdpSearch(Options{}, parent) {}

SsdpSearch::SsdpSearch(const Options &options, QObject *parent)
    : QObject(parent), m_options(options)
{
    m_repeatTimer.setSingleShot(true);
    m_windowTimer.setSingleShot(true);
    connect(&m_repeatTimer, &QTimer::timeout, this, &SsdpSearch::sendQuery);
    connect(&m_windowTimer, &QTimer::timeout, this, &SsdpSearch::endWindow);
    connect(&m_socket, &QUdpSocket::readyRead, this, &SsdpSearch::readPending);
}

SsdpSearch::~SsdpSearch() = default;

bool SsdpSearch::start()
{
    stop();
    m_seen.clear();
    m_conflicting.clear();
    m_datagrams = 0;
    m_limitLogged = false;
    if (!m_socket.bind(QHostAddress::AnyIPv4, 0)) {
        qCWarning(ssdpLog) << "cannot bind the search socket:" << m_socket.errorString();
        return false;
    }
    m_socket.setSocketOption(QAbstractSocket::MulticastTtlOption, kSsdpMulticastTtl);
    m_running = true;
    m_windowTimer.start(m_options.windowMs);
    m_repeatTimer.start(m_options.repeatMs);
    sendQuery();
    return true;
}

void SsdpSearch::stop()
{
    m_repeatTimer.stop();
    m_windowTimer.stop();
    m_socket.close();
    m_running = false;
}

void SsdpSearch::sendQuery()
{
    if (!m_running)
        return;
    const QByteArray request = ssdpSearchRequest();
    if (!m_options.target.isMulticast()) {
        m_socket.writeDatagram(request, m_options.target, m_options.port);
        return;
    }
    for (const CastInterface &iface : castInterfaces()) {
        m_socket.setMulticastInterface(iface.interface);
        m_socket.writeDatagram(request, m_options.target, m_options.port);
    }
}

void SsdpSearch::readPending()
{
    while (m_socket.hasPendingDatagrams()) {
        // One byte more than the limit tells an oversized datagram from one of exactly the limit
        const QNetworkDatagram datagram = m_socket.receiveDatagram(kMaxSsdpDatagramBytes + 1);
        if (!datagram.isValid())
            continue;
        if (++m_datagrams > kMaxSsdpDatagramsPerSearch)
            continue;
        handleDatagram(datagram.data(), datagram.senderAddress());
    }
}

void SsdpSearch::handleDatagram(const QByteArray &data, const QHostAddress &sender)
{
    const auto response = parseSsdpResponse(data, sender, m_options.senderPolicy);
    if (!response)
        return;
    if (response->searchTarget.compare(kMediaRendererSearchTarget, Qt::CaseInsensitive) != 0)
        return;
    const auto known = m_seen.constFind(response->udn);
    if (known != m_seen.constEnd()) {
        if (normalizedAddress(known.value()) != normalizedAddress(sender)
            && !m_conflicting.contains(response->udn)) {
            m_conflicting.insert(response->udn);
            qCWarning(ssdpLog) << "ignoring a second address for" << response->udn;
        }
        return;
    }
    if (m_seen.size() >= kMaxSsdpDevices) {
        if (!m_limitLogged)
            qCWarning(ssdpLog) << "more than" << kMaxSsdpDevices
                               << "devices answered, ignoring the rest";
        m_limitLogged = true;
        return;
    }
    m_seen.insert(response->udn, sender);
    Q_EMIT deviceFound(*response);
}

void SsdpSearch::endWindow()
{
    stop();
    Q_EMIT finished();
}

} // namespace Kinefin
