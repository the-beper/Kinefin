// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastText.h"
#include "cast/Mdns.h"

#include <QFile>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QUdpSocket>
#include <QtTest>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace Kinefin;

namespace {
constexpr int kDnsHeaderBytes = 12;
constexpr int kFloodDatagrams = 300;
static_assert(kFloodDatagrams > kMaxMdnsDatagramsPerSearch);
enum Section { Answers = 0, Authority = 1, Additional = 2 };

QByteArray be16(quint16 value)
{
    QByteArray out;
    out.append(char(value >> 8));
    out.append(char(value & 0xFF));
    return out;
}

QByteArray be32(quint32 value)
{
    QByteArray out = be16(quint16(value >> 16));
    out.append(be16(quint16(value & 0xFFFF)));
    return out;
}

QByteArray wireName(const QString &dotted)
{
    QByteArray out;
    for (const QString &label : dotted.split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
        const QByteArray bytes = label.toUtf8();
        out.append(char(bytes.size()));
        out.append(bytes);
    }
    out.append('\0');
    return out;
}

QByteArray pointerTo(int offset)
{
    return be16(quint16(0xC000 | offset));
}

// A response assembled record by record, sections in order
class Packet {
  public:
    explicit Packet(quint16 flags = 0x8400) : m_flags(flags) {}

    // Appends a record and returns where its rdata starts in the finished packet
    int add(int section, const QByteArray &owner, quint16 type, quint32 ttl, const QByteArray &data,
            quint16 recordClass = 0x8001)
    {
        Q_ASSERT(section >= m_section);
        m_section = section;
        ++m_counts[section];
        m_body.append(owner);
        m_body.append(be16(type));
        m_body.append(be16(recordClass));
        m_body.append(be32(ttl));
        m_body.append(be16(quint16(data.size())));
        const int dataAt = nextOffset();
        m_body.append(data);
        return dataAt;
    }

    int nextOffset() const { return kDnsHeaderBytes + int(m_body.size()); }

    QByteArray bytes() const
    {
        QByteArray out = be16(0);
        out.append(be16(m_flags));
        out.append(be16(0));
        for (const int count : m_counts)
            out.append(be16(quint16(count)));
        out.append(m_body);
        return out;
    }

  private:
    quint16 m_flags;
    int m_section = 0;
    int m_counts[3] = {0, 0, 0};
    QByteArray m_body;
};

QByteArray srvData(quint16 port, const QString &target)
{
    QByteArray out = be16(0);
    out.append(be16(0));
    out.append(be16(port));
    out.append(wireName(target));
    return out;
}

QByteArray txtData(const QStringList &strings)
{
    QByteArray out;
    for (const QString &string : strings) {
        const QByteArray bytes = string.toUtf8();
        out.append(char(bytes.size()));
        out.append(bytes);
    }
    return out;
}

QByteArray aData(const QHostAddress &address)
{
    return be32(address.toIPv4Address());
}

const QString kService = QStringLiteral("_googlecast._tcp.local");
const QString kInstance = QStringLiteral("Chromecast-1._googlecast._tcp.local");
const QString kHost = QStringLiteral("host-1.local");
const QString kId = QStringLiteral("00112233445566778899aabbccddeeff");
const QHostAddress kSender(QStringLiteral("192.168.1.20"));
// Where the rdata of a first record with the root name as owner starts: header 12, owner 1, record header 10
constexpr int kFirstRdata = 23;

QStringList castTxt()
{
    return {QStringLiteral("id=%1").arg(kId),     QStringLiteral("ve=05"),
            QStringLiteral("md=Google Nest Hub"), QStringLiteral("fn=Living room"),
            QStringLiteral("ca=231941"),          QStringLiteral("st=0")};
}

QStringList withoutKey(QStringList txt, const QString &key)
{
    const QString prefix = key + QLatin1Char('=');
    txt.removeIf([&prefix](const QString &string) { return string.startsWith(prefix); });
    return txt;
}

QStringList withKey(const QStringList &txt, const QString &key, const QString &value)
{
    QStringList out = withoutKey(txt, key);
    out.append(QStringLiteral("%1=%2").arg(key, value));
    return out;
}

struct Device {
    QString instance = kInstance;
    QString host = kHost;
    QHostAddress address = kSender;
    quint16 port = 8009;
    QStringList txt = castTxt();
    quint32 ttl = 120;
};

QString idOf(int n)
{
    return QStringLiteral("%1").arg(n, 32, 16, QLatin1Char('0'));
}

Device numbered(int n)
{
    Device device;
    device.instance = QStringLiteral("Chromecast-%1._googlecast._tcp.local").arg(n);
    device.host = QStringLiteral("host-%1.local").arg(n);
    device.address = QHostAddress(QStringLiteral("192.168.1.%1").arg(10 + n));
    device.txt = withKey(castTxt(), QStringLiteral("id"), idOf(n + 1));
    return device;
}

void addPtr(Packet &packet, int section, const Device &device)
{
    packet.add(section, wireName(kService), 12, device.ttl, wireName(device.instance), 0x0001);
}

void addSrv(Packet &packet, int section, const Device &device)
{
    packet.add(section, wireName(device.instance), 33, device.ttl,
               srvData(device.port, device.host));
}

void addTxt(Packet &packet, int section, const Device &device)
{
    packet.add(section, wireName(device.instance), 16, device.ttl, txtData(device.txt));
}

void addA(Packet &packet, int section, const Device &device)
{
    packet.add(section, wireName(device.host), 1, device.ttl, aData(device.address));
}

// The PTR in the answer section, SRV, TXT and A as additional records
QByteArray answer(const Device &device, quint16 flags = 0x8400)
{
    Packet packet(flags);
    addPtr(packet, Answers, device);
    addSrv(packet, Additional, device);
    addTxt(packet, Additional, device);
    addA(packet, Additional, device);
    return packet.bytes();
}

// An answer padded with an ignored record to exactly `size` bytes
QByteArray answerOfSize(const Device &device, int size)
{
    Packet packet;
    addPtr(packet, Answers, device);
    addSrv(packet, Additional, device);
    addTxt(packet, Additional, device);
    addA(packet, Additional, device);
    // The padding record has a one-byte owner and a ten-byte header
    const int padding = size - packet.nextOffset() - 11;
    packet.add(Additional, QByteArray(1, '\0'), 99, 120, QByteArray(padding, 'x'));
    return packet.bytes();
}

// A device whose SRV and TXT owner is `owner`, after an ignored record whose rdata is `filler` and starts at kFirstRdata
QByteArray deviceNamedBy(const QByteArray &filler, const QByteArray &owner)
{
    const Device device{};
    Packet packet;
    packet.add(Additional, QByteArray(1, '\0'), 99, 120, filler);
    packet.add(Additional, owner, 33, 120, srvData(device.port, device.host));
    packet.add(Additional, owner, 16, 120, txtData(device.txt));
    addA(packet, Additional, device);
    return packet.bytes();
}

QList<CastDeviceInfo> foundFrom(const QByteArray &datagram, const QHostAddress &sender = kSender)
{
    MdnsAccumulator accumulator(isAllowedRendererAddress);
    return accumulator.add(datagram, sender).found;
}

QList<CastDeviceInfo> foundWithTxt(const QStringList &txt)
{
    Device device;
    device.txt = txt;
    return foundFrom(answer(device));
}

QStringList junkStrings(int count)
{
    QStringList out;
    for (int i = 0; i < count; ++i)
        out.append(QStringLiteral("x%1=value").arg(i));
    return out;
}

QByteArray fixture(const QString &name)
{
    QFile file(QStringLiteral(KINEFIN_SOURCE_DIR "/tests/fixtures/") + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool allowLoopback(const QHostAddress &address)
{
    return address.isLoopback();
}

Device loopbackDevice()
{
    Device device;
    device.address = QHostAddress::LocalHost;
    return device;
}

// A unicast socket that answers every datagram it receives with the given replies, standing in for the devices
class FakeResponder {
  public:
    FakeResponder()
    {
        QObject::connect(&m_socket, &QUdpSocket::readyRead, &m_socket, [this] {
            while (m_socket.hasPendingDatagrams()) {
                const QNetworkDatagram datagram = m_socket.receiveDatagram();
                received.append(datagram.data());
                lastSender = datagram.senderAddress();
                lastSenderPort = datagram.senderPort();
                for (const QByteArray &reply : std::as_const(replies))
                    m_socket.writeDatagram(reply, datagram.senderAddress(), datagram.senderPort());
            }
        });
    }
    bool listen() { return m_socket.bind(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_socket.localPort(); }
    QUdpSocket &socket() { return m_socket; }

    QList<QByteArray> replies;
    QList<QByteArray> received;
    // Where the last datagram came from: the search socket, which a test can write to directly
    QHostAddress lastSender;
    quint16 lastSenderPort = 0;

  private:
    QUdpSocket m_socket;
};

bool allowLocalHostOnly(const QHostAddress &address)
{
    return address == QHostAddress(QHostAddress::LocalHost);
}

// A second device on the loopback address, with its own instance, host and id
Device secondLoopbackDevice()
{
    Device device = loopbackDevice();
    device.instance = QStringLiteral("Chromecast-2._googlecast._tcp.local");
    device.host = QStringLiteral("host-2.local");
    device.txt = withKey(castTxt(), QStringLiteral("id"), idOf(2));
    return device;
}

// Writes `count` copies of `datagram` a few at a time, with the event loop running in between so the receiver drains its socket buffer and no copy is lost to an overflow
void sendBurst(QUdpSocket &from, const QHostAddress &to, quint16 port, const QByteArray &datagram,
               int count)
{
    for (int i = 0; i < count; ++i) {
        from.writeDatagram(datagram, to, port);
        if (i % 20 == 19)
            QTest::qWait(2);
    }
}

// Counts the warnings about the datagram cap and passes everything else on
class CapWarningCounter {
  public:
    CapWarningCounter()
    {
        s_count = 0;
        s_previous = qInstallMessageHandler(handle);
    }
    ~CapWarningCounter() { qInstallMessageHandler(s_previous); }
    CapWarningCounter(const CapWarningCounter &) = delete;
    CapWarningCounter &operator=(const CapWarningCounter &) = delete;

    int count() const { return s_count; }

  private:
    static void handle(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        if (type == QtWarningMsg && message.contains(QLatin1String("datagrams in one search")))
            ++s_count;
        else if (s_previous)
            s_previous(type, context, message);
    }

    static inline int s_count = 0;
    static inline QtMessageHandler s_previous = nullptr;
};

MdnsSearch::Options loopbackOptions(quint16 port)
{
    MdnsSearch::Options options;
    options.target = QHostAddress::LocalHost;
    options.port = port;
    options.bindPort = 0;
    options.senderPort = 0;
    options.windowMs = 500;
    options.repeatMs = 100;
    options.senderPolicy = allowLoopback;
    return options;
}

// A UDP port bound without SO_REUSEADDR, so that no shared bind of the same port succeeds
class ExclusiveUdpPort {
  public:
    ExclusiveUdpPort()
    {
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0)
            return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        socklen_t length = sizeof(address);
        if (::bind(m_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0
            && ::getsockname(m_fd, reinterpret_cast<sockaddr *>(&address), &length) == 0)
            m_port = ntohs(address.sin_port);
    }
    ~ExclusiveUdpPort()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    ExclusiveUdpPort(const ExclusiveUdpPort &) = delete;
    ExclusiveUdpPort &operator=(const ExclusiveUdpPort &) = delete;

    quint16 port() const { return m_port; }

  private:
    int m_fd = -1;
    quint16 m_port = 0;
};
} // namespace

class TestCastMdns : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void mdnsQueryBytes()
    {
        const QByteArray golden = QByteArray::fromHex("000000000001000000000000"
                                                      "0b5f676f6f676c6563617374"
                                                      "045f746370"
                                                      "056c6f63616c"
                                                      "00"
                                                      "000c0001");
        QCOMPARE(golden.size(), 40);
        QCOMPARE(mdnsQuery(), golden);
    }

    void parsesPtrWithAdditionalSrvTxtA()
    {
        MdnsAccumulator accumulator(isAllowedRendererAddress);
        const MdnsUpdate update = accumulator.add(answer(Device{}), kSender);
        QVERIFY(update.gone.isEmpty());
        QCOMPARE(update.found.size(), 1);
        const CastDeviceInfo &device = update.found.at(0);
        QCOMPARE(device.id, kId);
        QCOMPARE(device.name, QStringLiteral("Living room"));
        QCOMPARE(device.model, QStringLiteral("Google Nest Hub"));
        QCOMPARE(device.address, kSender);
        QCOMPARE(device.port, quint16(8009));
        QCOMPARE(device.capabilities, 231941);
        QCOMPARE(device.version, 5);
        QCOMPARE(device.instance, kInstance);
        QCOMPARE(accumulator.deviceCount(), 1);
    }

    void readsAllThreeSectionsWithoutAPtr()
    {
        const Device device{};
        Packet packet;
        addTxt(packet, Answers, device);
        addSrv(packet, Authority, device);
        addA(packet, Additional, device);
        QCOMPARE(foundFrom(packet.bytes()).size(), 1);
    }

    void ignoresMessagesThatAreNotResponses()
    {
        QCOMPARE(foundFrom(answer(Device{}, 0x8400)).size(), 1);
        QCOMPARE(foundFrom(answer(Device{}, 0x8000)).size(), 1);
        // A query, an opcode other than 0 and a non-zero rcode
        for (const int flags : {0x0000, 0x0400, 0x8800, 0xA400, 0x8403}) {
            QVERIFY2(foundFrom(answer(Device{}, quint16(flags))).isEmpty(),
                     qPrintable(QString::number(flags, 16)));
        }
    }

    void mergesAnInstanceAcrossDatagrams()
    {
        const Device device{};
        Packet ptr;
        addPtr(ptr, Answers, device);
        Packet srv;
        addSrv(srv, Additional, device);
        Packet txt;
        addTxt(txt, Additional, device);
        Packet address;
        addA(address, Additional, device);

        MdnsAccumulator accumulator(isAllowedRendererAddress);
        QVERIFY(accumulator.add(ptr.bytes(), kSender).found.isEmpty());
        QVERIFY(accumulator.add(srv.bytes(), kSender).found.isEmpty());
        QVERIFY(accumulator.add(txt.bytes(), kSender).found.isEmpty());
        const MdnsUpdate last = accumulator.add(address.bytes(), kSender);
        QCOMPARE(last.found.size(), 1);
        QCOMPARE(last.found.at(0).id, kId);
        QCOMPARE(last.found.at(0).address, kSender);
        // Each id is reported once, so the same answer again adds nothing
        QVERIFY(accumulator.add(answer(device), kSender).found.isEmpty());
        QCOMPARE(accumulator.deviceCount(), 1);

        // The order of arrival does not matter, and the PTR is not needed
        MdnsAccumulator reversed(isAllowedRendererAddress);
        QVERIFY(reversed.add(address.bytes(), kSender).found.isEmpty());
        QVERIFY(reversed.add(txt.bytes(), kSender).found.isEmpty());
        QCOMPARE(reversed.add(srv.bytes(), kSender).found.size(), 1);
        QVERIFY(reversed.add(ptr.bytes(), kSender).found.isEmpty());
    }

    void refusesACompressionPointerLoop()
    {
        // The control: the owner points at a plain name
        QCOMPARE(foundFrom(deviceNamedBy(wireName(kInstance), pointerTo(kFirstRdata))).size(), 1);
        // A label and a pointer back to it never end
        const QByteArray loop = QByteArray::fromHex("0161") + pointerTo(kFirstRdata);
        QVERIFY(foundFrom(deviceNamedBy(loop, pointerTo(kFirstRdata))).isEmpty());
        // A pointer to itself
        QVERIFY(foundFrom(deviceNamedBy(pointerTo(kFirstRdata), pointerTo(kFirstRdata))).isEmpty());
    }

    void limitsPointerJumps()
    {
        // A name reached through `pointers` pointers that each point at the previous one, after the owner's own jump
        const auto chained = [](int pointers) {
            QByteArray filler = wireName(kInstance);
            int previous = kFirstRdata;
            for (int i = 0; i < pointers; ++i) {
                const int at = kFirstRdata + int(filler.size());
                filler.append(pointerTo(previous));
                previous = at;
            }
            return deviceNamedBy(filler, pointerTo(previous));
        };
        QCOMPARE(foundFrom(chained(kMaxMdnsPointerJumps - 1)).size(), 1);
        QVERIFY(foundFrom(chained(kMaxMdnsPointerJumps)).isEmpty());
    }

    void refusesAForwardPointer()
    {
        const Device device{};
        const auto build = [&device](const QByteArray &srvOwner, const QByteArray &txtOwner) {
            Packet packet;
            packet.add(Additional, srvOwner, 33, 120, srvData(device.port, device.host));
            packet.add(Additional, txtOwner, 16, 120, txtData(device.txt));
            addA(packet, Additional, device);
            return packet.bytes();
        };
        // The owner of the TXT record starts right after the SRV record, whose own owner is two bytes
        Packet probe;
        probe.add(Additional, pointerTo(0), 33, 120, srvData(device.port, device.host));
        const int txtOwnerAt = probe.nextOffset();

        QCOMPARE(foundFrom(build(wireName(kInstance), wireName(kInstance))).size(), 1);
        // A pointer back to an earlier name is fine
        QCOMPARE(foundFrom(build(wireName(kInstance), pointerTo(kDnsHeaderBytes))).size(), 1);
        // The same name reached by a pointer that looks ahead, and one past the end
        QVERIFY(foundFrom(build(pointerTo(txtOwnerAt), wireName(kInstance))).isEmpty());
        QVERIFY(foundFrom(build(pointerTo(0x3FFF), wireName(kInstance))).isEmpty());
    }

    void refusesTruncatedRecords()
    {
        const QByteArray full = answer(Device{});
        QCOMPARE(foundFrom(full).size(), 1);
        for (qsizetype size = 0; size < full.size(); ++size)
            QVERIFY2(foundFrom(full.left(size)).isEmpty(), qPrintable(QString::number(size)));

        // The header promises a record that is not there
        QByteArray counted = full;
        counted[11] = char(counted.at(11) + 1);
        QVERIFY(foundFrom(counted).isEmpty());

        // A broken record after a complete device voids the whole datagram
        const Device device{};
        Packet cut;
        addPtr(cut, Answers, device);
        addSrv(cut, Additional, device);
        addTxt(cut, Additional, device);
        addA(cut, Additional, device);
        cut.add(Additional, wireName(QStringLiteral("other.local")), 28, 120,
                QByteArray(16, '\x01'));
        QCOMPARE(foundFrom(cut.bytes()).size(), 1);
        QVERIFY(foundFrom(cut.bytes().chopped(4)).isEmpty());

        // A TXT string that runs past its record
        Packet overrun;
        addPtr(overrun, Answers, device);
        addSrv(overrun, Additional, device);
        addTxt(overrun, Additional, device);
        addA(overrun, Additional, device);
        overrun.add(Additional, wireName(QStringLiteral("Other-9._googlecast._tcp.local")), 16, 120,
                    QByteArray::fromHex("c878"));
        QVERIFY(foundFrom(overrun.bytes()).isEmpty());
    }

    void refusesOversizedDatagrams()
    {
        const Device device{};
        const QByteArray exact = answerOfSize(device, kMaxMdnsDatagramBytes);
        QCOMPARE(exact.size(), int(kMaxMdnsDatagramBytes));
        QCOMPARE(foundFrom(exact).size(), 1);
        const QByteArray over = answerOfSize(device, kMaxMdnsDatagramBytes + 1);
        QCOMPARE(over.size(), int(kMaxMdnsDatagramBytes) + 1);
        QVERIFY(foundFrom(over).isEmpty());
    }

    void capsRecordsAndTxt()
    {
        const Device device{};
        const auto filler = [](Packet &packet, int section, int count) {
            for (int i = 0; i < count; ++i)
                packet.add(section, wireName(QStringLiteral("filler.local")), 99, 120,
                           QByteArray());
        };

        // Records past the per-section limit are dropped
        const auto inOneSection = [&](int fillers) {
            Packet packet;
            filler(packet, Additional, fillers);
            addSrv(packet, Additional, device);
            addTxt(packet, Additional, device);
            addA(packet, Additional, device);
            return foundFrom(packet.bytes()).size();
        };
        QCOMPARE(inOneSection(kMaxMdnsRecordsPerSection - 3), 1);
        QCOMPARE(inOneSection(kMaxMdnsRecordsPerSection - 2), 0);

        // And past the limit of the whole message
        const auto inTheMessage = [&](int authorityFillers) {
            Packet packet;
            filler(packet, Answers, kMaxMdnsRecordsPerSection);
            filler(packet, Authority, authorityFillers);
            addSrv(packet, Additional, device);
            addTxt(packet, Additional, device);
            addA(packet, Additional, device);
            return foundFrom(packet.bytes()).size();
        };
        QCOMPARE(inTheMessage(kMaxMdnsRecords - kMaxMdnsRecordsPerSection - 3), 1);
        QCOMPARE(inTheMessage(kMaxMdnsRecords - kMaxMdnsRecordsPerSection - 2), 0);

        // TXT strings past the limit are not read: ca is the last string read, or the one after it
        const QStringList required{QStringLiteral("id=%1").arg(kId), QStringLiteral("ve=05"),
                                   QStringLiteral("fn=Living room")};
        QStringList lastRead = junkStrings(kMaxMdnsTxtStrings - 4);
        lastRead += required;
        lastRead.append(QStringLiteral("ca=231941"));
        QCOMPARE(lastRead.size(), int(kMaxMdnsTxtStrings));
        QCOMPARE(foundWithTxt(lastRead).size(), 1);
        QStringList withExtra = lastRead;
        withExtra += junkStrings(8);
        QCOMPARE(foundWithTxt(withExtra).size(), 1);
        QStringList tooLate = junkStrings(kMaxMdnsTxtStrings - 3);
        tooLate += required;
        tooLate.append(QStringLiteral("ca=231941"));
        QCOMPARE(tooLate.size(), int(kMaxMdnsTxtStrings) + 1);
        QVERIFY(foundWithTxt(tooLate).isEmpty());
    }

    void readsTxtStrings()
    {
        // Keys are lower-cased, the first of a duplicate wins, a zero-length string is skipped and a string without '=' is a key with an empty value
        const QStringList txt{QStringLiteral("ID=%1").arg(kId),
                              QStringLiteral("VE=05"),
                              QStringLiteral("Fn=First"),
                              QStringLiteral("FN=Second"),
                              QString(),
                              QStringLiteral("CA=231941"),
                              QStringLiteral("md")};
        const QList<CastDeviceInfo> found = foundWithTxt(txt);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).name, QStringLiteral("First"));
        QVERIFY(found.at(0).model.isEmpty());

        QStringList noValue = withoutKey(castTxt(), QStringLiteral("fn"));
        noValue.append(QStringLiteral("fn"));
        QVERIFY(foundWithTxt(noValue).isEmpty());
    }

    void normalisesTheGroupUuid()
    {
        const auto idFor = [](const QString &id) {
            const QList<CastDeviceInfo> found =
                foundWithTxt(withKey(castTxt(), QStringLiteral("id"), id));
            return found.isEmpty() ? QString() : found.at(0).id;
        };
        QCOMPARE(idFor(QStringLiteral("00000000-0000-0000-0000-000000000009")),
                 QStringLiteral("00000000000000000000000000000009"));
        QCOMPARE(idFor(QStringLiteral("AABBCCDD-EEFF-0011-2233-445566778899")),
                 QStringLiteral("aabbccddeeff00112233445566778899"));
        QCOMPARE(idFor(kId.toUpper()), kId);

        QString misplacedDash = QStringLiteral("00000000-0000-0000-0000-000000000009");
        misplacedDash.remove(8, 1);
        misplacedDash.insert(9, QLatin1Char('-'));
        QString dashInHex = kId;
        dashInHex.replace(5, 1, QLatin1Char('-'));
        QString notHex = kId;
        notHex.replace(0, 1, QLatin1Char('g'));
        const QString tooLong = kId + QStringLiteral("0");
        const QString leadingSpace = QStringLiteral(" ") + kId.left(31);
        const QStringList bad{
            QString(), kId.left(31), tooLong,       leadingSpace,
            notHex,    dashInHex,    misplacedDash, QString(36, QLatin1Char('a'))};
        for (const QString &id : bad)
            QVERIFY2(idFor(id).isEmpty(), qPrintable(id));
    }

    void dropsAudioOnlyDevices()
    {
        const auto withCapabilities = [](const QString &ca) {
            return foundWithTxt(withKey(castTxt(), QStringLiteral("ca"), ca)).size();
        };
        // 199204 is the capability word of the observed group: no bit 0
        QCOMPARE(withCapabilities(QStringLiteral("199204")), 0);
        QCOMPARE(withCapabilities(QStringLiteral("4")), 0);
        QCOMPARE(withCapabilities(QStringLiteral("0")), 0);
        QCOMPARE(withCapabilities(QStringLiteral("abc")), 0);
        QCOMPARE(withCapabilities(QString()), 0);
        QCOMPARE(withCapabilities(QStringLiteral("1")), 1);
        QCOMPARE(withCapabilities(QStringLiteral("199205")), 1);
        QVERIFY(foundWithTxt(withoutKey(castTxt(), QStringLiteral("ca"))).isEmpty());
    }

    void validatesTheTxtFields()
    {
        const auto withVersion = [](const QString &ve) {
            return foundWithTxt(withKey(castTxt(), QStringLiteral("ve"), ve)).size();
        };
        QCOMPARE(withVersion(QStringLiteral("2")), 1);
        QCOMPARE(withVersion(QStringLiteral("05")), 1);
        QCOMPARE(withVersion(QStringLiteral("1")), 0);
        QCOMPARE(withVersion(QStringLiteral("0")), 0);
        QCOMPARE(withVersion(QStringLiteral("abc")), 0);
        QCOMPARE(withVersion(QString()), 0);
        QVERIFY(foundWithTxt(withoutKey(castTxt(), QStringLiteral("ve"))).isEmpty());

        QVERIFY(foundWithTxt(withoutKey(castTxt(), QStringLiteral("fn"))).isEmpty());
        QVERIFY(foundWithTxt(withKey(castTxt(), QStringLiteral("fn"), QString())).isEmpty());
        QVERIFY(foundWithTxt(withoutKey(castTxt(), QStringLiteral("id"))).isEmpty());
        // md is optional
        const QList<CastDeviceInfo> noModel =
            foundWithTxt(withoutKey(castTxt(), QStringLiteral("md")));
        QCOMPARE(noModel.size(), 1);
        QVERIFY(noModel.at(0).model.isEmpty());

        const auto withPort = [](quint16 port) {
            Device device;
            device.port = port;
            return foundFrom(answer(device)).size();
        };
        QCOMPARE(withPort(0), 0);
        QCOMPARE(withPort(1), 1);
        QCOMPARE(withPort(65535), 1);
    }

    void sanitisesFnAndMd()
    {
        const QString bell(QChar(0x07));
        const QString bidiOverride(QChar(0x202E));
        const QString dirtyName = QStringLiteral("  Living") + bell + QStringLiteral(" room ")
                                  + bidiOverride + QStringLiteral(" ");
        const QString dirtyModel = bell + QStringLiteral("Hub") + bell;
        const QString controls = bell + bell;
        QStringList txt = withKey(castTxt(), QStringLiteral("fn"), dirtyName);
        txt = withKey(txt, QStringLiteral("md"), dirtyModel);
        QList<CastDeviceInfo> found = foundWithTxt(txt);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).name, QStringLiteral("Living room"));
        QCOMPARE(found.at(0).model, QStringLiteral("Hub"));

        // Cut to 64 characters
        txt = withKey(castTxt(), QStringLiteral("fn"), QString(100, QLatin1Char('a')));
        txt = withKey(txt, QStringLiteral("md"), QString(100, QLatin1Char('b')));
        found = foundWithTxt(txt);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).name.size(), int(kMaxDisplayChars));
        QCOMPARE(found.at(0).model.size(), int(kMaxDisplayChars));

        // A model of control characters is empty, a name of them leaves nothing to show
        found = foundWithTxt(withKey(castTxt(), QStringLiteral("md"), controls));
        QCOMPARE(found.size(), 1);
        QVERIFY(found.at(0).model.isEmpty());
        QVERIFY(foundWithTxt(withKey(castTxt(), QStringLiteral("fn"), controls)).isEmpty());
    }

    void requiresTheARecordToBeTheSender()
    {
        const Device device{};
        QCOMPARE(foundFrom(answer(device)).size(), 1);

        // The A record carries another address than the datagram's sender
        Device elsewhere;
        elsewhere.address = QHostAddress(QStringLiteral("192.168.1.99"));
        QVERIFY(foundFrom(answer(elsewhere), kSender).isEmpty());

        // The A record is for another host than the SRV target
        Packet otherHost;
        addPtr(otherHost, Answers, device);
        addSrv(otherHost, Additional, device);
        addTxt(otherHost, Additional, device);
        otherHost.add(Additional, wireName(QStringLiteral("other-host.local")), 1, 120,
                      aData(kSender));
        QVERIFY(foundFrom(otherHost.bytes()).isEmpty());

        // The host name is compared without regard to case
        Packet upperCase;
        addPtr(upperCase, Answers, device);
        addSrv(upperCase, Additional, device);
        addTxt(upperCase, Additional, device);
        upperCase.add(Additional, wireName(QStringLiteral("HOST-1.LOCAL")), 1, 120, aData(kSender));
        QCOMPARE(foundFrom(upperCase.bytes()).size(), 1);

        // A sender outside the policy
        for (const char *ip : {"8.8.8.8", "127.0.0.1", "224.0.0.251", "169.254.169.254"}) {
            Device outside;
            outside.address = QHostAddress(QString::fromLatin1(ip));
            QVERIFY2(foundFrom(answer(outside), outside.address).isEmpty(), ip);
        }

        // An IPv4-mapped sender is the IPv4 address
        const QList<CastDeviceInfo> mapped =
            foundFrom(answer(device), QHostAddress(QStringLiteral("::ffff:192.168.1.20")));
        QCOMPARE(mapped.size(), 1);
        QCOMPARE(mapped.at(0).address, kSender);
    }

    void ignoresASecondSenderForAnInstance()
    {
        const Device device{};
        const QHostAddress other(QStringLiteral("192.168.1.99"));
        Device ofOther = device;
        ofOther.address = other;
        Packet service;
        addSrv(service, Additional, device);
        addTxt(service, Additional, device);
        Packet addressOfOther;
        addA(addressOfOther, Additional, ofOther);
        Packet addressOfSender;
        addA(addressOfSender, Additional, device);

        MdnsAccumulator accumulator(isAllowedRendererAddress);
        QVERIFY(accumulator.add(service.bytes(), kSender).found.isEmpty());
        // An A record that equals its own sender does not complete what another sender began
        QVERIFY(accumulator.add(addressOfOther.bytes(), other).found.isEmpty());
        QCOMPARE(accumulator.add(addressOfSender.bytes(), kSender).found.size(), 1);
        // And once reported, the instance is not taken over
        QVERIFY(accumulator.add(answer(ofOther), other).found.isEmpty());
        QCOMPARE(accumulator.deviceCount(), 1);
    }

    void ignoresASecondAddressForAnId()
    {
        const Device first{};
        Device second = numbered(2);
        second.txt = first.txt;
        MdnsAccumulator accumulator(isAllowedRendererAddress);
        QCOMPARE(accumulator.add(answer(first), first.address).found.size(), 1);
        QVERIFY(accumulator.add(answer(second), second.address).found.isEmpty());
        QVERIFY(accumulator.add(answer(second), second.address).found.isEmpty());
        QCOMPARE(accumulator.deviceCount(), 1);

        // Another id from the same address is a device of its own
        second.txt = withKey(castTxt(), QStringLiteral("id"), idOf(7));
        const MdnsUpdate update = accumulator.add(answer(second), second.address);
        QCOMPARE(update.found.size(), 1);
        QCOMPARE(update.found.at(0).id, idOf(7));
        QCOMPARE(accumulator.deviceCount(), 2);
    }

    void stopsAtThirtyTwoDevices()
    {
        MdnsAccumulator accumulator(isAllowedRendererAddress);
        QStringList ids;
        for (int i = 0; i < kMaxMdnsDevices + 8; ++i) {
            const Device device = numbered(i);
            const MdnsUpdate update = accumulator.add(answer(device), device.address);
            for (const CastDeviceInfo &found : update.found)
                ids.append(found.id);
        }
        QCOMPARE(ids.size(), int(kMaxMdnsDevices));
        QCOMPARE(accumulator.deviceCount(), int(kMaxMdnsDevices));
        QCOMPARE(ids.first(), idOf(1));
        QCOMPARE(ids.last(), idOf(kMaxMdnsDevices));

        // A device that goes away frees a slot for one that answers again
        Device leaving = numbered(0);
        leaving.ttl = 0;
        Packet goodbye;
        addPtr(goodbye, Answers, leaving);
        QCOMPARE(accumulator.add(goodbye.bytes(), leaving.address).gone.size(), 1);
        const Device late = numbered(kMaxMdnsDevices);
        const MdnsUpdate update = accumulator.add(answer(late), late.address);
        QCOMPARE(update.found.size(), 1);
        QCOMPARE(update.found.at(0).id, idOf(kMaxMdnsDevices + 1));
        QCOMPARE(accumulator.deviceCount(), int(kMaxMdnsDevices));
    }

    void goodbyeDropsTheInstance()
    {
        const Device device{};
        Device zero = device;
        zero.ttl = 0;
        // A TTL of 0 on the PTR, the SRV or the TXT withdraws the instance
        for (int kind = 0; kind < 3; ++kind) {
            Packet goodbye;
            if (kind == 0)
                addPtr(goodbye, Answers, zero);
            else if (kind == 1)
                addSrv(goodbye, Additional, zero);
            else
                addTxt(goodbye, Additional, zero);

            MdnsAccumulator accumulator(isAllowedRendererAddress);
            QCOMPARE(accumulator.add(answer(device), kSender).found.size(), 1);
            const MdnsUpdate update = accumulator.add(goodbye.bytes(), kSender);
            QCOMPARE(update.gone, QStringList{kId});
            QVERIFY(update.found.isEmpty());
            QCOMPARE(accumulator.deviceCount(), 0);
            // A later answer reports the device again
            const MdnsUpdate again = accumulator.add(answer(device), kSender);
            QCOMPARE(again.found.size(), 1);
            QCOMPARE(again.found.at(0).id, kId);
        }

        // What does not withdraw it: the A record, another sender, an instance never seen
        MdnsAccumulator accumulator(isAllowedRendererAddress);
        QCOMPARE(accumulator.add(answer(device), kSender).found.size(), 1);
        Packet onlyA;
        addA(onlyA, Additional, zero);
        QVERIFY(accumulator.add(onlyA.bytes(), kSender).gone.isEmpty());
        Packet ptr;
        addPtr(ptr, Answers, zero);
        const QHostAddress stranger(QStringLiteral("192.168.1.99"));
        QVERIFY(accumulator.add(ptr.bytes(), stranger).gone.isEmpty());
        Device unknown = zero;
        unknown.instance = QStringLiteral("Other-9._googlecast._tcp.local");
        Packet unknownPtr;
        addPtr(unknownPtr, Answers, unknown);
        QVERIFY(accumulator.add(unknownPtr.bytes(), kSender).gone.isEmpty());
        QCOMPARE(accumulator.deviceCount(), 1);
    }

    void goldenCaptureOfTwoHubsAndAGroup()
    {
        const QByteArray first = fixture(QStringLiteral("mdns_googlecast_1.bin"));
        const QByteArray second = fixture(QStringLiteral("mdns_googlecast_2.bin"));
        QVERIFY(!first.isEmpty());
        QVERIFY(!second.isEmpty());
        const QHostAddress firstSender(QStringLiteral("192.168.7.10"));
        const QHostAddress secondSender(QStringLiteral("192.168.7.39"));

        MdnsAccumulator accumulator(isAllowedRendererAddress);
        const MdnsUpdate one = accumulator.add(first, firstSender);
        QVERIFY(one.gone.isEmpty());
        QCOMPARE(one.found.size(), 1);
        const CastDeviceInfo &hub = one.found.at(0);
        QCOMPARE(hub.name, QStringLiteral("Hub 01"));
        QCOMPARE(hub.model, QStringLiteral("Google Nest Hub"));
        QCOMPARE(hub.id, QStringLiteral("00000000000000000000000000000001"));
        QCOMPARE(hub.address, firstSender);
        QCOMPARE(hub.port, quint16(8009));
        QCOMPARE(hub.capabilities, 231941);
        QCOMPARE(hub.version, 5);
        const QString hubInstance = QStringLiteral(
            "Google-Nest-Hub-00000000000000000000000000000001._googlecast._tcp.local");
        QCOMPARE(hub.instance, hubInstance);

        // The group in the second capture has ca 199204, without the video bit: only its host's hub is reported
        const MdnsUpdate two = accumulator.add(second, secondSender);
        QCOMPARE(two.found.size(), 1);
        const CastDeviceInfo &display = two.found.at(0);
        QCOMPARE(display.name, QStringLiteral("Hub 02 display area"));
        QCOMPARE(display.model, QStringLiteral("Google Nest Hub"));
        QCOMPARE(display.id, QStringLiteral("00000000000000000000000000000006"));
        QCOMPARE(display.address, secondSender);
        QCOMPARE(display.port, quint16(8009));
        QCOMPARE(display.capabilities, 231941);
        QCOMPARE(display.version, 5);
        QCOMPARE(accumulator.deviceCount(), 2);
        // Both captures again report nothing
        QVERIFY(accumulator.add(first, firstSender).found.isEmpty());
        QVERIFY(accumulator.add(second, secondSender).found.isEmpty());

        // The same capture with the video bit set on the group: its UUID id and its own SRV port come out
        QByteArray patched = second;
        QVERIFY(patched.contains("ca=199204"));
        patched.replace("ca=199204", "ca=199205");
        const QList<CastDeviceInfo> withGroup = foundFrom(patched, secondSender);
        QCOMPARE(withGroup.size(), 2);
        QCOMPARE(withGroup.at(0).id, QStringLiteral("00000000000000000000000000000009"));
        QCOMPARE(withGroup.at(0).name, QStringLiteral("Test group"));
        QCOMPARE(withGroup.at(0).model, QStringLiteral("Google Cast Group"));
        QCOMPARE(withGroup.at(0).port, quint16(32134));
        QCOMPARE(withGroup.at(1).id, QStringLiteral("00000000000000000000000000000006"));
    }

    void searchAgainstAUnicastResponder()
    {
        FakeResponder responder;
        QVERIFY(responder.listen());
        // The first reply is over the datagram limit: it must be dropped, and the device of the second reported
        Device oversized = loopbackDevice();
        oversized.instance = QStringLiteral("Chromecast-2._googlecast._tcp.local");
        oversized.host = QStringLiteral("host-2.local");
        oversized.txt = withKey(castTxt(), QStringLiteral("id"), idOf(2));
        responder.replies = {answerOfSize(oversized, kMaxMdnsDatagramBytes + 1),
                             answer(loopbackDevice())};

        MdnsSearch search(loopbackOptions(responder.port()));
        QSignalSpy found(&search, &MdnsSearch::deviceFound);
        QSignalSpy finished(&search, &MdnsSearch::finished);
        QVERIFY(search.start());
        QVERIFY(search.running());
        QVERIFY(!search.usingFallbackPort());
        QVERIFY(finished.wait(3000));
        QVERIFY(!search.running());
        // Two queries are sent, each answered twice, and the device is still reported once
        QCOMPARE(responder.received.size(), 2);
        QCOMPARE(responder.received.at(0), mdnsQuery());
        QCOMPARE(found.size(), 1);
        const CastDeviceInfo device = found.at(0).at(0).value<CastDeviceInfo>();
        QCOMPARE(device.id, kId);
        QCOMPARE(device.name, QStringLiteral("Living room"));
        QCOMPARE(device.address, QHostAddress(QHostAddress::LocalHost));
    }

    void searchChecksTheSenderPort()
    {
        const auto reported = [](bool matching) -> qsizetype {
            FakeResponder responder;
            if (!responder.listen())
                return -1;
            responder.replies = {answer(loopbackDevice())};
            MdnsSearch::Options options = loopbackOptions(responder.port());
            options.senderPort = matching ? responder.port() : quint16(responder.port() ^ 1);
            options.windowMs = 300;
            MdnsSearch search(options);
            QSignalSpy found(&search, &MdnsSearch::deviceFound);
            QSignalSpy finished(&search, &MdnsSearch::finished);
            if (!search.start() || !finished.wait(3000))
                return -1;
            return found.size();
        };
        QCOMPARE(reported(true), 1);
        QCOMPARE(reported(false), 0);
    }

    void filteredDatagramsDoNotCountTowardsTheCap_data()
    {
        QTest::addColumn<bool>("wrongPort");
        QTest::newRow("wrong sender port") << true;
        QTest::newRow("disallowed sender") << false;
    }

    void filteredDatagramsDoNotCountTowardsTheCap()
    {
        QFETCH(bool, wrongPort);
        const CapWarningCounter warnings;
        FakeResponder responder;
        QVERIFY(responder.listen());
        // The disallowed run needs a sender the policy refuses: 127.0.0.2 is on the loopback interface too
        const QHostAddress intruderAddress(wrongPort ? QStringLiteral("127.0.0.1")
                                                     : QStringLiteral("127.0.0.2"));
        QUdpSocket intruder;
        if (!intruder.bind(intruderAddress, 0))
            QSKIP("cannot bind a second socket on the loopback interface");

        MdnsSearch::Options options = loopbackOptions(responder.port());
        options.repeatMs = 10000;
        options.windowMs = 3000;
        if (wrongPort)
            options.senderPort = responder.port();
        else
            options.senderPolicy = allowLocalHostOnly;
        MdnsSearch search(options);
        QSignalSpy found(&search, &MdnsSearch::deviceFound);
        QVERIFY(search.start());
        QVERIFY(QTest::qWaitFor([&responder] { return responder.lastSenderPort != 0; }, 3000));

        // The noise is a valid answer of another device: it would be reported if it got through
        Device other = secondLoopbackDevice();
        other.address = intruderAddress;
        sendBurst(intruder, responder.lastSender, responder.lastSenderPort, answer(other),
                  kFloodDatagrams);
        QTest::qWait(20);
        responder.socket().writeDatagram(answer(loopbackDevice()), responder.lastSender,
                                         responder.lastSenderPort);
        QVERIFY(QTest::qWaitFor([&found] { return found.size() > 0; }, 3000));
        QTest::qWait(50);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).at(0).value<CastDeviceInfo>().id, kId);
        QCOMPARE(warnings.count(), 0);
    }

    void theCapStillStopsAFloodFromAnAllowedSender()
    {
        const CapWarningCounter warnings;
        FakeResponder responder;
        QVERIFY(responder.listen());
        MdnsSearch::Options options = loopbackOptions(responder.port());
        options.repeatMs = 10000;
        options.windowMs = 3000;
        MdnsSearch search(options);
        QSignalSpy found(&search, &MdnsSearch::deviceFound);
        QVERIFY(search.start());
        QVERIFY(QTest::qWaitFor([&responder] { return responder.lastSenderPort != 0; }, 3000));

        // The cap's worth of valid answers of one device, then more that would report another
        QUdpSocket &device = responder.socket();
        sendBurst(device, responder.lastSender, responder.lastSenderPort, answer(loopbackDevice()),
                  kMaxMdnsDatagramsPerSearch);
        sendBurst(device, responder.lastSender, responder.lastSenderPort,
                  answer(secondLoopbackDevice()), kFloodDatagrams - kMaxMdnsDatagramsPerSearch);
        QVERIFY(QTest::qWaitFor([&warnings] { return warnings.count() > 0; }, 3000));
        QTest::qWait(50);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).at(0).value<CastDeviceInfo>().id, kId);
        QCOMPARE(warnings.count(), 1);
    }

    void searchReportsGoodbyes()
    {
        FakeResponder responder;
        QVERIFY(responder.listen());
        Device leaving = loopbackDevice();
        leaving.ttl = 0;
        Packet goodbye;
        addPtr(goodbye, Answers, leaving);
        responder.replies = {answer(loopbackDevice()), goodbye.bytes()};

        // One query only: the repeat falls after the window
        MdnsSearch::Options options = loopbackOptions(responder.port());
        options.windowMs = 400;
        options.repeatMs = 10000;
        MdnsSearch search(options);
        QSignalSpy found(&search, &MdnsSearch::deviceFound);
        QSignalSpy gone(&search, &MdnsSearch::deviceGone);
        QSignalSpy finished(&search, &MdnsSearch::finished);
        QVERIFY(search.start());
        QVERIFY(finished.wait(3000));
        QCOMPARE(found.size(), 1);
        QCOMPARE(gone.size(), 1);
        QCOMPARE(gone.at(0).at(0).toString(), kId);
    }

    void aNewSearchForgetsTheDevices()
    {
        FakeResponder responder;
        QVERIFY(responder.listen());
        responder.replies = {answer(loopbackDevice())};
        MdnsSearch search(loopbackOptions(responder.port()));
        QSignalSpy found(&search, &MdnsSearch::deviceFound);
        QSignalSpy finished(&search, &MdnsSearch::finished);
        QVERIFY(search.start());
        QVERIFY(finished.wait(3000));
        QCOMPARE(found.size(), 1);
        QVERIFY(search.start());
        QVERIFY(finished.wait(3000));
        QCOMPARE(found.size(), 2);
    }

    void stopEndsTheSearchWithoutFinishing()
    {
        FakeResponder responder;
        QVERIFY(responder.listen());
        MdnsSearch::Options options = loopbackOptions(responder.port());
        options.windowMs = 100;
        MdnsSearch search(options);
        QSignalSpy finished(&search, &MdnsSearch::finished);
        QVERIFY(search.start());
        search.stop();
        QVERIFY(!search.running());
        QVERIFY(!finished.wait(300));
    }

    void fallsBackToAnEphemeralPort()
    {
        FakeResponder responder;
        QVERIFY(responder.listen());
        responder.replies = {answer(loopbackDevice())};

        // A port that another socket holds exclusively cannot be shared: the search binds an ephemeral one and still hears the answers
        const ExclusiveUdpPort holder;
        QVERIFY(holder.port() != 0);
        MdnsSearch::Options options = loopbackOptions(responder.port());
        options.bindPort = holder.port();
        MdnsSearch search(options);
        QSignalSpy found(&search, &MdnsSearch::deviceFound);
        QSignalSpy finished(&search, &MdnsSearch::finished);
        QVERIFY(search.start());
        QVERIFY(search.usingFallbackPort());
        QVERIFY(finished.wait(3000));
        QCOMPARE(found.size(), 1);

        // Bind port 0 asks for an ephemeral port: that is not a fallback
        MdnsSearch ephemeral(loopbackOptions(responder.port()));
        QVERIFY(ephemeral.start());
        QVERIFY(!ephemeral.usingFallbackPort());
        ephemeral.stop();

        // A port that another socket shares is shared too
        QUdpSocket sharer;
        QVERIFY(sharer.bind(QHostAddress::AnyIPv4, 0,
                            QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint));
        options.bindPort = sharer.localPort();
        MdnsSearch shared(options);
        QVERIFY(shared.start());
        QVERIFY(!shared.usingFallbackPort());
        shared.stop();
    }

    // Opt-in on a network with Cast devices: KINEFIN_CAST_LIVE=1 runs the production search (port 5353, multicast on every interface) and expects at least one device
    void liveSearch()
    {
        if (qEnvironmentVariableIsEmpty("KINEFIN_CAST_LIVE"))
            QSKIP("no KINEFIN_CAST_LIVE, skipping the live check");
        Kinefin::MdnsSearch search;
        QSignalSpy found(&search, &Kinefin::MdnsSearch::deviceFound);
        QSignalSpy finished(&search, &Kinefin::MdnsSearch::finished);
        QVERIFY(search.start());
        QVERIFY(finished.wait(Kinefin::kMdnsWindowMs + 3000));
        QVERIFY2(found.count() >= 1, "no Cast device answered");
        for (const QList<QVariant> &arguments : std::as_const(found)) {
            const auto device = arguments.at(0).value<Kinefin::CastDeviceInfo>();
            qInfo() << "device" << device.model << "port" << device.port << "ca"
                    << device.capabilities << "fallback" << search.usingFallbackPort();
        }
    }
};

QTEST_GUILESS_MAIN(TestCastMdns)
#include "test_cast_mdns.moc"
