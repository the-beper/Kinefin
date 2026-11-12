// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QtTest>

#include "api/MediaSourceSelection.h"

using Kinefin::isValidJellyfinId;
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
};
QTEST_APPLESS_MAIN(TestMediaSourceSelection)
#include "test_media_source_selection.moc"
