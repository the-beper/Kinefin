// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cast/CastProto.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

namespace Kinefin {

// The roots a device's certificate chain must end at. Opaque: only CastDeviceAuth.cpp includes OpenSSL
class CastTrust {
  public:
    // No roots: every chain fails
    CastTrust();
    // The two Cast roots shipped in the resources :/cast/cast_root_ca.der and :/cast/eureka_root_ca.der
    static CastTrust google();
    // Roots given as DER, for the tests; a DER that does not parse is skipped, and rootCount() says how many were loaded
    static CastTrust fromDer(const QList<QByteArray> &roots);

    int rootCount() const;

  private:
    struct Impl;
    explicit CastTrust(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> m_impl;
    friend struct CastTrustAccess;
};

enum class CastAuthResult {
    Ok,
    // The TLS socket has no peer certificate
    NoPeerCertificate,
    // No trust anchor was loaded
    NoTrust,
    // The reply has no response, or is not the shape the checks need
    MalformedResponse,
    // The device answered with an AuthError
    DeviceError,
    // A signature algorithm other than PKCS#1 v1.5, or a hash other than SHA-1 and SHA-256
    UnsupportedAlgorithm,
    // A signature, a certificate or the chain over its cap
    TooLarge,
    // The chain does not verify against the roots, or the leaf is not usable as a client certificate
    BadChain,
    // The peer certificate does not parse or is not self-signed, or the signature over the nonce and the peer certificate does not verify with the leaf's key
    BadSignature,
    // The leaf key is not RSA of at least 2048 bits
    WeakKey,
    // The chain carries the audio-only policy 1.3.6.1.4.1.11129.2.5.2 and video is required
    AudioOnly,
};

struct CastAuthVerdict {
    CastAuthResult result = CastAuthResult::MalformedResponse;
    // A short English description for the log: no certificate bytes, no nonce, no signature
    QString detail;
    // Things that were logged and did not fail the check: a nonce that differs from ours, a certificate outside its validity, a peer certificate that lives more than four days
    QStringList warnings;
};

// The checks of the Cast sender: the TLS peer certificate parses and is self-signed (its signature verifies with its own public key, as a Cast device's does); the signature over (response nonce followed by the DER of the TLS peer certificate), RSA PKCS#1 v1.5 with SHA-256 or SHA-1 as the response says, with the public key of the leaf; the chain leaf, intermediates, root verified against `trust` only, purpose SSL client, with the validity times not checked (X509_V_FLAG_NO_CHECK_TIME) and no auth level set; the leaf is RSA of at least 2048 bits, and its key usage has digitalSignature when present; the audio-only policy OID anywhere in the chain is refused when `requireVideo`. Times, the sender nonce differing from `nonce` and the CRL are not enforced. It never throws and never logs bytes of the certificates
CastAuthVerdict verifyDeviceAuth(const CastTrust &trust, const QByteArray &nonce,
                                 const QByteArray &peerCertificateDer,
                                 const CastAuthMessage &message, bool requireVideo = true);

} // namespace Kinefin
