// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "LinkRouter.h"

#include <QRegularExpression>
#include <QUrl>

namespace Kinefin {
std::optional<QString> parseItemLink(const QString &input)
{
    // Match the source text, not a decoded/normalized QUrl path: percent escapes,
    // dot segments, a trailing slash, and attached userinfo must all be refused.
    static const QRegularExpression pattern(QStringLiteral(R"(\Ajellyfin://item/([a-f0-9]{32})\z)"),
                                            QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(input);
    if (!match.hasMatch())
        return std::nullopt;
    return match.captured(1).toLower();
}

std::optional<QString> itemFromActivationArguments(const QStringList &arguments)
{
    // Those switches take a value as a separate argv entry when not written as
    // --name=value. An option value is never a desktop link, even if it looks
    // like one; only a genuine positional argument can navigate.
    static const QStringList valueOptions{
        QStringLiteral("--profile"), QStringLiteral("--log-level"),
        QStringLiteral("--create-profile"), QStringLiteral("--set-default-profile"),
        QStringLiteral("--delete-profile")};
    bool skipValue = false;
    bool positionalOnly = false;
    std::optional<QString> found;
    for (const auto &argument : arguments) {
        if (skipValue) {
            skipValue = false;
            continue;
        }
        if (!positionalOnly && argument == QLatin1String("--")) {
            positionalOnly = true;
            continue;
        }
        if (!positionalOnly && argument.startsWith(QLatin1Char('-'))) {
            skipValue = valueOptions.contains(argument);
            continue;
        }
        const auto item = parseItemLink(argument);
        if (!item)
            continue;
        // A forwarded activation must not navigate to two arbitrary items.
        if (found)
            return std::nullopt;
        found = item;
    }
    return found;
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
