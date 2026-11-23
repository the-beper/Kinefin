// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastProto.h"

#include <QtTest>

#include <algorithm>
#include <initializer_list>

using namespace Kinefin;

namespace {
QByteArray cat(std::initializer_list<QByteArray> parts)
{
    QByteArray out;
    for (const QByteArray &part : parts)
        out.append(part);
    return out;
}

QByteArray varint(quint64 value)
{
    QByteArray out;
    do {
        char byte = char(value & 0x7f);
        value >>= 7;
        if (value)
            byte = char(byte | 0x80);
        out.append(byte);
    } while (value);
    return out;
}

QByteArray varintField(quint32 number, quint64 value)
{
    return cat({varint(quint64(number) << 3), varint(value)});
}

QByteArray bytesField(quint32 number, const QByteArray &bytes)
{
    return cat({varint((quint64(number) << 3) | 2), varint(quint64(bytes.size())), bytes});
}

QByteArray versionField()
{
    return varintField(1, 0);
}
QByteArray sourceField(const QByteArray &id = QByteArray("sender-0"))
{
    return bytesField(2, id);
}
QByteArray destinationField(const QByteArray &id = QByteArray("receiver-0"))
{
    return bytesField(3, id);
}
QByteArray
namespaceField(const QByteArray &ns = QByteArray("urn:x-cast:com.google.cast.tp.heartbeat"))
{
    return bytesField(4, ns);
}
QByteArray typeField(quint64 type)
{
    return varintField(5, type);
}
QByteArray utf8Field(const QByteArray &payload)
{
    return bytesField(6, payload);
}
QByteArray binaryField(const QByteArray &payload)
{
    return bytesField(7, payload);
}

// The bytes look like varint continuations on purpose: a skip that read them as a varint would lose the fields after
QByteArray fixed32Field(quint32 number)
{
    return cat({varint((quint64(number) << 3) | 5), QByteArray::fromHex("80ff80ff")});
}

QByteArray fixed64Field(quint32 number)
{
    return cat({varint((quint64(number) << 3) | 1), QByteArray::fromHex("80ff80ff80ff80ff")});
}

const QByteArray kPing = QByteArray(R"({"type":"PING"})");

// The body of sample(), a STRING message
QByteArray stringBody()
{
    return cat({versionField(), sourceField(), destinationField(), namespaceField(), typeField(0),
                utf8Field(kPing)});
}

// A STRING message with these ids and this payload, everything else as in sample()
QByteArray bodyWith(const QByteArray &source, const QByteArray &destination, const QByteArray &ns,
                    const QByteArray &payload = kPing)
{
    return cat({versionField(), sourceField(source), destinationField(destination),
                namespaceField(ns), typeField(0), utf8Field(payload)});
}

CastMessage sample()
{
    CastMessage message;
    message.sourceId = QStringLiteral("sender-0");
    message.destinationId = QStringLiteral("receiver-0");
    message.ns = QString(kCastNsHeartbeat);
    message.payload = kPing;
    return message;
}

QByteArray header(quint32 length)
{
    QByteArray out(4, '\0');
    for (int i = 0; i < 4; ++i)
        out[i] = char((length >> (24 - 8 * i)) & 0xff);
    return out;
}

QByteArray frameOf(const QByteArray &body)
{
    return cat({header(quint32(body.size())), body});
}

struct Fed {
    QList<QByteArray> bodies;
    qsizetype peakBuffered = 0;
    bool failed = false;
};

Fed feed(const QByteArray &stream, const QList<qsizetype> &chunkSizes)
{
    CastFramer framer;
    Fed fed;
    qsizetype position = 0;
    qsizetype turn = 0;
    while (position < stream.size()) {
        const qsizetype size =
            std::min(chunkSizes.at(turn++ % chunkSizes.size()), stream.size() - position);
        framer.append(stream.mid(position, size));
        position += size;
        fed.peakBuffered = std::max(fed.peakBuffered, framer.buffered());
        while (const auto body = framer.next())
            fed.bodies.append(*body);
    }
    fed.failed = framer.failed();
    return fed;
}

QByteArray authResponse(const QByteArray &signature, const QByteArray &leaf,
                        const QList<QByteArray> &intermediates)
{
    QByteArray fields = cat({bytesField(1, signature), bytesField(2, leaf)});
    for (const QByteArray &intermediate : intermediates)
        fields.append(bytesField(3, intermediate));
    return bytesField(2, fields);
}

// A DeviceAuthMessage with a response that has a signature and a leaf, then the bytes of `extra`
QByteArray responseWith(const QByteArray &extra = QByteArray())
{
    return bytesField(2,
                      cat({bytesField(1, QByteArray("s")), bytesField(2, QByteArray("l")), extra}));
}

// A DeviceAuthMessage with an error of error_type 1, then the bytes of `extra`
QByteArray errorWith(const QByteArray &extra)
{
    return bytesField(3, cat({varintField(1, 1), extra}));
}
} // namespace

class TestCastProto : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void connectGoldenBytes()
    {
        CastMessage message;
        message.sourceId = QString(kCastSenderId);
        message.destinationId = QString(kCastPlatformId);
        message.ns = QString(kCastNsConnection);
        message.payload = QByteArray(R"({"type":"CONNECT","userAgent":"Kinefin"})");

        // 4-byte length 0x6e = 110, then: version 0, source (8), destination (10), namespace (40), STRING, payload (40)
        const QByteArray expected = cat(
            {QByteArray::fromHex("0000006e"
                                 "0800"
                                 "1208"),
             QByteArray("sender-0"), QByteArray::fromHex("1a0a"), QByteArray("receiver-0"),
             QByteArray::fromHex("2228"), QByteArray("urn:x-cast:com.google.cast.tp.connection"),
             QByteArray::fromHex("2800"
                                 "3228"),
             QByteArray(R"({"type":"CONNECT","userAgent":"Kinefin"})")});
        QCOMPARE(expected.size(), qsizetype(114));
        QCOMPARE(frameCastMessage(message), expected);
        QCOMPARE(encodeCastMessage(message), expected.mid(4));

        const auto decoded = decodeCastMessage(expected.mid(4));
        QVERIFY(decoded);
        QCOMPARE(decoded->sourceId, QStringLiteral("sender-0"));
        QCOMPARE(decoded->destinationId, QStringLiteral("receiver-0"));
        QCOMPARE(decoded->ns, QStringLiteral("urn:x-cast:com.google.cast.tp.connection"));
        QVERIFY(!decoded->binary);
        QCOMPARE(decoded->payload, message.payload);
    }

    void binaryGoldenBytes()
    {
        CastMessage message;
        message.sourceId = QStringLiteral("a");
        message.destinationId = QStringLiteral("b");
        message.ns = QStringLiteral("n");
        message.binary = true;
        message.payload = QByteArray::fromHex("00ff");
        QCOMPARE(encodeCastMessage(message), QByteArray::fromHex("0800"
                                                                 "120161"
                                                                 "1a0162"
                                                                 "22016e"
                                                                 "2801"
                                                                 "3a0200ff"));
    }

    void authChallengeGoldenBytes()
    {
        QCOMPARE(encodeAuthChallenge(QByteArray::fromHex("000102030405060708090a0b0c0d0e0f")),
                 QByteArray::fromHex("0a141210"
                                     "000102030405060708090a0b0c0d0e0f"
                                     "1801"));
        QCOMPARE(encodeAuthChallenge(QByteArray::fromHex("ffffffffffffffffffffffffffffffff")),
                 QByteArray::fromHex("0a141210"
                                     "ffffffffffffffffffffffffffffffff"
                                     "1801"));
    }

    void roundTripsAStringAndABinaryMessage()
    {
        CastMessage text = sample();
        text.payload = QByteArray("{\"title\":\"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80\"}");
        const auto decodedText = decodeCastMessage(encodeCastMessage(text));
        QVERIFY(decodedText);
        QCOMPARE(decodedText->sourceId, text.sourceId);
        QCOMPARE(decodedText->destinationId, text.destinationId);
        QCOMPARE(decodedText->ns, text.ns);
        QVERIFY(!decodedText->binary);
        QCOMPARE(decodedText->payload, text.payload);

        CastMessage binary = sample();
        binary.ns = QString(kCastNsDeviceAuth);
        binary.binary = true;
        binary.payload = QByteArray(256, '\0');
        for (int i = 0; i < 256; ++i)
            binary.payload[i] = char(i);
        const auto decodedBinary = decodeCastMessage(encodeCastMessage(binary));
        QVERIFY(decodedBinary);
        QCOMPARE(decodedBinary->ns, binary.ns);
        QVERIFY(decodedBinary->binary);
        QCOMPARE(decodedBinary->payload, binary.payload);

        CastMessage empty = sample();
        empty.payload.clear();
        const auto decodedEmpty = decodeCastMessage(encodeCastMessage(empty));
        QVERIFY(decodedEmpty);
        QVERIFY(decodedEmpty->payload.isEmpty());

        CastFramer framer;
        framer.append(frameCastMessage(binary));
        const auto body = framer.next();
        QVERIFY(body);
        const auto viaFramer = decodeCastMessage(*body);
        QVERIFY(viaFramer);
        QCOMPARE(viaFramer->payload, binary.payload);
        QVERIFY(!framer.next());
        QVERIFY(!framer.failed());
    }

    void skipsUnknownFields()
    {
        const QByteArray body = cat({
            varintField(15, 300),
            bytesField(14, QByteArray(100, 'z')),
            versionField(),
            sourceField(),
            varintField(8, 1),
            bytesField(8, QByteArray("x")),
            bytesField(99, QByteArray("skip me")),
            destinationField(),
            varintField(9, 12345),
            varintField(1000, 1),
            namespaceField(),
            typeField(0),
            bytesField(21, QByteArray()),
            utf8Field(kPing),
            varintField(40, 7),
        });
        const auto decoded = decodeCastMessage(body);
        QVERIFY(decoded);
        const CastMessage expected = sample();
        QCOMPARE(decoded->sourceId, expected.sourceId);
        QCOMPARE(decoded->destinationId, expected.destinationId);
        QCOMPARE(decoded->ns, expected.ns);
        QVERIFY(!decoded->binary);
        QCOMPARE(decoded->payload, expected.payload);
    }

    void refusesUnsupportedWireTypes()
    {
        // Start group, end group and the two undefined types, each with enough bytes after the tag
        for (const quint64 wireType : {3, 4, 6, 7}) {
            const QByteArray unknown = cat({varint((30 << 3) | wireType), QByteArray(8, '\0')});
            const QByteArray label = QByteArray::number(wireType);
            QVERIFY2(!decodeCastMessage(cat({unknown, stringBody()})), label.constData());
            QVERIFY2(!decodeCastMessage(cat({stringBody(), unknown})), label.constData());
            QVERIFY2(!decodeAuthMessage(cat({unknown, responseWith()})), label.constData());
            QVERIFY2(!decodeAuthMessage(responseWith(unknown)), label.constData());
            QVERIFY2(!decodeAuthMessage(errorWith(unknown)), label.constData());
        }
        QVERIFY(!decodeCastMessage(cat({varintField(0, 1), stringBody()})));
        QVERIFY(decodeCastMessage(stringBody()));
    }

    void skipsUnknownFixedWidthFields()
    {
        const auto decoded = decodeCastMessage(cat({
            fixed32Field(30),
            versionField(),
            sourceField(),
            fixed64Field(31),
            destinationField(),
            namespaceField(),
            fixed32Field(8),
            typeField(0),
            fixed64Field(99),
            utf8Field(kPing),
            fixed32Field(1000),
        }));
        QVERIFY(decoded);
        const CastMessage expected = sample();
        QCOMPARE(decoded->sourceId, expected.sourceId);
        QCOMPARE(decoded->destinationId, expected.destinationId);
        QCOMPARE(decoded->ns, expected.ns);
        QCOMPARE(decoded->payload, expected.payload);

        const auto auth = decodeAuthMessage(
            cat({fixed64Field(9), responseWith(cat({fixed32Field(30), fixed64Field(31)})),
                 fixed32Field(10)}));
        QVERIFY(auth);
        QVERIFY(auth->response);
        QCOMPARE(auth->response->signature, QByteArray("s"));
        QCOMPARE(auth->response->clientAuthCertificate, QByteArray("l"));

        const auto error = decodeAuthMessage(errorWith(cat({fixed32Field(9), fixed64Field(10)})));
        QVERIFY(error);
        QVERIFY(error->error);
        QCOMPARE(error->error->errorType, 1);
    }

    void refusesATruncatedFixedWidthField()
    {
        const QByteArray tag32 = varint((30 << 3) | 5);
        const QByteArray tag64 = varint((30 << 3) | 1);
        // Exactly the right number of bytes is accepted, one byte less or the bare tag is not
        QVERIFY(decodeCastMessage(cat({stringBody(), tag32, QByteArray(4, 'x')})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), tag32, QByteArray(3, 'x')})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), tag32})));
        QVERIFY(decodeCastMessage(cat({stringBody(), tag64, QByteArray(8, 'x')})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), tag64, QByteArray(7, 'x')})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), tag64})));

        QVERIFY(decodeAuthMessage(cat({responseWith(), tag32, QByteArray(4, 'x')})));
        QVERIFY(!decodeAuthMessage(cat({responseWith(), tag32, QByteArray(3, 'x')})));
        QVERIFY(!decodeAuthMessage(cat({responseWith(), tag64, QByteArray(7, 'x')})));

        // A nested message ends where its length says, so the bytes after it do not complete the field
        QVERIFY(decodeAuthMessage(responseWith(cat({tag64, QByteArray(8, 'x')}))));
        QVERIFY(!decodeAuthMessage(responseWith(cat({tag64, QByteArray(7, 'x')}))));
        QVERIFY(!decodeAuthMessage(
            cat({responseWith(cat({tag32, QByteArray(3, 'x')})), QByteArray(8, 'x')})));
        QVERIFY(decodeAuthMessage(errorWith(cat({tag32, QByteArray(4, 'x')}))));
        QVERIFY(!decodeAuthMessage(errorWith(cat({tag32, QByteArray(3, 'x')}))));
        QVERIFY(!decodeAuthMessage(cat({errorWith(tag64), QByteArray(8, 'x')})));
    }

    void refusesAFixedWidthTypeOnAKnownField()
    {
        for (quint32 number = 1; number <= 7; ++number) {
            const QByteArray label = QByteArray::number(number);
            QVERIFY2(!decodeCastMessage(cat({stringBody(), fixed32Field(number)})),
                     label.constData());
            QVERIFY2(!decodeCastMessage(cat({stringBody(), fixed64Field(number)})),
                     label.constData());
            QVERIFY2(!decodeAuthMessage(responseWith(fixed32Field(number))), label.constData());
            QVERIFY2(!decodeAuthMessage(responseWith(fixed64Field(number))), label.constData());
        }
        // The response (2) and the error (3) of the auth message, then the error_type
        QVERIFY(!decodeAuthMessage(cat({responseWith(), fixed32Field(2)})));
        QVERIFY(!decodeAuthMessage(cat({responseWith(), fixed64Field(3)})));
        QVERIFY(!decodeAuthMessage(cat({responseWith(), fixed32Field(3)})));
        QVERIFY(!decodeAuthMessage(errorWith(fixed32Field(1))));
        QVERIFY(!decodeAuthMessage(errorWith(fixed64Field(1))));
    }

    void refusesElevenByteVarints()
    {
        const auto withType = [](const QByteArray &typeBytes) {
            return cat({versionField(), sourceField(), destinationField(), namespaceField(),
                        QByteArray::fromHex("28"), typeBytes, utf8Field(kPing)});
        };
        const QByteArray continued(9, char(0x80));
        // Ten bytes of value zero are a valid varint, an eleventh byte is not
        QVERIFY(decodeCastMessage(withType(cat({continued, QByteArray(1, '\0')}))));
        QVERIFY(!decodeCastMessage(
            withType(cat({continued, QByteArray(1, char(0x80)), QByteArray(1, '\0')}))));
        QVERIFY(!decodeCastMessage(withType(cat({continued, QByteArray(2, char(0xff))}))));

        const QByteArray eleven = cat({continued, QByteArray(1, char(0x80)), QByteArray(1, '\0')});
        const QByteArray nameTag = QByteArray::fromHex("12");
        QVERIFY(!decodeCastMessage(cat({eleven, stringBody()})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), nameTag, eleven, QByteArray("x")})));
        QVERIFY(!decodeAuthMessage(cat({nameTag, eleven, QByteArray("x")})));

        // A varint that is cut short by the end of the body
        QVERIFY(!decodeCastMessage(cat({stringBody(), QByteArray::fromHex("2880")})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), QByteArray::fromHex("80")})));
    }

    void refusesALengthPastTheEnd()
    {
        const QByteArray valid = stringBody();
        const QByteArray abc("abc");
        QVERIFY(decodeCastMessage(valid));
        QVERIFY(!decodeCastMessage(cat({valid, QByteArray::fromHex("12"), varint(10), abc})));
        QVERIFY(!decodeCastMessage(cat({valid, QByteArray::fromHex("22"), varint(100), abc})));
        QVERIFY(!decodeCastMessage(cat({valid, QByteArray::fromHex("22"), varint(1)})));
        // A length of 2^64 - 1 and one of 2^63
        const QByteArray huge = QByteArray::fromHex("ffffffffffffffffff01");
        QVERIFY(!decodeCastMessage(cat({valid, QByteArray::fromHex("32"), huge, abc})));
        const QByteArray twoToThe63 = varint(Q_UINT64_C(1) << 63);
        QVERIFY(!decodeCastMessage(cat({valid, QByteArray::fromHex("32"), twoToThe63, abc})));
        // Every proper prefix of a valid body is refused: cut inside a field, or a required field is missing
        for (qsizetype size = 0; size < valid.size(); ++size)
            QVERIFY2(!decodeCastMessage(valid.left(size)), QByteArray::number(size).constData());
    }

    void refusesAMissingNamespace()
    {
        const QByteArray version = versionField();
        const QByteArray source = sourceField();
        const QByteArray destination = destinationField();
        const QByteArray ns = namespaceField();
        const QByteArray type = typeField(0);
        const QByteArray payload = utf8Field(kPing);
        QVERIFY(decodeCastMessage(cat({version, source, destination, ns, type, payload})));

        QVERIFY(!decodeCastMessage(cat({version, source, destination, type, payload})));
        QVERIFY(!decodeCastMessage(cat({version, destination, ns, type, payload})));
        QVERIFY(!decodeCastMessage(cat({version, source, ns, type, payload})));
        QVERIFY(!decodeCastMessage(cat({version, source, destination, ns, payload})));
        QVERIFY(!decodeCastMessage(cat({version, source, destination, ns, type})));
        QVERIFY(!decodeCastMessage(QByteArray()));
        // Present but empty counts as missing
        QVERIFY(!decodeCastMessage(bodyWith(QByteArray("a"), QByteArray("b"), QByteArray())));
        QVERIFY(!decodeCastMessage(bodyWith(QByteArray(), QByteArray("b"), QByteArray("n"))));
        QVERIFY(!decodeCastMessage(bodyWith(QByteArray("a"), QByteArray(), QByteArray("n"))));
        // The protocol version is not required
        QVERIFY(decodeCastMessage(cat({source, destination, ns, type, payload})));
    }

    void refusesAPayloadTypeMismatch()
    {
        const auto fields = [](quint64 type, const QByteArray &payloadFields) {
            return cat({versionField(), sourceField(), destinationField(), namespaceField(),
                        typeField(type), payloadFields});
        };
        QVERIFY(decodeCastMessage(fields(0, utf8Field(kPing))));
        QVERIFY(decodeCastMessage(fields(1, binaryField(kPing))));

        QVERIFY(!decodeCastMessage(fields(1, utf8Field(kPing))));
        QVERIFY(!decodeCastMessage(fields(0, binaryField(kPing))));
        QVERIFY(!decodeCastMessage(fields(0, QByteArray())));
        QVERIFY(!decodeCastMessage(fields(1, QByteArray())));
        QVERIFY(!decodeCastMessage(fields(2, utf8Field(kPing))));
        QVERIFY(!decodeCastMessage(fields(2, binaryField(kPing))));
        QVERIFY(!decodeCastMessage(fields(Q_UINT64_C(0x100000000), utf8Field(kPing))));

        // A STRING payload must be UTF-8: a stray continuation byte, a truncated sequence, an overlong form
        const QList<QByteArray> invalidTexts = {
            QByteArray::fromHex("7b80"), QByteArray::fromHex("7bc3"), QByteArray::fromHex("c080")};
        for (const QByteArray &invalid : invalidTexts)
            QVERIFY2(!decodeCastMessage(fields(0, utf8Field(invalid))),
                     invalid.toHex().constData());
        // The same bytes are fine as a BINARY payload
        const QByteArray invalidUtf8 = QByteArray::fromHex("7b80c3");
        const auto binary = decodeCastMessage(fields(1, binaryField(invalidUtf8)));
        QVERIFY(binary);
        QCOMPARE(binary->payload, invalidUtf8);

        // When both are present the one that matches payload_type is used
        const QByteArray bin("bin");
        const auto both = decodeCastMessage(fields(0, cat({binaryField(bin), utf8Field(kPing)})));
        QVERIFY(both);
        QVERIFY(!both->binary);
        QCOMPARE(both->payload, kPing);
        const auto bothBinary =
            decodeCastMessage(fields(1, cat({utf8Field(kPing), binaryField(bin)})));
        QVERIFY(bothBinary);
        QVERIFY(bothBinary->binary);
        QCOMPARE(bothBinary->payload, bin);
    }

    void refusesAWrongWireTypeOnAKnownField()
    {
        QVERIFY(!decodeCastMessage(cat({bytesField(1, QByteArray("x")), stringBody()})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), varintField(2, 5)})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), varintField(3, 5)})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), varintField(4, 5)})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), bytesField(5, QByteArray("x"))})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), varintField(6, 5)})));
        QVERIFY(!decodeCastMessage(cat({stringBody(), varintField(7, 5)})));
    }

    void repeatedSingularFieldsKeepTheLastValue()
    {
        const QByteArray first("first");
        const QByteArray body =
            cat({versionField(), sourceField(first), sourceField(), destinationField(first),
                 destinationField(), namespaceField(first), namespaceField(), typeField(1),
                 typeField(0), utf8Field(first), utf8Field(kPing)});
        const auto decoded = decodeCastMessage(body);
        QVERIFY(decoded);
        const CastMessage expected = sample();
        QCOMPARE(decoded->sourceId, expected.sourceId);
        QCOMPARE(decoded->destinationId, expected.destinationId);
        QCOMPARE(decoded->ns, expected.ns);
        QVERIFY(!decoded->binary);
        QCOMPARE(decoded->payload, expected.payload);

        // The last occurrence is the one checked
        const auto replaced =
            decodeCastMessage(cat({stringBody(), sourceField(QByteArray("last"))}));
        QVERIFY(replaced);
        QCOMPARE(replaced->sourceId, QStringLiteral("last"));
        QVERIFY(!decodeCastMessage(cat({stringBody(), sourceField(QByteArray(65, 'a'))})));
    }

    void refusesAnOversizedIdAndNamespace()
    {
        const auto withIds = [](const QString &source, const QString &destination,
                                const QString &ns) {
            CastMessage message = sample();
            message.sourceId = source;
            message.destinationId = destination;
            message.ns = ns;
            return message;
        };
        const QString ok = QStringLiteral("receiver-0");
        const QString id64(64, QLatin1Char('a'));
        const QString id65(65, QLatin1Char('a'));
        const QString ns128(128, QLatin1Char('n'));
        const QString ns129(129, QLatin1Char('n'));
        // 32 characters of two bytes are 64 bytes, 33 are 66
        const QString wide32(32, QChar(0x00e9));
        const QString wide33(33, QChar(0x00e9));

        QVERIFY(!encodeCastMessage(withIds(id64, id64, ns128)).isEmpty());
        QVERIFY(!encodeCastMessage(withIds(wide32, wide32, ns128)).isEmpty());
        QVERIFY(encodeCastMessage(withIds(id65, ok, ok)).isEmpty());
        QVERIFY(encodeCastMessage(withIds(ok, id65, ok)).isEmpty());
        QVERIFY(encodeCastMessage(withIds(wide33, ok, ok)).isEmpty());
        QVERIFY(encodeCastMessage(withIds(ok, wide33, ok)).isEmpty());
        QVERIFY(encodeCastMessage(withIds(ok, ok, ns129)).isEmpty());
        QVERIFY(encodeCastMessage(withIds(QString(), ok, ok)).isEmpty());
        QVERIFY(encodeCastMessage(withIds(ok, QString(), ok)).isEmpty());
        QVERIFY(encodeCastMessage(withIds(ok, ok, QString())).isEmpty());
        QVERIFY(frameCastMessage(withIds(id65, ok, ok)).isEmpty());
        QVERIFY(frameCastMessage(withIds(ok, ok, ns129)).isEmpty());
        QVERIFY(frameCastMessage(withIds(QString(), ok, ok)).isEmpty());
        QVERIFY(!frameCastMessage(withIds(id64, id64, ns128)).isEmpty());

        const QByteArray a("a");
        const QByteArray b("b");
        const QByteArray n("n");
        const QByteArray id64Bytes(64, 'a');
        QVERIFY(decodeCastMessage(bodyWith(id64Bytes, id64Bytes, ns128.toUtf8())));
        QVERIFY(!decodeCastMessage(bodyWith(QByteArray(65, 'a'), b, n)));
        QVERIFY(!decodeCastMessage(bodyWith(a, QByteArray(65, 'b'), n)));
        QVERIFY(!decodeCastMessage(bodyWith(a, b, QByteArray(129, 'n'))));
        // 33 characters of two bytes are over the cap although they are fewer than 64 characters
        const QByteArray wideBytes = wide33.toUtf8();
        QVERIFY(!decodeCastMessage(bodyWith(wideBytes, b, n)));
        // A name that is not valid UTF-8 is refused instead of being replaced
        QVERIFY(!decodeCastMessage(bodyWith(QByteArray::fromHex("ff"), b, n)));
        QVERIFY(!decodeCastMessage(bodyWith(a, QByteArray::fromHex("c3"), n)));
        QVERIFY(!decodeCastMessage(bodyWith(a, b, QByteArray::fromHex("6efe"))));
    }

    void refusesABodyOverTheCap()
    {
        CastMessage message = sample();
        message.payload.clear();
        const qsizetype overhead = encodeCastMessage(message).size();
        // The payload length takes three bytes of varint instead of the one of the empty payload
        const qsizetype largest = kMaxCastFrameBytes - overhead - 2;
        QVERIFY(largest > 16384);

        message.payload = QByteArray(largest, 'p');
        const QByteArray body = encodeCastMessage(message);
        QCOMPARE(body.size(), kMaxCastFrameBytes);
        QCOMPARE(frameCastMessage(message).size(), kMaxCastFrameBytes + 4);
        const auto decoded = decodeCastMessage(body);
        QVERIFY(decoded);
        QCOMPARE(decoded->payload, message.payload);

        message.payload = QByteArray(largest + 1, 'p');
        QVERIFY(encodeCastMessage(message).isEmpty());
        QVERIFY(frameCastMessage(message).isEmpty());
        message.payload = QByteArray(kMaxCastFrameBytes, 'p');
        QVERIFY(encodeCastMessage(message).isEmpty());
        message.payload = QByteArray(kMaxCastFrameBytes + 1, 'p');
        QVERIFY(encodeCastMessage(message).isEmpty());
        message.binary = true;
        QVERIFY(encodeCastMessage(message).isEmpty());

        // The cap is on the whole body, so a big payload and a small one next to a big unknown field are both refused
        const QByteArray a("a");
        const QByteArray b("b");
        const QByteArray n("n");
        QVERIFY(!decodeCastMessage(bodyWith(a, b, n, QByteArray(kMaxCastFrameBytes + 1, 'p'))));
        const QByteArray padding = bytesField(50, QByteArray(kMaxCastFrameBytes, 'p'));
        QVERIFY(!decodeCastMessage(cat({stringBody(), padding})));
    }

    void framerRefusesZeroAndOversizeBeforeAllocating_data()
    {
        QTest::addColumn<quint32>("length");
        QTest::newRow("zero") << quint32(0);
        QTest::newRow("one over the cap") << quint32(65537);
        QTest::newRow("double the cap") << quint32(0x00020000);
        QTest::newRow("half of the range") << quint32(0x80000000);
        QTest::newRow("all ones") << quint32(0xffffffff);
    }
    void framerRefusesZeroAndOversizeBeforeAllocating()
    {
        QFETCH(quint32, length);
        const QByteArray head = header(length);
        for (const qsizetype firstChunk : {4, 3, 2, 1}) {
            CastFramer framer;
            framer.append(head.left(firstChunk));
            if (firstChunk < 4) {
                QVERIFY(!framer.failed());
                framer.append(head.mid(firstChunk));
            }
            QVERIFY(framer.failed());
            QVERIFY(framer.buffered() <= 4);
            QVERIFY(!framer.next());

            // A failed stream stays failed and takes nothing more
            framer.append(frameOf(QByteArray("ok")));
            QVERIFY(framer.failed());
            QVERIFY(framer.buffered() <= 4);
            QVERIFY(!framer.next());
        }

        // The body that follows the header in the same read is not kept
        CastFramer framer;
        framer.append(cat({head, QByteArray(100, 'b')}));
        QVERIFY(framer.failed());
        QVERIFY(framer.buffered() <= 4);
        QVERIFY(!framer.next());
    }

    void framerAcceptsTheLargestLength()
    {
        CastFramer framer;
        framer.append(header(quint32(kMaxCastFrameBytes)));
        QVERIFY(!framer.failed());
        QCOMPARE(framer.buffered(), qsizetype(4));
        QVERIFY(!framer.next());
        framer.append(QByteArray(kMaxCastFrameBytes - 1, 'x'));
        QVERIFY(!framer.next());
        framer.append(QByteArray(1, 'x'));
        const auto body = framer.next();
        QVERIFY(body);
        QCOMPARE(body->size(), kMaxCastFrameBytes);
        QCOMPARE(framer.buffered(), qsizetype(0));

        CastFramer smallest;
        smallest.append(frameOf(QByteArray("x")));
        const auto one = smallest.next();
        QVERIFY(one);
        QCOMPARE(*one, QByteArray("x"));
    }

    void framerFailsOnABadHeaderAfterAGoodFrame()
    {
        CastFramer framer;
        framer.append(cat({frameOf(QByteArray("good")), header(65537), QByteArray("tail")}));
        const auto good = framer.next();
        QVERIFY(good);
        QCOMPARE(*good, QByteArray("good"));
        QVERIFY(!framer.next());
        QVERIFY(framer.failed());
        QVERIFY(framer.buffered() <= 4);

        CastFramer zero;
        zero.append(cat({frameOf(QByteArray("good")), header(0)}));
        QVERIFY(zero.next());
        QVERIFY(!zero.next());
        QVERIFY(zero.failed());
    }

    void framerReassemblesSplitFrames()
    {
        const QList<QByteArray> bodies = {QByteArray("a"), QByteArray(300, 'x'),
                                          QByteArray(65536, 'y'), QByteArray("tail"),
                                          QByteArray(70, 'z')};
        QByteArray stream;
        for (const QByteArray &body : bodies)
            stream.append(frameOf(body));

        const Fed oneByOne = feed(stream, {1});
        QVERIFY(!oneByOne.failed);
        QCOMPARE(oneByOne.bodies, bodies);
        QVERIFY(oneByOne.peakBuffered <= 4 + kMaxCastFrameBytes);

        const Fed odd = feed(stream, {1, 2, 3, 5, 7, 11, 4093, 65539});
        QVERIFY(!odd.failed);
        QCOMPARE(odd.bodies, bodies);

        const Fed whole = feed(stream, {stream.size()});
        QVERIFY(!whole.failed);
        QCOMPARE(whole.bodies, bodies);

        const Fed header3 = feed(stream, {3, 4, 1});
        QVERIFY(!header3.failed);
        QCOMPARE(header3.bodies, bodies);

        // Several frames in one append come out one at a time, in order
        CastFramer framer;
        framer.append(cat({frameOf(QByteArray("one")), frameOf(QByteArray("two")),
                           frameOf(QByteArray("three")), header(5), QByteArray("fo")}));
        QCOMPARE(framer.next().value_or(QByteArray()), QByteArray("one"));
        QCOMPARE(framer.next().value_or(QByteArray()), QByteArray("two"));
        QCOMPARE(framer.next().value_or(QByteArray()), QByteArray("three"));
        QVERIFY(!framer.next());
        QVERIFY(!framer.failed());
        framer.append(QByteArray("ur!"));
        QCOMPARE(framer.next().value_or(QByteArray()), QByteArray("four!"));
        QVERIFY(!framer.next());
        QCOMPARE(framer.buffered(), qsizetype(0));

        // A header cut in the middle waits for the rest
        CastFramer split;
        split.append(header(2).left(3));
        QVERIFY(!split.next());
        QVERIFY(!split.failed());
        QCOMPARE(split.buffered(), qsizetype(3));
        split.append(header(2).mid(3));
        split.append(QByteArray("ab"));
        QCOMPARE(split.next().value_or(QByteArray()), QByteArray("ab"));
    }

    void decodesAnAuthResponse()
    {
        const QByteArray signature(256, 's');
        const QByteArray leaf(1000, 'L');
        const QByteArray first(700, '1');
        const QByteArray second(800, '2');
        const QByteArray nonce = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
        const QByteArray crl(3 * 1024, 'c');
        const QByteArray response = cat({
            bytesField(1, signature),
            bytesField(2, leaf),
            bytesField(3, first),
            bytesField(3, second),
            varintField(4, 1),
            bytesField(5, nonce),
            varintField(6, 1),
            bytesField(7, crl),
            varintField(15, 9),
        });
        const auto decoded = decodeAuthMessage(
            cat({varintField(9, 1), bytesField(2, response), bytesField(10, QByteArray("x"))}));
        QVERIFY(decoded);
        QVERIFY(!decoded->error);
        QVERIFY(decoded->response);
        const CastAuthResponse &parsed = *decoded->response;
        QCOMPARE(parsed.signature, signature);
        QCOMPARE(parsed.clientAuthCertificate, leaf);
        QCOMPARE(parsed.intermediateCertificates, (QList<QByteArray>{first, second}));
        QCOMPARE(parsed.signatureAlgorithm, 1);
        QCOMPARE(parsed.senderNonce, nonce);
        QCOMPARE(parsed.hashAlgorithm, 1);
        QCOMPARE(parsed.crlBytes, qsizetype(3072));

        const auto pss =
            decodeAuthMessage(bytesField(2, cat({bytesField(1, signature), bytesField(2, leaf),
                                                 varintField(4, 2), varintField(6, 0)})));
        QVERIFY(pss);
        QCOMPARE(pss->response->signatureAlgorithm, 2);
        QCOMPARE(pss->response->hashAlgorithm, 0);
    }

    void decodesAnAuthError()
    {
        for (const int type : {0, 1, 2}) {
            const auto decoded = decodeAuthMessage(bytesField(3, varintField(1, quint64(type))));
            QVERIFY(decoded);
            QVERIFY(!decoded->response);
            QVERIFY(decoded->error);
            QCOMPARE(decoded->error->errorType, type);
        }
        const auto bare = decodeAuthMessage(bytesField(3, QByteArray()));
        QVERIFY(bare);
        QVERIFY(bare->error);
        QCOMPARE(bare->error->errorType, 0);

        // Unknown fields inside the error are skipped, a bad one is not
        const auto padded =
            decodeAuthMessage(bytesField(3, cat({varintField(9, 1), varintField(1, 1)})));
        QVERIFY(padded);
        QCOMPARE(padded->error->errorType, 1);
        QVERIFY(!decodeAuthMessage(bytesField(3, bytesField(1, QByteArray("x")))));
        QVERIFY(!decodeAuthMessage(bytesField(3, varintField(1, Q_UINT64_C(1) << 40))));
        const QByteArray cutShort = cat({varintField(1, 1), QByteArray::fromHex("12"), varint(9)});
        QVERIFY(!decodeAuthMessage(bytesField(3, cutShort)));
        QVERIFY(!decodeAuthMessage(varintField(3, 1)));

        // With both present the caller sees both
        const QByteArray response = authResponse(QByteArray("s"), QByteArray("l"), {});
        const auto both = decodeAuthMessage(cat({response, bytesField(3, varintField(1, 2))}));
        QVERIFY(both);
        QVERIFY(both->response);
        QVERIFY(both->error);
        QCOMPARE(both->error->errorType, 2);
    }

    void refusesAnAuthMessageWithNeitherResponseNorError()
    {
        QVERIFY(!decodeAuthMessage(QByteArray()));
        QVERIFY(!decodeAuthMessage(encodeAuthChallenge(QByteArray(16, 'n'))));
        QVERIFY(!decodeAuthMessage(cat({varintField(4, 1), bytesField(5, QByteArray("x"))})));
        QVERIFY(decodeAuthMessage(authResponse(QByteArray("s"), QByteArray("l"), {})));
        // An empty response message is still a response, the verifier refuses it for what it lacks
        const auto empty = decodeAuthMessage(bytesField(2, QByteArray()));
        QVERIFY(empty);
        QVERIFY(empty->response);
        QVERIFY(empty->response->signature.isEmpty());
        QVERIFY(empty->response->clientAuthCertificate.isEmpty());
        // A response that is not length-delimited, or cut short, is malformed and not "neither"
        const QByteArray overrun = cat({QByteArray::fromHex("12"), varint(50), QByteArray("abc")});
        QVERIFY(!decodeAuthMessage(varintField(2, 1)));
        QVERIFY(!decodeAuthMessage(overrun));
        QVERIFY(!decodeAuthMessage(bytesField(2, cat({bytesField(1, QByteArray("s")), overrun}))));
        QVERIFY(!decodeAuthMessage(bytesField(2, varintField(1, 1))));
    }

    void refusesTooManyIntermediatesAndOversizeCertificates()
    {
        const QByteArray signature(kMaxCastAuthSignatureBytes, 's');
        const QByteArray leaf(kMaxCastCertificateBytes, 'L');
        const QByteArray intermediate(kMaxCastCertificateBytes, 'i');

        const QByteArray bigSignature(kMaxCastAuthSignatureBytes + 1, 's');
        const QByteArray bigCertificate(kMaxCastCertificateBytes + 1, 'c');

        const QList<QByteArray> three(3, intermediate);
        QVERIFY(decodeAuthMessage(authResponse(signature, leaf, three)));
        QVERIFY(decodeAuthMessage(authResponse(signature, leaf, {})));
        const QList<QByteArray> four(4, intermediate);
        const QList<QByteArray> many(10, QByteArray("i"));
        QVERIFY(!decodeAuthMessage(authResponse(signature, leaf, four)));
        QVERIFY(!decodeAuthMessage(authResponse(signature, leaf, many)));

        QVERIFY(!decodeAuthMessage(authResponse(bigSignature, leaf, {})));
        QVERIFY(!decodeAuthMessage(authResponse(signature, bigCertificate, {})));
        QVERIFY(!decodeAuthMessage(authResponse(signature, leaf, {bigCertificate})));
        QVERIFY(!decodeAuthMessage(authResponse(signature, leaf, {intermediate, bigCertificate})));

        // The caps hold for a second occurrence as well
        const QByteArray twice = cat({bytesField(1, QByteArray("s")), bytesField(1, bigSignature)});
        QVERIFY(!decodeAuthMessage(bytesField(2, twice)));
    }

    void defaultsOfAnAuthResponse()
    {
        const auto decoded =
            decodeAuthMessage(authResponse(QByteArray("sig"), QByteArray("leaf"), {}));
        QVERIFY(decoded);
        QVERIFY(decoded->response);
        const CastAuthResponse &response = *decoded->response;
        QCOMPARE(response.signature, QByteArray("sig"));
        QCOMPARE(response.clientAuthCertificate, QByteArray("leaf"));
        QVERIFY(response.intermediateCertificates.isEmpty());
        QCOMPARE(response.signatureAlgorithm, 1);
        QCOMPARE(response.hashAlgorithm, 0);
        QVERIFY(response.senderNonce.isEmpty());
        QCOMPARE(response.crlBytes, qsizetype(0));

        // A value that does not fit an int is malformed, an unknown one is kept for the verifier to refuse
        QVERIFY(!decodeAuthMessage(bytesField(2, varintField(4, Q_UINT64_C(1) << 32))));
        QVERIFY(!decodeAuthMessage(bytesField(2, varintField(6, ~Q_UINT64_C(0)))));
        const auto unknown =
            decodeAuthMessage(bytesField(2, cat({varintField(4, 7), varintField(6, 9)})));
        QVERIFY(unknown);
        QCOMPARE(unknown->response->signatureAlgorithm, 7);
        QCOMPARE(unknown->response->hashAlgorithm, 9);
    }
};

QTEST_GUILESS_MAIN(TestCastProto)
#include "test_cast_proto.moc"
