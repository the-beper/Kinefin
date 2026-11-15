// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "imagecache/ImageNetworkAccess.h"

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <memory>

class QNetworkReply;

namespace Kinefin {
class ImageOrigin;
class Mpris;
class PlayerController;

// The cover the desktop shows for the playing item: its primary image, fetched like every image of the app (server's own base URL, no credentials, no redirect, size cap), decoded within bounds, scaled to 512 px and written as a private PNG in `directory`, which is emptied at start and at exit. The desktop gets the local file, never the server's address
class MprisArtwork final : public QObject {
    Q_OBJECT
  public:
    static constexpr int MaxSide = 512;

    MprisArtwork(Mpris &mpris, PlayerController &player, std::shared_ptr<ImageOrigin> origin,
                 const QString &directory, QObject *parent = nullptr);
    ~MprisArtwork() override;

  private:
    void follow();
    void store(const QString &itemId, const QByteArray &body);

    Mpris &m_mpris;
    PlayerController &m_player;
    ImageNetworkAccess m_network;
    QString m_directory;
    QUrl m_requested;
    QPointer<QNetworkReply> m_reply;
    QString m_file;
};
} // namespace Kinefin
