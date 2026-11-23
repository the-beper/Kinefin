// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/Mdns.h"

#include "cast/CastNetwork.h"
#include "cast/CastText.h"

#include <QLoggingCategory>
#include <QNetworkDatagram>
#include <QVariant>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace Kinefin {
namespace {
Q_LOGGING_CATEGORY(mdnsLog, "kinefin.cast.mdns")

constexpr int kHeaderBytes = 12;
constexpr int kMaxNameBytes = 255;
// Instances and A records one accumulator tracks whatever their state, so that a flood of names cannot grow it
constexpr qsizetype kMaxTrackedEntries = 256;

constexpr quint16 kTypeA = 1;
constexpr quint16 kTypePtr = 12;
constexpr quint16 kTypeTxt = 16;
constexpr quint16 kTypeSrv = 33;
constexpr quint16 kClassIn = 1;
constexpr QLatin1StringView kInstanceSuffix{"._googlecast._tcp.local"};

struct DnsRecord {
    // 0 for a record that is not read: another type or class
    quint16 type = 0;
    quint32 ttl = 0;
    QString owner;
    // PTR and SRV
    QString target;
    quint16 port = 0;
    QHash<QString, QString> txt;
    quint32 ipv4 = 0;
};

// The callers check that `pos` and `pos + 1` lie inside the datagram
quint16 be16(const QByteArray &d, qsizetype pos)
{
    return quint16((quint8(d.at(pos)) << 8) | quint8(d.at(pos + 1)));
}

quint32 be32(const QByteArray &d, qsizetype pos)
{
    return (quint32(be16(d, pos)) << 16) | be16(d, pos + 2);
}

// Reads the name at `pos` into `name` and the offset right after it, where it started, into `end`. The labels before the first compression pointer must end before `limit`
bool readName(const QByteArray &d, qsizetype pos, qsizetype limit, QString *name, qsizetype *end)
{
    QString result;
    // The root label
    qsizetype wireBytes = 1;
    qsizetype bound = limit;
    int jumps = 0;
    bool jumped = false;
    while (true) {
        if (pos >= bound)
            return false;
        const quint8 length = quint8(d.at(pos));
        if (length == 0) {
            if (!jumped)
                *end = pos + 1;
            break;
        }
        // The two high bits of a length byte: 00 a label of at most 63 bytes, 11 a pointer, 01 and 10 reserved
        if ((length & 0xC0) == 0xC0) {
            if (pos + 1 >= bound)
                return false;
            const qsizetype target = ((length & 0x3F) << 8) | quint8(d.at(pos + 1));
            if (target >= pos || ++jumps > kMaxMdnsPointerJumps)
                return false;
            if (!jumped)
                *end = pos + 2;
            jumped = true;
            pos = target;
            bound = d.size();
            continue;
        }
        if ((length & 0xC0) != 0 || pos + 1 + length > bound)
            return false;
        wireBytes += 1 + length;
        if (wireBytes > kMaxNameBytes)
            return false;
        if (!result.isEmpty())
            result += QLatin1Char('.');
        result += QString::fromUtf8(d.constData() + pos + 1, length);
        pos += 1 + length;
    }
    *name = result;
    return true;
}

bool readTxt(const QByteArray &d, qsizetype pos, qsizetype end, QHash<QString, QString> *txt)
{
    for (int strings = 0; pos < end && strings < kMaxMdnsTxtStrings; ++strings) {
        const qsizetype length = quint8(d.at(pos++));
        if (length > end - pos)
            return false;
        qsizetype equals = -1;
        for (qsizetype i = 0; i < length && equals < 0; ++i) {
            if (d.at(pos + i) == '=')
                equals = i;
        }
        const qsizetype keyLength = equals < 0 ? length : equals;
        if (keyLength > 0) {
            const QString key = QString::fromLatin1(d.constData() + pos, keyLength).toLower();
            if (!txt->contains(key)) {
                txt->insert(key, equals < 0 ? QString()
                                            : QString::fromUtf8(d.constData() + pos + equals + 1,
                                                                length - equals - 1));
            }
        }
        pos += length;
    }
    return true;
}

// Reads the record at `pos` and moves `pos` past it. False when it does not fit the datagram or what is read of it is malformed
bool readRecord(const QByteArray &d, qsizetype *pos, DnsRecord *record)
{
    qsizetype fixed = 0;
    if (!readName(d, *pos, d.size(), &record->owner, &fixed) || fixed + 10 > d.size())
        return false;
    const quint16 type = be16(d, fixed);
    // The top bit of the class is the cache-flush flag
    const quint16 recordClass = be16(d, fixed + 2) & 0x7FFF;
    record->ttl = be32(d, fixed + 4);
    const qsizetype dataStart = fixed + 10;
    const qsizetype dataEnd = dataStart + be16(d, fixed + 8);
    if (dataEnd > d.size())
        return false;
    *pos = dataEnd;
    if (recordClass != kClassIn)
        return true;
    qsizetype nameEnd = 0;
    switch (type) {
    case kTypePtr:
        record->type = type;
        return readName(d, dataStart, dataEnd, &record->target, &nameEnd);
    case kTypeSrv:
        record->type = type;
        if (dataEnd - dataStart < 6)
            return false;
        record->port = be16(d, dataStart + 4);
        return readName(d, dataStart + 6, dataEnd, &record->target, &nameEnd);
    case kTypeTxt:
        record->type = type;
        return readTxt(d, dataStart, dataEnd, &record->txt);
    case kTypeA:
        record->type = type;
        if (dataEnd - dataStart != 4)
            return false;
        record->ipv4 = be32(d, dataStart);
        return true;
    default:
        return true;
    }
}

// The records worth reading of one response, nullopt when the datagram is refused as a whole
std::optional<QList<DnsRecord>> parseMessage(const QByteArray &d)
{
    if (d.size() < kHeaderBytes || d.size() > kMaxMdnsDatagramBytes)
        return std::nullopt;
    // QR set, opcode 0 and rcode 0: a response, not a query, which the multicast group delivers too
    if ((be16(d, 2) & 0xF80F) != 0x8000)
        return std::nullopt;
    qsizetype pos = kHeaderBytes;
    QString ignored;
    for (int questions = be16(d, 4); questions > 0; --questions) {
        qsizetype end = 0;
        if (!readName(d, pos, d.size(), &ignored, &end) || end + 4 > d.size())
            return std::nullopt;
        pos = end + 4;
    }
    QList<DnsRecord> records;
    int total = 0;
    for (const qsizetype countAt : {6, 8, 10}) {
        const int count = be16(d, countAt);
        for (int i = 0; i < count; ++i) {
            if (i >= kMaxMdnsRecordsPerSection || total >= kMaxMdnsRecords)
                return records;
            DnsRecord record;
            if (!readRecord(d, &pos, &record))
                return std::nullopt;
            ++total;
            if (record.type != 0)
                records.append(std::move(record));
        }
    }
    return records;
}

bool isInstanceName(const QString &name)
{
    return name.size() > kInstanceSuffix.size()
           && name.endsWith(kInstanceSuffix, Qt::CaseInsensitive);
}

QString hostKey(const QHostAddress &sender, const QString &host)
{
    return sender.toString() + QLatin1Char('|') + host.toLower();
}

// Digits only, at most int
std::optional<int> parseDecimal(const QString &text)
{
    if (text.isEmpty() || text.size() > 10)
        return std::nullopt;
    qint64 value = 0;
    for (const QChar c : text) {
        if (c.unicode() < u'0' || c.unicode() > u'9')
            return std::nullopt;
        value = value * 10 + (c.unicode() - u'0');
    }
    if (value > std::numeric_limits<int>::max())
        return std::nullopt;
    return int(value);
}

// 32 hex characters, or a 36-character UUID with dashes, as 32 lower-case hex characters
std::optional<QString> normalizedCastId(const QString &text)
{
    QString hex = text;
    if (hex.size() == 36) {
        for (const qsizetype dash : {8, 13, 18, 23}) {
            if (hex.at(dash) != QLatin1Char('-'))
                return std::nullopt;
        }
        hex.remove(QLatin1Char('-'));
    }
    if (hex.size() != 32)
        return std::nullopt;
    for (const QChar c : std::as_const(hex)) {
        const char16_t u = c.unicode();
        const bool isHex =
            (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
        if (!isHex)
            return std::nullopt;
    }
    return hex.toLower();
}

std::optional<CastDeviceInfo> deviceFrom(const QString &instance, quint16 port,
                                         const QHash<QString, QString> &txt,
                                         const QHostAddress &address)
{
    const auto version = parseDecimal(txt.value(QStringLiteral("ve")));
    const auto capabilities = parseDecimal(txt.value(QStringLiteral("ca")));
    const auto id = normalizedCastId(txt.value(QStringLiteral("id")));
    if (port == 0 || !version || *version < 2 || !capabilities || !(*capabilities & 1) || !id)
        return std::nullopt;
    CastDeviceInfo device;
    device.name = sanitizeDisplayText(txt.value(QStringLiteral("fn")));
    if (device.name.isEmpty())
        return std::nullopt;
    device.model = sanitizeDisplayText(txt.value(QStringLiteral("md")));
    device.id = *id;
    device.address = address;
    device.port = port;
    device.capabilities = *capabilities;
    device.version = *version;
    device.instance = instance;
    return device;
}
} // namespace

QByteArray mdnsQuery()
{
    // Id 0, flags 0, one question and no other record
    QByteArray query = QByteArray::fromHex("000000000001000000000000");
    const QByteArray service(kGoogleCastService.data(), kGoogleCastService.size());
    for (const QByteArray &label : service.split('.')) {
        query.append(char(label.size()));
        query.append(label);
    }
    // The root label, then type PTR and class IN
    query.append(QByteArray::fromHex("00000c0001"));
    return query;
}

MdnsAccumulator::MdnsAccumulator(AddressPolicy policy) : m_policy(policy) {}

int MdnsAccumulator::deviceCount() const
{
    return int(m_ids.size());
}

auto MdnsAccumulator::instanceFor(const QString &name, const QHostAddress &sender) -> Instance *
{
    const QString key = name.toLower();
    auto it = m_instances.find(key);
    if (it == m_instances.end()) {
        if (m_instances.size() >= kMaxTrackedEntries)
            return nullptr;
        Instance fresh;
        fresh.name = name;
        fresh.sender = sender;
        fresh.order = m_nextOrder++;
        it = m_instances.insert(key, fresh);
    }
    return it.value().sender == sender ? &it.value() : nullptr;
}

void MdnsAccumulator::withdraw(const QString &name, const QHostAddress &sender, MdnsUpdate &update)
{
    const auto it = m_instances.constFind(name.toLower());
    if (it == m_instances.constEnd() || it.value().sender != sender)
        return;
    if (it.value().reported) {
        update.gone.append(it.value().id);
        m_ids.remove(it.value().id);
    }
    m_instances.erase(it);
}

void MdnsAccumulator::reportComplete(const QHostAddress &sender, MdnsUpdate &update)
{
    QList<Instance *> ready;
    for (auto it = m_instances.begin(); it != m_instances.end(); ++it) {
        Instance &instance = it.value();
        if (instance.sender == sender && !instance.reported && !instance.rejected && instance.hasSrv
            && instance.hasTxt && m_hosts.contains(hostKey(sender, instance.target)))
            ready.append(&instance);
    }
    std::sort(ready.begin(), ready.end(),
              [](const Instance *a, const Instance *b) { return a->order < b->order; });

    for (Instance *instance : std::as_const(ready)) {
        const auto device = deviceFrom(instance->name, instance->port, instance->txt, sender);
        if (!device) {
            instance->rejected = true;
            continue;
        }
        const auto known = m_ids.constFind(device->id);
        if (known != m_ids.constEnd()) {
            if (known.value() != sender && !m_conflicting.contains(device->id)) {
                m_conflicting.insert(device->id);
                qCWarning(mdnsLog) << "ignoring a second address for" << device->id;
            }
            instance->rejected = true;
            continue;
        }
        if (m_ids.size() >= kMaxMdnsDevices) {
            if (!m_limitLogged)
                qCWarning(mdnsLog)
                    << "more than" << kMaxMdnsDevices << "devices answered, ignoring the rest";
            m_limitLogged = true;
            continue;
        }
        m_ids.insert(device->id, sender);
        instance->reported = true;
        instance->id = device->id;
        update.found.append(*device);
    }
}

MdnsUpdate MdnsAccumulator::add(const QByteArray &datagram, const QHostAddress &sender)
{
    MdnsUpdate update;
    if (!m_policy || !m_policy(sender))
        return update;
    const auto records = parseMessage(datagram);
    if (!records || records->isEmpty())
        return update;
    const QHostAddress from = normalizedAddress(sender);

    for (const DnsRecord &record : *records) {
        switch (record.type) {
        case kTypePtr:
            if (record.owner.compare(kGoogleCastService, Qt::CaseInsensitive) != 0
                || !isInstanceName(record.target))
                break;
            if (record.ttl == 0)
                withdraw(record.target, from, update);
            else
                instanceFor(record.target, from);
            break;
        case kTypeSrv:
        case kTypeTxt:
            if (!isInstanceName(record.owner))
                break;
            if (record.ttl == 0) {
                withdraw(record.owner, from, update);
            } else if (Instance *instance = instanceFor(record.owner, from)) {
                instance->rejected = false;
                if (record.type == kTypeSrv) {
                    instance->target = record.target;
                    instance->port = record.port;
                    instance->hasSrv = true;
                } else {
                    instance->txt = record.txt;
                    instance->hasTxt = true;
                }
            }
            break;
        case kTypeA:
            if (record.ttl != 0 && QHostAddress(record.ipv4) == from
                && m_hosts.size() < kMaxTrackedEntries)
                m_hosts.insert(hostKey(from, record.owner));
            break;
        default:
            break;
        }
    }
    reportComplete(from, update);
    return update;
}

MdnsSearch::MdnsSearch(QObject *parent) : MdnsSearch(Options{}, parent) {}

MdnsSearch::MdnsSearch(const Options &options, QObject *parent)
    : QObject(parent), m_options(options), m_accumulator(options.senderPolicy)
{
    m_repeatTimer.setSingleShot(true);
    m_windowTimer.setSingleShot(true);
    connect(&m_repeatTimer, &QTimer::timeout, this, &MdnsSearch::sendQuery);
    connect(&m_windowTimer, &QTimer::timeout, this, &MdnsSearch::endWindow);
    connect(&m_socket, &QUdpSocket::readyRead, this, &MdnsSearch::readPending);
}

MdnsSearch::~MdnsSearch() = default;

bool MdnsSearch::start()
{
    stop();
    m_accumulator = MdnsAccumulator(m_options.senderPolicy);
    m_datagrams = 0;
    m_limitLogged = false;
    m_fallback = false;
    const QAbstractSocket::BindMode mode =
        QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint;
    bool bound = m_socket.bind(QHostAddress::AnyIPv4, m_options.bindPort, mode);
    if (!bound && m_options.bindPort != 0) {
        bound = m_socket.bind(QHostAddress::AnyIPv4, 0, mode);
        m_fallback = bound;
    }
    if (!bound) {
        qCWarning(mdnsLog) << "cannot bind the search socket:" << m_socket.errorString();
        return false;
    }
    if (m_options.target.isMulticast()) {
        m_socket.setSocketOption(QAbstractSocket::MulticastTtlOption, QVariant(kMdnsMulticastTtl));
        for (const CastInterface &entry : castInterfaces()) {
            if (!m_socket.joinMulticastGroup(m_options.target, entry.interface))
                qCDebug(mdnsLog) << "cannot join" << m_options.target.toString() << "on"
                                 << entry.interface.name() << m_socket.errorString();
        }
    }
    m_running = true;
    m_windowTimer.start(m_options.windowMs);
    m_repeatTimer.start(m_options.repeatMs);
    sendQuery();
    return true;
}

void MdnsSearch::stop()
{
    m_repeatTimer.stop();
    m_windowTimer.stop();
    m_socket.close();
    m_running = false;
}

void MdnsSearch::sendQuery()
{
    if (!m_running)
        return;
    const QByteArray query = mdnsQuery();
    if (!m_options.target.isMulticast()) {
        m_socket.writeDatagram(query, m_options.target, m_options.port);
        return;
    }
    for (const CastInterface &entry : castInterfaces()) {
        m_socket.setMulticastInterface(entry.interface);
        m_socket.writeDatagram(query, m_options.target, m_options.port);
    }
}

void MdnsSearch::readPending()
{
    while (m_running && m_socket.hasPendingDatagrams()) {
        // One byte more than the limit tells an oversized datagram from one of exactly the limit
        const QNetworkDatagram datagram = m_socket.receiveDatagram(kMaxMdnsDatagramBytes + 1);
        if (!datagram.isValid())
            continue;
        if (datagram.data().size() > kMaxMdnsDatagramBytes)
            continue;
        if (m_options.senderPort != 0 && datagram.senderPort() != m_options.senderPort)
            continue;
        if (!m_options.senderPolicy || !m_options.senderPolicy(datagram.senderAddress()))
            continue;
        // Only datagrams that pass the filters count, so noise on the LAN (our own looped-back queries, other hosts' queries) cannot use the cap up before a device answers
        if (++m_datagrams > kMaxMdnsDatagramsPerSearch) {
            if (!m_limitLogged)
                qCWarning(mdnsLog) << "more than" << kMaxMdnsDatagramsPerSearch
                                   << "datagrams in one search, ignoring the rest";
            m_limitLogged = true;
            continue;
        }
        const MdnsUpdate update = m_accumulator.add(datagram.data(), datagram.senderAddress());
        for (const QString &id : update.gone)
            Q_EMIT deviceGone(id);
        for (const CastDeviceInfo &device : update.found)
            Q_EMIT deviceFound(device);
    }
}

void MdnsSearch::endWindow()
{
    stop();
    Q_EMIT finished();
}

} // namespace Kinefin
