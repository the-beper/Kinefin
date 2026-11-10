// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/DeviceProfile.h"

#include <QJsonArray>

namespace Kinefin {
namespace {
QJsonObject typeOnly(const QString &type)
{
    return {{QStringLiteral("Type"), type}};
}

QJsonObject condition(const QString &type, const QString &property, const QString &value)
{
    return {{QStringLiteral("Condition"), type},
            {QStringLiteral("Property"), property},
            {QStringLiteral("Value"), value}};
}

QJsonObject videoCodecProfile(const QJsonArray &conditions, const QString &codec = {})
{
    QJsonObject profile{{QStringLiteral("Type"), QStringLiteral("Video")},
                        {QStringLiteral("Conditions"), conditions}};
    if (!codec.isEmpty())
        profile.insert(QStringLiteral("Codec"), codec);
    return profile;
}

QString transcodeVideoCodecs(const DeviceProfileOptions &options)
{
    // Preferring H.265 has no effect while HEVC transcoding is not allowed
    if (!options.allowTranscodeToHevc)
        return QStringLiteral("h264,mpeg4,mpeg2video");
    if (options.preferTranscodeToH265)
        return QStringLiteral("h265,hevc,h264,mpeg4,mpeg2video");
    return QStringLiteral("h264,h265,hevc,mpeg4,mpeg2video");
}

QJsonArray directPlayProfiles(const DeviceProfileOptions &options)
{
    QJsonArray profiles{typeOnly(QStringLiteral("Audio")), typeOnly(QStringLiteral("Photo"))};
    if (!options.alwaysForceTranscode)
        profiles.append(typeOnly(QStringLiteral("Video")));
    return profiles;
}

QJsonArray transcodingProfiles(const DeviceProfileOptions &options)
{
    const QJsonObject video{
        {QStringLiteral("Type"), QStringLiteral("Video")},
        {QStringLiteral("Container"), QStringLiteral("ts")},
        {QStringLiteral("Protocol"), QStringLiteral("hls")},
        {QStringLiteral("AudioCodec"), QStringLiteral("aac,mp3,ac3,opus,vorbis")},
        {QStringLiteral("VideoCodec"), transcodeVideoCodecs(options)},
        // The server field is a string
        {QStringLiteral("MaxAudioChannels"),
         options.channels == QStringLiteral("2.0") ? QStringLiteral("2") : QStringLiteral("6")}};
    const QJsonObject photo{{QStringLiteral("Type"), QStringLiteral("Photo")},
                            {QStringLiteral("Container"), QStringLiteral("jpeg")}};
    return {typeOnly(QStringLiteral("Audio")), video, photo};
}

// The conditions are server-side requirements: a source that fails one is transcoded. They are sent without IsRequired, which the server reads as true: a source whose property is unknown fails the condition too
QJsonArray codecProfiles(const DeviceProfileOptions &options)
{
    const QString notEquals = QStringLiteral("NotEquals");
    const QString equals = QStringLiteral("Equals");
    const QString lessThanEqual = QStringLiteral("LessThanEqual");
    const QString rangeType = QStringLiteral("VideoRangeType");
    const QString width = QStringLiteral("Width");

    QJsonArray profiles;
    if (options.forceTranscodeDovi)
        profiles.append(
            videoCodecProfile({condition(notEquals, rangeType, QStringLiteral("DOVI"))}));
    if (options.forceTranscodeHdr)
        profiles.append(videoCodecProfile({condition(equals, rangeType, QStringLiteral("SDR"))}));
    if (options.forceTranscodeHi10p) {
        profiles.append(videoCodecProfile(
            {condition(lessThanEqual, QStringLiteral("VideoBitDepth"), QStringLiteral("8"))}));
    }
    // Width 0 matches no real stream, so every stream of the codec is transcoded
    if (options.forceTranscodeHevc) {
        for (const QString &codec : {QStringLiteral("hevc"), QStringLiteral("h265")})
            profiles.append(
                videoCodecProfile({condition(equals, width, QStringLiteral("0"))}, codec));
    }
    if (options.forceTranscodeAv1) {
        profiles.append(videoCodecProfile({condition(equals, width, QStringLiteral("0"))},
                                          QStringLiteral("av1")));
    }
    if (options.forceTranscode4k) {
        profiles.append(videoCodecProfile(
            {condition(lessThanEqual, width, QStringLiteral("1920")),
             condition(lessThanEqual, QStringLiteral("Height"), QStringLiteral("1080"))}));
    }
    return profiles;
}

QJsonArray subtitleProfiles()
{
    QJsonArray profiles;
    const auto add = [&profiles](const QString &format, const QString &method) {
        profiles.append(
            QJsonObject{{QStringLiteral("Format"), format}, {QStringLiteral("Method"), method}});
    };
    const QString external = QStringLiteral("External");
    const QString embed = QStringLiteral("Embed");
    for (const QString &format :
         {QStringLiteral("srt"), QStringLiteral("ass"), QStringLiteral("sub"),
          QStringLiteral("ssa"), QStringLiteral("smi")}) {
        add(format, external);
        add(format, embed);
    }
    for (const QString &format : {QStringLiteral("pgssub"), QStringLiteral("dvdsub"),
                                  QStringLiteral("dvbsub"), QStringLiteral("pgs")}) {
        add(format, embed);
    }
    return profiles;
}
} // namespace

QJsonObject buildDeviceProfile(const DeviceProfileOptions &options)
{
    return {{QStringLiteral("Name"), QStringLiteral("Kinefin")},
            {QStringLiteral("MaxStaticBitrate"), 1000000000},
            {QStringLiteral("MusicStreamingTranscodingBitrate"), 1280000},
            {QStringLiteral("DirectPlayProfiles"), directPlayProfiles(options)},
            {QStringLiteral("TranscodingProfiles"), transcodingProfiles(options)},
            {QStringLiteral("ContainerProfiles"), QJsonArray()},
            {QStringLiteral("CodecProfiles"), codecProfiles(options)},
            {QStringLiteral("SubtitleProfiles"), subtitleProfiles()}};
}
} // namespace Kinefin
