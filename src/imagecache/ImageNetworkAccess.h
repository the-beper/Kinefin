// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "imagecache/ImageOrigin.h"

#include <QNetworkAccessManager>
#include <QString>

#include <memory>

namespace Kinefin {

// The network access of QML images: only GET of a server image URL under the shared origin, never with credentials or cookies, a redirect reply is aborted (QML would otherwise follow it itself), an oversized body is aborted. Everything else gets a reply that fails at once without touching the network
class ImageNetworkAccess : public QNetworkAccessManager {
  public:
    // 256 MiB; the disk cache evicts older entries beyond it
    static constexpr qint64 kImageCacheBytes = 256LL * 1024 * 1024;
    // A larger image body is aborted: a hostile server could otherwise stream gigabytes into the loader's memory
    static constexpr qint64 kMaxImageBytes = 32LL * 1024 * 1024;

    // `cacheDirectory` empty means no disk cache; a directory that cannot be created or made private (0700) is logged and also means no disk cache
    explicit ImageNetworkAccess(std::shared_ptr<ImageOrigin> origin,
                                const QString &cacheDirectory = {}, QObject *parent = nullptr);

    // For tests
    void setMaxImageBytes(qint64 bytes) { m_maxImageBytes = bytes; }

  protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoingData = nullptr) override;

  private:
    std::shared_ptr<ImageOrigin> m_origin;
    qint64 m_maxImageBytes = kMaxImageBytes;
};

} // namespace Kinefin
