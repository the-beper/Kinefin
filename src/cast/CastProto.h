// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>

#include <optional>

namespace Kinefin {

// A CASTV2 frame body is at most 64 KiB (Chromium's MessageFramer::max_body_size)
inline constexpr qsizetype kMaxCastFrameBytes = 65536;
inline constexpr int kMaxCastIdBytes = 64;
inline constexpr int kMaxCastNamespaceBytes = 128;
// The device authentication reply: a signature, a leaf and at most this many intermediates, each certificate within kMaxCastCertificateBytes
inline constexpr int kMaxCastAuthSignatureBytes = 1024;
inline constexpr int kMaxCastCertificateBytes = 4096;
inline constexpr int kMaxCastIntermediates = 3;

inline constexpr QLatin1StringView kCastSenderId{"sender-0"};
inline constexpr QLatin1StringView kCastPlatformId{"receiver-0"};
inline constexpr QLatin1StringView kCastNsConnection{"urn:x-cast:com.google.cast.tp.connection"};
inline constexpr QLatin1StringView kCastNsHeartbeat{"urn:x-cast:com.google.cast.tp.heartbeat"};
inline constexpr QLatin1StringView kCastNsReceiver{"urn:x-cast:com.google.cast.receiver"};
inline constexpr QLatin1StringView kCastNsMedia{"urn:x-cast:com.google.cast.media"};
inline constexpr QLatin1StringView kCastNsDeviceAuth{"urn:x-cast:com.google.cast.tp.deviceauth"};

// One CastMessage of the Cast V2 protocol (protocol_version 0): the ids, the namespace and a STRING payload (UTF-8, the JSON) or a BINARY one
struct CastMessage {
    QString sourceId;
    QString destinationId;
    QString ns;
    bool binary = false;
    QByteArray payload;
};

// The protobuf body of `message` without the length prefix: protocol_version 0, source_id, destination_id, namespace, payload_type and payload_utf8 or payload_binary. Empty when an id or the namespace is empty or over its cap, or the payload is over kMaxCastFrameBytes
QByteArray encodeCastMessage(const CastMessage &message);
// The 4-byte big-endian length of the body followed by the body; empty when encodeCastMessage is
QByteArray frameCastMessage(const CastMessage &message);
// nullopt for anything malformed: a field cut short, a varint over 10 bytes, a group or an undefined wire type (3, 4, 6, 7), a known field with the wrong wire type, a missing source, destination, namespace or payload type, an id or the namespace over its cap, a payload over kMaxCastFrameBytes, a STRING payload that is not valid UTF-8, a payload_type other than 0 and 1. Unknown fields are skipped (fixed32 and fixed64 ones included) and the deprecated chunking fields are ignored
std::optional<CastMessage> decodeCastMessage(QByteArrayView body);

// Cuts the byte stream of the socket into frame bodies. The length is checked on the header, before anything is kept for the body: 0 or over kMaxCastFrameBytes fails the stream for good
class CastFramer {
  public:
    void append(const QByteArray &data);
    // The next complete body, nullopt while more bytes are needed or after failed()
    std::optional<QByteArray> next();
    bool failed() const { return m_failed; }
    // The bytes held now: never more than a header and one body
    qsizetype buffered() const { return m_buffer.size(); }

  private:
    QByteArray m_buffer;
    bool m_failed = false;
};

// DeviceAuthMessage with an AuthChallenge: the sender_nonce and hash_algorithm SHA256; the signature algorithm stays at its default (PKCS#1 v1.5). For a 16-byte nonce the bytes are 0a 14 12 10 <nonce> 18 01
QByteArray encodeAuthChallenge(const QByteArray &nonce);

// AuthResponse. The defaults are the protocol's: PKCS#1 v1.5 and SHA-1 when the field is absent
struct CastAuthResponse {
    QByteArray signature;
    QByteArray clientAuthCertificate;
    QList<QByteArray> intermediateCertificates;
    // 1 RSASSA_PKCS1v15, 2 RSASSA_PSS
    int signatureAlgorithm = 1;
    QByteArray senderNonce;
    // 0 SHA1, 1 SHA256
    int hashAlgorithm = 0;
    // The size of the CRL bundle: it is not read
    qsizetype crlBytes = 0;
};

// AuthError.error_type: 0 INTERNAL_ERROR, 1 NO_TLS, 2 SIGNATURE_ALGORITHM_UNAVAILABLE
struct CastAuthError {
    int errorType = 0;
};

struct CastAuthMessage {
    std::optional<CastAuthResponse> response;
    std::optional<CastAuthError> error;
};

// nullopt when the bytes are malformed, neither a response nor an error is present, a signature is over kMaxCastAuthSignatureBytes, a certificate over kMaxCastCertificateBytes or there are more than kMaxCastIntermediates intermediates. Unknown fields are skipped
std::optional<CastAuthMessage> decodeAuthMessage(QByteArrayView body);

} // namespace Kinefin
