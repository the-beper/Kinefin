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
} // namespace Kinefin
