// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastDeviceAuth.h"

#include <QFile>
#include <QLoggingCategory>

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <ctime>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

Q_LOGGING_CATEGORY(deviceAuthLog, "kinefin.cast.auth")

// The macro declares the resource initializer in the enclosing namespace, so this stays at global scope
static void initCastRoots()
{
    Q_INIT_RESOURCE(cast_roots);
}

namespace Kinefin {
namespace {
constexpr int kMinLeafKeyBits = 2048;
constexpr int kMaxPeerLifetimeDays = 4;
constexpr const char *kAudioOnlyPolicyOid = "1.3.6.1.4.1.11129.2.5.2";

template <typename T, auto Free> struct Deleter {
    void operator()(T *object) const { Free(object); }
};

struct CertificateStackFree {
    void operator()(STACK_OF(X509) * stack) const { sk_X509_free(stack); }
};

using CertificatePtr = std::unique_ptr<X509, Deleter<X509, X509_free>>;
using CertificateStackPtr = std::unique_ptr<STACK_OF(X509), CertificateStackFree>;
using StoreContextPtr =
    std::unique_ptr<X509_STORE_CTX, Deleter<X509_STORE_CTX, X509_STORE_CTX_free>>;
using DigestContextPtr = std::unique_ptr<EVP_MD_CTX, Deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
using ObjectPtr = std::unique_ptr<ASN1_OBJECT, Deleter<ASN1_OBJECT, ASN1_OBJECT_free>>;
using PoliciesPtr =
    std::unique_ptr<CERTIFICATEPOLICIES, Deleter<CERTIFICATEPOLICIES, CERTIFICATEPOLICIES_free>>;

// OpenSSL keeps its errors in a per-thread queue: start from an empty one and leave none behind for the next user of the thread
struct ErrorQueueScope {
    ErrorQueueScope() { ERR_clear_error(); }
    ~ErrorQueueScope() { ERR_clear_error(); }
};

CertificatePtr parseCertificate(const QByteArray &der)
{
    if (der.isEmpty() || der.size() > std::numeric_limits<int>::max())
        return nullptr;
    const auto *begin = reinterpret_cast<const unsigned char *>(der.constData());
    const unsigned char *cursor = begin;
    CertificatePtr certificate(d2i_X509(nullptr, &cursor, static_cast<long>(der.size())));
    if (!certificate || cursor != begin + der.size())
        return nullptr;
    return certificate;
}

QString opensslError()
{
    const unsigned long code = ERR_peek_last_error();
    if (code == 0)
        return QStringLiteral("no OpenSSL error recorded");
    if (const char *reason = ERR_reason_error_string(code))
        return QString::fromLatin1(reason);
    char buffer[256];
    ERR_error_string_n(code, buffer, sizeof(buffer));
    return QString::fromLatin1(buffer);
}
} // namespace

struct CastTrust::Impl {
    std::shared_ptr<X509_STORE> store;
    int roots = 0;
};

struct CastTrustAccess {
    // Null when no root is loaded
    static X509_STORE *store(const CastTrust &trust)
    {
        if (!trust.m_impl || trust.m_impl->roots <= 0)
            return nullptr;
        return trust.m_impl->store.get();
    }
};

CastTrust::CastTrust() = default;

CastTrust::CastTrust(std::shared_ptr<const Impl> impl) : m_impl(std::move(impl)) {}

CastTrust CastTrust::google()
{
    initCastRoots();
    QList<QByteArray> roots;
    for (const QString &path :
         {QStringLiteral(":/cast/cast_root_ca.der"), QStringLiteral(":/cast/eureka_root_ca.der")}) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            roots.append(file.readAll());
    }
    return fromDer(roots);
}

CastTrust CastTrust::fromDer(const QList<QByteArray> &roots)
{
    // A new store has no lookup method: no system CA directory or bundle is ever consulted
    std::shared_ptr<X509_STORE> store(X509_STORE_new(), X509_STORE_free);
    if (!store)
        return CastTrust();
    int loaded = 0;
    for (const QByteArray &der : roots) {
        const CertificatePtr certificate = parseCertificate(der);
        if (certificate && X509_STORE_add_cert(store.get(), certificate.get()) == 1)
            ++loaded;
    }
    ERR_clear_error();
    if (loaded == 0)
        return CastTrust();
    auto impl = std::make_shared<Impl>();
    impl->store = std::move(store);
    impl->roots = loaded;
    return CastTrust(std::move(impl));
}

int CastTrust::rootCount() const
{
    return m_impl ? m_impl->roots : 0;
}

namespace {
CastAuthVerdict verdictOf(CastAuthResult result, const QString &detail)
{
    CastAuthVerdict verdict;
    verdict.result = result;
    verdict.detail = detail;
    return verdict;
}

const char *authErrorName(int errorType)
{
    switch (errorType) {
    case 0:
        return "INTERNAL_ERROR";
    case 1:
        return "NO_TLS";
    case 2:
        return "SIGNATURE_ALGORITHM_UNAVAILABLE";
    default:
        return "unknown";
    }
}

void warnOutsideValidity(const X509 *certificate, const QString &what, time_t now,
                         QStringList &warnings)
{
    const int notBeforeCmp = ASN1_TIME_cmp_time_t(X509_get0_notBefore(certificate), now);
    const int notAfterCmp = ASN1_TIME_cmp_time_t(X509_get0_notAfter(certificate), now);
    if (notBeforeCmp == -2 || notAfterCmp == -2)
        warnings.append(what + QStringLiteral(" has a validity period that does not parse"));
    else if (notBeforeCmp > 0)
        warnings.append(what + QStringLiteral(" is not valid yet"));
    else if (notAfterCmp < 0)
        warnings.append(what + QStringLiteral(" has expired"));
}

void warnLongLived(const X509 *certificate, QStringList &warnings)
{
    int days = 0;
    int seconds = 0;
    if (ASN1_TIME_diff(&days, &seconds, X509_get0_notBefore(certificate),
                       X509_get0_notAfter(certificate))
        != 1)
        return;
    if (days > kMaxPeerLifetimeDays || (days == kMaxPeerLifetimeDays && seconds > 0))
        warnings.append(QStringLiteral("the peer certificate is valid for more than %1 days")
                            .arg(kMaxPeerLifetimeDays));
}

enum class Policy { Absent, AudioOnly, Unreadable };

// An extension that is there and cannot be read counts as Unreadable, never as Absent: a parse failure must not hide the audio-only policy
Policy audioOnlyPolicy(const X509 *certificate, const ASN1_OBJECT *audioOnly)
{
    if (X509_get_ext_by_NID(certificate, NID_certificate_policies, -1) < 0)
        return Policy::Absent;
    const PoliciesPtr policies(static_cast<CERTIFICATEPOLICIES *>(
        X509_get_ext_d2i(certificate, NID_certificate_policies, nullptr, nullptr)));
    if (!policies)
        return Policy::Unreadable;
    for (int i = 0; i < sk_POLICYINFO_num(policies.get()); ++i) {
        const POLICYINFO *info = sk_POLICYINFO_value(policies.get(), i);
        if (info && info->policyid && OBJ_cmp(info->policyid, audioOnly) == 0)
            return Policy::AudioOnly;
    }
    return Policy::Absent;
}

bool signatureVerifies(EVP_PKEY *key, const EVP_MD *digest, const QByteArray &signature,
                       const QByteArray &data, QString &problem)
{
    const DigestContextPtr context(EVP_MD_CTX_new());
    EVP_PKEY_CTX *keyContext = nullptr;
    if (!context || EVP_DigestVerifyInit(context.get(), &keyContext, digest, nullptr, key) != 1
        || EVP_PKEY_CTX_set_rsa_padding(keyContext, RSA_PKCS1_PADDING) <= 0) {
        problem = QStringLiteral("the signature check could not start: ") + opensslError();
        return false;
    }
    const auto *signatureBytes = reinterpret_cast<const unsigned char *>(signature.constData());
    const auto *dataBytes = reinterpret_cast<const unsigned char *>(data.constData());
    const int result =
        EVP_DigestVerify(context.get(), signatureBytes, static_cast<size_t>(signature.size()),
                         dataBytes, static_cast<size_t>(data.size()));
    if (result != 1)
        problem = QStringLiteral("the signature does not verify");
    return result == 1;
}

// Empty when the response fits the caps; they are checked before any certificate is parsed
QString oversizeReason(const CastAuthResponse &response, const QByteArray &peerCertificateDer)
{
    if (response.signature.size() > kMaxCastAuthSignatureBytes)
        return QStringLiteral("the signature is over its cap");
    if (response.intermediateCertificates.size() > kMaxCastIntermediates)
        return QStringLiteral("the response has more than %1 intermediate certificates")
            .arg(kMaxCastIntermediates);
    bool certificatesFit = response.clientAuthCertificate.size() <= kMaxCastCertificateBytes
                           && peerCertificateDer.size() <= kMaxCastCertificateBytes;
    for (const QByteArray &der : response.intermediateCertificates)
        certificatesFit = certificatesFit && der.size() <= kMaxCastCertificateBytes;
    if (!certificatesFit)
        return QStringLiteral("a certificate is over its cap");
    return QString();
}

CastAuthVerdict runChecks(const CastTrust &trust, const QByteArray &nonce,
                          const QByteArray &peerCertificateDer, const CastAuthMessage &message,
                          bool requireVideo, QStringList &warnings)
{
    if (peerCertificateDer.isEmpty())
        return verdictOf(CastAuthResult::NoPeerCertificate,
                         QStringLiteral("the TLS socket has no peer certificate"));
    X509_STORE *store = CastTrustAccess::store(trust);
    if (!store)
        return verdictOf(CastAuthResult::NoTrust, QStringLiteral("no trust anchor is loaded"));
    if (message.error)
        return verdictOf(CastAuthResult::DeviceError,
                         QStringLiteral("the device answered with AuthError %1 (error_type %2)")
                             .arg(QLatin1StringView(authErrorName(message.error->errorType)))
                             .arg(message.error->errorType));
    if (!message.response)
        return verdictOf(CastAuthResult::MalformedResponse,
                         QStringLiteral("the reply has no AuthResponse"));

    const CastAuthResponse &response = *message.response;
    if (response.senderNonce != nonce)
        warnings.append(QStringLiteral("the nonce of the response differs from the challenge"));
    if (response.signatureAlgorithm != 1)
        return verdictOf(CastAuthResult::UnsupportedAlgorithm,
                         QStringLiteral("signature algorithm %1 is not RSASSA_PKCS1v15")
                             .arg(response.signatureAlgorithm));
    if (response.hashAlgorithm != 0 && response.hashAlgorithm != 1)
        return verdictOf(CastAuthResult::UnsupportedAlgorithm,
                         QStringLiteral("hash algorithm %1 is neither SHA1 nor SHA256")
                             .arg(response.hashAlgorithm));
    if (response.signature.isEmpty() || response.clientAuthCertificate.isEmpty())
        return verdictOf(CastAuthResult::MalformedResponse,
                         QStringLiteral("the response has no signature or no leaf certificate"));
    if (const QString reason = oversizeReason(response, peerCertificateDer); !reason.isEmpty())
        return verdictOf(CastAuthResult::TooLarge, reason);

    const CertificatePtr leaf = parseCertificate(response.clientAuthCertificate);
    if (!leaf)
        return verdictOf(CastAuthResult::BadChain,
                         QStringLiteral("the leaf certificate does not parse"));
    std::vector<CertificatePtr> intermediates;
    for (const QByteArray &der : response.intermediateCertificates) {
        CertificatePtr intermediate = parseCertificate(der);
        if (!intermediate)
            return verdictOf(CastAuthResult::BadChain,
                             QStringLiteral("intermediate certificate %1 does not parse")
                                 .arg(intermediates.size() + 1));
        intermediates.push_back(std::move(intermediate));
    }

    const time_t now = std::time(nullptr);
    warnOutsideValidity(leaf.get(), QStringLiteral("the leaf certificate"), now, warnings);
    for (size_t i = 0; i < intermediates.size(); ++i)
        warnOutsideValidity(intermediates[i].get(),
                            QStringLiteral("intermediate certificate %1").arg(i + 1), now,
                            warnings);

    // The signed data is the nonce followed by the peer DER with no length framing: only a self-signed peer proves its sender holds the TLS key, a certificate that carries a relayed device's DER in its signature field fails here
    const CertificatePtr peer = parseCertificate(peerCertificateDer);
    if (!peer)
        return verdictOf(CastAuthResult::BadSignature,
                         QStringLiteral("peer certificate does not parse"));
    EVP_PKEY *peerKey = X509_get0_pubkey(peer.get());
    if (!peerKey || X509_verify(peer.get(), peerKey) != 1)
        return verdictOf(CastAuthResult::BadSignature,
                         QStringLiteral("peer certificate is not self-signed"));
    warnOutsideValidity(peer.get(), QStringLiteral("the peer certificate"), now, warnings);
    warnLongLived(peer.get(), warnings);

    EVP_PKEY *key = X509_get0_pubkey(leaf.get());
    if (!key || !EVP_PKEY_is_a(key, "RSA") || EVP_PKEY_get_bits(key) < kMinLeafKeyBits)
        return verdictOf(
            CastAuthResult::WeakKey,
            QStringLiteral("the leaf key is not RSA of at least %1 bits").arg(kMinLeafKeyBits));
    if ((X509_get_key_usage(leaf.get()) & KU_DIGITAL_SIGNATURE) == 0)
        return verdictOf(CastAuthResult::BadChain,
                         QStringLiteral("the key usage of the leaf lacks digitalSignature"));

    const CertificateStackPtr untrusted(sk_X509_new_null());
    const StoreContextPtr context(X509_STORE_CTX_new());
    bool ready = untrusted && context;
    for (const CertificatePtr &intermediate : intermediates)
        ready = ready && sk_X509_push(untrusted.get(), intermediate.get()) != 0;
    if (!ready || X509_STORE_CTX_init(context.get(), store, leaf.get(), untrusted.get()) != 1
        || X509_STORE_CTX_set_purpose(context.get(), X509_PURPOSE_SSL_CLIENT) != 1)
        return verdictOf(CastAuthResult::BadChain,
                         QStringLiteral("the chain check could not start: ") + opensslError());
    // Times are not checked: Google's intermediate certificate expired on 2025-03-09 and broke Google's own apps, while the pinned roots run to 2032 (Eureka) and 2034 (Cast)
    X509_VERIFY_PARAM_set_flags(X509_STORE_CTX_get0_param(context.get()),
                                X509_V_FLAG_NO_CHECK_TIME);
    // auth_level stays at its default of -1, which skips the key and signature strength checks: SHA-1 signed chains verify unless the build itself refuses SHA-1 signatures
    if (X509_verify_cert(context.get()) != 1) {
        const int error = X509_STORE_CTX_get_error(context.get());
        return verdictOf(CastAuthResult::BadChain,
                         QStringLiteral("the chain does not verify at depth %1: %2")
                             .arg(X509_STORE_CTX_get_error_depth(context.get()))
                             .arg(QString::fromLatin1(X509_verify_cert_error_string(error))));
    }

    const ObjectPtr audioOnlyOid(OBJ_txt2obj(kAudioOnlyPolicyOid, 1));
    STACK_OF(X509) *chain = X509_STORE_CTX_get0_chain(context.get());
    if (!audioOnlyOid || !chain)
        return verdictOf(CastAuthResult::BadChain, QStringLiteral("the verified chain is missing"));
    bool audioOnly = false;
    for (int i = 0; i < sk_X509_num(chain); ++i) {
        const Policy policy = audioOnlyPolicy(sk_X509_value(chain, i), audioOnlyOid.get());
        if (policy == Policy::Unreadable)
            return verdictOf(CastAuthResult::BadChain,
                             QStringLiteral("the certificate policies of the chain do not parse"));
        audioOnly = audioOnly || policy == Policy::AudioOnly;
    }
    if (audioOnly && requireVideo)
        return verdictOf(
            CastAuthResult::AudioOnly,
            QStringLiteral("the chain carries the audio-only policy and video is required"));

    QString problem;
    const EVP_MD *digest = response.hashAlgorithm == 1 ? EVP_sha256() : EVP_sha1();
    const QByteArray signedData = response.senderNonce + peerCertificateDer;
    if (!signatureVerifies(key, digest, response.signature, signedData, problem))
        return verdictOf(CastAuthResult::BadSignature, problem);

    return verdictOf(CastAuthResult::Ok, audioOnly ? QStringLiteral("authenticated, audio only")
                                                   : QStringLiteral("authenticated"));
}
} // namespace

CastAuthVerdict verifyDeviceAuth(const CastTrust &trust, const QByteArray &nonce,
                                 const QByteArray &peerCertificateDer,
                                 const CastAuthMessage &message, bool requireVideo)
{
    const ErrorQueueScope errorQueue;
    QStringList warnings;
    CastAuthVerdict verdict =
        runChecks(trust, nonce, peerCertificateDer, message, requireVideo, warnings);
    verdict.warnings = std::move(warnings);
    qCDebug(deviceAuthLog) << "device authentication result" << static_cast<int>(verdict.result)
                           << verdict.detail;
    for (const QString &warning : std::as_const(verdict.warnings))
        qCDebug(deviceAuthLog) << "device authentication warning" << warning;
    return verdict;
}

} // namespace Kinefin
