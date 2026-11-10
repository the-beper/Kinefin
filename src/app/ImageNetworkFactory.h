// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "imagecache/ImageOrigin.h"

#include <QQmlNetworkAccessManagerFactory>
#include <QString>

#include <memory>

namespace Kinefin {

// Gives QML images an ImageNetworkAccess; QML calls create() on its own network thread, hence the shared, locked origin
class ImageNetworkFactory : public QQmlNetworkAccessManagerFactory {
  public:
    ImageNetworkFactory(std::shared_ptr<ImageOrigin> origin, QString cacheDirectory);

    QNetworkAccessManager *create(QObject *parent) override;

  private:
    std::shared_ptr<ImageOrigin> m_origin;
    QString m_cacheDirectory;
};

} // namespace Kinefin
