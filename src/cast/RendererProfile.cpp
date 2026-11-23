// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/RendererProfile.h"

#include <QJsonArray>

#include <array>

namespace Kinefin {
namespace {
struct ContainerMimes {
    QLatin1StringView container;
    // Whether the direct play profile names it (the others are aliases of a Jellyfin container name)
    bool profile;
    // The canonical type first, then the names some sinks use instead: the first one the sink lists is sent
    std::array<QLatin1StringView, 4> mimes;
};

constexpr QLatin1StringView kNone{};

const std::array<ContainerMimes, 8> &containerTable()
{
    static const std::array<ContainerMimes, 8> table{{
        {QLatin1StringView("mp4"),
         true,
         {QLatin1StringView("video/mp4"), QLatin1StringView("video/quicktime"), kNone, kNone}},
        {QLatin1StringView("m4v"),
         true,
         {QLatin1StringView("video/mp4"), QLatin1StringView("video/quicktime"), kNone, kNone}},
        {QLatin1StringView("mkv"),
         true,
         {QLatin1StringView("video/x-matroska"), QLatin1StringView("video/x-mkv"), kNone, kNone}},
        {QLatin1StringView("matroska"),
         false,
         {QLatin1StringView("video/x-matroska"), QLatin1StringView("video/x-mkv"), kNone, kNone}},
        {QLatin1StringView("ts"),
         true,
         {QLatin1StringView("video/mpeg"), QLatin1StringView("video/mp2t"),
          QLatin1StringView("video/vnd.dlna.mpeg-tts"), QLatin1StringView("video/mpegts")}},
        {QLatin1StringView("mpegts"),
         false,
         {QLatin1StringView("video/mpeg"), QLatin1StringView("video/mp2t"),
          QLatin1StringView("video/vnd.dlna.mpeg-tts"), QLatin1StringView("video/mpegts")}},
        {QLatin1StringView("webm"), true, {QLatin1StringView("video/webm"), kNone, kNone, kNone}},
        {QLatin1StringView("avi"),
         true,
         {QLatin1StringView("video/avi"), QLatin1StringView("video/x-msvideo"), kNone, kNone}},
    }};
    return table;
}

// The first MIME type of the entry the sink lists, or an empty string
QString firstListed(const ContainerMimes &entry, const QStringList &sink)
{
    for (const QLatin1StringView mime : entry.mimes) {
        if (!mime.isEmpty() && sink.contains(mime, Qt::CaseInsensitive))
            return QString(mime);
    }
    return {};
}

QJsonObject subtitleProfile(const QString &format, const QString &method)
{
    return {{QStringLiteral("Format"), format}, {QStringLiteral("Method"), method}};
}

QJsonArray subtitleProfiles(bool burnIn)
{
    QJsonArray profiles;
    if (!burnIn)
        return profiles;
    for (const QString &format :
         {QStringLiteral("srt"), QStringLiteral("ass"), QStringLiteral("sub"),
          QStringLiteral("ssa"), QStringLiteral("smi"), QStringLiteral("pgssub"),
          QStringLiteral("dvdsub"), QStringLiteral("dvbsub"), QStringLiteral("pgs")}) {
        profiles.append(subtitleProfile(format, QStringLiteral("Encode")));
    }
    return profiles;
}
} // namespace

QString rendererMime(const QString &container, const QStringList &sinkMimeTypes)
{
    bool isTransport = false;
    for (const QString &token : container.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString name = token.trimmed().toLower();
        for (const ContainerMimes &entry : containerTable()) {
            if (name != entry.container)
                continue;
            if (const QString mime = firstListed(entry, sinkMimeTypes); !mime.isEmpty())
                return mime;
            isTransport = isTransport || name == QLatin1StringView("ts")
                          || name == QLatin1StringView("mpegts");
        }
    }
    return isTransport ? QStringLiteral("video/mpeg") : QString();
}

QJsonObject rendererDeviceProfile(const QStringList &sinkMimeTypes, bool burnIn, bool transcodeOnly)
{
    QStringList containers;
    if (!transcodeOnly) {
        for (const ContainerMimes &entry : containerTable()) {
            if (entry.profile && !firstListed(entry, sinkMimeTypes).isEmpty())
                containers.append(QString(entry.container));
        }
    }
    QJsonArray directPlay;
    if (!containers.isEmpty()) {
        directPlay.append(
            QJsonObject{{QStringLiteral("Type"), QStringLiteral("Video")},
                        {QStringLiteral("Container"), containers.join(QLatin1Char(','))},
                        {QStringLiteral("VideoCodec"), QStringLiteral("h264")},
                        {QStringLiteral("AudioCodec"), QStringLiteral("aac,ac3,mp3")}});
    }
    const QJsonObject transcode{{QStringLiteral("Type"), QStringLiteral("Video")},
                                {QStringLiteral("Container"), QStringLiteral("ts")},
                                {QStringLiteral("VideoCodec"), QStringLiteral("h264")},
                                {QStringLiteral("AudioCodec"), QStringLiteral("aac")},
                                {QStringLiteral("Protocol"), QStringLiteral("http")},
                                {QStringLiteral("Context"), QStringLiteral("Streaming")},
                                // The server field is a string
                                {QStringLiteral("MaxAudioChannels"), QStringLiteral("2")}};
    return {{QStringLiteral("Name"), QStringLiteral("Kinefin Cast")},
            {QStringLiteral("MaxStaticBitrate"), 1000000000},
            {QStringLiteral("DirectPlayProfiles"), directPlay},
            {QStringLiteral("TranscodingProfiles"), QJsonArray{transcode}},
            {QStringLiteral("ContainerProfiles"), QJsonArray()},
            {QStringLiteral("CodecProfiles"), QJsonArray()},
            {QStringLiteral("SubtitleProfiles"), subtitleProfiles(burnIn)}};
}

} // namespace Kinefin
