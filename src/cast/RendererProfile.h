// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace Kinefin {

// The `DeviceProfile` of a PlaybackInfo request for a DLNA renderer; `sinkMimeTypes` are the lower-case MIME types of its GetProtocolInfo sink (parseSinkProtocolInfo). Direct play: the containers among mp4, m4v, mkv, ts, webm and avi whose MIME type the sink lists (see rendererMime) with h264 video and aac, ac3 or mp3 audio; none when `transcodeOnly` or when the sink lists no such container, so an empty sink means the server transcodes. Transcode: MPEG-TS over http with h264 and stereo aac. Subtitles: with `burnIn` every format is requested as `Encode`, so the server burns the chosen one into the video; without it none are requested
QJsonObject rendererDeviceProfile(const QStringList &sinkMimeTypes, bool burnIn,
                                  bool transcodeOnly);

// The MIME type to send for `container`, a Jellyfin container name or a comma separated list of them (`mkv`, `matroska,webm`, `mov,mp4,m4a`): the first the sink lists of those the container maps to (mp4 and m4v video/mp4, video/quicktime; mkv and matroska video/x-matroska, video/x-mkv; ts and mpegts video/mpeg, video/mp2t, video/vnd.dlna.mpeg-tts, video/mpegts; webm video/webm; avi video/avi, video/x-msvideo). Empty when the sink lists none, except for ts, the transcode container, which gets video/mpeg when the sink lists no MPEG-TS type either
QString rendererMime(const QString &container, const QStringList &sinkMimeTypes);

} // namespace Kinefin
