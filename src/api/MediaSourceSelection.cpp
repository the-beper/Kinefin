// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/MediaSourceSelection.h"

#include <QJsonArray>
#include <QJsonValue>
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
    // Any other value is not echoed back.
    return QStringLiteral("Playback refused by the server");
}

bool flag(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isBool() && value.toBool();
}

// The server base path, still percent-encoded and without a trailing slash, as the prefix of every media path.
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
    // Host and port only: user info of the base must never reach a media URL.
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

bool carriesToken(const QUrl &url, QByteArrayView accessToken)
{
    if (accessToken.isEmpty())
        return false;
    const QString token = QString::fromUtf8(accessToken);
    return url.toString(QUrl::FullyEncoded).contains(token, Qt::CaseInsensitive)
           || url.toString(QUrl::FullyDecoded).contains(token, Qt::CaseInsensitive);
}

// A `TranscodingUrl` is root-relative and carries the access token as `ApiKey` (live-verified on 12.4): anything else, an absolute URL, a protocol-relative one, a path with dot segments, is refused, and the token query parameters are dropped.
std::optional<QUrl> transcodingUrl(const QUrl &serverBase, const QString &raw,
                                   QByteArrayView accessToken)
{
    if (!raw.startsWith(QLatin1Char('/')) || raw.startsWith(QStringLiteral("//"))
        || raw.contains(QLatin1Char('\\')) || raw.contains(QLatin1Char('#')))
        return std::nullopt;
    const QString path = raw.section(QLatin1Char('?'), 0, 0);
    // Judge each segment as the server will see it: percent-decoded dot segments, slashes, backslashes and control characters are refused.
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
} // namespace

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
    // Fail closed: the token check below needs the token, and nothing plays unauthenticated anyway.
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

    // Live and other sources that must be opened first are out of scope (movies and TV series only).
    QList<QJsonObject> candidates;
    for (const QJsonValue &value : sources.toArray()) {
        const QJsonObject source = value.toObject();
        const QJsonValue id = source.value(QStringLiteral("Id"));
        if (id.isString() && isValidJellyfinId(id.toString())
            && !flag(source, QStringLiteral("RequiresOpening")))
            candidates.append(source);
    }

    // Direct play first, across all sources: the static route serves the original file.
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
        return {result, {}};
    }

    // Direct stream alone is not used: the static URL would bypass the force-transcode rules, and the server's remux comes back as a TranscodingUrl.
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
        return {result, {}};
    }
    return failure(QStringLiteral("No playable media source"));
}
} // namespace Kinefin
