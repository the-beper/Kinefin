// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastProto.h"

#include <algorithm>
#include <limits>

namespace Kinefin {
namespace {
constexpr qsizetype kFrameHeaderBytes = 4;
constexpr int kMaxVarintBytes = 10;
constexpr quint64 kPayloadTypeString = 0;
constexpr quint64 kPayloadTypeBinary = 1;
constexpr quint64 kHashAlgorithmSha256 = 1;

enum WireType : int { Varint = 0, Fixed64 = 1, LengthDelimited = 2, Fixed32 = 5 };

// Field numbers of cast_channel.proto in Open Screen
namespace CastMessageField {
enum : quint32 {
    ProtocolVersion = 1,
    SourceId,
    DestinationId,
    Namespace,
    PayloadType,
    PayloadUtf8,
    PayloadBinary
};
}
namespace DeviceAuthField {
enum : quint32 { Challenge = 1, Response, Error };
}
namespace AuthChallengeField {
enum : quint32 { SenderNonce = 2, HashAlgorithm = 3 };
}
namespace AuthResponseField {
enum : quint32 {
    Signature = 1,
    ClientAuthCertificate,
    IntermediateCertificate,
    SignatureAlgorithm,
    SenderNonce,
    HashAlgorithm,
    Crl
};
}
namespace AuthErrorField {
enum : quint32 { ErrorType = 1 };
}

void writeVarint(QByteArray &out, quint64 value)
{
    while (value >= 0x80) {
        out.append(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.append(static_cast<char>(value));
}

void writeTag(QByteArray &out, quint32 number, WireType wireType)
{
    writeVarint(out, (quint64(number) << 3) | quint64(wireType));
}

void writeVarintField(QByteArray &out, quint32 number, quint64 value)
{
    writeTag(out, number, Varint);
    writeVarint(out, value);
}

void writeBytesField(QByteArray &out, quint32 number, const QByteArray &bytes)
{
    writeTag(out, number, LengthDelimited);
    writeVarint(out, quint64(bytes.size()));
    out.append(bytes);
}

struct Field {
    quint32 number = 0;
    int wireType = Varint;
    quint64 varint = 0;
    QByteArrayView bytes;
};

class Reader {
  public:
    explicit Reader(QByteArrayView data) : m_data(data) {}

    bool atEnd() const { return m_pos >= m_data.size(); }

    bool next(Field &field)
    {
        field = Field{};
        quint64 tag = 0;
        if (!readVarint(tag) || tag > std::numeric_limits<quint32>::max())
            return false;
        field.number = quint32(tag >> 3);
        field.wireType = int(tag & 7);
        if (field.number == 0)
            return false;
        switch (field.wireType) {
        case Varint:
            return readVarint(field.varint);
        case LengthDelimited: {
            quint64 length = 0;
            if (!readVarint(length) || length > quint64(m_data.size() - m_pos))
                return false;
            field.bytes = m_data.sliced(m_pos, qsizetype(length));
            m_pos += qsizetype(length);
            return true;
        }
        case Fixed64:
        case Fixed32: {
            // Nothing is kept: the decoders refuse a known field number with this wire type and skip the rest, as a conforming parser does
            const qsizetype size = field.wireType == Fixed64 ? 8 : 4;
            if (size > m_data.size() - m_pos)
                return false;
            m_pos += size;
            return true;
        }
        default:
            // Groups (3 and 4) and the undefined types 6 and 7
            return false;
        }
    }

  private:
    bool readVarint(quint64 &value)
    {
        value = 0;
        for (int i = 0; i < kMaxVarintBytes; ++i) {
            if (m_pos >= m_data.size())
                return false;
            const auto byte = static_cast<quint8>(m_data.at(m_pos++));
            value |= quint64(byte & 0x7f) << (7 * i);
            if (!(byte & 0x80))
                return true;
        }
        return false;
    }

    QByteArrayView m_data;
    qsizetype m_pos = 0;
};

bool takeBytes(const Field &field, std::optional<QByteArrayView> &slot)
{
    if (field.wireType != LengthDelimited)
        return false;
    slot = field.bytes;
    return true;
}

bool takeInt(const Field &field, int &value)
{
    if (field.wireType != Varint || field.varint > quint64(std::numeric_limits<int>::max()))
        return false;
    value = int(field.varint);
    return true;
}

bool isText(QByteArrayView text, qsizetype maxBytes)
{
    return !text.isEmpty() && text.size() <= maxBytes && text.isValidUtf8();
}

quint32 frameLength(const QByteArray &buffer)
{
    quint32 length = 0;
    for (qsizetype i = 0; i < kFrameHeaderBytes; ++i)
        length = (length << 8) | static_cast<quint8>(buffer.at(i));
    return length;
}

bool lengthAllowed(quint32 length)
{
    return length >= 1 && length <= quint32(kMaxCastFrameBytes);
}

std::optional<CastAuthResponse> decodeAuthResponse(QByteArrayView body)
{
    CastAuthResponse response;
    Reader reader(body);
    Field field;
    while (!reader.atEnd()) {
        if (!reader.next(field))
            return std::nullopt;
        switch (field.number) {
        case AuthResponseField::Signature:
            if (field.wireType != LengthDelimited
                || field.bytes.size() > kMaxCastAuthSignatureBytes)
                return std::nullopt;
            response.signature = field.bytes.toByteArray();
            break;
        case AuthResponseField::ClientAuthCertificate:
            if (field.wireType != LengthDelimited || field.bytes.size() > kMaxCastCertificateBytes)
                return std::nullopt;
            response.clientAuthCertificate = field.bytes.toByteArray();
            break;
        case AuthResponseField::IntermediateCertificate:
            if (field.wireType != LengthDelimited || field.bytes.size() > kMaxCastCertificateBytes
                || response.intermediateCertificates.size() >= kMaxCastIntermediates)
                return std::nullopt;
            response.intermediateCertificates.append(field.bytes.toByteArray());
            break;
        case AuthResponseField::SignatureAlgorithm:
            if (!takeInt(field, response.signatureAlgorithm))
                return std::nullopt;
            break;
        case AuthResponseField::SenderNonce:
            if (field.wireType != LengthDelimited)
                return std::nullopt;
            response.senderNonce = field.bytes.toByteArray();
            break;
        case AuthResponseField::HashAlgorithm:
            if (!takeInt(field, response.hashAlgorithm))
                return std::nullopt;
            break;
        case AuthResponseField::Crl:
            if (field.wireType != LengthDelimited)
                return std::nullopt;
            response.crlBytes = field.bytes.size();
            break;
        default:
            break;
        }
    }
    return response;
}

std::optional<CastAuthError> decodeAuthError(QByteArrayView body)
{
    CastAuthError error;
    Reader reader(body);
    Field field;
    while (!reader.atEnd()) {
        if (!reader.next(field))
            return std::nullopt;
        if (field.number == AuthErrorField::ErrorType && !takeInt(field, error.errorType))
            return std::nullopt;
    }
    return error;
}
} // namespace

QByteArray encodeCastMessage(const CastMessage &message)
{
    const QByteArray source = message.sourceId.toUtf8();
    const QByteArray destination = message.destinationId.toUtf8();
    const QByteArray ns = message.ns.toUtf8();
    if (source.isEmpty() || source.size() > kMaxCastIdBytes || destination.isEmpty()
        || destination.size() > kMaxCastIdBytes || ns.isEmpty()
        || ns.size() > kMaxCastNamespaceBytes || message.payload.size() > kMaxCastFrameBytes)
        return {};
    QByteArray body;
    body.reserve(message.payload.size() + 256);
    writeVarintField(body, CastMessageField::ProtocolVersion, 0);
    writeBytesField(body, CastMessageField::SourceId, source);
    writeBytesField(body, CastMessageField::DestinationId, destination);
    writeBytesField(body, CastMessageField::Namespace, ns);
    writeVarintField(body, CastMessageField::PayloadType,
                     message.binary ? kPayloadTypeBinary : kPayloadTypeString);
    writeBytesField(
        body, message.binary ? CastMessageField::PayloadBinary : CastMessageField::PayloadUtf8,
        message.payload);
    if (body.size() > kMaxCastFrameBytes)
        return {};
    return body;
}

QByteArray frameCastMessage(const CastMessage &message)
{
    const QByteArray body = encodeCastMessage(message);
    if (body.isEmpty())
        return {};
    const auto length = quint32(body.size());
    QByteArray frame;
    frame.reserve(kFrameHeaderBytes + body.size());
    for (int shift = 24; shift >= 0; shift -= 8)
        frame.append(static_cast<char>((length >> shift) & 0xff));
    frame.append(body);
    return frame;
}

std::optional<CastMessage> decodeCastMessage(QByteArrayView body)
{
    if (body.size() > kMaxCastFrameBytes)
        return std::nullopt;
    std::optional<QByteArrayView> source;
    std::optional<QByteArrayView> destination;
    std::optional<QByteArrayView> ns;
    std::optional<QByteArrayView> utf8Payload;
    std::optional<QByteArrayView> binaryPayload;
    std::optional<quint64> payloadType;
    Reader reader(body);
    Field field;
    // A singular field that appears twice keeps its last value
    while (!reader.atEnd()) {
        if (!reader.next(field))
            return std::nullopt;
        switch (field.number) {
        case CastMessageField::ProtocolVersion:
            if (field.wireType != Varint)
                return std::nullopt;
            break;
        case CastMessageField::SourceId:
            if (!takeBytes(field, source))
                return std::nullopt;
            break;
        case CastMessageField::DestinationId:
            if (!takeBytes(field, destination))
                return std::nullopt;
            break;
        case CastMessageField::Namespace:
            if (!takeBytes(field, ns))
                return std::nullopt;
            break;
        case CastMessageField::PayloadType:
            if (field.wireType != Varint)
                return std::nullopt;
            payloadType = field.varint;
            break;
        case CastMessageField::PayloadUtf8:
            if (!takeBytes(field, utf8Payload))
                return std::nullopt;
            break;
        case CastMessageField::PayloadBinary:
            if (!takeBytes(field, binaryPayload))
                return std::nullopt;
            break;
        default:
            break;
        }
    }
    if (!source || !destination || !ns || !payloadType || *payloadType > kPayloadTypeBinary)
        return std::nullopt;
    if (!isText(*source, kMaxCastIdBytes) || !isText(*destination, kMaxCastIdBytes)
        || !isText(*ns, kMaxCastNamespaceBytes))
        return std::nullopt;
    const bool binary = *payloadType == kPayloadTypeBinary;
    // The field that matches payload_type is the payload and the other one is ignored, as Chromium does
    const std::optional<QByteArrayView> &payload = binary ? binaryPayload : utf8Payload;
    if (!payload || (!binary && !payload->isValidUtf8()))
        return std::nullopt;
    CastMessage message;
    message.sourceId = QString::fromUtf8(*source);
    message.destinationId = QString::fromUtf8(*destination);
    message.ns = QString::fromUtf8(*ns);
    message.binary = binary;
    message.payload = payload->toByteArray();
    return message;
}

void CastFramer::append(const QByteArray &data)
{
    if (m_failed)
        return;
    qsizetype taken = 0;
    // The header is completed on its own first, so a bad length is seen before a body byte is kept
    if (m_buffer.size() < kFrameHeaderBytes) {
        taken = std::min(kFrameHeaderBytes - m_buffer.size(), data.size());
        m_buffer.append(data.constData(), taken);
        if (m_buffer.size() == kFrameHeaderBytes && !lengthAllowed(frameLength(m_buffer))) {
            m_failed = true;
            m_buffer.clear();
            return;
        }
    }
    m_buffer.append(data.constData() + taken, data.size() - taken);
}

std::optional<QByteArray> CastFramer::next()
{
    if (m_failed || m_buffer.size() < kFrameHeaderBytes)
        return std::nullopt;
    const quint32 length = frameLength(m_buffer);
    if (!lengthAllowed(length)) {
        m_failed = true;
        m_buffer.clear();
        return std::nullopt;
    }
    const qsizetype total = kFrameHeaderBytes + qsizetype(length);
    if (m_buffer.size() < total)
        return std::nullopt;
    QByteArray body = m_buffer.mid(kFrameHeaderBytes, qsizetype(length));
    m_buffer.remove(0, total);
    return body;
}

QByteArray encodeAuthChallenge(const QByteArray &nonce)
{
    QByteArray challenge;
    writeBytesField(challenge, AuthChallengeField::SenderNonce, nonce);
    writeVarintField(challenge, AuthChallengeField::HashAlgorithm, kHashAlgorithmSha256);
    QByteArray message;
    writeBytesField(message, DeviceAuthField::Challenge, challenge);
    return message;
}

std::optional<CastAuthMessage> decodeAuthMessage(QByteArrayView body)
{
    CastAuthMessage message;
    Reader reader(body);
    Field field;
    while (!reader.atEnd()) {
        if (!reader.next(field))
            return std::nullopt;
        switch (field.number) {
        case DeviceAuthField::Response:
            if (field.wireType != LengthDelimited)
                return std::nullopt;
            message.response = decodeAuthResponse(field.bytes);
            if (!message.response)
                return std::nullopt;
            break;
        case DeviceAuthField::Error:
            if (field.wireType != LengthDelimited)
                return std::nullopt;
            message.error = decodeAuthError(field.bytes);
            if (!message.error)
                return std::nullopt;
            break;
        default:
            break;
        }
    }
    if (!message.response && !message.error)
        return std::nullopt;
    return message;
}

} // namespace Kinefin
