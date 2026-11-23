// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "CastTestPki.h"
#include "cast/CastDeviceAuth.h"
#include "cast/CastProto.h"

#include <QtTest>

#include <optional>

using namespace Kinefin;

namespace {
const QByteArray kNonce("0123456789abcdef");

QString describe(const CastAuthVerdict &verdict)
{
    return QStringLiteral("result %1: %2")
        .arg(static_cast<int>(verdict.result))
        .arg(verdict.detail);
}

#define VERIFY_RESULT(expression, expected)                                                        \
    do {                                                                                           \
        const CastAuthVerdict verdictUnderTest = (expression);                                     \
        QVERIFY2(verdictUnderTest.result == (expected), qPrintable(describe(verdictUnderTest)));   \
    } while (false)

bool decode(const QByteArray &bytes, CastAuthMessage &message)
{
    const std::optional<CastAuthMessage> decoded = decodeAuthMessage(bytes);
    if (decoded)
        message = *decoded;
    return decoded.has_value();
}

CastAuthVerdict verify(const CastTestPki &pki, const CastAuthMessage &message,
                       bool requireVideo = true)
{
    return verifyDeviceAuth(pki.trust(), kNonce, pki.tlsCertificate().toDer(), message,
                            requireVideo);
}

// The response a device would send when its signature covers `peer` instead of the TLS certificate
bool decodeSignedOver(const CastTestPki &pki, const QByteArray &peer, CastAuthMessage &message)
{
    CastTestPki::AuthOptions options;
    options.signedPeerOverride = peer;
    return decode(pki.authResponseMessage(kNonce, options), message);
}

CastAuthVerdict verifyPeer(const CastTestPki &pki, const QByteArray &peer)
{
    CastAuthMessage message;
    if (!decodeSignedOver(pki, peer, message))
        return CastAuthVerdict();
    return verifyDeviceAuth(pki.trust(), kNonce, peer, message);
}

bool hasWarning(const CastAuthVerdict &verdict, const QString &text)
{
    return !verdict.warnings.filter(text).isEmpty();
}

const CastTestPki &sharedPki()
{
    static const CastTestPki pki;
    return pki;
}
} // namespace

class TestCastDeviceAuth : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void initTestCase() { QVERIFY(sharedPki().isValid()); }

    void aGeneratedChainVerifies()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        const CastAuthVerdict verdict = verify(pki, message);
        VERIFY_RESULT(verdict, CastAuthResult::Ok);
        QVERIFY2(verdict.warnings.isEmpty(),
                 qPrintable(verdict.warnings.join(QStringLiteral("; "))));
    }

    void theResponseCarriesTheChain()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        QVERIFY(message.response);
        QVERIFY(!message.error);
        const CastAuthResponse &response = *message.response;
        QCOMPARE(response.signatureAlgorithm, 1);
        QCOMPARE(response.hashAlgorithm, 1);
        QCOMPARE(response.senderNonce, kNonce);
        QCOMPARE(response.clientAuthCertificate, pki.leafDer());
        QCOMPARE(response.intermediateCertificates, QList<QByteArray>{pki.intermediateDer()});
        QCOMPARE(response.signature.size(), 256);
    }

    void theTlsIdentityLoads()
    {
        const CastTestPki &pki = sharedPki();
        const QSslCertificate certificate = pki.tlsCertificate();
        QVERIFY(!certificate.isNull());
        QVERIFY(!pki.tlsKey().isNull());
        QCOMPARE(pki.tlsKey().algorithm(), QSsl::Ec);
        const qint64 lifetime = certificate.effectiveDate().secsTo(certificate.expiryDate());
        QCOMPARE(lifetime, qint64(2 * 24 * 60 * 60));
        QCOMPARE(pki.trust().rootCount(), 1);
    }

    void aSignatureOverAnotherPeerFails()
    {
        const CastTestPki &pki = sharedPki();
        CastTestPki::AuthOptions options;
        options.signOverOtherPeer = true;
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce, options), message));
        VERIFY_RESULT(verify(pki, message), CastAuthResult::BadSignature);

        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        const CastAuthVerdict otherPeer =
            verifyDeviceAuth(pki.trust(), kNonce, pki.rootDer(), message);
        VERIFY_RESULT(otherPeer, CastAuthResult::BadSignature);
    }

    void aSelfSignedPeerCertificateVerifiesWithEcAndRsaKeys()
    {
        const CastTestPki &pki = sharedPki();
        for (const bool rsaKey : {false, true}) {
            const QByteArray peer = CastTestPki::peerCertificateDer(rsaKey);
            QVERIFY(!peer.isEmpty());
            const CastAuthVerdict verdict = verifyPeer(pki, peer);
            VERIFY_RESULT(verdict, CastAuthResult::Ok);
            QVERIFY2(verdict.warnings.isEmpty(),
                     qPrintable(verdict.warnings.join(QStringLiteral("; "))));
        }
    }

    void aPeerCertificateSignedByAnotherKeyFails()
    {
        const CastTestPki &pki = sharedPki();
        for (const bool rsaKey : {false, true}) {
            const QByteArray peer = CastTestPki::peerCertificateDer(rsaKey, true);
            QVERIFY(!peer.isEmpty());
            const CastAuthVerdict verdict = verifyPeer(pki, peer);
            VERIFY_RESULT(verdict, CastAuthResult::BadSignature);
            QVERIFY2(verdict.detail.contains(QStringLiteral("not self-signed")),
                     qPrintable(describe(verdict)));
        }

        // The device's own certificate with the last byte of its signature changed still parses
        QByteArray tampered = pki.tlsCertificate().toDer();
        tampered.data()[tampered.size() - 1] ^= 0x01;
        const CastAuthVerdict verdict = verifyPeer(pki, tampered);
        VERIFY_RESULT(verdict, CastAuthResult::BadSignature);
        QVERIFY2(verdict.detail.contains(QStringLiteral("not self-signed")),
                 qPrintable(describe(verdict)));
    }

    void aCertificateThatCarriesARelayedDeviceFails()
    {
        // The signature field holds a nonce and the DER of a genuine device's TLS certificate, so the bytes that device signs for the challenge are the nonce followed by this certificate
        const CastTestPki &pki = sharedPki();
        const QByteArray relayed = pki.tlsCertificate().toDer();
        const QByteArray forged = CastTestPki::peerCertificateDer(false, false, kNonce + relayed);
        QVERIFY(!forged.isEmpty());
        QVERIFY(forged.endsWith(relayed));
        const CastAuthVerdict verdict = verifyPeer(pki, forged);
        VERIFY_RESULT(verdict, CastAuthResult::BadSignature);
        QVERIFY2(verdict.detail.contains(QStringLiteral("not self-signed")),
                 qPrintable(describe(verdict)));
    }

    void anUnparsablePeerCertificateFails()
    {
        const CastTestPki &pki = sharedPki();
        const QByteArray valid = pki.tlsCertificate().toDer();
        QByteArray truncated = valid;
        truncated.chop(5);
        const QByteArray trailing = valid + QByteArray("xyz");
        const QByteArray garbage("not a certificate");
        for (const QByteArray &peer : {garbage, truncated, trailing}) {
            const CastAuthVerdict verdict = verifyPeer(pki, peer);
            VERIFY_RESULT(verdict, CastAuthResult::BadSignature);
            QVERIFY2(verdict.detail.contains(QStringLiteral("does not parse")),
                     qPrintable(describe(verdict)));
        }
    }

    void aTamperedSignatureFails()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        CastAuthMessage tamperedSignature = message;
        tamperedSignature.response->signature.data()[10] ^= 0x01;
        VERIFY_RESULT(verify(pki, tamperedSignature), CastAuthResult::BadSignature);

        CastAuthMessage tamperedNonce = message;
        tamperedNonce.response->senderNonce.data()[3] ^= 0x01;
        const CastAuthVerdict verdict = verify(pki, tamperedNonce);
        VERIFY_RESULT(verdict, CastAuthResult::BadSignature);
        QVERIFY(hasWarning(verdict, QStringLiteral("nonce")));
    }

    void wrongKeyLeafSignatureFails()
    {
        const CastTestPki &pki = sharedPki();
        CastTestPki::AuthOptions options;
        options.wrongKeyLeafSignature = true;
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce, options), message));
        VERIFY_RESULT(verify(pki, message), CastAuthResult::BadSignature);
    }

    void anUnknownRootFails()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        const QByteArray peer = pki.tlsCertificate().toDer();

        const CastTestPki stranger;
        QVERIFY(stranger.isValid());
        VERIFY_RESULT(verifyDeviceAuth(stranger.trust(), kNonce, peer, message),
                      CastAuthResult::BadChain);

        const CastTrust google = CastTrust::google();
        QCOMPARE(google.rootCount(), 2);
        VERIFY_RESULT(verifyDeviceAuth(google, kNonce, peer, message), CastAuthResult::BadChain);

        CastTestPki::AuthOptions options;
        options.useOtherRootLeaf = true;
        QVERIFY(decode(pki.authResponseMessage(kNonce, options), message));
        VERIFY_RESULT(verify(pki, message), CastAuthResult::BadChain);
    }

    void aMissingIntermediateFails()
    {
        const CastTestPki &pki = sharedPki();
        CastTestPki::AuthOptions options;
        options.omitIntermediate = true;
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce, options), message));
        QVERIFY(message.response->intermediateCertificates.isEmpty());
        VERIFY_RESULT(verify(pki, message), CastAuthResult::BadChain);
    }

    void audioOnlyRefusedForVideo()
    {
        CastTestPki::Options options;
        options.audioOnly = true;
        const CastTestPki pki(options);
        QVERIFY(pki.isValid());
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        VERIFY_RESULT(verify(pki, message, true), CastAuthResult::AudioOnly);
        VERIFY_RESULT(verify(pki, message, false), CastAuthResult::Ok);
    }

    void a1024BitLeafFails()
    {
        CastTestPki::Options options;
        options.leafBits = 1024;
        const CastTestPki pki(options);
        if (!pki.isValid())
            QSKIP("this libcrypto does not generate 1024-bit RSA keys");
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        VERIFY_RESULT(verify(pki, message), CastAuthResult::WeakKey);
    }

    void aLeafWithoutSignatureUsageFails()
    {
        CastTestPki::Options options;
        options.leafWithoutSignatureUsage = true;
        const CastTestPki pki(options);
        QVERIFY(pki.isValid());
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        const CastAuthVerdict verdict = verify(pki, message);
        VERIFY_RESULT(verdict, CastAuthResult::BadChain);
        QVERIFY(verdict.detail.contains(QStringLiteral("digitalSignature")));
    }

    void pssRefused()
    {
        const CastTestPki &pki = sharedPki();
        CastTestPki::AuthOptions options;
        options.pss = true;
        const QByteArray bytes = pki.authResponseMessage(kNonce, options);
        if (bytes.isEmpty())
            QSKIP("this libcrypto cannot sign with RSA-PSS");
        CastAuthMessage message;
        QVERIFY(decode(bytes, message));
        QCOMPARE(message.response->signatureAlgorithm, 2);
        VERIFY_RESULT(verify(pki, message), CastAuthResult::UnsupportedAlgorithm);
    }

    void sha1HashStillVerifies()
    {
        const CastTestPki &pki = sharedPki();
        if (!pki.sha1SignaturesAvailable())
            QSKIP("this libcrypto refuses SHA-1 signatures");
        CastTestPki::AuthOptions options;
        options.sha1 = true;
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce, options), message));
        QCOMPARE(message.response->hashAlgorithm, 0);
        VERIFY_RESULT(verify(pki, message), CastAuthResult::Ok);
    }

    void nonceMismatchOnlyLogged()
    {
        const CastTestPki &pki = sharedPki();
        CastTestPki::AuthOptions options;
        options.nonceOverride = QByteArray("fedcba9876543210");
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce, options), message));
        QCOMPARE(message.response->senderNonce, options.nonceOverride);
        const CastAuthVerdict verdict = verify(pki, message);
        VERIFY_RESULT(verdict, CastAuthResult::Ok);
        QCOMPARE(verdict.warnings.size(), 1);
        QVERIFY(hasWarning(verdict, QStringLiteral("nonce")));
    }

    void expiredCertificatesStillVerify()
    {
        CastTestPki::Options options;
        options.expired = true;
        const CastTestPki pki(options);
        QVERIFY(pki.isValid());
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        const CastAuthVerdict verdict = verify(pki, message);
        VERIFY_RESULT(verdict, CastAuthResult::Ok);
        QVERIFY(hasWarning(verdict, QStringLiteral("the leaf certificate has expired")));
        QVERIFY(hasWarning(verdict, QStringLiteral("intermediate certificate 1 has expired")));
        QVERIFY(!hasWarning(verdict, QStringLiteral("peer")));
    }

    void aLongLivedPeerCertificateOnlyWarns()
    {
        CastTestPki::Options options;
        options.tlsDays = 10;
        const CastTestPki pki(options);
        QVERIFY(pki.isValid());
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        const CastAuthVerdict verdict = verify(pki, message);
        VERIFY_RESULT(verdict, CastAuthResult::Ok);
        QCOMPARE(verdict.warnings.size(), 1);
        QVERIFY(hasWarning(verdict, QStringLiteral("more than 4 days")));
    }

    void fourIntermediatesFail()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        CastAuthResponse response = *message.response;
        response.intermediateCertificates = QList<QByteArray>(4, pki.intermediateDer());

        CastAuthMessage ignored;
        QVERIFY(!decode(CastTestPki::encodeAuthResponse(response), ignored));

        CastAuthMessage handBuilt;
        handBuilt.response = response;
        VERIFY_RESULT(verify(pki, handBuilt), CastAuthResult::TooLarge);
    }

    void oversizedPartsFail()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));

        CastAuthMessage longSignature = message;
        longSignature.response->signature = QByteArray(kMaxCastAuthSignatureBytes + 1, 's');
        VERIFY_RESULT(verify(pki, longSignature), CastAuthResult::TooLarge);

        CastAuthMessage longLeaf = message;
        longLeaf.response->clientAuthCertificate = QByteArray(kMaxCastCertificateBytes + 1, 'c');
        VERIFY_RESULT(verify(pki, longLeaf), CastAuthResult::TooLarge);

        CastAuthMessage longIntermediate = message;
        longIntermediate.response->intermediateCertificates = {
            QByteArray(kMaxCastCertificateBytes + 1, 'i')};
        VERIFY_RESULT(verify(pki, longIntermediate), CastAuthResult::TooLarge);

        const QByteArray longPeer(kMaxCastCertificateBytes + 1, 'p');
        VERIFY_RESULT(verifyDeviceAuth(pki.trust(), kNonce, longPeer, message),
                      CastAuthResult::TooLarge);
    }

    void anAuthErrorFails()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(CastTestPki::authErrorMessage(2), message));
        QVERIFY(message.error);
        QVERIFY(!message.response);
        const CastAuthVerdict verdict = verify(pki, message);
        VERIFY_RESULT(verdict, CastAuthResult::DeviceError);
        QVERIFY(verdict.detail.contains(QStringLiteral("SIGNATURE_ALGORITHM_UNAVAILABLE")));

        QVERIFY(decode(CastTestPki::authErrorMessage(1), message));
        QVERIFY(verify(pki, message).detail.contains(QStringLiteral("NO_TLS")));
    }

    void noPeerCertificateFails()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        const CastAuthVerdict verdict =
            verifyDeviceAuth(pki.trust(), kNonce, QByteArray(), message);
        VERIFY_RESULT(verdict, CastAuthResult::NoPeerCertificate);
    }

    void emptyTrustFails()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));
        const QByteArray peer = pki.tlsCertificate().toDer();

        QCOMPARE(CastTrust().rootCount(), 0);
        VERIFY_RESULT(verifyDeviceAuth(CastTrust(), kNonce, peer, message),
                      CastAuthResult::NoTrust);

        const CastTrust garbage =
            CastTrust::fromDer({QByteArray("not a certificate"), QByteArray()});
        QCOMPARE(garbage.rootCount(), 0);
        VERIFY_RESULT(verifyDeviceAuth(garbage, kNonce, peer, message), CastAuthResult::NoTrust);
    }

    void truncatedLeafFails()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));

        CastAuthMessage truncated = message;
        truncated.response->clientAuthCertificate.chop(10);
        const CastAuthVerdict verdict = verify(pki, truncated);
        QVERIFY2(verdict.result == CastAuthResult::BadChain
                     || verdict.result == CastAuthResult::MalformedResponse,
                 qPrintable(describe(verdict)));

        CastAuthMessage trailing = message;
        trailing.response->clientAuthCertificate.append("xyz");
        VERIFY_RESULT(verify(pki, trailing), CastAuthResult::BadChain);

        CastAuthMessage noLeaf = message;
        noLeaf.response->clientAuthCertificate.clear();
        VERIFY_RESULT(verify(pki, noLeaf), CastAuthResult::MalformedResponse);
    }

    void unusableResponsesFail()
    {
        const CastTestPki &pki = sharedPki();
        CastAuthMessage message;
        QVERIFY(decode(pki.authResponseMessage(kNonce), message));

        VERIFY_RESULT(verify(pki, CastAuthMessage()), CastAuthResult::MalformedResponse);

        CastAuthMessage noSignature = message;
        noSignature.response->signature.clear();
        VERIFY_RESULT(verify(pki, noSignature), CastAuthResult::MalformedResponse);

        CastAuthMessage unknownHash = message;
        unknownHash.response->hashAlgorithm = 2;
        VERIFY_RESULT(verify(pki, unknownHash), CastAuthResult::UnsupportedAlgorithm);

        CastAuthMessage unknownSignature = message;
        unknownSignature.response->signatureAlgorithm = 0;
        VERIFY_RESULT(verify(pki, unknownSignature), CastAuthResult::UnsupportedAlgorithm);
    }

    void thePinnedRootsLoad()
    {
        const CastTrust google = CastTrust::google();
        QCOMPARE(google.rootCount(), 2);
        const QStringList paths{QStringLiteral(":/cast/cast_root_ca.der"),
                                QStringLiteral(":/cast/eureka_root_ca.der")};
        for (const QString &path : paths) {
            QFile file(path);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
            QCOMPARE(CastTrust::fromDer({file.readAll()}).rootCount(), 1);
        }
    }
};

QTEST_GUILESS_MAIN(TestCastDeviceAuth)
#include "test_cast_device_auth.moc"
