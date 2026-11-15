// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/MprisArtwork.h"

#include "integration/Mpris.h"
#include "player/PlayerController.h"
#include "settings/ProfilePaths.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QNetworkReply>

namespace Kinefin {
namespace {
// More pixels than any poster needs: a larger image is not decoded at all
constexpr qint64 kMaxDecodedPixels = 12'000'000;

// The formats a server sends for posters, told apart by their signature: no other image plugin (SVG, PSD, EXR...) ever sees server bytes, not even to recognise them
QByteArray posterFormat(const QByteArray &body)
{
    if (body.startsWith(QByteArrayView("\xFF\xD8\xFF")))
        return QByteArrayLiteral("jpeg");
    if (body.startsWith(QByteArrayView("\x89PNG\r\n\x1A\n")))
        return QByteArrayLiteral("png");
    if (body.size() >= 12 && body.startsWith(QByteArrayView("RIFF"))
        && body.mid(8, 4) == QByteArrayView("WEBP"))
        return QByteArrayLiteral("webp");
    return {};
}
} // namespace

MprisArtwork::MprisArtwork(Mpris &mpris, PlayerController &player,
                           std::shared_ptr<ImageOrigin> origin, const QString &directory,
                           QObject *parent)
    : QObject(parent), m_mpris(mpris), m_player(player), m_network(std::move(origin)),
      m_directory(directory)
{
    // No folder, no artwork: an empty path must never turn the cleanup into a removal of `/mpris`
    if (m_directory.isEmpty())
        return;
    QDir(m_directory).removeRecursively();
    connect(&m_player, &PlayerController::changed, this, &MprisArtwork::follow);
}

MprisArtwork::~MprisArtwork()
{
    if (m_reply)
        m_reply->abort();
    if (!m_directory.isEmpty())
        QDir(m_directory).removeRecursively();
}

void MprisArtwork::follow()
{
    const QUrl url = m_player.artworkUrl();
    if (url == m_requested)
        return;
    m_requested = url;
    if (m_reply)
        m_reply->abort();
    if (url.isEmpty())
        return;
    const QString itemId = m_player.itemId();
    QNetworkReply *reply = m_network.get(QNetworkRequest(url));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, itemId] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || itemId != m_player.itemId())
            return;
        store(itemId, reply->readAll());
    });
}

void MprisArtwork::store(const QString &itemId, const QByteArray &body)
{
    QBuffer buffer;
    buffer.setData(body);
    if (!buffer.open(QIODevice::ReadOnly))
        return;
    const QByteArray format = posterFormat(body);
    if (format.isEmpty())
        return;
    QImageReader reader(&buffer, format);
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > kMaxDecodedPixels)
        return;
    QImage image = reader.read();
    if (image.isNull())
        return;
    if (image.width() > MaxSide || image.height() > MaxSide)
        image = image.scaled(MaxSide, MaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer out(&png);
    if (!out.open(QIODevice::WriteOnly) || !image.save(&out, "PNG"))
        return;
    // The file is named after the item's track id, the only part of the item that leaves the app anyway
    const QString name = mprisTrackId(itemId).path().section(QLatin1Char('/'), -1);
    if (!QDir().mkpath(m_directory))
        return;
    const QString path = m_directory + QLatin1Char('/') + name + QStringLiteral(".png");
    if (!writePrivateFile(path, png))
        return;
    if (!m_file.isEmpty() && m_file != path)
        QFile::remove(m_file);
    m_file = path;
    m_mpris.setArtwork(itemId, QUrl::fromLocalFile(path));
}
} // namespace Kinefin
