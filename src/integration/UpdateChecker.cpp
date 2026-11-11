// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/UpdateChecker.h"

#include <QCoreApplication>
#include <QList>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>

namespace Kinefin {
namespace {
const QUrl kLatestReleaseUrl(
    QStringLiteral("https://github.com/the-beper/Kinefin/releases/latest"));
const QString kLatestSuffix = QStringLiteral("/releases/latest");
const QString kTagPrefix = QStringLiteral("/releases/tag/");

std::optional<QList<int>> components(const QString &version)
{
    // GitHub's final tags use digits, dots and numeric revision separators.
    // Reject labels (e.g. -rc1), leading v, empty components and excessive
    // numbers instead of trusting QVersionNumber's partial parses.
    static const QRegularExpression pattern(QStringLiteral("^[0-9]+(?:[.-][0-9]+)*$"));
    if (version.size() > 48 || !pattern.match(version).hasMatch())
        return std::nullopt;

    const QStringList fields = version.split(QRegularExpression(QStringLiteral("[.-]")));
    if (fields.isEmpty() || fields.size() > 10)
        return std::nullopt;
    QList<int> result;
    result.reserve(fields.size());
    for (const QString &field : fields) {
        if (field.size() > 9)
            return std::nullopt;
        bool ok = false;
        const int value = field.toInt(&ok);
        if (!ok)
            return std::nullopt;
        result.append(value);
    }
    return result;
}

bool isRedirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : UpdateChecker(QCoreApplication::applicationVersion(), kLatestReleaseUrl, parent)
{
}

UpdateChecker::UpdateChecker(const QString &currentVersion, const QUrl &latestUrl, QObject *parent)
    : QObject(parent), m_currentVersion(currentVersion), m_latestUrl(latestUrl)
{
}

UpdateChecker::~UpdateChecker()
{
    cancel();
}

std::optional<int> UpdateChecker::compareVersions(const QString &current,
                                                   const QString &candidate)
{
    const auto left = components(current);
    const auto right = components(candidate);
    if (!left || !right)
        return std::nullopt;
    for (qsizetype i = 0; i < qMax(left->size(), right->size()); ++i) {
        const int a = i < left->size() ? left->at(i) : 0;
        const int b = i < right->size() ? right->at(i) : 0;
        if (a != b)
            return a < b ? 1 : -1;
    }
    return 0;
}

QUrl UpdateChecker::releaseFromRedirect(const QUrl &target) const
{
    if (!target.isValid() || target.isEmpty() || !m_latestUrl.isValid()
        || !m_latestUrl.path().endsWith(kLatestSuffix))
        return {};
    // Do not accept dot-segment normalization hidden in the redirect target.
    // A path that contains traversal is not a canonical release URL.
    const QString rawPath = target.path(QUrl::FullyEncoded);
    for (const QString &segment : rawPath.split(QLatin1Char('/'))) {
        const QString decoded = QUrl::fromPercentEncoding(segment.toLatin1());
        if (decoded == QStringLiteral(".") || decoded == QStringLiteral(".."))
            return {};
    }
    const QUrl url = m_latestUrl.resolved(target);
    if (!url.isValid() || url.scheme() != m_latestUrl.scheme()
        || url.host() != m_latestUrl.host() || url.port() != m_latestUrl.port()
        || !url.userName().isEmpty() || !url.password().isEmpty() || url.hasQuery()
        || url.hasFragment())
        return {};

    const QString expected = m_latestUrl.path().chopped(kLatestSuffix.size()) + kTagPrefix;
    const QString path = url.path();
    if (!path.startsWith(expected))
        return {};
    const QString tag = path.mid(expected.size());
    // release.yml publishes v<version> tags. A bare numeric tag is also
    // accepted for older releases; the version used for comparison has no v.
    const QString version = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
    // A URL may have an encoded slash/dot. Require the original path to be
    // canonical as well as its decoded value before allowing the browser link.
    if (tag.isEmpty() || !components(version)
        || url.path(QUrl::FullyEncoded) != expected + tag)
        return {};
    return url;
}

bool UpdateChecker::check()
{
    if (checking())
        return false;
    if (!components(m_currentVersion) || !m_latestUrl.isValid()
        || !m_latestUrl.path().endsWith(kLatestSuffix)
        || (m_latestUrl.scheme() != QStringLiteral("https")
            && !(m_latestUrl.scheme() == QStringLiteral("http")
                 && (m_latestUrl.host() == QStringLiteral("127.0.0.1")
                     || m_latestUrl.host() == QStringLiteral("localhost"))))) {
        Q_EMIT checkFailed();
        return false;
    }

    QNetworkRequest request(m_latestUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    request.setRawHeader("Accept", "text/html");
    request.setRawHeader("User-Agent", "Kinefin-update-check");
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    Q_EMIT checkingChanged();
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_reply != reply)
            return;
        m_reply = nullptr;
        Q_EMIT checkingChanged();

        // Never follow a 30x to an arbitrary host or parse an HTTP 200 HTML
        // page. Only the redirect path carries the version we trust.
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QUrl target = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const QUrl release = releaseFromRedirect(target);
        const bool valid = reply->error() == QNetworkReply::NoError && isRedirect(status)
                           && !release.isEmpty();
        reply->deleteLater();
        if (!valid) {
            Q_EMIT checkFailed();
            return;
        }
        const QString tag = release.path().mid(
            m_latestUrl.path().chopped(kLatestSuffix.size()).size() + kTagPrefix.size());
        const QString version = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
        const auto comparison = compareVersions(m_currentVersion, version);
        if (!comparison) {
            Q_EMIT checkFailed();
        } else if (*comparison > 0) {
            Q_EMIT updateAvailable(version, release);
        } else {
            Q_EMIT upToDate();
        }
    });
    return true;
}

void UpdateChecker::cancel()
{
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    Q_EMIT checkingChanged();
}

} // namespace Kinefin
