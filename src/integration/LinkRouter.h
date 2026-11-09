// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <optional>

namespace Kinefin {
// Provisional, explicit internal link grammar: jellyfin://item/<32-hex-id>.
std::optional<QString> parseItemLink(const QString &input);
} // namespace Kinefin
