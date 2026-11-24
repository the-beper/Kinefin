// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastProfile.h"

#include <QJsonArray>

#include <array>
#include <optional>

namespace Kinefin {
namespace {
// Google publishes no bitrate limit, jellyfin-chromecast hard-codes 120 Mbit/s
constexpr int kMaxBitrate = 120000000;

// A zero level or bit depth leaves that property unconstrained
struct Limits {
    int level;
    int bitDepth;
    int width;
    int height;
    int framerate;
    bool notAnamorphic;
};

struct Model {
    QLatin1StringView md;
    QLatin1StringView containers;
    Limits h264;
    std::optional<Limits> hevc;
    std::optional<Limits> vp9;
};

struct SubtitleFormat {
    QLatin1StringView name;
    bool text;
};

const Model kBaseline{QLatin1StringView(),
                      QLatin1StringView("mp4,m4v"),
                      {41, 8, 1920, 1080, 30, true},
                      std::nullopt,
                      std::nullopt};

const std::array<Model, 3> kModels{{
    {QLatin1StringView("Google Nest Hub"),
     QLatin1StringView("mp4,m4v,mkv"),
     {41, 8, 1280, 720, 60, true},
     std::nullopt,
     Limits{0, 0, 1280, 720, 60, false}},
    {QLatin1StringView("Google Nest Hub Max"),
     QLatin1StringView("mp4,m4v,mkv"),
     {41, 8, 1280, 720, 30, true},
     std::nullopt,
     Limits{0, 0, 1280, 720, 30, false}},
    {QLatin1StringView("Chromecast Ultra"),
     QLatin1StringView("mp4,m4v"),
     {42, 8, 1920, 1080, 60, true},
     Limits{153, 0, 3840, 2160, 60, false},
     Limits{0, 0, 3840, 2160, 60, false}},
}};

const std::array<SubtitleFormat, 11> kSubtitleFormats{{
    {QLatin1StringView("vtt"), true},
    {QLatin1StringView("webvtt"), true},
    {QLatin1StringView("srt"), true},
    {QLatin1StringView("subrip"), true},
    {QLatin1StringView("ass"), true},
    {QLatin1StringView("ssa"), true},
    {QLatin1StringView("mov_text"), true},
    {QLatin1StringView("pgssub"), false},
    {QLatin1StringView("dvdsub"), false},
    {QLatin1StringView("dvbsub"), false},
    {QLatin1StringView("pgs"), false},
}};

const Model &modelFor(const QString &md)
{
    for (const Model &model : kModels) {
        if (md == model.md)
            return model;
    }
    return kBaseline;
}

QJsonObject condition(const QString &type, const QString &property, const QString &value)
{
    return {{QStringLiteral("Condition"), type},
            {QStringLiteral("Property"), property},
            {QStringLiteral("Value"), value}};
}

QJsonObject atMost(const QString &property, int value)
{
    return condition(QStringLiteral("LessThanEqual"), property, QString::number(value));
}

QJsonArray conditions(const Limits &limits)
{
    QJsonArray list;
    if (limits.level > 0)
        list.append(atMost(QStringLiteral("VideoLevel"), limits.level));
    if (limits.bitDepth > 0)
        list.append(atMost(QStringLiteral("VideoBitDepth"), limits.bitDepth));
    list.append(atMost(QStringLiteral("Width"), limits.width));
    list.append(atMost(QStringLiteral("Height"), limits.height));
    list.append(atMost(QStringLiteral("VideoFramerate"), limits.framerate));
    if (limits.notAnamorphic) {
        list.append(condition(QStringLiteral("NotEquals"), QStringLiteral("IsAnamorphic"),
                              QStringLiteral("true")));
    }
    return list;
}

QJsonObject videoCodecProfile(const QString &codec, const Limits &limits)
{
    return {{QStringLiteral("Type"), QStringLiteral("Video")},
            {QStringLiteral("Codec"), codec},
            {QStringLiteral("Conditions"), conditions(limits)}};
}

QJsonArray codecProfiles(const Model &model)
{
    QJsonArray profiles{videoCodecProfile(QStringLiteral("h264"), model.h264)};
    if (model.hevc)
        profiles.append(videoCodecProfile(QStringLiteral("hevc"), *model.hevc));
    if (model.vp9)
        profiles.append(videoCodecProfile(QStringLiteral("vp9"), *model.vp9));
    profiles.append(QJsonObject{
        {QStringLiteral("Type"), QStringLiteral("VideoAudio")},
        {QStringLiteral("Codec"), QStringLiteral("aac,mp3")},
        {QStringLiteral("Conditions"), QJsonArray{atMost(QStringLiteral("AudioChannels"), 2)}}});
    return profiles;
}

QJsonObject directPlayProfile(const QString &containers, const QString &videoCodec,
                              const QString &audioCodec)
{
    return {{QStringLiteral("Type"), QStringLiteral("Video")},
            {QStringLiteral("Container"), containers},
            {QStringLiteral("VideoCodec"), videoCodec},
            {QStringLiteral("AudioCodec"), audioCodec}};
}

QJsonArray directPlayProfiles(const Model &model)
{
    QJsonArray profiles{
        directPlayProfile(QString(model.containers),
                          model.hevc ? QStringLiteral("h264,hevc") : QStringLiteral("h264"),
                          QStringLiteral("aac,mp3"))};
    if (model.vp9) {
        profiles.append(directPlayProfile(QStringLiteral("webm"), QStringLiteral("vp9"),
                                          QStringLiteral("vorbis")));
    }
    return profiles;
}

// Matroska, not MP4: on a Nest Hub Jellyfin's progressive fragmented MP4 (chunked, no Range) stopped growing after about 35 MB, ended there with FINISHED and had no sound, while the same transcode in Matroska played on with sound (seen live); Matroska is untested on the other models
QJsonObject transcodingProfile()
{
    return {{QStringLiteral("Type"), QStringLiteral("Video")},
            {QStringLiteral("Container"), QStringLiteral("mkv")},
            {QStringLiteral("Protocol"), QStringLiteral("http")},
            {QStringLiteral("Context"), QStringLiteral("Streaming")},
            {QStringLiteral("VideoCodec"), QStringLiteral("h264")},
            {QStringLiteral("AudioCodec"), QStringLiteral("aac")},
            // The server field is a string
            {QStringLiteral("MaxAudioChannels"), QStringLiteral("2")}};
}

QJsonArray subtitleProfiles(bool burnIn)
{
    QJsonArray profiles;
    for (const SubtitleFormat &format : kSubtitleFormats) {
        const bool external = format.text && !burnIn;
        profiles.append(
            QJsonObject{{QStringLiteral("Format"), QString(format.name)},
                        {QStringLiteral("Method"),
                         external ? QStringLiteral("External") : QStringLiteral("Encode")}});
    }
    return profiles;
}
} // namespace

QJsonObject castDeviceProfile(const QString &model, bool burnInSubtitles, bool transcodeOnly)
{
    const Model &entry = modelFor(model);
    QJsonArray directPlay;
    if (!transcodeOnly)
        directPlay = directPlayProfiles(entry);
    return {{QStringLiteral("Name"), QStringLiteral("Kinefin Google Cast")},
            {QStringLiteral("MaxStreamingBitrate"), kMaxBitrate},
            {QStringLiteral("MaxStaticBitrate"), kMaxBitrate},
            {QStringLiteral("DirectPlayProfiles"), directPlay},
            {QStringLiteral("TranscodingProfiles"), QJsonArray{transcodingProfile()}},
            {QStringLiteral("CodecProfiles"), codecProfiles(entry)},
            {QStringLiteral("SubtitleProfiles"), subtitleProfiles(burnInSubtitles)}};
}

QString castMime(const QString &container, bool transcode)
{
    if (transcode)
        return QStringLiteral("video/x-matroska");
    for (const QString &token : container.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString name = token.trimmed().toLower();
        if (name == QLatin1StringView("mp4") || name == QLatin1StringView("m4v"))
            return QStringLiteral("video/mp4");
        if (name == QLatin1StringView("mkv") || name == QLatin1StringView("matroska"))
            return QStringLiteral("video/x-matroska");
        if (name == QLatin1StringView("webm"))
            return QStringLiteral("video/webm");
    }
    return {};
}

} // namespace Kinefin
