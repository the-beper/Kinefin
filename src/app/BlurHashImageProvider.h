// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QQuickImageProvider>

namespace Kinefin {
// The pictures of BlurHashes, for `image://blurhash/<id>` where the id is the hash written in hexadecimal (a hash holds characters that a URL path cannot carry). It reads nothing from the network or the disk: an id that is no valid hash gives no picture. The size asked for is the size of the picture, 32 by 18 when none is
class BlurHashImageProvider : public QQuickImageProvider {
  public:
    BlurHashImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
} // namespace Kinefin
