// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringView>

namespace Kinefin {

// Longest renderer name, manufacturer or model kept for the picker
inline constexpr int kMaxDisplayChars = 64;

// Plain text for the interface from a string a renderer sent: control, format (bidi override, zero width), private-use, unassigned and lone surrogate code points are dropped, whitespace runs become one space, the result is trimmed and cut at maxChars code points. Markup characters stay: the interface shows them literally
QString sanitizeDisplayText(QStringView text, int maxChars = kMaxDisplayChars);

// Text or attribute content for an XML 1.0 document: & < > " ' become entities and the code points XML 1.0 forbids (most control characters, lone surrogates, U+FFFE and U+FFFF) are dropped
QString xmlEscape(QStringView text);

} // namespace Kinefin
