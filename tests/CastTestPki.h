// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// A throwaway Cast PKI generated in memory with libcrypto (root, intermediate, leaf, all SHA-256) and the TLS identity of a fake device, plus the DeviceAuthMessage bytes a device would send. Header-only; the tests that include it link OpenSSL::Crypto
#pragma once

#include "cast/CastDeviceAuth.h"
#include "cast/CastProto.h"

#include <QByteArray>
#include <QList>
#include <QSslCertificate>
#include <QSslKey>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <ctime>
#include <memory>

namespace CastTestPkiDetail {

template <typename T, auto Free> struct Deleter {
    void operator()(T *object) const { Free(object); }
};

using KeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using CertPtr = std::unique_ptr<X509, Deleter<X509, X509_free>>;
using NamePtr = std::unique_ptr<X509_NAME, Deleter<X509_NAME, X509_NAME_free>>;
using ExtensionPtr = std::unique_ptr<X509_EXTENSION, Deleter<X509_EXTENSION, X509_EXTENSION_free>>;
using DigestContextPtr = std::unique_ptr<EVP_MD_CTX, Deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
using BioPtr = std::unique_ptr<BIO, Deleter<BIO, BIO_free_all>>;

inline KeyPtr generateRsa(int bits)
{
    return KeyPtr(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<size_t>(bits)));
}

inline KeyPtr generateEc()
{
    return KeyPtr(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
}

inline QByteArray toDer(const X509 *certificate)
{
    unsigned char *der = nullptr;
    const int size = i2d_X509(certificate, &der);
    if (size <= 0)
        return QByteArray();
    QByteArray out(reinterpret_cast<const char *>(der), size);
    OPENSSL_free(der);
    return out;
}

struct CertSpec {
    QByteArray commonName;
    EVP_PKEY *subjectKey = nullptr;
    // Null: self-signed with subjectKey
    X509 *issuer = nullptr;
    EVP_PKEY *issuerKey = nullptr;
    long serial = 1;
    // Seconds from now
    long notBefore = 0;
    long notAfter = 0;
    const char *basicConstraints = nullptr;
    const char *keyUsage = nullptr;
    const char *policies = nullptr;
};

inline bool addExtension(X509 *certificate, X509 *issuer, int nid, const char *value)
{
    X509V3_CTX context;
    X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    const ExtensionPtr extension(X509V3_EXT_nconf_nid(nullptr, &context, nid, value));
    return extension && X509_add_ext(certificate, extension.get(), -1) == 1;
}

// The config-string form of the policies extension needs a config database that a bare context has not, so the structure is built by hand
inline bool addPolicy(X509 *certificate, const char *oid)
{
    CERTIFICATEPOLICIES *policies = sk_POLICYINFO_new_null();
    POLICYINFO *info = POLICYINFO_new();
    if (!policies || !info) {
        POLICYINFO_free(info);
        CERTIFICATEPOLICIES_free(policies);
        return false;
    }
    ASN1_OBJECT_free(info->policyid);
    info->policyid = OBJ_txt2obj(oid, 1);
    if (!info->policyid || sk_POLICYINFO_push(policies, info) <= 0) {
        POLICYINFO_free(info);
        CERTIFICATEPOLICIES_free(policies);
        return false;
    }
    const int added =
        X509_add1_ext_i2d(certificate, NID_certificate_policies, policies, 0, X509V3_ADD_DEFAULT);
    CERTIFICATEPOLICIES_free(policies);
    return added == 1;
}

inline CertPtr issue(const CertSpec &spec, std::time_t now)
{
    CertPtr certificate(X509_new());
    NamePtr subject(X509_NAME_new());
    if (!certificate || !subject
        || X509_NAME_add_entry_by_txt(
               subject.get(), "CN", MBSTRING_ASC,
               reinterpret_cast<const unsigned char *>(spec.commonName.constData()),
               static_cast<int>(spec.commonName.size()), -1, 0)
               != 1)
        return CertPtr();
    X509 *issuerCertificate = spec.issuer ? spec.issuer : certificate.get();
    const X509_NAME *issuerName = spec.issuer ? X509_get_subject_name(spec.issuer) : subject.get();
    EVP_PKEY *signingKey = spec.issuerKey ? spec.issuerKey : spec.subjectKey;
    if (X509_set_version(certificate.get(), X509_VERSION_3) != 1
        || ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), spec.serial) != 1
        || !X509_time_adj(X509_getm_notBefore(certificate.get()), spec.notBefore, &now)
        || !X509_time_adj(X509_getm_notAfter(certificate.get()), spec.notAfter, &now)
        || X509_set_subject_name(certificate.get(), subject.get()) != 1
        || X509_set_issuer_name(certificate.get(), issuerName) != 1
        || X509_set_pubkey(certificate.get(), spec.subjectKey) != 1)
        return CertPtr();
    if (spec.basicConstraints
        && !addExtension(certificate.get(), issuerCertificate, NID_basic_constraints,
                         spec.basicConstraints))
        return CertPtr();
    if (spec.keyUsage
        && !addExtension(certificate.get(), issuerCertificate, NID_key_usage, spec.keyUsage))
        return CertPtr();
    if (spec.policies && !addPolicy(certificate.get(), spec.policies))
        return CertPtr();
    if (X509_sign(certificate.get(), signingKey, EVP_sha256()) <= 0)
        return CertPtr();
    return certificate;
}

// RSA PKCS#1 v1.5, or PSS when `pss`; empty when the library refuses the digest or the padding
inline QByteArray sign(EVP_PKEY *key, const EVP_MD *digest, bool pss, const QByteArray &data)
{
    const DigestContextPtr context(EVP_MD_CTX_new());
    EVP_PKEY_CTX *keyContext = nullptr;
    if (!context || !key
        || EVP_DigestSignInit(context.get(), &keyContext, digest, nullptr, key) != 1)
        return QByteArray();
    if (EVP_PKEY_CTX_set_rsa_padding(keyContext, pss ? RSA_PKCS1_PSS_PADDING : RSA_PKCS1_PADDING)
        <= 0)
        return QByteArray();
    if (pss && EVP_PKEY_CTX_set_rsa_pss_saltlen(keyContext, RSA_PSS_SALTLEN_DIGEST) <= 0)
        return QByteArray();
    const auto *bytes = reinterpret_cast<const unsigned char *>(data.constData());
    size_t length = 0;
    if (EVP_DigestSign(context.get(), nullptr, &length, bytes, static_cast<size_t>(data.size()))
        != 1)
        return QByteArray();
    QByteArray signature(static_cast<qsizetype>(length), Qt::Uninitialized);
    if (EVP_DigestSign(context.get(), reinterpret_cast<unsigned char *>(signature.data()), &length,
                       bytes, static_cast<size_t>(data.size()))
        != 1)
        return QByteArray();
    signature.resize(static_cast<qsizetype>(length));
    return signature;
}

inline bool verifies(EVP_PKEY *key, const EVP_MD *digest, const QByteArray &signature,
                     const QByteArray &data)
{
    const DigestContextPtr context(EVP_MD_CTX_new());
    EVP_PKEY_CTX *keyContext = nullptr;
    if (!context || !key
        || EVP_DigestVerifyInit(context.get(), &keyContext, digest, nullptr, key) != 1)
        return false;
    return EVP_DigestVerify(context.get(),
                            reinterpret_cast<const unsigned char *>(signature.constData()),
                            static_cast<size_t>(signature.size()),
                            reinterpret_cast<const unsigned char *>(data.constData()),
                            static_cast<size_t>(data.size()))
           == 1;
}

inline void putVarint(QByteArray &out, quint64 value)
{
    while (value >= 0x80) {
        out.append(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.append(static_cast<char>(value));
}

inline void putBytesField(QByteArray &out, int field, const QByteArray &bytes)
{
    putVarint(out, (static_cast<quint64>(field) << 3) | 2);
    putVarint(out, static_cast<quint64>(bytes.size()));
    out.append(bytes);
}

inline void putVarintField(QByteArray &out, int field, quint64 value)
{
    putVarint(out, static_cast<quint64>(field) << 3);
    putVarint(out, value);
}

} // namespace CastTestPkiDetail

class CastTestPki {
  public:
    struct Options {
        // The policy 1.3.6.1.4.1.11129.2.5.2 in the leaf
        bool audioOnly = false;
        int leafBits = 2048;
        // The root, the intermediate and the leaf ended 200 days ago (they started 400 days ago)
        bool expired = false;
        // The leaf key usage is keyEncipherment only
        bool leafWithoutSignatureUsage = false;
        // How long the TLS certificate of the fake device lives, starting an hour ago
        int tlsDays = 2;
    };

    struct AuthOptions {
        bool sha1 = false;
        bool pss = false;
        // The signature covers the nonce and the root's DER instead of the TLS certificate
        bool signOverOtherPeer = false;
        // The signature covers the nonce and these bytes instead of the TLS certificate: the certificate a test then hands to the verifier as the peer
        QByteArray signedPeerOverride;
        bool omitIntermediate = false;
        // The response echoes this nonce (and signs with it) instead of the challenge's
        QByteArray nonceOverride;
        // The chain and the key come from a second, unrelated PKI
        bool useOtherRootLeaf = false;
        // The signature is made with the intermediate's key instead of the leaf's
        bool wrongKeyLeafSignature = false;
    };

    CastTestPki() : CastTestPki(Options{}) {}
    explicit CastTestPki(const Options &options) { generate(options); }

    // False when libcrypto could not generate something: every accessor is then empty
    bool isValid() const { return m_valid; }

    QByteArray rootDer() const { return m_rootDer; }
    QByteArray intermediateDer() const { return m_intermediateDer; }
    QByteArray leafDer() const { return m_leafDer; }
    Kinefin::CastTrust trust() const { return Kinefin::CastTrust::fromDer({m_rootDer}); }

    // The self-signed certificate and key a fake device uses for TLS
    QSslCertificate tlsCertificate() const { return QSslCertificate(m_tlsDer, QSsl::Der); }
    QSslKey tlsKey() const { return QSslKey(m_tlsKeyPem, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey); }

    // Whether this libcrypto signs and verifies with SHA-1 at all: some distributions disable it
    bool sha1SignaturesAvailable() const
    {
        using namespace CastTestPkiDetail;
        const QByteArray probe("sha-1 probe");
        const QByteArray signature = sign(m_leafKey.get(), EVP_sha1(), false, probe);
        return !signature.isEmpty() && verifies(m_leafKey.get(), EVP_sha1(), signature, probe);
    }

    // The bytes of the DeviceAuthMessage a device sends for `challengeNonce`; empty when libcrypto refuses to sign (SHA-1 or PSS on a restrictive build)
    QByteArray authResponseMessage(const QByteArray &challengeNonce) const
    {
        return authResponseMessage(challengeNonce, AuthOptions{});
    }

    QByteArray authResponseMessage(const QByteArray &challengeNonce,
                                   const AuthOptions &options) const
    {
        using namespace CastTestPkiDetail;
        if (!m_valid)
            return QByteArray();
        std::unique_ptr<CastTestPki> other;
        if (options.useOtherRootLeaf) {
            other = std::make_unique<CastTestPki>();
            if (!other->m_valid)
                return QByteArray();
        }
        const CastTestPki &signer = other ? *other : *this;
        const QByteArray nonce =
            options.nonceOverride.isEmpty() ? challengeNonce : options.nonceOverride;
        QByteArray signedPeer = options.signOverOtherPeer ? m_rootDer : m_tlsDer;
        if (!options.signedPeerOverride.isEmpty())
            signedPeer = options.signedPeerOverride;
        EVP_PKEY *key =
            options.wrongKeyLeafSignature ? signer.m_intermediateKey.get() : signer.m_leafKey.get();
        const QByteArray signature =
            sign(key, options.sha1 ? EVP_sha1() : EVP_sha256(), options.pss, nonce + signedPeer);
        if (signature.isEmpty())
            return QByteArray();

        Kinefin::CastAuthResponse response;
        response.signature = signature;
        response.clientAuthCertificate = signer.m_leafDer;
        if (!options.omitIntermediate)
            response.intermediateCertificates.append(signer.m_intermediateDer);
        response.signatureAlgorithm = options.pss ? 2 : 1;
        response.senderNonce = nonce;
        response.hashAlgorithm = options.sha1 ? 0 : 1;
        return encodeAuthResponse(response);
    }

    // DeviceAuthMessage{response{...}} with exactly the fields of `response` (all of them, including the algorithms), for the cases a real device would not produce
    static QByteArray encodeAuthResponse(const Kinefin::CastAuthResponse &response)
    {
        using namespace CastTestPkiDetail;
        QByteArray body;
        putBytesField(body, 1, response.signature);
        putBytesField(body, 2, response.clientAuthCertificate);
        for (const QByteArray &intermediate : response.intermediateCertificates)
            putBytesField(body, 3, intermediate);
        putVarintField(body, 4, static_cast<quint64>(response.signatureAlgorithm));
        if (!response.senderNonce.isEmpty())
            putBytesField(body, 5, response.senderNonce);
        putVarintField(body, 6, static_cast<quint64>(response.hashAlgorithm));
        QByteArray message;
        putBytesField(message, 2, body);
        return message;
    }

    // A TLS certificate for a fake device (two days, starting an hour ago) with a fresh EC or RSA key that is discarded. Self-signed by default, signed by a second key when `signedByOtherKey`; a non-empty `signatureField` replaces the signature bytes with them. Empty when libcrypto refuses
    static QByteArray peerCertificateDer(bool rsaKey, bool signedByOtherKey = false,
                                         const QByteArray &signatureField = QByteArray())
    {
        using namespace CastTestPkiDetail;
        KeyPtr key = rsaKey ? generateRsa(2048) : generateEc();
        KeyPtr otherKey = signedByOtherKey ? generateEc() : KeyPtr();
        if (!key || (signedByOtherKey && !otherKey))
            return QByteArray();
        CertSpec spec;
        spec.commonName = "Kinefin Test Peer";
        spec.subjectKey = key.get();
        spec.issuerKey = otherKey.get();
        spec.serial = 5;
        spec.notBefore = -3600;
        spec.notAfter = 2L * 24 * 60 * 60 - 3600;
        const CertPtr certificate = issue(spec, std::time(nullptr));
        if (!certificate)
            return QByteArray();
        if (!signatureField.isEmpty()) {
            // The signature lives inside the certificate object: it is overwritten there (the unused-bits flag set by signing stays at zero) and the certificate encoded again
            const ASN1_BIT_STRING *signature = nullptr;
            X509_get0_signature(&signature, nullptr, certificate.get());
            auto *editable = const_cast<ASN1_BIT_STRING *>(signature);
            if (!editable
                || ASN1_STRING_set(editable, signatureField.constData(),
                                   static_cast<int>(signatureField.size()))
                       != 1)
                return QByteArray();
        }
        return toDer(certificate.get());
    }

    // DeviceAuthMessage{error{error_type}}
    static QByteArray authErrorMessage(int errorType)
    {
        using namespace CastTestPkiDetail;
        QByteArray error;
        putVarintField(error, 1, static_cast<quint64>(errorType));
        QByteArray message;
        putBytesField(message, 3, error);
        return message;
    }

  private:
    void generate(const Options &options)
    {
        using namespace CastTestPkiDetail;
        static std::atomic<int> counter{0};
        const QByteArray tag = QByteArray::number(++counter);

        KeyPtr rootKey = generateRsa(2048);
        m_intermediateKey = generateRsa(2048);
        m_leafKey = generateRsa(options.leafBits);
        KeyPtr tlsKey = generateEc();
        if (!rootKey || !m_intermediateKey || !m_leafKey || !tlsKey)
            return;

        // One reading of the clock, so the validity of every certificate is exact
        const std::time_t now = std::time(nullptr);
        const long day = 24L * 60 * 60;
        const long notBefore = options.expired ? -400 * day : -3600;
        const long notAfter = options.expired ? -200 * day : 365 * day;

        CertSpec rootSpec;
        rootSpec.commonName = "Kinefin Test Root " + tag;
        rootSpec.subjectKey = rootKey.get();
        rootSpec.serial = 1;
        rootSpec.notBefore = notBefore;
        rootSpec.notAfter = notAfter;
        rootSpec.basicConstraints = "critical,CA:TRUE,pathlen:2";
        rootSpec.keyUsage = "critical,keyCertSign,cRLSign";
        const CertPtr root = issue(rootSpec, now);
        if (!root)
            return;

        CertSpec intermediateSpec;
        intermediateSpec.commonName = "Kinefin Test Intermediate " + tag;
        intermediateSpec.subjectKey = m_intermediateKey.get();
        intermediateSpec.issuer = root.get();
        intermediateSpec.issuerKey = rootKey.get();
        intermediateSpec.serial = 2;
        intermediateSpec.notBefore = notBefore;
        intermediateSpec.notAfter = notAfter;
        intermediateSpec.basicConstraints = "critical,CA:TRUE,pathlen:0";
        intermediateSpec.keyUsage = "critical,keyCertSign,cRLSign";
        const CertPtr intermediate = issue(intermediateSpec, now);
        if (!intermediate)
            return;

        CertSpec leafSpec;
        leafSpec.commonName = "Kinefin Test Device " + tag;
        leafSpec.subjectKey = m_leafKey.get();
        leafSpec.issuer = intermediate.get();
        leafSpec.issuerKey = m_intermediateKey.get();
        leafSpec.serial = 3;
        leafSpec.notBefore = notBefore;
        leafSpec.notAfter = notAfter;
        leafSpec.basicConstraints = "CA:FALSE";
        leafSpec.keyUsage = options.leafWithoutSignatureUsage ? "critical,keyEncipherment"
                                                              : "critical,digitalSignature";
        if (options.audioOnly)
            leafSpec.policies = "1.3.6.1.4.1.11129.2.5.2";
        const CertPtr leaf = issue(leafSpec, now);
        if (!leaf)
            return;

        CertSpec tlsSpec;
        tlsSpec.commonName = "Kinefin Test TLS " + tag;
        tlsSpec.subjectKey = tlsKey.get();
        tlsSpec.serial = 4;
        tlsSpec.notBefore = -3600;
        tlsSpec.notAfter = options.tlsDays * day - 3600;
        const CertPtr tls = issue(tlsSpec, now);
        if (!tls)
            return;

        const BioPtr pem(BIO_new(BIO_s_mem()));
        if (!pem
            || PEM_write_bio_PKCS8PrivateKey(pem.get(), tlsKey.get(), nullptr, nullptr, 0, nullptr,
                                             nullptr)
                   != 1)
            return;
        char *pemData = nullptr;
        const long pemSize = BIO_get_mem_data(pem.get(), &pemData);
        if (pemSize <= 0 || !pemData)
            return;
        m_tlsKeyPem = QByteArray(pemData, static_cast<qsizetype>(pemSize));

        m_rootDer = toDer(root.get());
        m_intermediateDer = toDer(intermediate.get());
        m_leafDer = toDer(leaf.get());
        m_tlsDer = toDer(tls.get());
        m_valid = !m_rootDer.isEmpty() && !m_intermediateDer.isEmpty() && !m_leafDer.isEmpty()
                  && !m_tlsDer.isEmpty();
    }

    bool m_valid = false;
    QByteArray m_rootDer;
    QByteArray m_intermediateDer;
    QByteArray m_leafDer;
    QByteArray m_tlsDer;
    QByteArray m_tlsKeyPem;
    CastTestPkiDetail::KeyPtr m_intermediateKey;
    CastTestPkiDetail::KeyPtr m_leafKey;
};
