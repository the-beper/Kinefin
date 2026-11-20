// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/Didl.h"

#include "cast/CastText.h"

#include <QRegularExpression>

namespace Kinefin {

QString didlProtocolInfo(const QString &mime)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9.+-]*/[A-Za-z0-9][A-Za-z0-9.+-]*$"));
    if (!pattern.match(mime).hasMatch())
        return {};
    return QStringLiteral("http-get:*:%1:*").arg(mime);
}

QString didlItem(const QString &title, const QString &url, const QString &mime)
{
    const QString protocolInfo = didlProtocolInfo(mime);
    if (protocolInfo.isEmpty())
        return {};
    const QString cleanTitle = title.left(kMaxDidlTitleChars);
    return QStringLiteral("<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
                          "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                          "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
                          "<item id=\"0\" parentID=\"-1\" restricted=\"1\">"
                          "<dc:title>%1</dc:title>"
                          "<upnp:class>object.item.videoItem</upnp:class>"
                          "<res protocolInfo=\"%2\">%3</res>"
                          "</item></DIDL-Lite>")
        .arg(xmlEscape(cleanTitle), xmlEscape(protocolInfo), xmlEscape(url));
}

} // namespace Kinefin
