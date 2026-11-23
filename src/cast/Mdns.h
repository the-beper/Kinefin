// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cast/Ssdp.h"

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUdpSocket>

namespace Kinefin {

// An mDNS datagram is read only up to this size; a larger one is dropped
inline constexpr int kMaxMdnsDatagramBytes = 9000;
// Records one section and one message hold before the rest is dropped
inline constexpr int kMaxMdnsRecordsPerSection = 64;
inline constexpr int kMaxMdnsRecords = 128;
// A compression pointer must point before the label that holds it, and at most this many are followed in one name
inline constexpr int kMaxMdnsPointerJumps = 32;
// TXT strings one record holds
inline constexpr int kMaxMdnsTxtStrings = 32;
// Devices and datagrams one search reports and reads
inline constexpr int kMaxMdnsDevices = 32;
inline constexpr int kMaxMdnsDatagramsPerSearch = 256;
// Sent at 0 s and 1 s, a 5 s window, TTL 255
inline constexpr int kMdnsRepeatMs = 1000;
inline constexpr int kMdnsWindowMs = 5000;
inline constexpr int kMdnsMulticastTtl = 255;
inline constexpr QLatin1StringView kGoogleCastService{"_googlecast._tcp.local"};

// A Cast device whose PTR, SRV, TXT and A records were all seen and passed the checks
struct CastDeviceInfo {
    // 32 lower-case hex characters: the TXT id with any dashes removed (a group has a 36-character UUID)
    QString id;
    // The TXT fn and md, sanitised and cut to 64 characters
    QString name;
    QString model;
    // The A record, which equals the datagram's sender
    QHostAddress address;
    quint16 port = 8009;
    // The TXT ca capability bits and ve version
    int capabilities = 0;
    int version = 0;
    // The DNS-SD instance name, the key the records are merged by
    QString instance;

    bool operator==(const CastDeviceInfo &other) const = default;
};

// A PTR query for _googlecast._tcp.local, class IN with no QU bit, one question, no other record
QByteArray mdnsQuery();

struct MdnsUpdate {
    // Devices that became complete with this datagram, in the order their instances appeared
    QList<CastDeviceInfo> found;
    // Ids whose instance was withdrawn (a TTL of 0 on its PTR, SRV or TXT)
    QStringList gone;
};

// Merges the datagrams of one search. A device is reported once it has SRV, TXT and an A record for the SRV target, and that A record equals the datagram's sender; the same instance from another sender, or the same id from another address, is ignored. The checks: the sender passes `policy`; records are bounded (kMaxMdnsRecordsPerSection, kMaxMdnsRecords, kMaxMdnsPointerJumps, labels of 63 bytes, names of 255, rdlength inside the packet); only PTR, SRV, TXT and A are read; TXT keys are lower-cased, at most kMaxMdnsTxtStrings, the first of a duplicate wins; ve is at least 2, fn is required, `ca & 1` (video out) is required, the port is 1 to 65535; at most kMaxMdnsDevices ids
class MdnsAccumulator {
  public:
    explicit MdnsAccumulator(AddressPolicy policy = isLocalRendererAddress);
    MdnsUpdate add(const QByteArray &datagram, const QHostAddress &sender);
    int deviceCount() const;

  private:
    struct Instance {
        // The instance name as sent, and the SRV target and port
        QString name;
        QString target;
        quint16 port = 0;
        bool hasSrv = false;
        bool hasTxt = false;
        QHash<QString, QString> txt;
        // The first sender that mentioned the instance, records of any other sender are ignored
        QHostAddress sender;
        quint32 order = 0;
        bool reported = false;
        // Complete but failed the checks, until a new SRV or TXT record arrives
        bool rejected = false;
        QString id;
    };

    // The instance of a name, created on first sight; null for another sender or when too many are tracked
    auto instanceFor(const QString &name, const QHostAddress &sender) -> Instance *;
    void withdraw(const QString &name, const QHostAddress &sender, MdnsUpdate &update);
    void reportComplete(const QHostAddress &sender, MdnsUpdate &update);

    AddressPolicy m_policy;
    // Keyed by the lower-case instance name
    QHash<QString, Instance> m_instances;
    // "sender|host" of every A record that equals the sender of its datagram
    QSet<QString> m_hosts;
    QHash<QString, QHostAddress> m_ids;
    QSet<QString> m_conflicting;
    quint32 m_nextOrder = 0;
    bool m_limitLogged = false;
};

// One search for Cast devices: sends mdnsQuery() at 0 and kMdnsRepeatMs, on every interface of castInterfaces() (see CastNetwork.h), collects the answers for the window, then reports finished(). The socket binds to the bind port with ShareAddress | ReuseAddressHint and joins 224.0.0.251 on each interface; when that bind fails it binds an ephemeral port (legacy unicast, RFC 6762 6.7). Used only while the picker is open
class MdnsSearch : public QObject {
    Q_OBJECT

  public:
    // The defaults are the production values; the tests aim target and port at a unicast socket, shorten the times and accept any sender port and address
    struct Options {
        QHostAddress target = QHostAddress(QStringLiteral("224.0.0.251"));
        // Where the queries go
        quint16 port = 5353;
        // The port the socket binds to
        quint16 bindPort = 5353;
        // A datagram must come from this port; 0 accepts any (tests)
        quint16 senderPort = 5353;
        int windowMs = kMdnsWindowMs;
        int repeatMs = kMdnsRepeatMs;
        AddressPolicy senderPolicy = isLocalRendererAddress;
    };

    explicit MdnsSearch(QObject *parent = nullptr);
    explicit MdnsSearch(const Options &options, QObject *parent = nullptr);
    ~MdnsSearch() override;

    bool running() const { return m_running; }
    // True when the shared bind failed and the search listens on an ephemeral port
    bool usingFallbackPort() const { return m_fallback; }

    // Starts a search and forgets the devices of the previous one. False, with one warning, when no socket can be bound
    bool start();
    // Ends the search without finished()
    void stop();

  Q_SIGNALS:
    void deviceFound(const Kinefin::CastDeviceInfo &device);
    void deviceGone(const QString &id);
    void finished();

  private:
    void sendQuery();
    void readPending();
    void endWindow();

    Options m_options;
    QUdpSocket m_socket;
    QTimer m_repeatTimer;
    QTimer m_windowTimer;
    MdnsAccumulator m_accumulator;
    int m_datagrams = 0;
    bool m_running = false;
    bool m_fallback = false;
    bool m_limitLogged = false;
};

} // namespace Kinefin
