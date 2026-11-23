// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QtTest>

#include "api/MediaSourceSelection.h"

using Kinefin::castSubtitleUrl;
using Kinefin::isTextSubtitleCodec;
using Kinefin::isValidJellyfinId;
using Kinefin::pickStream;
using Kinefin::PlaybackMediaInfo;
using Kinefin::PlaybackSource;
using Kinefin::selectPlaybackSource;
using Kinefin::SourceSelection;

namespace {
const QString kItem = QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
const QString kSourceId = QStringLiteral("c1b2a3d4e5f6071829304a5b6c7d8e9f");
const QString kSession = QStringLiteral("b6c7d8e9f0a1b2c3");
// The access token of tests/fixtures/authenticate_success.json, embedded as ApiKey by the transcode fixture
const QByteArray kToken = "f2f4b7c1ee8a4d0bb2a5f9d1f9e0e2ab";
const QUrl kBase(QStringLiteral("https://jf.example"));
const QUrl kPrefixedBase(QStringLiteral("https://jf.example:8920/jellyfin/"));

QJsonObject fixture(const QString &name)
{
    QFile file(QStringLiteral(KINEFIN_SOURCE_DIR "/tests/fixtures/") + name
               + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject source(const QString &id, bool directPlay, bool transcoding,
                   const QString &transcodingUrl = {})
{
    QJsonObject object{{QStringLiteral("Id"), id},
                       {QStringLiteral("SupportsDirectPlay"), directPlay},
                       {QStringLiteral("SupportsDirectStream"), directPlay},
                       {QStringLiteral("SupportsTranscoding"), transcoding}};
    if (!transcodingUrl.isEmpty())
        object.insert(QStringLiteral("TranscodingUrl"), transcodingUrl);
    return object;
}

QJsonObject response(const QJsonArray &sources)
{
    return {{QStringLiteral("MediaSources"), sources}, {QStringLiteral("PlaySessionId"), kSession}};
}

SourceSelection select(const QJsonObject &json, const QUrl &base = kBase)
{
    return selectPlaybackSource(json, kItem, base, kToken);
}

QJsonObject videoStream()
{
    return {{QStringLiteral("Type"), QStringLiteral("Video")},
            {QStringLiteral("Index"), 0},
            {QStringLiteral("Codec"), QStringLiteral("hevc")},
            {QStringLiteral("Profile"), QStringLiteral("Main 10")},
            {QStringLiteral("Level"), 153},
            {QStringLiteral("BitRate"), 18'000'000},
            {QStringLiteral("RealFrameRate"), 23.976},
            {QStringLiteral("BitDepth"), 10},
            {QStringLiteral("PixelFormat"), QStringLiteral("yuv420p10le")},
            {QStringLiteral("VideoRange"), QStringLiteral("HDR")},
            {QStringLiteral("VideoRangeType"), QStringLiteral("HDR10")},
            {QStringLiteral("ColorSpace"), QStringLiteral("bt2020nc")},
            {QStringLiteral("ColorPrimaries"), QStringLiteral("bt2020")},
            {QStringLiteral("ColorTransfer"), QStringLiteral("smpte2084")},
            {QStringLiteral("Width"), 3840},
            {QStringLiteral("Height"), 2160}};
}

QJsonObject audioStream(int index, const QString &codec, bool isDefault)
{
    return {{QStringLiteral("Type"), QStringLiteral("Audio")},
            {QStringLiteral("Index"), index},
            {QStringLiteral("Codec"), codec},
            {QStringLiteral("Channels"), 6},
            {QStringLiteral("BitRate"), 640'000},
            {QStringLiteral("SampleRate"), 48000},
            {QStringLiteral("IsDefault"), isDefault}};
}

QJsonObject withStreams(QJsonObject mediaSource, const QJsonArray &streams)
{
    mediaSource.insert(QStringLiteral("Container"), QStringLiteral("mkv"));
    mediaSource.insert(QStringLiteral("Size"), 12'345'678'901LL);
    mediaSource.insert(QStringLiteral("Bitrate"), 19'000'000);
    mediaSource.insert(QStringLiteral("MediaStreams"), streams);
    return mediaSource;
}

QJsonObject transcodeSource(const QString &query)
{
    return withStreams(source(kSourceId, false, true,
                              QStringLiteral("/videos/x/master.m3u8?%1&ApiKey=%2")
                                  .arg(query, QString::fromLatin1(kToken))),
                       QJsonArray{videoStream(), audioStream(1, QStringLiteral("dts"), true)});
}
} // namespace

class TestMediaSourceSelection : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void directUsesStaticStream()
    {
        const SourceSelection selection = select(fixture(QStringLiteral("playbackinfo_direct")));
        QVERIFY2(selection.source, qPrintable(selection.failure));
        QCOMPARE(selection.source->method, PlaybackSource::Method::Direct);
        QCOMPARE(selection.source->itemId, kItem);
        QCOMPARE(selection.source->mediaSourceId, kSourceId);
        QCOMPARE(selection.source->playSessionId, kSession);
        QCOMPARE(
            selection.source->url.toString(),
            QStringLiteral(
                "https://jf.example/Videos/%1/stream?static=true&mediaSourceId=%2&playSessionId=%3")
                .arg(kItem, kSourceId, kSession));
        // Path is never opened: the URL does not mention the file
        QVERIFY(!selection.source->url.toString().contains(QStringLiteral("Dune")));
    }

    void directKeepsServerBasePath()
    {
        const SourceSelection selection =
            select(fixture(QStringLiteral("playbackinfo_direct")), kPrefixedBase);
        QVERIFY(selection.source);
        QCOMPARE(selection.source->url.host(), QStringLiteral("jf.example"));
        QCOMPARE(selection.source->url.port(), 8920);
        QCOMPARE(selection.source->url.path(),
                 QStringLiteral("/jellyfin/Videos/%1/stream").arg(kItem));
    }

    void transcodeKeepsBasePathAndStripsApiKey()
    {
        const QJsonObject json = fixture(QStringLiteral("playbackinfo_transcode"));
        // The fixture mirrors the server: the token travels as ApiKey in the query
        QVERIFY(json.value(QStringLiteral("MediaSources"))
                    .toArray()
                    .at(0)
                    .toObject()
                    .value(QStringLiteral("TranscodingUrl"))
                    .toString()
                    .contains(QStringLiteral("ApiKey=")));
        const SourceSelection selection = select(json, kPrefixedBase);
        QVERIFY2(selection.source, qPrintable(selection.failure));
        const QUrl url = selection.source->url;
        QCOMPARE(selection.source->method, PlaybackSource::Method::Transcode);
        QCOMPARE(selection.source->mediaSourceId, kSourceId);
        QCOMPARE(
            url.path(),
            QStringLiteral("/jellyfin/videos/c7dd3b1a-c7dd-3b1a-9d42-f2ab74f8a1b9/master.m3u8"));
        const QUrlQuery query(url);
        QVERIFY(!query.hasQueryItem(QStringLiteral("ApiKey")));
        QCOMPARE(query.queryItemValue(QStringLiteral("VideoBitrate")), QStringLiteral("8000000"));
        QCOMPARE(query.queryItemValue(QStringLiteral("MediaSourceId")), kSourceId);
        QVERIFY(!url.toString().contains(QString::fromLatin1(kToken)));
    }

    // The bit rate a transcode is made at comes from its URL, digits only and within 1 kbit/s to 1 Gbit/s; a direct play has none
    void transcodeTargetBitrateIsBounded_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<qint64>("expected");
        QTest::newRow("video and audio")
            << QStringLiteral("VideoBitrate=7616000&AudioBitrate=384000") << qint64(8'000'000);
        QTest::newRow("video only") << QStringLiteral("VideoBitrate=2000000") << qint64(2'000'000);
        QTest::newRow("audio alone is no video rate")
            << QStringLiteral("AudioBitrate=384000") << qint64(0);
        QTest::newRow("letters") << QStringLiteral("VideoBitrate=12ab&AudioBitrate=384000")
                                 << qint64(0);
        QTest::newRow("negative") << QStringLiteral("VideoBitrate=-5000000") << qint64(0);
        QTest::newRow("too large") << QStringLiteral("VideoBitrate=99999999999") << qint64(0);
        QTest::newRow("too small") << QStringLiteral("VideoBitrate=999") << qint64(0);
        QTest::newRow("trailing line break")
            << QStringLiteral("VideoBitrate=4000000%0A") << qint64(0);
        QTest::newRow("bad audio is left out")
            << QStringLiteral("VideoBitrate=4000000&AudioBitrate=x") << qint64(4'000'000);
    }
    void transcodeTargetBitrateIsBounded()
    {
        QFETCH(QString, query);
        QFETCH(qint64, expected);
        const QString url =
            QStringLiteral("/videos/") + kItem + QStringLiteral("/master.m3u8?") + query;
        const SourceSelection result = select(response({source(kSourceId, false, true, url)}));
        QVERIFY(result.source);
        QCOMPARE(result.source->media.targetBitrate, expected);
        const SourceSelection direct = select(response({source(kSourceId, true, false)}));
        QVERIFY(direct.source);
        QCOMPARE(direct.source->media.targetBitrate, qint64(0));
    }

    void directWinsAcrossSources()
    {
        const QString transcodeUrl = QStringLiteral("/videos/x/master.m3u8?VideoBitrate=1");
        const SourceSelection selection =
            select(response({source(QStringLiteral("aaaa"), false, true, transcodeUrl),
                             source(QStringLiteral("bbbb"), true, true)}));
        QVERIFY(selection.source);
        QCOMPARE(selection.source->method, PlaybackSource::Method::Direct);
        QCOMPARE(selection.source->mediaSourceId, QStringLiteral("bbbb"));
    }

    void directStreamAloneIsNotPlayable()
    {
        QJsonObject directStreamOnly = source(kSourceId, false, false);
        directStreamOnly.insert(QStringLiteral("SupportsDirectStream"), true);
        const SourceSelection selection = select(response({directStreamOnly}));
        QVERIFY(!selection.source);
        QCOMPARE(selection.failure, QStringLiteral("No playable media source"));
    }

    // The shape a 12.4 server returns when nothing is compatible: HTTP 200, no ErrorCode, every Supports* false (observed live)
    void noCompatibleSourceFails()
    {
        const SourceSelection selection =
            select(fixture(QStringLiteral("playbackinfo_no_compatible")));
        QVERIFY(!selection.source);
        QCOMPARE(selection.failure, QStringLiteral("No playable media source"));
    }

    void unsafeTranscodingUrlRejected_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("absolute") << QStringLiteral("https://evil.example/videos/x/master.m3u8");
        QTest::newRow("protocol relative") << QStringLiteral("//evil.example/videos/x/master.m3u8");
        QTest::newRow("scheme without authority") << QStringLiteral("http:/videos/x/master.m3u8");
        QTest::newRow("no leading slash") << QStringLiteral("videos/x/master.m3u8");
        QTest::newRow("dot segments") << QStringLiteral("/videos/../Users/master.m3u8");
        QTest::newRow("backslash") << QStringLiteral("/videos\\x/master.m3u8");
        QTest::newRow("fragment") << QStringLiteral("/videos/x/master.m3u8#frag");
        QTest::newRow("encoded dot segments") << QStringLiteral("/videos/%2e%2e/Users/master.m3u8");
        QTest::newRow("encoded uppercase dot") << QStringLiteral("/videos/%2E/master.m3u8");
        QTest::newRow("encoded slash") << QStringLiteral("/videos/a%2fb/master.m3u8");
        QTest::newRow("encoded backslash") << QStringLiteral("/videos/a%5Cb/master.m3u8");
        QTest::newRow("encoded nul") << QStringLiteral("/videos/a%00b/master.m3u8");
        QTest::newRow("encoded newline") << QStringLiteral("/videos/a%0Ab/master.m3u8");
        QTest::newRow("trailing dot dot") << QStringLiteral("/videos/x/..");
        QTest::newRow("single dot") << QStringLiteral("/videos/./master.m3u8");
        QTest::newRow("invalid escape in query") << QStringLiteral("/videos/x/master.m3u8?a=%zz");
    }
    void unsafeTranscodingUrlRejected()
    {
        QFETCH(QString, url);
        const SourceSelection unsafe = select(response({source(kSourceId, false, true, url)}));
        QVERIFY2(!unsafe.source, qPrintable(url));
        QCOMPARE(unsafe.failure, QStringLiteral("No playable media source"));
        // A safe sibling after the unsafe source is still used
        const SourceSelection sibling =
            select(response({source(QStringLiteral("aaaa"), false, true, url),
                             source(QStringLiteral("bbbb"), false, true,
                                    QStringLiteral("/videos/x/master.m3u8?a=1"))}));
        QVERIFY(sibling.source);
        QCOMPARE(sibling.source->mediaSourceId, QStringLiteral("bbbb"));
    }

    void tokenParametersStrippedAnyCaseAndResidualTokenRejected()
    {
        const SourceSelection stripped = select(
            response({source(kSourceId, false, true,
                             QStringLiteral("/videos/x/master.m3u8?a=1&api_key=%1&B=2&APIKEY=%1")
                                 .arg(QString::fromLatin1(kToken)))}));
        QVERIFY(stripped.source);
        QCOMPARE(stripped.source->url.query(), QStringLiteral("a=1&B=2"));

        // Encoded and bare spellings of the key go too
        const SourceSelection spellings = select(
            response({source(kSourceId, false, true,
                             QStringLiteral("/videos/x/master.m3u8?a=1&api%5Fkey=%1&ApiKey&b=2")
                                 .arg(QString::fromLatin1(kToken)))}));
        QVERIFY(spellings.source);
        QCOMPARE(spellings.source->url.query(), QStringLiteral("a=1&b=2"));

        // The token compare ignores case
        QVERIFY(!select(response({source(kSourceId, false, true,
                                         QStringLiteral("/videos/x/master.m3u8?foo=%1")
                                             .arg(QString::fromLatin1(kToken).toUpper()))}))
                     .source);

        // The token under any other name is a refusal, raw or percent-encoded. The ninth character percent-encoded: the same token on the wire
        const QString plain = QString::fromLatin1(kToken);
        const QString encoded = plain.left(8) + QLatin1Char('%')
                                + QString::fromLatin1(QByteArray(1, kToken.at(8)).toHex())
                                + plain.mid(9);
        for (const QString &leaky :
             {QStringLiteral("/videos/x/master.m3u8?foo=%1").arg(QString::fromLatin1(kToken)),
              QStringLiteral("/videos/%1/master.m3u8").arg(QString::fromLatin1(kToken)),
              QStringLiteral("/videos/x/master.m3u8?foo=%1").arg(encoded)}) {
            QVERIFY2(!select(response({source(kSourceId, false, true, leaky)})).source,
                     qPrintable(leaky));
        }
    }

    void requiresOpeningAndInvalidIdsAreSkipped()
    {
        QJsonObject live = source(kSourceId, true, true);
        live.insert(QStringLiteral("RequiresOpening"), true);
        QCOMPARE(select(response({live})).failure, QStringLiteral("No playable media source"));
        QCOMPARE(select(response({source(QStringLiteral("a/b"), true, true)})).failure,
                 QStringLiteral("No playable media source"));
        QCOMPARE(select(response({source(QString(), true, true)})).failure,
                 QStringLiteral("No playable media source"));
    }

    void envelopeErrors_data()
    {
        QTest::addColumn<QJsonObject>("json");
        const QJsonValue sources = QJsonArray{source(kSourceId, true, true)};
        QTest::newRow("empty object") << QJsonObject();
        QTest::newRow("empty sources") << response({});
        QTest::newRow("sources not an array")
            << QJsonObject{{QStringLiteral("MediaSources"), QStringLiteral("x")},
                           {QStringLiteral("PlaySessionId"), kSession}};
        QTest::newRow("missing session") << QJsonObject{{QStringLiteral("MediaSources"), sources}};
        QTest::newRow("session with slash")
            << QJsonObject{{QStringLiteral("MediaSources"), sources},
                           {QStringLiteral("PlaySessionId"), QStringLiteral("a/b")}};
        QTest::newRow("session not a string") << QJsonObject{
            {QStringLiteral("MediaSources"), sources}, {QStringLiteral("PlaySessionId"), 7}};
    }
    void envelopeErrors()
    {
        QFETCH(QJsonObject, json);
        const SourceSelection selection = select(json);
        QVERIFY(!selection.source);
        QCOMPARE(selection.failure, QStringLiteral("Invalid playback response"));
    }

    void errorCodesAreNamedButNeverEchoed()
    {
        const auto failureFor = [](const QString &code) {
            QJsonObject json = response({source(kSourceId, true, true)});
            json.insert(QStringLiteral("ErrorCode"), code);
            return select(json).failure;
        };
        QCOMPARE(failureFor(QStringLiteral("NotAllowed")),
                 QStringLiteral("Playback is not allowed"));
        QCOMPARE(failureFor(QStringLiteral("NoCompatibleStream")),
                 QStringLiteral("No compatible stream"));
        QCOMPARE(failureFor(QStringLiteral("RateLimitExceeded")),
                 QStringLiteral("Playback rate limit exceeded"));
        const QString unknown = failureFor(QStringLiteral("<b>Surprise</b>"));
        QCOMPARE(unknown, QStringLiteral("Playback refused by the server"));
    }

    void rejectsBadInput()
    {
        const QJsonObject json = fixture(QStringLiteral("playbackinfo_direct"));
        QVERIFY(!json.isEmpty());
        QCOMPARE(selectPlaybackSource(json, QStringLiteral("a/b"), kBase, kToken).failure,
                 QStringLiteral("Invalid item id"));
        QCOMPARE(selectPlaybackSource(json, QString(), kBase, kToken).failure,
                 QStringLiteral("Invalid item id"));
        for (const QUrl &base :
             {QUrl(), QUrl(QStringLiteral("ftp://jf.example")), QUrl(QStringLiteral("https:///x")),
              QUrl(QStringLiteral("/relative"))}) {
            QCOMPARE(selectPlaybackSource(json, kItem, base, kToken).failure,
                     QStringLiteral("Invalid server address"));
        }
    }

    void baseWithUserInfoIsRefused()
    {
        const QJsonObject json = fixture(QStringLiteral("playbackinfo_direct"));
        QVERIFY(!json.isEmpty());
        QCOMPARE(select(json, QUrl(QStringLiteral("https://user:secret@jf.example"))).failure,
                 QStringLiteral("Invalid server address"));
    }

    void basePathStaysEncoded()
    {
        const QJsonObject json = fixture(QStringLiteral("playbackinfo_direct"));
        const SourceSelection selection =
            select(json, QUrl(QStringLiteral("https://jf.example/a%20b/")));
        QVERIFY(selection.source);
        QCOMPARE(selection.source->url.path(QUrl::FullyEncoded),
                 QStringLiteral("/a%20b/Videos/%1/stream").arg(kItem));
        // A literal percent in the base path is not decoded a second time
        const SourceSelection percent =
            select(json, QUrl(QStringLiteral("https://jf.example/a%2541/")));
        QVERIFY(percent.source);
        QCOMPARE(percent.source->url.path(QUrl::FullyEncoded),
                 QStringLiteral("/a%2541/Videos/%1/stream").arg(kItem));
    }

    void emptyTokenFailsClosed()
    {
        const QJsonObject json = fixture(QStringLiteral("playbackinfo_direct"));
        QVERIFY(!json.isEmpty());
        QCOMPARE(selectPlaybackSource(json, kItem, kBase, QByteArrayView()).failure,
                 QStringLiteral("Not authenticated"));
    }

    void transcodeNeedsFlagAndStringUrl()
    {
        QJsonObject notAString = source(kSourceId, false, true);
        notAString.insert(QStringLiteral("TranscodingUrl"), 7);
        QCOMPARE(select(response({notAString})).failure,
                 QStringLiteral("No playable media source"));
        QCOMPARE(select(response({source(kSourceId, false, false,
                                         QStringLiteral("/videos/x/master.m3u8"))}))
                     .failure,
                 QStringLiteral("No playable media source"));
    }

    void invalidFirstSourceThenValidOne()
    {
        const SourceSelection selection =
            select(response({source(QStringLiteral("a/b"), true, true),
                             source(QStringLiteral("bbbb"), true, true)}));
        QVERIFY(selection.source);
        QCOMPARE(selection.source->mediaSourceId, QStringLiteral("bbbb"));
    }

    void authenticatesAndBoundsExternalSubtitleRoutes()
    {
        const QJsonObject stream{
            {QStringLiteral("Type"), QStringLiteral("Subtitle")},
            {QStringLiteral("IsExternal"), true},
            {QStringLiteral("IsTextSubtitleStream"), true},
            {QStringLiteral("DeliveryMethod"), QStringLiteral("External")},
            {QStringLiteral("Index"), 3},
            {QStringLiteral("DisplayTitle"), QStringLiteral("Italian subtitles")},
            {QStringLiteral("DeliveryUrl"),
             QStringLiteral("/Videos/%1/Subtitles/3/0/Stream.srt?ApiKey=%2")
                 .arg(kItem, QString::fromLatin1(kToken))}};
        QJsonObject mediaSource = source(kSourceId, true, true);
        mediaSource.insert(QStringLiteral("MediaStreams"), QJsonArray{stream});
        const auto selected = select(response({mediaSource}), kPrefixedBase);
        QVERIFY(selected.source);
        QCOMPARE(selected.source->externalSubtitles.size(), 1);
        const auto &subtitle = selected.source->externalSubtitles.constFirst();
        QCOMPARE(subtitle.index, 3);
        QCOMPARE(subtitle.format, QStringLiteral("srt"));
        QCOMPARE(subtitle.label, QStringLiteral("Italian subtitles"));
        QCOMPARE(subtitle.url.host(), QStringLiteral("jf.example"));
        QCOMPARE(subtitle.url.path(),
                 QStringLiteral("/jellyfin/Videos/%1/Subtitles/3/0/Stream.srt").arg(kItem));
        QVERIFY(!subtitle.url.hasQuery());
        QVERIFY(!subtitle.url.toString().contains(QString::fromLatin1(kToken)));
    }

    // In a transcode the server extracts the file's own text streams and sends them as External (12.4): they are offered with the sidecar files, through the same URL checks; an image stream stays burned in
    void aTranscodeOffersTheFilesOwnTextSubtitles()
    {
        const auto subtitle = [](int index, bool external, bool text, const QString &method) {
            return QJsonObject{
                {QStringLiteral("Type"), QStringLiteral("Subtitle")},
                {QStringLiteral("IsExternal"), external},
                {QStringLiteral("IsTextSubtitleStream"), text},
                {QStringLiteral("DeliveryMethod"), method},
                {QStringLiteral("Index"), index},
                {QStringLiteral("DisplayTitle"), QStringLiteral("Track %1").arg(index)},
                {QStringLiteral("DeliveryUrl"),
                 QStringLiteral("/Videos/%1/%2/Subtitles/%3/0/Stream.%4?ApiKey=%5")
                     .arg(kItem, kSourceId)
                     .arg(index)
                     .arg(text ? QStringLiteral("srt") : QStringLiteral("sup"),
                          QString::fromLatin1(kToken))}};
        };
        const QJsonArray streams{videoStream(), subtitle(0, true, true, QStringLiteral("External")),
                                 subtitle(4, false, true, QStringLiteral("External")),
                                 subtitle(5, false, false, QStringLiteral("Encode"))};
        QJsonObject transcode = transcodeSource(QStringLiteral("VideoCodec=h264"));
        transcode.insert(QStringLiteral("MediaStreams"), streams);
        const auto selection = select(response({transcode}));
        QVERIFY(selection.source);
        QCOMPARE(selection.source->method, Kinefin::PlaybackSource::Method::Transcode);
        QCOMPARE(selection.source->externalSubtitles.size(), 2);
        QCOMPARE(selection.source->externalSubtitles.at(0).index, 0);
        QCOMPARE(selection.source->externalSubtitles.at(1).index, 4);
        // The server extracts the file's own stream on the first request: the client waits minutes for it, not seconds
        QVERIFY(!selection.source->externalSubtitles.at(0).extracted);
        QVERIFY(selection.source->externalSubtitles.at(1).extracted);
        QVERIFY(!selection.source->externalSubtitles.at(1).url.hasQuery());
        QVERIFY(!selection.source->externalSubtitles.at(1).url.toString().contains(
            QString::fromLatin1(kToken)));
        // A direct play has the file's own streams in the file: only the sidecar is a server file
        QJsonObject direct = source(kSourceId, true, true);
        direct.insert(QStringLiteral("MediaStreams"), streams);
        const auto played = select(response({direct}));
        QVERIFY(played.source);
        QCOMPARE(played.source->method, Kinefin::PlaybackSource::Method::Direct);
        QCOMPARE(played.source->externalSubtitles.size(), 1);
        QCOMPARE(played.source->externalSubtitles.constFirst().index, 0);
    }

    void rejectsUnsafeExternalSubtitles()
    {
        for (const QString &raw :
             {QStringLiteral("//evil.invalid/Videos/id/Subtitles/0/Stream.srt"),
              QStringLiteral("/Videos/../Users/Me.srt"),
              QStringLiteral("/Videos/id/Subtitles/0/Stream.exe"),
              QStringLiteral("/Videos/id/Subtitles/0/Stream.srt?Other=1"),
              QStringLiteral("/Videos/id/Subtitles/0/Stream.srt#secret")}) {
            QJsonObject stream{{QStringLiteral("Type"), QStringLiteral("Subtitle")},
                               {QStringLiteral("IsExternal"), true},
                               {QStringLiteral("IsTextSubtitleStream"), true},
                               {QStringLiteral("DeliveryMethod"), QStringLiteral("External")},
                               {QStringLiteral("Index"), 1},
                               {QStringLiteral("DeliveryUrl"), raw}};
            QJsonObject mediaSource = source(kSourceId, true, true);
            mediaSource.insert(QStringLiteral("MediaStreams"), QJsonArray{stream});
            const auto selection = select(response({mediaSource}));
            QVERIFY2(selection.source, qPrintable(raw));
            QVERIFY2(selection.source->externalSubtitles.isEmpty(), qPrintable(raw));
        }
    }

    void directPlayKeepsTheFactsOfTheSource()
    {
        const QJsonObject mediaSource =
            withStreams(source(kSourceId, true, true),
                        QJsonArray{videoStream(), audioStream(1, QStringLiteral("aac"), false),
                                   audioStream(2, QStringLiteral("eac3"), true)});
        const auto selection = select(response({mediaSource}));
        QVERIFY(selection.source);
        const PlaybackMediaInfo &media = selection.source->media;
        QCOMPARE(media.streamType, QStringLiteral("File"));
        QCOMPARE(media.protocol, QStringLiteral("https"));
        QVERIFY(media.transcodeReasons.isEmpty());
        QVERIFY(media.targetVideoCodec.isEmpty());
        QCOMPARE(media.source.container, QStringLiteral("mkv"));
        QCOMPARE(media.source.size, 12'345'678'901LL);
        QCOMPARE(media.source.bitrate, 19'000'000LL);
        const auto *video = pickStream(media.source, u"Video");
        QVERIFY(video);
        QCOMPARE(video->codec, QStringLiteral("hevc"));
        QCOMPARE(video->profile, QStringLiteral("Main 10"));
        QCOMPARE(video->level, 153.0);
        QCOMPARE(video->bitrate, 18'000'000LL);
        QCOMPARE(video->frameRate, 23.976);
        QCOMPARE(video->bitDepth, 10);
        QCOMPARE(video->pixelFormat, QStringLiteral("yuv420p10le"));
        QCOMPARE(video->videoRange, QStringLiteral("HDR"));
        QCOMPARE(video->videoRangeType, QStringLiteral("HDR10"));
        QCOMPARE(video->colorSpace, QStringLiteral("bt2020nc"));
        QCOMPARE(video->colorPrimaries, QStringLiteral("bt2020"));
        QCOMPARE(video->colorTransfer, QStringLiteral("smpte2084"));
        // The default audio track unless an index names one
        const auto *audio = pickStream(media.source, u"Audio");
        QVERIFY(audio);
        QCOMPARE(audio->codec, QStringLiteral("eac3"));
        QCOMPARE(audio->channels, 6);
        QCOMPARE(audio->sampleRate, 48000);
        QCOMPARE(audio->bitrate, 640'000LL);
        QCOMPARE(pickStream(media.source, u"Audio", 1)->codec, QStringLiteral("aac"));
        // An index nothing has falls back to the default track
        QCOMPARE(pickStream(media.source, u"Audio", 9)->codec, QStringLiteral("eac3"));
        QVERIFY(!pickStream(media.source, u"Subtitle"));
    }

    void firstAudioStreamWhenNoneIsDefault()
    {
        const auto selection = select(
            response({withStreams(source(kSourceId, true, true),
                                  QJsonArray{audioStream(1, QStringLiteral("aac"), false),
                                             audioStream(2, QStringLiteral("eac3"), false)})}));
        QVERIFY(selection.source);
        QCOMPARE(pickStream(selection.source->media.source, u"Audio")->codec,
                 QStringLiteral("aac"));
        QVERIFY(!pickStream(selection.source->media.source, u"Video"));
    }

    void hostileMediaFactsAreCappedAndRangeChecked()
    {
        QJsonObject video = videoStream();
        video.insert(QStringLiteral("Codec"), QString(500, QLatin1Char('h')));
        video.insert(QStringLiteral("Profile"), QStringLiteral("Main\n<b>10</b>\x01"));
        video.insert(QStringLiteral("PixelFormat"), QStringLiteral("a\u202eb"));
        video.insert(QStringLiteral("Level"), 1e12);
        video.insert(QStringLiteral("RealFrameRate"), -5);
        video.insert(QStringLiteral("AverageFrameRate"), 25.0);
        video.insert(QStringLiteral("BitDepth"), -1);
        video.insert(QStringLiteral("BitRate"), QStringLiteral("fast"));
        video.insert(QStringLiteral("ColorSpace"), 7);
        video.insert(QStringLiteral("ColorTransfer"), QStringLiteral("smpte2084\n"));
        QJsonObject audio = audioStream(1, QStringLiteral("aac"), true);
        audio.insert(QStringLiteral("Channels"), 1000);
        audio.insert(QStringLiteral("SampleRate"), 1e12);
        QJsonObject mediaSource =
            withStreams(source(kSourceId, true, true), QJsonArray{video, audio});
        mediaSource.insert(QStringLiteral("Container"), QString(500, QLatin1Char('c')));
        mediaSource.insert(QStringLiteral("Size"), -1);
        mediaSource.insert(QStringLiteral("Bitrate"), 1e15);
        const auto selection = select(response({mediaSource}));
        QVERIFY(selection.source);
        const PlaybackMediaInfo &media = selection.source->media;
        QCOMPARE(media.source.container.size(), 64);
        QCOMPARE(media.source.size, 0LL);
        QCOMPARE(media.source.bitrate, 0LL);
        const auto *parsedVideo = pickStream(media.source, u"Video");
        QVERIFY(parsedVideo);
        QCOMPARE(parsedVideo->codec.size(), 64);
        // A token with a control or bidi character is dropped whole, markup stays plain text
        QVERIFY(parsedVideo->profile.isEmpty());
        QVERIFY(parsedVideo->pixelFormat.isEmpty());
        QCOMPARE(parsedVideo->level, 0.0);
        QCOMPARE(parsedVideo->frameRate, 25.0);
        QCOMPARE(parsedVideo->bitDepth, 0);
        QCOMPARE(parsedVideo->bitrate, 0LL);
        QVERIFY(parsedVideo->colorSpace.isEmpty());
        // A final newline is no printable character either
        QVERIFY(parsedVideo->colorTransfer.isEmpty());
        const auto *parsedAudio = pickStream(media.source, u"Audio");
        QVERIFY(parsedAudio);
        QCOMPARE(parsedAudio->channels, 0);
        QCOMPARE(parsedAudio->sampleRate, 0);
    }

    void aHostileStreamListIsCapped()
    {
        QJsonArray streams;
        for (int i = 0; i < 200; ++i)
            streams.append(audioStream(i, QStringLiteral("aac"), false));
        const auto selection =
            select(response({withStreams(source(kSourceId, true, true), streams)}));
        QVERIFY(selection.source);
        QCOMPARE(selection.source->media.source.streams.size(),
                 int(Kinefin::ItemDetails::MaxStreams));
    }

    void transcodeKeepsReasonsAndTargetCodecs()
    {
        const auto selection = select(response({transcodeSource(QStringLiteral(
            "VideoCodec=h264,hevc&AudioCodec=aac,mp3&TranscodeReasons=VideoCodecNotSupported,"
            "AudioCodecNotSupported"))}));
        QVERIFY(selection.source);
        const PlaybackMediaInfo &media = selection.source->media;
        QCOMPARE(media.streamType, QStringLiteral("HLS"));
        QCOMPARE(media.protocol, QStringLiteral("https"));
        QCOMPARE(media.targetVideoCodec, QStringLiteral("h264"));
        QCOMPARE(media.targetAudioCodec, QStringLiteral("aac"));
        QCOMPARE(media.transcodeReasons, (QStringList{QStringLiteral("VideoCodecNotSupported"),
                                                      QStringLiteral("AudioCodecNotSupported")}));
    }

    void theTargetVideoCodecIsTheUrlsFirstNeverTheSources()
    {
        // Audio-only reasons say nothing about the video: the target is what the URL lists first
        const auto selection = select(response(
            {transcodeSource(QStringLiteral("VideoCodec=h264,hevc&AudioCodec=aac&TranscodeReasons="
                                            "AudioCodecNotSupported,ContainerNotSupported"))}));
        QVERIFY(selection.source);
        QCOMPARE(selection.source->media.targetVideoCodec, QStringLiteral("h264"));
        QCOMPARE(selection.source->media.targetAudioCodec, QStringLiteral("aac"));
        // No video codec in the URL leaves the target empty, whatever the source's video is
        const auto none = select(response({transcodeSource(QStringLiteral("AudioCodec=aac"))}));
        QVERIFY(none.source);
        QVERIFY(none.source->media.targetVideoCodec.isEmpty());
    }

    void transcodeReasonsAreFilteredAndCapped()
    {
        QStringList names;
        for (int i = 0; i < 40; ++i)
            names << QStringLiteral("R") + QChar(u'A' + i % 26) + QChar(u'a' + i / 26);
        const QString hostile = QStringLiteral("Bad%20Name,%3Cscript%3E,Under_score,Digit1,,")
                                + QString(65, u'x') + QLatin1Char(',')
                                + names.join(QLatin1Char(',')) + QStringLiteral(",RAa");
        const auto selection =
            select(response({transcodeSource(QStringLiteral("TranscodeReasons=") + hostile)}));
        QVERIFY(selection.source);
        QStringList reasons = selection.source->media.transcodeReasons;
        QCOMPARE(reasons.size(), int(PlaybackMediaInfo::MaxTranscodeReasons));
        QVERIFY(!reasons.contains(QStringLiteral("Bad Name")));
        for (const QString &reason : reasons)
            QVERIFY2(
                QRegularExpression(QStringLiteral("^[A-Za-z]{1,64}$")).match(reason).hasMatch(),
                qPrintable(reason));
        QCOMPARE(reasons.first(), names.first());
        QCOMPARE(reasons.removeDuplicates(), 0);
        // A repeated name is listed once
        const auto repeated =
            select(response({transcodeSource(QStringLiteral("TranscodeReasons=RAa,RAa,RBa,RAa"))}));
        QVERIFY(repeated.source);
        QCOMPARE(repeated.source->media.transcodeReasons,
                 (QStringList{QStringLiteral("RAa"), QStringLiteral("RBa")}));
    }

    void transcodeReasonsFallBackToTheSourceField()
    {
        QJsonObject mediaSource = transcodeSource(QStringLiteral("VideoCodec=h264"));
        mediaSource.insert(QStringLiteral("TranscodeReasons"),
                           QJsonArray{QStringLiteral("DirectPlayError"), 7,
                                      QStringLiteral("not valid"),
                                      QStringLiteral("AudioIsExternal")});
        const auto selection = select(response({mediaSource}));
        QVERIFY(selection.source);
        QCOMPARE(
            selection.source->media.transcodeReasons,
            (QStringList{QStringLiteral("DirectPlayError"), QStringLiteral("AudioIsExternal")}));
        // The query wins when it has reasons
        mediaSource.insert(
            QStringLiteral("TranscodingUrl"),
            QStringLiteral("/videos/x/master.m3u8?TranscodeReasons=VideoCodecNotSupported"));
        const auto both = select(response({mediaSource}));
        QVERIFY(both.source);
        QCOMPARE(both.source->media.transcodeReasons,
                 (QStringList{QStringLiteral("VideoCodecNotSupported")}));
    }

    void hostileTargetCodecsAreDropped()
    {
        const auto selection = select(response({transcodeSource(QStringLiteral(
            "VideoCodec=h264%3Brm%20-rf&AudioCodec=%3Cb%3E&TranscodeReasons=DirectPlayError"))}));
        QVERIFY(selection.source);
        QVERIFY(selection.source->media.targetVideoCodec.isEmpty());
        QVERIFY(selection.source->media.targetAudioCodec.isEmpty());
        QCOMPARE(selection.source->media.transcodeReasons,
                 (QStringList{QStringLiteral("DirectPlayError")}));
    }

    void streamTypeOfATranscodeFollowsTheUrlExtension()
    {
        const auto remux = select(response({withStreams(
            source(kSourceId, false, true, QStringLiteral("/videos/x/stream.mp4?VideoCodec=h264")),
            QJsonArray{videoStream()})}));
        QVERIFY(remux.source);
        QCOMPARE(remux.source->media.streamType, QStringLiteral("MP4"));
        const auto bare = select(response(
            {source(kSourceId, false, true, QStringLiteral("/videos/x/master?VideoCodec=h264"))}));
        QVERIFY(bare.source);
        QVERIFY(bare.source->media.streamType.isEmpty());
    }

    void theMediaInfoNeverHoldsTheUrlOrTheToken()
    {
        const auto selection = select(response({transcodeSource(
            QStringLiteral("VideoCodec=h264&TranscodeReasons=DirectPlayError&MediaSourceId=")
            + kSourceId)}));
        QVERIFY(selection.source);
        const PlaybackMediaInfo &media = selection.source->media;
        QStringList strings{media.streamType,       media.protocol,    media.targetVideoCodec,
                            media.targetAudioCodec, media.source.name, media.source.container};
        strings += media.transcodeReasons;
        for (const auto &stream : media.source.streams)
            strings << stream.type << stream.codec << stream.profile << stream.title
                    << stream.language << stream.pixelFormat << stream.videoRange
                    << stream.videoRangeType << stream.colorSpace << stream.colorPrimaries
                    << stream.colorTransfer << stream.channelLayout;
        for (const QString &text : strings) {
            QVERIFY2(!text.contains(QString::fromLatin1(kToken)), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("master.m3u8")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("jf.example")), qPrintable(text));
        }
    }

    void jellyfinIdGrammar()
    {
        QVERIFY(isValidJellyfinId(u"c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9"));
        QVERIFY(isValidJellyfinId(u"c7dd3b1a-c7dd-3b1a-9d42-f2ab74f8a1b9"));
        QVERIFY(isValidJellyfinId(QString(64, QLatin1Char('a'))));
        QVERIFY(!isValidJellyfinId(QString(65, QLatin1Char('a'))));
        QVERIFY(!isValidJellyfinId(u""));
        for (const QString &bad : {QStringLiteral("a/b"), QStringLiteral("a b"),
                                   QStringLiteral("a?b"), QStringLiteral("..")})
            QVERIFY2(!isValidJellyfinId(bad), qPrintable(bad));
    }

    void castSubtitleUrlHasTheTicksSegmentAndKeepsThePrefix()
    {
        const QUrl base(QStringLiteral("https://jf.example/jellyfin"));
        const QString item = QStringLiteral("0123456789abcdef0123456789abcdef");
        const QString source = QStringLiteral("fedcba9876543210fedcba9876543210");
        const QUrl url = castSubtitleUrl(base, item, source, 3, 900'000'000);
        QCOMPARE(url.toString(),
                 QStringLiteral("https://jf.example/jellyfin/Videos/%1/%2/Subtitles/3/900000000/"
                                "Stream.vtt")
                     .arg(item, source));
        QVERIFY(url.query().isEmpty());
        QVERIFY(castSubtitleUrl(base, QStringLiteral("../x"), source, 3, 0).isEmpty());
        QVERIFY(castSubtitleUrl(base, item, QStringLiteral("a/b"), 3, 0).isEmpty());
        QVERIFY(castSubtitleUrl(base, item, source, -1, 0).isEmpty());
        QVERIFY(castSubtitleUrl(base, item, source, 3, -1).isEmpty());
    }

    void textSubtitleCodecsAreTheOnesTheServerConvertsToWebVtt()
    {
        for (const QString &codec :
             {QStringLiteral("subrip"), QStringLiteral("SRT"), QStringLiteral("ass"),
              QStringLiteral("ssa"), QStringLiteral("vtt"), QStringLiteral("WebVTT"),
              QStringLiteral("mov_text"), QStringLiteral(" subrip ")})
            QVERIFY2(isTextSubtitleCodec(codec), qPrintable(codec));
        for (const QString &codec :
             {QStringLiteral("pgssub"), QStringLiteral("dvdsub"), QStringLiteral("dvbsub"),
              QStringLiteral("hdmv_pgs_subtitle"), QStringLiteral(""), QStringLiteral("unknown")})
            QVERIFY2(!isTextSubtitleCodec(codec), qPrintable(codec));
    }
};
QTEST_APPLESS_MAIN(TestMediaSourceSelection)
#include "test_media_source_selection.moc"
