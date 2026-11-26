// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "BlurHashImageProvider.h"

#include "imagecache/BlurHash.h"

#include <QRegularExpression>

namespace Kinefin {
QImage BlurHashImageProvider::requestImage(const QString &id, QSize *size,
                                           const QSize &requestedSize)
{
    // Hexadecimal digits only: anything else is no id of ours
    static const QRegularExpression hex(QStringLiteral("^(?:[0-9a-f]{2})+$"));
    QImage image;
    if (hex.match(id).hasMatch()) {
        const QString hash = QString::fromLatin1(QByteArray::fromHex(id.toLatin1()));
        image = decodeBlurHash(hash, requestedSize.width() > 0 ? requestedSize.width() : 32,
                               requestedSize.height() > 0 ? requestedSize.height() : 18);
    }
    if (size)
        *size = image.size();
    return image;
}
} // namespace Kinefin
