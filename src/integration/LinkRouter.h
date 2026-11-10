// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QUrl>
#include <optional>

namespace Kinefin {
// Provisional, explicit internal link grammar: jellyfin://item/<32-hex-id>
std::optional<QString> parseItemLink(const QString &input);
// A link that may be handed to the host browser: absolute http or https with a host and no credentials. Anything else (file:, javascript:, custom schemes, relative text) is refused
std::optional<QUrl> parseWebLink(const QString &input);
// The admin dashboard of the server at `baseUrl`: `<base>/web/#/dashboard`. A base that ends in `/web` already is not doubled; the base's query and fragment are dropped. Refused like parseWebLink
std::optional<QUrl> adminDashboardUrl(const QString &baseUrl);
} // namespace Kinefin
