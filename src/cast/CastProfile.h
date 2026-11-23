// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>

namespace Kinefin {

// The device profile PlaybackInfo is asked with for a Google Cast device on the Default Media Receiver, chosen by the TXT `md` model string (exact match; an unknown model gets the baseline). `burnInSubtitles` makes every subtitle format Method Encode (the server burns the chosen subtitle in); otherwise text formats are External and the image formats Encode. `transcodeOnly` leaves the direct play profiles empty
QJsonObject castDeviceProfile(const QString &model, bool burnInSubtitles, bool transcodeOnly);

// The contentType of a LOAD for a stream: `video/mp4` for a transcode (it is a fragmented MP4) and for the containers mp4 and m4v, `video/x-matroska` for mkv and matroska, `video/webm` for webm; empty for any other container
QString castMime(const QString &container, bool transcode);

} // namespace Kinefin
