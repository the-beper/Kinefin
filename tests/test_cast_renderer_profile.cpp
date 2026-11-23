// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/RendererProfile.h"

#include <QJsonArray>
#include <QtTest>

using namespace Kinefin;

namespace {
QJsonObject directPlay(const QString &containers)
{
    return {{QStringLiteral("Type"), QStringLiteral("Video")},
            {QStringLiteral("Container"), containers},
            {QStringLiteral("VideoCodec"), QStringLiteral("h264")},
            {QStringLiteral("AudioCodec"), QStringLiteral("aac,ac3,mp3")}};
}

QJsonObject transcode()
{
    return {{QStringLiteral("Type"), QStringLiteral("Video")},
            {QStringLiteral("Container"), QStringLiteral("ts")},
            {QStringLiteral("VideoCodec"), QStringLiteral("h264")},
            {QStringLiteral("AudioCodec"), QStringLiteral("aac")},
            {QStringLiteral("Protocol"), QStringLiteral("http")},
            {QStringLiteral("Context"), QStringLiteral("Streaming")},
            {QStringLiteral("MaxAudioChannels"), QStringLiteral("2")}};
}

QJsonObject golden(const QJsonArray &directPlayProfiles, const QJsonArray &subtitleProfiles = {})
{
    return {{QStringLiteral("Name"), QStringLiteral("Kinefin Cast")},
            {QStringLiteral("MaxStaticBitrate"), 1000000000},
            {QStringLiteral("DirectPlayProfiles"), directPlayProfiles},
            {QStringLiteral("TranscodingProfiles"), QJsonArray{transcode()}},
            {QStringLiteral("ContainerProfiles"), QJsonArray()},
            {QStringLiteral("CodecProfiles"), QJsonArray()},
            {QStringLiteral("SubtitleProfiles"), subtitleProfiles}};
}
} // namespace

class TestRendererProfile : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void sinkListsGiveGoldenProfiles()
    {
        QCOMPARE(
            rendererDeviceProfile({QStringLiteral("video/mp4"), QStringLiteral("video/x-matroska"),
                                   QStringLiteral("video/mpeg"), QStringLiteral("audio/mpeg")},
                                  false, false),
            golden({directPlay(QStringLiteral("mp4,m4v,mkv,ts"))}));
        QCOMPARE(
            rendererDeviceProfile({QStringLiteral("video/webm"), QStringLiteral("video/x-msvideo")},
                                  false, false),
            golden({directPlay(QStringLiteral("webm,avi"))}));
        QCOMPARE(rendererDeviceProfile({QStringLiteral("video/mp2t"), QStringLiteral("video/mp4")},
                                       false, false),
                 golden({directPlay(QStringLiteral("mp4,m4v,ts"))}));
        QCOMPARE(rendererDeviceProfile({QStringLiteral("VIDEO/MP4")}, false, false),
                 golden({directPlay(QStringLiteral("mp4,m4v"))}));
    }

    void anEmptyOrUnusefulSinkMeansTranscode()
    {
        QCOMPARE(rendererDeviceProfile({}, false, false), golden({}));
        QCOMPARE(rendererDeviceProfile({QStringLiteral("audio/mpeg"), QStringLiteral("image/jpeg")},
                                       false, false),
                 golden({}));
    }

    void quicktimeAndMpegtsSinksDirectPlay()
    {
        const QString quicktime = QStringLiteral("video/quicktime");
        const QString mpegts = QStringLiteral("video/mpegts");
        const QString canonical = QStringLiteral("video/mp4");
        const QStringList mixed{quicktime, mpegts, QStringLiteral("video/x-matroska")};
        const QStringList audioOnly{QStringLiteral("audio/mp4"), QStringLiteral("audio/mpegts")};
        const QJsonObject mp4 = golden({directPlay(QStringLiteral("mp4,m4v"))});
        QCOMPARE(rendererDeviceProfile({quicktime}, false, false), mp4);
        QCOMPARE(rendererDeviceProfile({QStringLiteral("Video/QuickTime")}, false, false), mp4);
        QCOMPARE(rendererDeviceProfile({quicktime, canonical}, false, false), mp4);
        QCOMPARE(rendererDeviceProfile({mpegts}, false, false),
                 golden({directPlay(QStringLiteral("ts"))}));
        QCOMPARE(rendererDeviceProfile(mixed, false, false),
                 golden({directPlay(QStringLiteral("mp4,m4v,mkv,ts"))}));
        QCOMPARE(rendererDeviceProfile(mixed, false, true), golden({}));
        QCOMPARE(rendererDeviceProfile(audioOnly, false, false), golden({}));
        QCOMPARE(rendererDeviceProfile({}, false, false), golden({}));
    }

    void transcodeOnlyDropsDirectPlay()
    {
        QCOMPARE(rendererDeviceProfile({QStringLiteral("video/mp4")}, false, true), golden({}));
    }

    void burnInRequestsEveryFormatAsEncode()
    {
        const QJsonObject profile =
            rendererDeviceProfile({QStringLiteral("video/mp4")}, true, false);
        const QJsonArray subtitles = profile.value(QStringLiteral("SubtitleProfiles")).toArray();
        QCOMPARE(subtitles.size(), 9);
        QStringList formats;
        for (const QJsonValue &entry : subtitles) {
            QCOMPARE(entry.toObject().value(QStringLiteral("Method")).toString(),
                     QStringLiteral("Encode"));
            formats.append(entry.toObject().value(QStringLiteral("Format")).toString());
        }
        for (const char *format :
             {"srt", "ass", "sub", "ssa", "smi", "pgssub", "dvdsub", "dvbsub", "pgs"})
            QVERIFY2(formats.contains(QLatin1StringView(format)), format);
        QVERIFY(rendererDeviceProfile({QStringLiteral("video/mp4")}, false, false)
                    .value(QStringLiteral("SubtitleProfiles"))
                    .toArray()
                    .isEmpty());
    }

    void theTranscodeIsAlwaysTsOverHttp()
    {
        for (const bool transcodeOnly : {false, true}) {
            const QJsonArray profiles =
                rendererDeviceProfile({QStringLiteral("video/mp4")}, false, transcodeOnly)
                    .value(QStringLiteral("TranscodingProfiles"))
                    .toArray();
            QCOMPARE(profiles.size(), 1);
            QCOMPARE(profiles.at(0).toObject(), transcode());
        }
    }

    void mimeForAContainer_data()
    {
        QTest::addColumn<QString>("container");
        QTest::addColumn<QStringList>("sink");
        QTest::addColumn<QString>("mime");
        const QStringList all{QStringLiteral("video/mp4"),   QStringLiteral("video/x-matroska"),
                              QStringLiteral("video/x-mkv"), QStringLiteral("video/mpeg"),
                              QStringLiteral("video/mp2t"),  QStringLiteral("video/webm"),
                              QStringLiteral("video/avi"),   QStringLiteral("video/x-msvideo")};
        QTest::newRow("mp4") << QStringLiteral("mp4") << all << QStringLiteral("video/mp4");
        QTest::newRow("m4v") << QStringLiteral("m4v") << all << QStringLiteral("video/mp4");
        QTest::newRow("ffmpeg mp4 list")
            << QStringLiteral("mov,mp4,m4a,3gp,3g2,mj2") << all << QStringLiteral("video/mp4");
        QTest::newRow("mkv first listed")
            << QStringLiteral("mkv") << all << QStringLiteral("video/x-matroska");
        QTest::newRow("mkv second only")
            << QStringLiteral("mkv") << QStringList{QStringLiteral("video/x-mkv")}
            << QStringLiteral("video/x-mkv");
        QTest::newRow("matroska,webm")
            << QStringLiteral("matroska,webm") << QStringList{QStringLiteral("video/x-mkv")}
            << QStringLiteral("video/x-mkv");
        QTest::newRow("webm token")
            << QStringLiteral("matroska,webm") << QStringList{QStringLiteral("video/webm")}
            << QStringLiteral("video/webm");
        QTest::newRow("ts") << QStringLiteral("ts") << all << QStringLiteral("video/mpeg");
        QTest::newRow("ts second")
            << QStringLiteral("ts") << QStringList{QStringLiteral("video/mp2t")}
            << QStringLiteral("video/mp2t");
        QTest::newRow("mpegts") << QStringLiteral("mpegts")
                                << QStringList{QStringLiteral("video/vnd.dlna.mpeg-tts")}
                                << QStringLiteral("video/vnd.dlna.mpeg-tts");
        QTest::newRow("avi") << QStringLiteral("AVI") << all << QStringLiteral("video/avi");
        QTest::newRow("avi second")
            << QStringLiteral("avi") << QStringList{QStringLiteral("video/x-msvideo")}
            << QStringLiteral("video/x-msvideo");
        QTest::newRow("mp4 quicktime only")
            << QStringLiteral("mp4") << QStringList{QStringLiteral("video/quicktime")}
            << QStringLiteral("video/quicktime");
        QTest::newRow("m4v quicktime only")
            << QStringLiteral("m4v") << QStringList{QStringLiteral("video/quicktime")}
            << QStringLiteral("video/quicktime");
        QTest::newRow("ffmpeg mp4 list quicktime only")
            << QStringLiteral("mov,mp4,m4a,3gp,3g2,mj2")
            << QStringList{QStringLiteral("video/quicktime")} << QStringLiteral("video/quicktime");
        QTest::newRow("mp4 and quicktime keeps mp4")
            << QStringLiteral("mp4")
            << QStringList{QStringLiteral("video/quicktime"), QStringLiteral("video/mp4")}
            << QStringLiteral("video/mp4");
        QTest::newRow("ts mpegts only")
            << QStringLiteral("ts") << QStringList{QStringLiteral("video/mpegts")}
            << QStringLiteral("video/mpegts");
        QTest::newRow("mpegts container mpegts only")
            << QStringLiteral("mpegts") << QStringList{QStringLiteral("video/mpegts")}
            << QStringLiteral("video/mpegts");
        QTest::newRow("ts mp2t wins over mpegts")
            << QStringLiteral("ts")
            << QStringList{QStringLiteral("video/mpegts"), QStringLiteral("video/mp2t")}
            << QStringLiteral("video/mp2t");
        QTest::newRow("ts dlna type wins over mpegts")
            << QStringLiteral("ts")
            << QStringList{QStringLiteral("video/mpegts"),
                           QStringLiteral("video/vnd.dlna.mpeg-tts")}
            << QStringLiteral("video/vnd.dlna.mpeg-tts");
        QTest::newRow("mp4 audio only sink")
            << QStringLiteral("mp4") << QStringList{QStringLiteral("audio/mp4")} << QString();
        QTest::newRow("not listed")
            << QStringLiteral("mp4") << QStringList{QStringLiteral("video/webm")} << QString();
        QTest::newRow("empty sink") << QStringLiteral("mkv") << QStringList() << QString();
        QTest::newRow("unknown container") << QStringLiteral("flv") << all << QString();
        QTest::newRow("empty container") << QString() << all << QString();
        QTest::newRow("ts falls back for the transcode")
            << QStringLiteral("ts") << QStringList() << QStringLiteral("video/mpeg");
        QTest::newRow("ts falls back with an unrelated sink")
            << QStringLiteral("ts") << QStringList{QStringLiteral("video/mp4")}
            << QStringLiteral("video/mpeg");
    }
    void mimeForAContainer()
    {
        QFETCH(QString, container);
        QFETCH(QStringList, sink);
        QFETCH(QString, mime);
        QCOMPARE(rendererMime(container, sink), mime);
    }
};

QTEST_GUILESS_MAIN(TestRendererProfile)
#include "test_cast_renderer_profile.moc"
