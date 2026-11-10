// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/ImageNetworkFactory.h"

#include "imagecache/ImageNetworkAccess.h"

namespace Kinefin {

ImageNetworkFactory::ImageNetworkFactory(std::shared_ptr<ImageOrigin> origin,
                                         QString cacheDirectory)
    : m_origin(std::move(origin)), m_cacheDirectory(std::move(cacheDirectory))
{
}

QNetworkAccessManager *ImageNetworkFactory::create(QObject *parent)
{
    return new ImageNetworkAccess(m_origin, m_cacheDirectory, parent);
}

} // namespace Kinefin
