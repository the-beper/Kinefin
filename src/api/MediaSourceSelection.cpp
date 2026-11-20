// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/MediaSourceSelection.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrlQuery>

namespace Kinefin {
namespace {
SourceSelection failure(const QString &reason)
{
    return {std::nullopt, reason};
}

QString errorCodeReason(const QString &code)
{
    if (code == QStringLiteral("NotAllowed"))
        return QStringLiteral("Playback is not allowed");
    if (code == QStringLiteral("NoCompatibleStream"))
        return QStringLiteral("No compatible stream");
    if (code == QStringLiteral("RateLimitExceeded"))
        return QStringLiteral("Playback rate limit exceeded");
    // Any other value is not echoed back
    return QStringLiteral("Playback refused by the server");
}

bool flag(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isBool() && value.toBool();
}

// The server base path, still percent-encoded and without a trailing slash, as the prefix of every media path
QString basePath(const QUrl &serverBase)
{
    QString path = serverBase.path(QUrl::FullyEncoded);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

QUrl absoluteUrl(const QUrl &serverBase, const QString &pathAndQuery)
{
    QUrl url;
    url.setScheme(serverBase.scheme());
    // Host and port only: user info of the base must never reach a media URL
    url.setHost(serverBase.host());
    url.setPort(serverBase.port());
    url.setPath(basePath(serverBase) + pathAndQuery.section(QLatin1Char('?'), 0, 0),
                QUrl::StrictMode);
    const QString query = pathAndQuery.section(QLatin1Char('?'), 1);
    if (!query.isEmpty())
        url.setQuery(query, QUrl::StrictMode);
    return url;
}

PlaybackSource baseSource(const QString &itemId, const QString &mediaSourceId,
                          const QString &playSessionId, PlaybackSource::Method method)
{
    PlaybackSource source;
    source.itemId = itemId;
    source.mediaSourceId = mediaSourceId;
    source.playSessionId = playSessionId;
    source.method = method;
    return source;
}

// A `TranscodingUrl` is root-relative and carries the access token as `ApiKey` (live-verified on 12.4): anything else, an absolute URL, a protocol-relative one, a path with dot segments, is refused, and the token query parameters are dropped
std::optional<QUrl> transcodingUrl(const QUrl &serverBase, const QString &raw,
                                   QByteArrayView accessToken)
{
    if (!raw.startsWith(QLatin1Char('/')) || raw.startsWith(QStringLiteral("//"))
        || raw.contains(QLatin1Char('\\')) || raw.contains(QLatin1Char('#')))
        return std::nullopt;
    const QString path = raw.section(QLatin1Char('?'), 0, 0);
    // Judge each segment as the server will see it: percent-decoded dot segments, slashes, backslashes and control characters are refused
    for (const QString &segment : path.split(QLatin1Char('/'))) {
        const QString decoded = QUrl::fromPercentEncoding(segment.toUtf8());
        if (decoded == QStringLiteral("..") || decoded == QStringLiteral("."))
            return std::nullopt;
        for (const QChar character : decoded) {
            if (character == QLatin1Char('/') || character == QLatin1Char('\\')
                || character.unicode() < 0x20 || character.unicode() == 0x7f)
                return std::nullopt;
        }
    }
    QStringList kept;
    for (const QString &pair :
         raw.section(QLatin1Char('?'), 1).split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
        const QString key =
            QUrl::fromPercentEncoding(pair.section(QLatin1Char('='), 0, 0).toUtf8()).toLower();
        if (key != QStringLiteral("apikey") && key != QStringLiteral("api_key"))
            kept << pair;
    }
    const QUrl url = absoluteUrl(
        serverBase,
        path + (kept.isEmpty() ? QString() : QLatin1Char('?') + kept.join(QLatin1Char('&'))));
    if (!url.isValid() || carriesToken(url, accessToken))
        return std::nullopt;
    return url;
}

QString queryValue(const QUrlQuery &query, const QString &key)
{
    const auto items = query.queryItems(QUrl::FullyDecoded);
    for (const auto &item : items) {
        if (item.first.compare(key, Qt::CaseInsensitive) == 0)
            return item.second;
    }
    return {};
}

// The first entry of a comma list of codecs, when it is a plain name
QString firstCodec(const QString &list)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.-]{1,32}$"));
    const QString first = list.section(QLatin1Char(','), 0, 0).trimmed();
    return pattern.match(first).hasMatch() ? first : QString();
}

// Names of the server's `TranscodeReasons` enum: letters only, a few, each once
QStringList transcodeReasonNames(const QStringList &candidates)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z]{1,64}$"));
    QStringList names;
    for (const QString &candidate : candidates) {
        const QString name = candidate.trimmed();
        if (names.size() == PlaybackMediaInfo::MaxTranscodeReasons)
            break;
        if (pattern.match(name).hasMatch() && !names.contains(name))
            names.append(name);
    }
    return names;
}

QStringList reasonsOfSource(const QJsonObject &mediaSource)
{
    const QJsonValue value = mediaSource.value(QStringLiteral("TranscodeReasons"));
    QStringList candidates;
    if (value.isString()) {
        candidates = value.toString().left(2048).split(QLatin1Char(','));
    } else if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            if (candidates.size() == 4 * PlaybackMediaInfo::MaxTranscodeReasons)
                break;
            if (entry.isString())
                candidates.append(entry.toString().left(64));
        }
    }
    return transcodeReasonNames(candidates);
}

// `rawTranscodingUrl` is the server's string, read for its codec and reason parameters only
PlaybackMediaInfo mediaInfo(const QJsonObject &mediaSource, const QUrl &url,
                            const QString &rawTranscodingUrl)
{
    PlaybackMediaInfo info;
    info.source = parseMediaSource(mediaSource);
    info.protocol = url.scheme();
    if (rawTranscodingUrl.isEmpty()) {
        info.streamType = QStringLiteral("File");
        return info;
    }
    const QString extension = url.path().section(QLatin1Char('.'), -1).toLower();
    static const QRegularExpression plain(QStringLiteral("^[a-z0-9]{1,8}$"));
    info.streamType = extension == QLatin1String("m3u8")  ? QStringLiteral("HLS")
                      : plain.match(extension).hasMatch() ? extension.toUpper()
                                                          : QString();
    const QUrlQuery query(rawTranscodingUrl.section(QLatin1Char('?'), 1));
    info.targetVideoCodec = firstCodec(queryValue(query, QStringLiteral("VideoCodec")));
    info.targetAudioCodec = firstCodec(queryValue(query, QStringLiteral("AudioCodec")));
    info.transcodeReasons = transcodeReasonNames(
        queryValue(query, QStringLiteral("TranscodeReasons")).left(2048).split(QLatin1Char(',')));
    // Digits only and a sane range: a server value never becomes a number it did not spell out
    const auto bitrate = [&query](const QString &key) -> qint64 {
        static const QRegularExpression digits(
            QRegularExpression::anchoredPattern(QStringLiteral("[0-9]{4,10}")));
        const QString value = queryValue(query, key);
        if (!digits.match(value).hasMatch())
            return 0;
        const qint64 number = value.toLongLong();
        return number >= 1'000 && number <= 1'000'000'000 ? number : 0;
    };
    const qint64 video = bitrate(QStringLiteral("VideoBitrate"));
    const qint64 audio = bitrate(QStringLiteral("AudioBitrate"));
    info.targetBitrate = video > 0 ? video + audio : 0;
    if (info.transcodeReasons.isEmpty())
        info.transcodeReasons = reasonsOfSource(mediaSource);
    return info;
}

// The text subtitles the server delivers as files: its sidecar files, and in a transcode also the file's own streams, which the server extracts (12.4 sends them as External with a DeliveryUrl of the same shape); a direct play has those in the file it plays
void addExternalSubtitles(PlaybackSource &playback, const QJsonObject &mediaSource,
                          const QUrl &serverBase, QByteArrayView token)
{
    const bool transcode = playback.method == PlaybackSource::Method::Transcode;
    const QJsonArray streams = mediaSource.value(QStringLiteral("MediaStreams")).toArray();
    for (const QJsonValue &value : streams) {
        if (playback.externalSubtitles.size() == 32)
            return;
        const QJsonObject stream = value.toObject();
        if (stream.value(QStringLiteral("Type")).toString() != QLatin1String("Subtitle")
            || (!flag(stream, QStringLiteral("IsExternal")) && !transcode)
            || !flag(stream, QStringLiteral("IsTextSubtitleStream"))
            || stream.value(QStringLiteral("DeliveryMethod")).toString()
                   != QLatin1String("External"))
            continue;
        const int index = stream.value(QStringLiteral("Index")).toInt(-1);
        if (index < 0 || index >= 4096)
            continue;
        const QString raw = stream.value(QStringLiteral("DeliveryUrl")).toString();
        const std::optional<QUrl> url = transcodingUrl(serverBase, raw, token);
        const QString allowed = serverBase.path().remove(QRegularExpression(QStringLiteral("/+$")))
                                + QStringLiteral("/Videos/");
        if (!url || !url->path().startsWith(allowed) || url->hasQuery())
            continue;
        ExternalSubtitle subtitle;
        subtitle.index = index;
        subtitle.url = *url;
        subtitle.language = stream.value(QStringLiteral("Language")).toString().left(32);
        subtitle.label = stream.value(QStringLiteral("DisplayTitle")).toString().left(100);
        if (subtitle.label.isEmpty())
            subtitle.label = subtitle.language;
        const QString extension = url->path().section(QLatin1Char('.'), -1).toLower();
        if (extension != QLatin1String("vtt") && extension != QLatin1String("srt")
            && extension != QLatin1String("ass") && extension != QLatin1String("ssa"))
            continue;
        subtitle.format = extension;
        subtitle.extracted = !flag(stream, QStringLiteral("IsExternal"));
        playback.externalSubtitles.append(subtitle);
    }
}
} // namespace

bool carriesToken(const QUrl &url, QByteArrayView accessToken)
{
    if (accessToken.isEmpty())
        return false;
    const QString token = QString::fromUtf8(accessToken);
    // QUrl refuses FullyDecoded for a whole URL: the decoded form is the encoded one, unescaped
    return url.toString(QUrl::FullyEncoded).contains(token, Qt::CaseInsensitive)
           || QString::fromUtf8(QByteArray::fromPercentEncoding(url.toEncoded()))
                  .contains(token, Qt::CaseInsensitive);
}

bool isValidJellyfinId(QStringView id)
{
    if (id.isEmpty() || id.size() > 64)
        return false;
    for (const QChar character : id) {
        const char16_t c = character.unicode();
        const bool allowed = (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z')
                             || (c >= u'A' && c <= u'Z') || c == u'-';
        if (!allowed)
            return false;
    }
    return true;
}

const StreamSummary *pickStream(const SourceSummary &source, QStringView type, int index)
{
    const StreamSummary *fallback = nullptr;
    const StreamSummary *first = nullptr;
    for (const StreamSummary &stream : source.streams) {
        if (stream.type != type)
            continue;
        if (index >= 0 && stream.index == index)
            return &stream;
        if (!fallback && stream.isDefault)
            fallback = &stream;
        if (!first)
            first = &stream;
    }
    return fallback ? fallback : first;
}

SourceSelection selectPlaybackSource(const QJsonObject &response, const QString &itemId,
                                     const QUrl &serverBase, QByteArrayView accessToken)
{
    const bool baseUsable = serverBase.isValid() && !serverBase.host().isEmpty()
                            && serverBase.userInfo().isEmpty()
                            && (serverBase.scheme() == QStringLiteral("http")
                                || serverBase.scheme() == QStringLiteral("https"));
    if (!baseUsable)
        return failure(QStringLiteral("Invalid server address"));
    if (!isValidJellyfinId(itemId))
        return failure(QStringLiteral("Invalid item id"));
    // Fail closed: the token check below needs the token, and nothing plays unauthenticated anyway
    if (accessToken.isEmpty())
        return failure(QStringLiteral("Not authenticated"));

    const QJsonValue code = response.value(QStringLiteral("ErrorCode"));
    if (code.isString() && !code.toString().isEmpty())
        return failure(errorCodeReason(code.toString()));

    const QJsonValue sources = response.value(QStringLiteral("MediaSources"));
    const QJsonValue session = response.value(QStringLiteral("PlaySessionId"));
    if (!sources.isArray() || sources.toArray().isEmpty() || !session.isString()
        || !isValidJellyfinId(session.toString()))
        return failure(QStringLiteral("Invalid playback response"));
    const QString playSessionId = session.toString();

    // Live and other sources that must be opened first are out of scope (movies and TV series only)
    QList<QJsonObject> candidates;
    for (const QJsonValue &value : sources.toArray()) {
        const QJsonObject source = value.toObject();
        const QJsonValue id = source.value(QStringLiteral("Id"));
        if (id.isString() && isValidJellyfinId(id.toString())
            && !flag(source, QStringLiteral("RequiresOpening")))
            candidates.append(source);
    }

    // Direct play first, across all sources: the static route serves the original file
    for (const QJsonObject &source : candidates) {
        if (!flag(source, QStringLiteral("SupportsDirectPlay")))
            continue;
        const QString mediaSourceId = source.value(QStringLiteral("Id")).toString();
        QUrl url = absoluteUrl(serverBase,
                               QStringLiteral("/Videos/") + itemId + QStringLiteral("/stream"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("static"), QStringLiteral("true"));
        query.addQueryItem(QStringLiteral("mediaSourceId"), mediaSourceId);
        query.addQueryItem(QStringLiteral("playSessionId"), playSessionId);
        url.setQuery(query);
        PlaybackSource result =
            baseSource(itemId, mediaSourceId, playSessionId, PlaybackSource::Method::Direct);
        result.url = url;
        result.media = mediaInfo(source, url, {});
        addExternalSubtitles(result, source, serverBase, accessToken);
        return {result, {}};
    }

    // Direct stream alone is not used: the static URL would bypass the force-transcode rules, and the server's remux comes back as a TranscodingUrl
    for (const QJsonObject &source : candidates) {
        if (!flag(source, QStringLiteral("SupportsTranscoding")))
            continue;
        const QJsonValue raw = source.value(QStringLiteral("TranscodingUrl"));
        if (!raw.isString())
            continue;
        const std::optional<QUrl> url = transcodingUrl(serverBase, raw.toString(), accessToken);
        if (!url)
            continue;
        PlaybackSource result = baseSource(itemId, source.value(QStringLiteral("Id")).toString(),
                                           playSessionId, PlaybackSource::Method::Transcode);
        result.url = *url;
        result.media = mediaInfo(source, *url, raw.toString());
        addExternalSubtitles(result, source, serverBase, accessToken);
        return {result, {}};
    }
    return failure(QStringLiteral("No playable media source"));
}
} // namespace Kinefin
