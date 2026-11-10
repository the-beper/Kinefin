// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "LinkRouter.h"

#include <QRegularExpression>
#include <QUrl>

namespace Kinefin {
std::optional<QString> parseItemLink(const QString &input)
{
    const QUrl url(input, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QStringLiteral("jellyfin")
        || url.host() != QStringLiteral("item") || !url.userInfo().isEmpty()
        || !url.query().isEmpty() || !url.fragment().isEmpty() || url.port() != -1) {
        return std::nullopt;
    }
    const QString path = url.path();
    static const QRegularExpression idPattern(QStringLiteral("^/[a-fA-F0-9]{32}$"));
    if (!idPattern.match(path).hasMatch()) {
        return std::nullopt;
    }
    return path.mid(1).toLower();
}

std::optional<QUrl> parseWebLink(const QString &input)
{
    const QUrl url(input, QUrl::StrictMode);
    const QString scheme = url.scheme();
    if (!url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        return std::nullopt;
    }
    return url;
}

std::optional<QUrl> adminDashboardUrl(const QString &baseUrl)
{
    auto link = parseWebLink(baseUrl);
    if (!link)
        return std::nullopt;
    QString path = link->path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (path.endsWith(QLatin1String("/web"), Qt::CaseInsensitive))
        path.chop(4);
    link->setPath(path + QStringLiteral("/web/"));
    link->setQuery(QString());
    link->setFragment(QStringLiteral("/dashboard"));
    return link;
}
} // namespace Kinefin
