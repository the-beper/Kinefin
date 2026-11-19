// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>
#include <optional>

namespace Kinefin {
// The only accepted desktop link: jellyfin://item/<32-hex-id>, no credentials, queries, fragments, redirects or percent-encoded alternatives. Returns a lowercase ID
std::optional<QString> parseItemLink(const QString &input);
// KDBusService may forward argv[0], command-line options and their values along with the link. Accept only one positional item link, not an option value
std::optional<QString> itemFromActivationArguments(const QStringList &arguments);
// A link that may be handed to the host browser: absolute http or https with a host and no credentials. Anything else (file:, javascript:, custom schemes, relative text) is refused
std::optional<QUrl> parseWebLink(const QString &input);
// The admin dashboard of the server at `baseUrl`: `<base>/web/#/dashboard`. A base that ends in `/web` already is not doubled; the base's query and fragment are dropped. Refused like parseWebLink
std::optional<QUrl> adminDashboardUrl(const QString &baseUrl);
} // namespace Kinefin
