// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastProfile.h"

#include <QJsonArray>
#include <QtTest>

using namespace Kinefin;

namespace {
const QStringList kModels{QString(), QStringLiteral("Chromecast"),
                          QStringLiteral("Google Nest Hub"), QStringLiteral("Google Nest Hub Max"),
                          QStringLiteral("Chromecast Ultra")};

QJsonArray arrayOf(const QJsonObject &profile, const char *key)
{
    return profile.value(QLatin1StringView(key)).toArray();
}

QJsonObject condition(const char *type, const char *property, const char *value)
{
    return {{QStringLiteral("Condition"), QString::fromLatin1(type)},
            {QStringLiteral("Property"), QString::fromLatin1(property)},
            {QStringLiteral("Value"), QString::fromLatin1(value)}};
}

QJsonObject atMost(const char *property, const char *value)
{
    return condition("LessThanEqual", property, value);
}

QJsonObject notAnamorphic()
{
    return condition("NotEquals", "IsAnamorphic", "true");
}

QJsonObject directPlay(const char *containers, const char *videoCodec, const char *audioCodec)
{
    return {{QStringLiteral("Type"), QStringLiteral("Video")},
            {QStringLiteral("Container"), QString::fromLatin1(containers)},
            {QStringLiteral("VideoCodec"), QString::fromLatin1(videoCodec)},
            {QStringLiteral("AudioCodec"), QString::fromLatin1(audioCodec)}};
}

QJsonObject videoLimits(const char *codec, const QJsonArray &conditions)
{
    return {{QStringLiteral("Type"), QStringLiteral("Video")},
            {QStringLiteral("Codec"), QString::fromLatin1(codec)},
            {QStringLiteral("Conditions"), conditions}};
}

QJsonObject audioLimits()
{
    return {{QStringLiteral("Type"), QStringLiteral("VideoAudio")},
            {QStringLiteral("Codec"), QStringLiteral("aac,mp3")},
            {QStringLiteral("Conditions"), QJsonArray{atMost("AudioChannels", "2")}}};
}

QJsonObject transcode()
{
    return {{QStringLiteral("Type"), QStringLiteral("Video")},
            {QStringLiteral("Container"), QStringLiteral("mkv")},
            {QStringLiteral("Protocol"), QStringLiteral("http")},
            {QStringLiteral("Context"), QStringLiteral("Streaming")},
            {QStringLiteral("VideoCodec"), QStringLiteral("h264")},
            {QStringLiteral("AudioCodec"), QStringLiteral("aac")},
            {QStringLiteral("MaxAudioChannels"), QStringLiteral("2")}};
}

QJsonArray baselineH264()
{
    return {atMost("VideoLevel", "41"), atMost("VideoBitDepth", "8"),   atMost("Width", "1920"),
            atMost("Height", "1080"),   atMost("VideoFramerate", "30"), notAnamorphic()};
}

QJsonArray hubH264(const char *framerate)
{
    return {
        atMost("VideoLevel", "41"), atMost("VideoBitDepth", "8"),        atMost("Width", "1280"),
        atMost("Height", "720"),    atMost("VideoFramerate", framerate), notAnamorphic()};
}

QJsonArray hubVp9(const char *framerate)
{
    return {atMost("Width", "1280"), atMost("Height", "720"), atMost("VideoFramerate", framerate)};
}

QMap<QString, QString> subtitleMethods(const QJsonObject &profile)
{
    QMap<QString, QString> methods;
    for (const QJsonValue &entry : arrayOf(profile, "SubtitleProfiles")) {
        methods.insert(entry.toObject().value(QStringLiteral("Format")).toString(),
                       entry.toObject().value(QStringLiteral("Method")).toString());
    }
    return methods;
}

const QStringList kTextSubtitles{QStringLiteral("vtt"),     QStringLiteral("webvtt"),
                                 QStringLiteral("srt"),     QStringLiteral("subrip"),
                                 QStringLiteral("ass"),     QStringLiteral("ssa"),
                                 QStringLiteral("mov_text")};
const QStringList kImageSubtitles{QStringLiteral("pgssub"), QStringLiteral("dvdsub"),
                                  QStringLiteral("dvbsub"), QStringLiteral("pgs")};
} // namespace

class TestCastProfile : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void modelRowsGiveGoldenProfiles_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QJsonArray>("directPlay");
        QTest::addColumn<QJsonArray>("codecProfiles");
        QTest::newRow("Chromecast")
            << QStringLiteral("Chromecast") << QJsonArray{directPlay("mp4,m4v", "h264", "aac,mp3")}
            << QJsonArray{videoLimits("h264", baselineH264()), audioLimits()};
        QTest::newRow("Google Nest Hub")
            << QStringLiteral("Google Nest Hub")
            << QJsonArray{directPlay("mp4,m4v,mkv", "h264", "aac,mp3"),
                          directPlay("webm", "vp9", "vorbis")}
            << QJsonArray{videoLimits("h264", hubH264("60")), videoLimits("vp9", hubVp9("60")),
                          audioLimits()};
        QTest::newRow("Google Nest Hub Max")
            << QStringLiteral("Google Nest Hub Max")
            << QJsonArray{directPlay("mp4,m4v,mkv", "h264", "aac,mp3"),
                          directPlay("webm", "vp9", "vorbis")}
            << QJsonArray{videoLimits("h264", hubH264("30")), videoLimits("vp9", hubVp9("30")),
                          audioLimits()};
        QTest::newRow("Chromecast Ultra")
            << QStringLiteral("Chromecast Ultra")
            << QJsonArray{directPlay("mp4,m4v", "h264,hevc", "aac,mp3"),
                          directPlay("webm", "vp9", "vorbis")}
            << QJsonArray{
                   videoLimits("h264", {atMost("VideoLevel", "42"), atMost("VideoBitDepth", "8"),
                                        atMost("Width", "1920"), atMost("Height", "1080"),
                                        atMost("VideoFramerate", "60"), notAnamorphic()}),
                   videoLimits("hevc", {atMost("VideoLevel", "153"), atMost("Width", "3840"),
                                        atMost("Height", "2160"), atMost("VideoFramerate", "60")}),
                   videoLimits("vp9", {atMost("Width", "3840"), atMost("Height", "2160"),
                                       atMost("VideoFramerate", "60")}),
                   audioLimits()};
    }
    void modelRowsGiveGoldenProfiles()
    {
        QFETCH(QString, model);
        QFETCH(QJsonArray, directPlay);
        QFETCH(QJsonArray, codecProfiles);
        const QJsonObject profile = castDeviceProfile(model, false, false);
        QCOMPARE(arrayOf(profile, "DirectPlayProfiles"), directPlay);
        QCOMPARE(arrayOf(profile, "CodecProfiles"), codecProfiles);
    }

    void unknownModelsGetTheBaseline()
    {
        const QJsonObject baseline = castDeviceProfile(QStringLiteral("Chromecast"), false, false);
        QCOMPARE(arrayOf(baseline, "DirectPlayProfiles"),
                 QJsonArray{directPlay("mp4,m4v", "h264", "aac,mp3")});
        for (const QString &model :
             {QString(), QStringLiteral("Chromecast HD"), QStringLiteral("Nest Audio"),
              QStringLiteral("google nest hub"), QStringLiteral("Google Nest Hub "),
              QStringLiteral("Chromecast Ultra 4K")}) {
            QVERIFY2(castDeviceProfile(model, false, false) == baseline, qPrintable(model));
        }
    }

    void everyModelKeepsTheSameTopLevelKeysTranscodeAndBitrates()
    {
        QStringList expectedKeys{QStringLiteral("Name"),
                                 QStringLiteral("MaxStreamingBitrate"),
                                 QStringLiteral("MaxStaticBitrate"),
                                 QStringLiteral("DirectPlayProfiles"),
                                 QStringLiteral("TranscodingProfiles"),
                                 QStringLiteral("CodecProfiles"),
                                 QStringLiteral("SubtitleProfiles")};
        expectedKeys.sort();
        for (const QString &model : kModels) {
            for (const bool transcodeOnly : {false, true}) {
                const QJsonObject profile = castDeviceProfile(model, false, transcodeOnly);
                QStringList keys = profile.keys();
                keys.sort();
                QVERIFY2(keys == expectedKeys, qPrintable(model));
                QVERIFY(!profile.value(QStringLiteral("Name")).toString().isEmpty());
                QCOMPARE(profile.value(QStringLiteral("MaxStreamingBitrate")).toInt(), 120000000);
                QCOMPARE(profile.value(QStringLiteral("MaxStaticBitrate")).toInt(), 120000000);
                QCOMPARE(arrayOf(profile, "TranscodingProfiles"), QJsonArray{transcode()});
            }
        }
    }

    void transcodeOnlyEmptiesDirectPlayAndKeepsTheRest()
    {
        for (const QString &model : kModels) {
            for (const bool burnIn : {false, true}) {
                QJsonObject full = castDeviceProfile(model, burnIn, false);
                QJsonObject only = castDeviceProfile(model, burnIn, true);
                QVERIFY2(!arrayOf(full, "DirectPlayProfiles").isEmpty(), qPrintable(model));
                QVERIFY2(only.value(QStringLiteral("DirectPlayProfiles")).isArray(),
                         qPrintable(model));
                QVERIFY2(arrayOf(only, "DirectPlayProfiles").isEmpty(), qPrintable(model));
                full.remove(QStringLiteral("DirectPlayProfiles"));
                only.remove(QStringLiteral("DirectPlayProfiles"));
                QVERIFY2(full == only, qPrintable(model));
            }
        }
    }

    void everyDirectPlayVideoCodecHasLimits()
    {
        for (const QString &model : kModels) {
            const QJsonObject profile = castDeviceProfile(model, false, false);
            QStringList limited;
            for (const QJsonValue &entry : arrayOf(profile, "CodecProfiles")) {
                if (entry.toObject().value(QStringLiteral("Type")).toString()
                    == QStringLiteral("Video"))
                    limited.append(entry.toObject().value(QStringLiteral("Codec")).toString());
            }
            for (const QJsonValue &entry : arrayOf(profile, "DirectPlayProfiles")) {
                const QStringList codecs = entry.toObject()
                                               .value(QStringLiteral("VideoCodec"))
                                               .toString()
                                               .split(QLatin1Char(','));
                for (const QString &codec : codecs)
                    QVERIFY2(limited.contains(codec), qPrintable(model + QLatin1Char(' ') + codec));
            }
        }
    }

    void textSubtitlesAreExternalAndImageSubtitlesEncode()
    {
        for (const QString &model : kModels) {
            const QJsonObject profile = castDeviceProfile(model, false, false);
            const QMap<QString, QString> methods = subtitleMethods(profile);
            QCOMPARE(arrayOf(profile, "SubtitleProfiles").size(),
                     kTextSubtitles.size() + kImageSubtitles.size());
            QCOMPARE(methods.size(), kTextSubtitles.size() + kImageSubtitles.size());
            for (const QString &format : kTextSubtitles)
                QCOMPARE(methods.value(format), QStringLiteral("External"));
            for (const QString &format : kImageSubtitles)
                QCOMPARE(methods.value(format), QStringLiteral("Encode"));
        }
    }

    void burnInEncodesEveryFormat()
    {
        for (const QString &model : kModels) {
            for (const bool transcodeOnly : {false, true}) {
                const QJsonObject profile = castDeviceProfile(model, true, transcodeOnly);
                const QMap<QString, QString> methods = subtitleMethods(profile);
                QCOMPARE(arrayOf(profile, "SubtitleProfiles").size(),
                         kTextSubtitles.size() + kImageSubtitles.size());
                QCOMPARE(methods.size(), kTextSubtitles.size() + kImageSubtitles.size());
                for (const QString &format : kTextSubtitles + kImageSubtitles)
                    QCOMPARE(methods.value(format), QStringLiteral("Encode"));
            }
        }
    }

    void conditionsCarryOnlyTheKeysTheServerReads()
    {
        const QStringList expectedKeys{QStringLiteral("Condition"), QStringLiteral("Property"),
                                       QStringLiteral("Value")};
        for (const QString &model : kModels) {
            for (const bool transcodeOnly : {false, true}) {
                const QJsonObject profile = castDeviceProfile(model, false, transcodeOnly);
                const QJsonArray codecProfiles = arrayOf(profile, "CodecProfiles");
                QVERIFY2(!codecProfiles.isEmpty(), qPrintable(model));
                for (const QJsonValue &codecProfile : codecProfiles) {
                    const QJsonArray conditions =
                        codecProfile.toObject().value(QStringLiteral("Conditions")).toArray();
                    QVERIFY2(!conditions.isEmpty(), qPrintable(model));
                    for (const QJsonValue &entry : conditions) {
                        const QJsonObject object = entry.toObject();
                        QStringList keys = object.keys();
                        keys.sort();
                        QVERIFY2(keys == expectedKeys, qPrintable(model));
                        QVERIFY2(object.value(QStringLiteral("Value")).isString(),
                                 qPrintable(model));
                    }
                }
                const QJsonArray transcodes = arrayOf(profile, "TranscodingProfiles");
                QCOMPARE(transcodes.size(), 1);
                const QJsonValue channels =
                    transcodes.at(0).toObject().value(QStringLiteral("MaxAudioChannels"));
                QVERIFY(channels.isString());
                QCOMPARE(channels.toString(), QStringLiteral("2"));
            }
        }
    }

    void castMimeForAContainer_data()
    {
        QTest::addColumn<QString>("container");
        QTest::addColumn<bool>("transcode");
        QTest::addColumn<QString>("mime");
        const QString mp4 = QStringLiteral("video/mp4");
        const QString mkv = QStringLiteral("video/x-matroska");
        const QString webm = QStringLiteral("video/webm");
        QTest::newRow("mp4") << QStringLiteral("mp4") << false << mp4;
        QTest::newRow("m4v") << QStringLiteral("m4v") << false << mp4;
        QTest::newRow("MP4") << QStringLiteral("MP4") << false << mp4;
        QTest::newRow("mkv") << QStringLiteral("mkv") << false << mkv;
        QTest::newRow("matroska") << QStringLiteral("matroska") << false << mkv;
        QTest::newRow("MKV") << QStringLiteral("MKV") << false << mkv;
        QTest::newRow("webm") << QStringLiteral("webm") << false << webm;
        QTest::newRow("ffmpeg mp4 list") << QStringLiteral("mov,mp4,m4a") << false << mp4;
        QTest::newRow("list with spaces") << QStringLiteral("mov, mp4") << false << mp4;
        QTest::newRow("first recognised token wins")
            << QStringLiteral("matroska,webm") << false << mkv;
        QTest::newRow("ts") << QStringLiteral("ts") << false << QString();
        QTest::newRow("avi") << QStringLiteral("avi") << false << QString();
        QTest::newRow("unrecognised list") << QStringLiteral("mov,m4a") << false << QString();
        QTest::newRow("empty") << QString() << false << QString();
        QTest::newRow("transcode mp4") << QStringLiteral("mp4") << true << mkv;
        QTest::newRow("transcode mkv") << QStringLiteral("mkv") << true << mkv;
        QTest::newRow("transcode webm") << QStringLiteral("webm") << true << mkv;
        QTest::newRow("transcode ts") << QStringLiteral("ts") << true << mkv;
        QTest::newRow("transcode empty") << QString() << true << mkv;
    }
    void castMimeForAContainer()
    {
        QFETCH(QString, container);
        QFETCH(bool, transcode);
        QFETCH(QString, mime);
        QCOMPARE(castMime(container, transcode), mime);
    }
};

QTEST_GUILESS_MAIN(TestCastProfile)
#include "test_cast_profile.moc"
