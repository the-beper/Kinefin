// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace Kinefin {

// Longest title put into the metadata; renderers show it on a screen
inline constexpr int kMaxDidlTitleChars = 256;

// `http-get:*:<mime>:*`, without DLNA.ORG flags (added only when a renderer needs them). Empty when `mime` is not `type/subtype` of letters, digits and `.+-`
QString didlProtocolInfo(const QString &mime);

// The `CurrentURIMetaData` of one video: a DIDL-Lite document with one `object.item.videoItem` whose `res` is `url`. The title (cut at kMaxDidlTitleChars) and the URL are escaped as XML; the document is plain text for the renderer, and soapRequestBody escapes it again as the argument text. Empty when the MIME type is not valid
QString didlItem(const QString &title, const QString &url, const QString &mime);

} // namespace Kinefin
