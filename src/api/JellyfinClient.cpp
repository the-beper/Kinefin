// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "JellyfinClient.h"

#include <QDate>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslError>
#include <QUrlQuery>

#include <algorithm>

namespace Kinefin {
namespace {
constexpr int MaxResponseBytes = 1'048'576;
constexpr int MaxPublicRedirects = 3;
constexpr int MaxRecommendationCategories = 20;
constexpr int MaxRecommendationItems = 100;
constexpr int MaxSuggestions = 100;
constexpr int MaxUpcomingEpisodes = 100;
// The server asks the provider (OpenSubtitles) while the request waits and sends nothing meanwhile: the usual 15 s transfer timeout is too short for that round trip
constexpr int RemoteSubtitleTimeoutMs = 60'000;
const QString publicSuffix = QStringLiteral("/System/Info/Public");

bool validServerUrl(const QUrl &url)
{
    return url.isValid() && !url.isRelative()
           && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
           && !url.host().isEmpty() && url.userName().isEmpty() && url.password().isEmpty()
           && !url.hasQuery() && !url.hasFragment() && url.port(-1) != 0;
}

bool validHeaderField(const QString &field)
{
    if (field.isEmpty() || field.size() > 128)
        return false;
    for (const QChar c : field) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f || c == QLatin1Char('"')
            || c == QLatin1Char('\\'))
            return false;
    }
    return true;
}

QUrl normalizedBase(QUrl url)
{
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    url.setPath(path);
    return url;
}

// Public discovery is unauthenticated, so a scheme/port upgrade is safe on the same hostname. Never accept a different host or a TLS downgrade
bool allowedPublicRedirect(const QUrl &from, const QUrl &to)
{
    if (!validServerUrl(to) || from.host().compare(to.host(), Qt::CaseInsensitive) != 0)
        return false;
    if (from.scheme() == QLatin1String("https") && to.scheme() != QLatin1String("https"))
        return false;
    return to.path().endsWith(publicSuffix);
}

QUrl baseForPublicEndpoint(QUrl url)
{
    if (!validServerUrl(url) || !url.path().endsWith(publicSuffix))
        return {};
    QString path = url.path();
    path.chop(publicSuffix.size());
    url.setPath(path);
    return normalizedBase(url);
}

bool isLoopback(const QUrl &url)
{
    if (url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
        return true;
    QHostAddress address;
    return address.setAddress(url.host()) && address.isLoopback();
}

bool parseObject(QNetworkReply *reply, QJsonObject *object)
{
    const QByteArray bytes = reply->read(MaxResponseBytes + 1);
    if (bytes.size() > MaxResponseBytes)
        return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    *object = document.object();
    return true;
}

bool parseArray(QNetworkReply *reply, QJsonArray *array)
{
    const QByteArray bytes = reply->read(MaxResponseBytes + 1);
    if (bytes.size() > MaxResponseBytes)
        return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
        return false;
    // Every entry must be an object: a bare array of anything else is malformed contract
    for (const QJsonValue &value : document.array()) {
        if (!value.isObject())
            return false;
    }
    *array = document.array();
    return true;
}

// The query token vocabularies: exactly the 12.4 OpenAPI enums (ItemFields, BaseItemKind, ImageType) plus the upstream ItemFilter enum the `filters` parameter rides (the spec leaves it a plain string). A member outside its set is refused without a request
bool isValidTokenFromSet(const QString &token, std::initializer_list<QStringView> set)
{
    for (QStringView name : set) {
        if (QStringView(token.constData(), qsizetype(token.size())) == name)
            return true;
    }
    return false;
}

bool isValidItemFieldToken(const QString &token)
{
    return isValidTokenFromSet(token, {
                                          u"AirTime",
                                          u"CanDelete",
                                          u"CanDownload",
                                          u"ChannelInfo",
                                          u"Chapters",
                                          u"Trickplay",
                                          u"ChildCount",
                                          u"CumulativeRunTimeTicks",
                                          u"CustomRating",
                                          u"DateCreated",
                                          u"DateLastMediaAdded",
                                          u"DisplayPreferencesId",
                                          u"Etag",
                                          u"ExternalUrls",
                                          u"Genres",
                                          u"ItemCounts",
                                          u"MediaSourceCount",
                                          u"MediaSources",
                                          u"OriginalTitle",
                                          u"Overview",
                                          u"ParentId",
                                          u"Path",
                                          u"People",
                                          u"PlayAccess",
                                          u"ProductionLocations",
                                          u"ProviderIds",
                                          u"PrimaryImageAspectRatio",
                                          u"RecursiveItemCount",
                                          u"Settings",
                                          u"SeriesStudio",
                                          u"SortName",
                                          u"SpecialEpisodeNumbers",
                                          u"Studios",
                                          u"Taglines",
                                          u"Tags",
                                          u"RemoteTrailers",
                                          u"MediaStreams",
                                          u"SeasonUserData",
                                          u"DateLastRefreshed",
                                          u"DateLastSaved",
                                          u"RefreshState",
                                          u"ChannelImage",
                                          u"EnableMediaSourceDisplay",
                                          u"Width",
                                          u"Height",
                                          u"ExtraIds",
                                          u"LocalTrailerCount",
                                          u"IsHD",
                                          u"SpecialFeatureCount",
                                      });
}

bool isValidBaseItemKindToken(const QString &token)
{
    return isValidTokenFromSet(token, {
                                          u"AggregateFolder",
                                          u"Audio",
                                          u"AudioBook",
                                          u"BasePluginFolder",
                                          u"Book",
                                          u"BoxSet",
                                          u"Channel",
                                          u"ChannelFolderItem",
                                          u"CollectionFolder",
                                          u"Episode",
                                          u"Folder",
                                          u"Genre",
                                          u"ManualPlaylistsFolder",
                                          u"Movie",
                                          u"LiveTvChannel",
                                          u"LiveTvProgram",
                                          u"MusicAlbum",
                                          u"MusicArtist",
                                          u"MusicGenre",
                                          u"MusicVideo",
                                          u"Person",
                                          u"Photo",
                                          u"PhotoAlbum",
                                          u"Playlist",
                                          u"PlaylistsFolder",
                                          u"Program",
                                          u"Recording",
                                          u"Season",
                                          u"Series",
                                          u"Studio",
                                          u"Trailer",
                                          u"TvChannel",
                                          u"TvProgram",
                                          u"UserRootFolder",
                                          u"UserView",
                                          u"Video",
                                          u"Year",
                                      });
}

bool isValidItemFilterToken(const QString &token)
{
    return isValidTokenFromSet(token, {
                                          u"IsFolder",
                                          u"IsNotFolder",
                                          u"IsUnplayed",
                                          u"IsPlayed",
                                          u"IsFavorite",
                                          u"IsResumable",
                                          u"Likes",
                                          u"Dislikes",
                                          u"IsFavoriteOrLikes",
                                      });
}

bool isValidSortOrderToken(const QString &token)
{
    return token == QLatin1String("Ascending") || token == QLatin1String("Descending");
}

bool isValidSortByToken(const QString &token)
{
    // The spec leaves sortBy a free string; the shape rule keeps query injection out
    if (token.isEmpty() || token.size() > 64)
        return false;
    for (const QChar c : token) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
            return false;
    }
    return true;
}

bool isValidImageTypeToken(const QString &token)
{
    return isValidTokenFromSet(token, {
                                          u"Primary",
                                          u"Art",
                                          u"Backdrop",
                                          u"Banner",
                                          u"Logo",
                                          u"Thumb",
                                          u"Disc",
                                          u"Box",
                                          u"Screenshot",
                                          u"Menu",
                                          u"Chapter",
                                          u"BoxRear",
                                          u"Profile",
                                      });
}

bool isValidImageFormatToken(const QString &token)
{
    return token == QLatin1String("jpg") || token == QLatin1String("gif")
           || token == QLatin1String("png") || token == QLatin1String("webp");
}

bool isValidImageTagToken(const QString &token)
{
    if (token.size() > 64)
        return false;
    for (const QChar c : token) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('-'))
            return false;
    }
    return true;
}

// The avatar tag of a user: ASCII letters, digits and '-' only, so it is safe in a URL and in a cache key
bool isValidUserImageTag(QStringView tag)
{
    if (tag.isEmpty() || tag.size() > 64)
        return false;
    for (const QChar character : tag) {
        const char16_t c = character.unicode();
        const bool allowed = (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z')
                             || (c >= u'A' && c <= u'Z') || c == u'-';
        if (!allowed)
            return false;
    }
    return true;
}

// A user name from the server: bounded, one line, no control characters (remote text, shown as plain text); empty when it does not qualify
QString validUserName(const QJsonValue &value)
{
    const QString name = value.toString();
    if (name.trimmed().isEmpty() || name.size() > MaxUserNameChars)
        return {};
    for (const QChar c : name) {
        const char16_t u = c.unicode();
        if (u < 0x20 || (u >= 0x7f && u <= 0x9f) || u == 0x2028 || u == 0x2029)
            return {};
    }
    return name;
}

QString validUserImageTag(const QJsonValue &value)
{
    const QString tag = value.toString();
    return isValidUserImageTag(tag) ? tag : QString();
}

// `Policy.IsAdministrator` of a UserDto: false when the policy or the flag is absent or the flag is not a bool
bool userIsAdministrator(const QJsonObject &user)
{
    const QJsonValue flag =
        user.value(QStringLiteral("Policy")).toObject().value(QStringLiteral("IsAdministrator"));
    return flag.isBool() && flag.toBool();
}

// `Policy.EnableContentDownloading` of a UserDto: false when the policy or the flag is absent or the flag is not a bool
bool userCanDownload(const QJsonObject &user)
{
    const QJsonValue flag = user.value(QStringLiteral("Policy"))
                                .toObject()
                                .value(QStringLiteral("EnableContentDownloading"));
    return flag.isBool() && flag.toBool();
}

// `Policy.EnableSubtitleManagement` of a UserDto: false when absent or not a bool
bool userCanManageSubtitles(const QJsonObject &user)
{
    const QJsonValue flag = user.value(QStringLiteral("Policy"))
                                .toObject()
                                .value(QStringLiteral("EnableSubtitleManagement"));
    return flag.isBool() && flag.toBool();
}

// `Policy.SyncPlayAccess` of a UserDto: one of the three values the server knows, else empty
QString userSyncPlayAccess(const QJsonObject &user)
{
    const QString access = user.value(QStringLiteral("Policy"))
                               .toObject()
                               .value(QStringLiteral("SyncPlayAccess"))
                               .toString();
    return access == QLatin1String("CreateAndJoinGroups") || access == QLatin1String("JoinGroups")
                   || access == QLatin1String("None")
               ? access
               : QString();
}

// Splits a comma-separated query value into tokens, refusing empty members
bool splitTokenList(const QString &value, QList<QString> *tokens)
{
    *tokens = value.split(QLatin1Char(','), Qt::KeepEmptyParts);
    for (const QString &token : *tokens) {
        if (token.isEmpty() || token.trimmed() != token)
            return false;
    }
    return true;
}

// Qt warns about any POST without a content type, an empty one included
QNetworkReply *postEmpty(QNetworkAccessManager &network, QNetworkRequest request)
{
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return network.post(request, QByteArray());
}

// A list of Jellyfin ids for a playlist call: at most MaxPlaylistWriteIds, each valid, and not empty unless `allowEmpty`
bool validPlaylistIds(const QStringList &ids, bool allowEmpty)
{
    if ((ids.isEmpty() && !allowEmpty) || ids.size() > MaxPlaylistWriteIds)
        return false;
    for (const QString &id : ids) {
        if (!isValidJellyfinId(id))
            return false;
    }
    return true;
}

// The key a playlist id is tracked by: a GUID may be written dashed or dashless, and in either case
QString playlistKey(const QString &playlistId)
{
    return QString(playlistId).remove(QLatin1Char('-')).toLower();
}

// A create has no playlist yet: it takes one slot of its own, which no id can collide with
QString playlistWriteKey(bool isCreate, const QString &playlistId)
{
    return isCreate ? QStringLiteral("+create") : playlistKey(playlistId);
}
} // namespace

bool isValidHeaderField(const QString &field)
{
    return validHeaderField(field);
}

bool isValidFilterText(const QString &text)
{
    if (text.isEmpty() || text.size() > MaxFilterValueChars)
        return false;
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (u < 0x20 || (u >= 0x7f && u <= 0x9f) || u == 0x2028 || u == 0x2029 || u == u'|')
            return false;
    }
    return true;
}

QString normalizedPlaylistName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || !QStringView(trimmed).isValidUtf16())
        return {};
    qsizetype codePoints = 0;
    bool visible = false;
    for (qsizetype i = 0; i < trimmed.size(); ++i) {
        char32_t cp = trimmed.at(i).unicode();
        if (QChar::isHighSurrogate(cp) && i + 1 < trimmed.size()) {
            cp = QChar::surrogateToUcs4(trimmed.at(i), trimmed.at(i + 1));
            ++i;
        }
        if (++codePoints > MaxPlaylistNameChars)
            return {};
        switch (QChar::category(cp)) {
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Other_Surrogate:
        case QChar::Other_NotAssigned:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph:
            return {};
        default:
            break;
        }
        if (QChar::isNonCharacter(cp))
            return {};
        visible = visible || !QChar::isSpace(cp);
    }
    return visible ? trimmed : QString();
}

JellyfinClient::JellyfinClient(QObject *parent) : QObject(parent)
{
    m_reportRetryTimer.setSingleShot(true);
    connect(&m_reportRetryTimer, &QTimer::timeout, this, [this] { sendNextPlaybackReport(); });
    connect(this, &JellyfinClient::loginSucceeded, this, [this] { reportCapabilities(); });
    // Every request of the client goes through m_network. The error is accepted only while the setting is on; otherwise it is remembered, so a reply that ends in an error afterwards is told from a plain failure
    connect(&m_network, &QNetworkAccessManager::sslErrors, this, [this](QNetworkReply *reply) {
        if (m_ignoreCertificateErrors)
            reply->ignoreSslErrors();
        else
            reply->setProperty("certificateRejected", true);
    });
}

JellyfinClient::~JellyfinClient()
{
    disconnectFromServer();
}

bool JellyfinClient::setDeviceIdentity(const QString &name, const QString &id,
                                       const QString &version)
{
    if (m_state == State::Authenticating || m_state == State::Authenticated
        || m_state == State::QuickStarting || m_state == State::QuickPending
        || m_state == State::QuickAuthenticating || !validHeaderField(name) || !validHeaderField(id)
        || !validHeaderField(version))
        return false;
    m_deviceName = name;
    m_deviceId = id;
    m_version = version;
    return true;
}

void JellyfinClient::setIgnoreCertificateErrors(bool ignore)
{
    if (ignore == m_ignoreCertificateErrors)
        return;
    m_ignoreCertificateErrors = ignore;
    // A kept-alive connection accepted with an invalid certificate would carry the next requests without a new handshake: off, the pool starts over
    if (!ignore)
        m_network.clearConnectionCache();
    Q_EMIT ignoreCertificateErrorsChanged(ignore);
}

void JellyfinClient::setAllowInsecureHttp(bool allowed)
{
    // This opt-in is scoped to the current server and resets on disconnect
    if (m_state == State::Disconnected || m_state == State::Probing || m_state == State::Ready)
        m_allowInsecureHttp = allowed;
}

bool JellyfinClient::connectToServer(const QUrl &baseUrl)
{
    if (!validServerUrl(baseUrl))
        return false;
    disconnectFromServer();
    m_base = normalizedBase(baseUrl);
    m_state = State::Probing;
    startProbe(endpoint(publicSuffix), m_generation, 0);
    return true;
}

void JellyfinClient::startProbe(const QUrl &url, quint64 generation, int redirects)
{
    // The sslErrors of the manager (see the constructor) fire only for a certificate that failed verification, never for a port that does not speak TLS
    QNetworkReply *reply = m_network.get(requestFor(url, false));
    attach(reply, generation, [this, generation, redirects](QNetworkReply *finished) {
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (finished->error() != QNetworkReply::NoError
            && finished->property("certificateRejected").toBool()) {
            failProbe(0, QStringLiteral("Server certificate rejected"), true);
            return;
        }
        const QVariant redirect = finished->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (status >= 300 && status < 400 && redirect.isValid()) {
            const QUrl target = finished->url().resolved(redirect.toUrl());
            if (redirects >= MaxPublicRedirects
                || !allowedPublicRedirect(finished->url(), target)) {
                failProbe(status, QStringLiteral("Unsafe or excessive server redirect"));
                return;
            }
            startProbe(target, generation, redirects + 1);
            return;
        }
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            failProbe(status, QStringLiteral("Server identification request failed"));
            return;
        }
        QJsonObject data;
        if (!parseObject(finished, &data)
            || data.value(QStringLiteral("Id")).toString().trimmed().isEmpty()) {
            failProbe(status, QStringLiteral("Invalid Jellyfin server identity"));
            return;
        }
        const QUrl resolvedBase = baseForPublicEndpoint(finished->url());
        if (resolvedBase.isEmpty()) {
            failProbe(status, QStringLiteral("Invalid resolved API base"));
            return;
        }
        m_base = resolvedBase;
        m_serverId = data.value(QStringLiteral("Id")).toString();
        m_state = State::Ready;
        Q_EMIT serverReady(m_serverId);
    });
}

bool JellyfinClient::transportAllowsAuthentication() const
{
    return m_base.scheme() == QLatin1String("https") || isLoopback(m_base) || m_allowInsecureHttp;
}

QByteArray JellyfinClient::identityAuthorization() const
{
    return QByteArray("MediaBrowser Client=\"Kinefin\", Device=\"") + m_deviceName.toUtf8()
           + "\", DeviceId=\"" + m_deviceId.toUtf8() + "\", Version=\"" + m_version.toUtf8() + "\"";
}

QByteArray JellyfinClient::tokenAuthorization() const
{
    return identityAuthorization() + ", Token=\"" + m_token + "\"";
}

bool JellyfinClient::authenticate(const QString &username, const QString &password)
{
    if (m_state != State::Ready || m_deviceId.isEmpty() || username.isEmpty() || password.isEmpty())
        return false;
    if (!transportAllowsAuthentication()) {
        Q_EMIT requestFailed(
            0, QStringLiteral("Credentials require HTTPS or an explicit insecure-HTTP opt-in"));
        return false;
    }
    m_state = State::Authenticating;
    const QJsonObject data{{QStringLiteral("Username"), username},
                           {QStringLiteral("Pw"), password}};
    QNetworkRequest request =
        requestFor(endpoint(QStringLiteral("/Users/AuthenticateByName")), false);
    request.setRawHeader("Authorization", identityAuthorization());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply =
        m_network.post(request, QJsonDocument(data).toJson(QJsonDocument::Compact));
    attach(reply, m_generation,
           [this](QNetworkReply *finished) { finishAuthentication(finished); });
    return true;
}

bool JellyfinClient::initiateQuickConnect()
{
    if (m_state != State::Ready || m_deviceId.isEmpty())
        return false;
    // Both the eventual bearer token and the Quick Connect secret are credentials
    if (!transportAllowsAuthentication()) {
        Q_EMIT requestFailed(
            0, QStringLiteral("Quick Connect requires HTTPS or an explicit insecure-HTTP opt-in"));
        return false;
    }
    m_state = State::QuickStarting;
    QNetworkRequest request = requestFor(endpoint(QStringLiteral("/QuickConnect/Initiate")), false);
    request.setRawHeader("Authorization", identityAuthorization());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_network.post(request, QByteArrayLiteral("{}"));
    attach(reply, m_generation, [this](QNetworkReply *finished) {
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            m_state = State::Ready;
            Q_EMIT requestFailed(status, QStringLiteral("Quick Connect initiation failed"));
            return;
        }
        QJsonObject data;
        if (!parseObject(finished, &data)) {
            m_state = State::Ready;
            Q_EMIT requestFailed(status,
                                 QStringLiteral("Invalid Quick Connect initiation response"));
            return;
        }
        const QString code = data.value(QStringLiteral("Code")).toString();
        const QString secret = data.value(QStringLiteral("Secret")).toString();
        const QJsonValue approved = data.value(QStringLiteral("Authenticated"));
        if (!validHeaderField(code) || code.size() > 32 || !validHeaderField(secret)
            || (!approved.isUndefined() && !approved.isBool()) || approved.toBool()) {
            m_state = State::Ready;
            Q_EMIT requestFailed(status, QStringLiteral("Invalid Quick Connect pairing identity"));
            return;
        }
        m_quickConnectSecret = secret.toUtf8();
        m_state = State::QuickPending;
        // Expose only the short user-facing pairing code, never the secret
        Q_EMIT quickConnectCodeReady(code);
    });
    return true;
}

bool JellyfinClient::pollQuickConnect()
{
    if (m_state != State::QuickPending || m_quickPollInFlight || m_quickConnectSecret.isEmpty())
        return false;
    QUrl url = endpoint(QStringLiteral("/QuickConnect/Connect"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("secret"), QString::fromUtf8(m_quickConnectSecret));
    url.setQuery(query);
    m_quickPollInFlight = true;
    QNetworkReply *reply = m_network.get(requestFor(url, false));
    attach(reply, m_generation, [this](QNetworkReply *finished) {
        m_quickPollInFlight = false;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            cancelQuickConnect();
            Q_EMIT requestFailed(status, QStringLiteral("Quick Connect status request failed"));
            return;
        }
        QJsonObject data;
        if (!parseObject(finished, &data)
            || !data.value(QStringLiteral("Authenticated")).isBool()) {
            cancelQuickConnect();
            Q_EMIT requestFailed(status, QStringLiteral("Invalid Quick Connect status response"));
            return;
        }
        const QJsonValue echoedSecret = data.value(QStringLiteral("Secret"));
        if (!echoedSecret.isUndefined()
            && (!echoedSecret.isString()
                || echoedSecret.toString().toUtf8() != m_quickConnectSecret)) {
            cancelQuickConnect();
            Q_EMIT requestFailed(status,
                                 QStringLiteral("Mismatched Quick Connect pairing identity"));
            return;
        }
        if (!data.value(QStringLiteral("Authenticated")).toBool()) {
            Q_EMIT quickConnectAwaitingApproval();
            return;
        }
        exchangeQuickConnectSecret();
    });
    return true;
}

void JellyfinClient::exchangeQuickConnectSecret()
{
    if (m_state != State::QuickPending || m_quickConnectSecret.isEmpty())
        return;
    m_state = State::QuickAuthenticating;
    QNetworkRequest request =
        requestFor(endpoint(QStringLiteral("/Users/AuthenticateWithQuickConnect")), false);
    request.setRawHeader("Authorization", identityAuthorization());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject data{{QStringLiteral("Secret"), QString::fromUtf8(m_quickConnectSecret)}};
    QNetworkReply *reply =
        m_network.post(request, QJsonDocument(data).toJson(QJsonDocument::Compact));
    attach(reply, m_generation,
           [this](QNetworkReply *finished) { finishAuthentication(finished); });
}

void JellyfinClient::finishAuthentication(QNetworkReply *finished)
{
    const bool quick = m_state == State::QuickAuthenticating;
    const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    clearQuickConnectSecret();
    m_state = State::Ready;
    if (status != 200 || finished->error() != QNetworkReply::NoError) {
        Q_EMIT requestFailed(status, quick ? QStringLiteral("Quick Connect authentication failed")
                                           : QStringLiteral("Authentication failed"));
        return;
    }
    QJsonObject result;
    if (!parseObject(finished, &result)) {
        Q_EMIT requestFailed(status, QStringLiteral("Invalid authentication response"));
        return;
    }
    const QString token = result.value(QStringLiteral("AccessToken")).toString();
    const QJsonObject user = result.value(QStringLiteral("User")).toObject();
    const QString userId = user.value(QStringLiteral("Id")).toString();
    const QString reportedServerId = result.value(QStringLiteral("ServerId")).toString();
    if (token.isEmpty() || userId.isEmpty() || reportedServerId != m_serverId
        || !validHeaderField(token)) {
        Q_EMIT requestFailed(status, QStringLiteral("Invalid authentication identity"));
        return;
    }
    m_token = token.toUtf8();
    m_userId = userId;
    m_userName = validUserName(user.value(QStringLiteral("Name")));
    m_userImageTag = validUserImageTag(user.value(QStringLiteral("PrimaryImageTag")));
    m_isAdministrator = userIsAdministrator(user);
    m_canDownload = userCanDownload(user);
    m_canManageSubtitles = userCanManageSubtitles(user);
    m_syncPlayAccess = userSyncPlayAccess(user);
    m_state = State::Authenticated;
    Q_EMIT loginSucceeded(m_userId);
}

void JellyfinClient::cancelQuickConnect()
{
    if (m_state != State::QuickStarting && m_state != State::QuickPending
        && m_state != State::QuickAuthenticating)
        return;
    abortPending();
    clearQuickConnectSecret();
    m_state = State::Ready;
}

bool JellyfinClient::isQuickConnectPending() const
{
    return m_state == State::QuickStarting || m_state == State::QuickPending
           || m_state == State::QuickAuthenticating;
}

void JellyfinClient::clearQuickConnectSecret()
{
    m_quickConnectSecret.fill('\0');
    m_quickConnectSecret.clear();
    m_quickPollInFlight = false;
}

namespace {
// The image types a card can show, one tag per type, like jellyfin-web asks for them
void addImageTypeParameters(QUrlQuery &wire, const QString &extraType = QString())
{
    wire.addQueryItem(QStringLiteral("enableImageTypes"),
                      extraType.isEmpty()
                          ? QStringLiteral("Primary,Backdrop,Thumb")
                          : QString(QStringLiteral("Primary,Backdrop,Thumb,") + extraType));
    wire.addQueryItem(QStringLiteral("imageTypeLimit"), QStringLiteral("1"));
}

// A text of a pipe-delimited filter list, percent-encoded here once: QUrlQuery keeps a %XX of its input as it is, so a '%' or '+' of the text must already be escaped
QString encodedText(const QString &text)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

// The server-side library filters of an `ItemQuery`: AND between the parameters, OR inside each list the server reads. Everything here was checked by validListingQuery
void addLibraryFilterParameters(QUrlQuery &wire, const ItemQuery &query)
{
    const auto addTrue = [&wire](const char *name, bool set) {
        if (set)
            wire.addQueryItem(QLatin1String(name), QStringLiteral("true"));
    };
    addTrue("hasSubtitles", query.hasSubtitles);
    addTrue("hasTrailer", query.hasTrailer);
    addTrue("hasSpecialFeature", query.hasSpecialFeature);
    addTrue("hasThemeSong", query.hasThemeSong);
    addTrue("hasThemeVideo", query.hasThemeVideo);
    if (query.isHd.has_value())
        wire.addQueryItem(QStringLiteral("isHd"),
                          *query.isHd ? QStringLiteral("true") : QStringLiteral("false"));
    addTrue("is4K", query.is4K);
    addTrue("is3D", query.is3D);
    if (!query.videoTypes.isEmpty())
        wire.addQueryItem(QStringLiteral("videoTypes"), query.videoTypes.join(QLatin1Char(',')));
    if (!query.seriesStatus.isEmpty())
        wire.addQueryItem(QStringLiteral("seriesStatus"),
                          query.seriesStatus.join(QLatin1Char(',')));
    if (!query.studioIds.isEmpty())
        wire.addQueryItem(QStringLiteral("studioIds"), query.studioIds.join(QLatin1Char('|')));
    if (!query.tags.isEmpty())
        wire.addQueryItem(QStringLiteral("tags"), encodedText(query.tags.join(QLatin1Char('|'))));
    if (!query.officialRatings.isEmpty())
        wire.addQueryItem(QStringLiteral("officialRatings"),
                          encodedText(query.officialRatings.join(QLatin1Char('|'))));
    if (!query.years.isEmpty()) {
        QStringList years;
        for (const int year : query.years)
            years.append(QString::number(year));
        wire.addQueryItem(QStringLiteral("years"), years.join(QLatin1Char(',')));
    }
    if (query.parentIndexNumber >= 0)
        wire.addQueryItem(QStringLiteral("parentIndexNumber"),
                          QString::number(query.parentIndexNumber));
    addTrue("isMissing", query.isMissing);
    addTrue("isUnaired", query.isUnaired);
}

// The /Items query of one item listing, or when `query.itemIds` is set an ids= resolution that carries only the ids and the fields (the details-refresh form): documented by ItemQuery. `userId` rides along on every listing call
QUrlQuery listingQuery(const ItemQuery &query, const QString &userId)
{
    QUrlQuery wire;
    if (!query.itemIds.isEmpty()) {
        wire.addQueryItem(QStringLiteral("ids"), query.itemIds.join(QLatin1Char(',')));
    } else {
        if (!query.parentId.isEmpty())
            wire.addQueryItem(QStringLiteral("parentId"), query.parentId);
        if (!query.includeItemTypes.isEmpty())
            wire.addQueryItem(QStringLiteral("includeItemTypes"), query.includeItemTypes);
        if (!query.excludeItemTypes.isEmpty())
            wire.addQueryItem(QStringLiteral("excludeItemTypes"), query.excludeItemTypes);
        if (!query.mediaTypes.isEmpty())
            wire.addQueryItem(QStringLiteral("mediaTypes"), query.mediaTypes);
        if (!query.genreIds.isEmpty())
            wire.addQueryItem(QStringLiteral("genreIds"), query.genreIds.join(QLatin1Char('|')));
        addLibraryFilterParameters(wire, query);
        if (!query.searchTerm.isEmpty())
            // Encoded here, once: QUrlQuery keeps a %XX of its input as it is, so a typed "%25" must already be "%2525"
            wire.addQueryItem(QStringLiteral("searchTerm"),
                              QString::fromLatin1(QUrl::toPercentEncoding(query.searchTerm)));
        if (!query.sortBy.isEmpty()) {
            wire.addQueryItem(QStringLiteral("sortBy"), query.sortBy);
            wire.addQueryItem(QStringLiteral("sortOrder"), query.sortOrder.isEmpty()
                                                               ? QStringLiteral("Ascending")
                                                               : query.sortOrder);
        }
        if (query.recursive)
            wire.addQueryItem(QStringLiteral("recursive"), QStringLiteral("true"));
        if (!query.filters.isEmpty())
            wire.addQueryItem(QStringLiteral("filters"), query.filters);
        if (query.startIndex >= 0)
            wire.addQueryItem(QStringLiteral("startIndex"), QString::number(query.startIndex));
        if (query.limit >= 0)
            wire.addQueryItem(QStringLiteral("limit"), QString::number(query.limit));
    }
    QStringList fields = query.fields;
    if (query.cardImages) {
        // What a card shows besides the images: the aspect ratio and the date the New badge needs
        for (const QString &field :
             {QStringLiteral("PrimaryImageAspectRatio"), QStringLiteral("DateCreated")}) {
            if (!fields.contains(field))
                fields.append(field);
        }
    }
    if (!fields.isEmpty())
        wire.addQueryItem(QStringLiteral("fields"), fields.join(QLatin1Char(',')));
    if (query.cardImages)
        addImageTypeParameters(wire, query.cardImageType);
    wire.addQueryItem(QStringLiteral("userId"), userId);
    return wire;
}

void addRowImageParameters(QUrlQuery &wire, const QString &extraFields = QString())
{
    wire.addQueryItem(QStringLiteral("fields"),
                      QStringLiteral("PrimaryImageAspectRatio") + extraFields);
    addImageTypeParameters(wire);
}

// A comma-separated list of BaseItemKind tokens
bool validKindList(const QString &kinds)
{
    QList<QString> tokens;
    if (!splitTokenList(kinds, &tokens))
        return false;
    for (const QString &token : tokens) {
        if (!isValidBaseItemKindToken(token))
            return false;
    }
    return true;
}

// A bounded list of tokens that all belong to `set`
bool validTokenList(const QStringList &list, std::initializer_list<QStringView> set)
{
    if (list.size() > MaxFilterValues)
        return false;
    for (const QString &token : list) {
        if (!isValidTokenFromSet(token, set))
            return false;
    }
    return true;
}

// The library filter members of an `ItemQuery`: bounded lists, whitelisted tokens, valid ids and texts, and no contradiction (Played with Unplayed, SD with 4K)
bool validLibraryFilters(const ItemQuery &query)
{
    if (!validTokenList(query.videoTypes, {u"BluRay", u"Dvd"})
        || !validTokenList(query.seriesStatus, {u"Continuing", u"Ended", u"Unreleased"}))
        return false;
    for (const QStringList &list : {query.tags, query.officialRatings}) {
        if (list.size() > MaxFilterValues)
            return false;
        for (const QString &text : list) {
            if (!isValidFilterText(text))
                return false;
        }
    }
    if (query.years.size() > MaxFilterValues)
        return false;
    for (const int year : query.years) {
        if (year < 1 || year > 9999)
            return false;
    }
    if (query.studioIds.size() > MaxFilterValues)
        return false;
    for (const QString &id : query.studioIds) {
        if (!isValidJellyfinId(id))
            return false;
    }
    if (query.parentIndexNumber < -1 || query.parentIndexNumber > 9999)
        return false;
    return !(query.isHd.has_value() && !*query.isHd && query.is4K);
}

// Validates an `ItemQuery` against the token vocabularies. Kept with the helpers because both fetchItems and tests rely on the refusal semantics
bool validListingQuery(const ItemQuery &query)
{
    QList<QString> tokens;
    if (!validLibraryFilters(query))
        return false;
    if (!query.parentId.isEmpty() && !isValidJellyfinId(query.parentId))
        return false;
    for (const QString &value : {query.includeItemTypes, query.excludeItemTypes}) {
        if (!value.isEmpty() && !validKindList(value))
            return false;
    }
    static const QStringList mediaTypes{QStringLiteral("Video"), QStringLiteral("Audio"),
                                        QStringLiteral("Photo"), QStringLiteral("Book")};
    if (!query.mediaTypes.isEmpty()) {
        const QStringList wanted = query.mediaTypes.split(QLatin1Char(','));
        if (!std::all_of(wanted.cbegin(), wanted.cend(),
                         [](const QString &type) { return mediaTypes.contains(type); }))
            return false;
    }
    for (const QString &id : query.itemIds) {
        if (!isValidJellyfinId(id))
            return false;
    }
    if (query.genreIds.size() > MaxGenreIds)
        return false;
    for (const QString &id : query.genreIds) {
        if (!isValidJellyfinId(id))
            return false;
    }
    if (!query.cardImageType.isEmpty()
        && !isValidTokenFromSet(query.cardImageType, {u"Banner", u"Disc", u"Logo"}))
        return false;
    if (!query.searchTerm.isEmpty() && query.searchTerm.size() > 512)
        return false;
    if (!query.sortBy.isEmpty()) {
        if (!splitTokenList(query.sortBy, &tokens))
            return false;
        for (const QString &token : tokens) {
            if (!isValidSortByToken(token))
                return false;
        }
    }
    if (!query.sortOrder.isEmpty() && !isValidSortOrderToken(query.sortOrder))
        return false;
    if (query.startIndex < -1 || query.limit < -1)
        return false;
    if (!query.filters.isEmpty()) {
        if (!splitTokenList(query.filters, &tokens))
            return false;
        for (const QString &token : tokens) {
            if (!isValidItemFilterToken(token))
                return false;
        }
        if (tokens.contains(QStringLiteral("IsPlayed"))
            && tokens.contains(QStringLiteral("IsUnplayed")))
            return false;
    }
    for (const QString &field : query.fields) {
        if (!isValidItemFieldToken(field))
            return false;
    }
    return true;
}

// The wire form of an Items listing query. The server reads a literal '+' as a space: a typed term such as "C++" must reach it as %2B (a space is already %20)
QString encodedItemsQuery(const ItemQuery &query, const QString &userId)
{
    QString encoded = listingQuery(query, userId).toString(QUrl::FullyEncoded);
    encoded.replace(QLatin1Char('+'), QStringLiteral("%2B"));
    return encoded;
}
} // namespace

int JellyfinClient::itemsQueryChars(const ItemQuery &query) const
{
    return encodedItemsQuery(query, m_userId).size();
}

bool JellyfinClient::fetchItems(const ItemQuery &query, int channel)
{
    if (m_state != State::Authenticated || !validListingQuery(query))
        return false;
    const QString encodedQuery = encodedItemsQuery(query, m_userId);
    // A bound on the whole URL, refused before the pending request of the channel is touched
    if (encodedQuery.size() > MaxItemsQueryChars)
        return false;
    const QString family = QStringLiteral("Items");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Items"));
    url.setQuery(encodedQuery);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT itemsReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::fetchResumeItems(int limit, int channel, int startIndex)
{
    if (m_state != State::Authenticated || limit <= 0 || startIndex < -1)
        return false;
    const QString family = QStringLiteral("Resume");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/UserItems/Resume"));
    QUrlQuery wire;
    wire.addQueryItem(QStringLiteral("mediaTypes"), QStringLiteral("Video"));
    addRowImageParameters(wire, QStringLiteral(",DateCreated"));
    if (startIndex >= 0)
        wire.addQueryItem(QStringLiteral("startIndex"), QString::number(startIndex));
    wire.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT resumeItemsReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::fetchLatestItems(const QString &parentId, int limit, int channel,
                                      std::optional<bool> isPlayed)
{
    if (m_state != State::Authenticated || limit <= 0
        || (!parentId.isEmpty() && !isValidJellyfinId(parentId)))
        return false;
    const QString family = QStringLiteral("Latest");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Items/Latest"));
    QUrlQuery wire;
    if (!parentId.isEmpty())
        wire.addQueryItem(QStringLiteral("parentId"), parentId);
    if (isPlayed)
        wire.addQueryItem(QStringLiteral("isPlayed"),
                          *isPlayed ? QStringLiteral("true") : QStringLiteral("false"));
    addRowImageParameters(wire, QStringLiteral(",DateCreated"));
    wire.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        if (!listingArray(finished, family, channel, &items))
            return;
        Q_EMIT latestItemsReady(items, channel);
    });
    return true;
}

bool JellyfinClient::fetchNextUp(const QString &seriesId, const QString &parentId,
                                 const QString &nextUpDateCutoff, bool enableRewatching, int limit,
                                 int channel)
{
    const QDate cutoff = nextUpDateCutoff.isEmpty()
                             ? QDate{}
                             : QDate::fromString(nextUpDateCutoff, QStringLiteral("yyyy-MM-dd"));
    if (m_state != State::Authenticated || limit <= 0
        || (!seriesId.isEmpty() && !isValidJellyfinId(seriesId))
        || (!parentId.isEmpty() && !isValidJellyfinId(parentId))
        || (!nextUpDateCutoff.isEmpty() && !cutoff.isValid()))
        return false;
    const QString family = QStringLiteral("NextUp");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Shows/NextUp"));
    QUrlQuery wire;
    if (!seriesId.isEmpty())
        wire.addQueryItem(QStringLiteral("seriesId"), seriesId);
    if (!parentId.isEmpty())
        wire.addQueryItem(QStringLiteral("parentId"), parentId);
    if (cutoff.isValid())
        wire.addQueryItem(QStringLiteral("nextUpDateCutoff"), cutoff.toString(u"yyyy-MM-dd"));
    wire.addQueryItem(QStringLiteral("enableRewatching"),
                      enableRewatching ? QStringLiteral("true") : QStringLiteral("false"));
    // One series is the details page asking for its next episode, which shows the overview
    addRowImageParameters(wire, seriesId.isEmpty() ? QString() : QStringLiteral(",Overview"));
    wire.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT nextUpReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::fetchSeasons(const QString &seriesId, int channel)
{
    if (m_state != State::Authenticated || !isValidJellyfinId(seriesId))
        return false;
    const QString family = QStringLiteral("Seasons");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Shows/") + seriesId + QStringLiteral("/Seasons"));
    QUrlQuery wire;
    addImageTypeParameters(wire);
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT seasonsReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::fetchEpisodes(const QString &seriesId, const QString &seasonId, int startIndex,
                                   int limit, int channel, bool forRuleScan)
{
    if (m_state != State::Authenticated || !isValidJellyfinId(seriesId)
        || (!seasonId.isEmpty() && !isValidJellyfinId(seasonId)) || startIndex < -1 || limit < -1)
        return false;
    const QString family = QStringLiteral("Episodes");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Shows/") + seriesId + QStringLiteral("/Episodes"));
    QUrlQuery wire;
    if (!seasonId.isEmpty())
        wire.addQueryItem(QStringLiteral("seasonId"), seasonId);
    if (startIndex >= 0)
        wire.addQueryItem(QStringLiteral("startIndex"), QString::number(startIndex));
    if (limit >= 0)
        wire.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    wire.addQueryItem(QStringLiteral("fields"),
                      forRuleScan ? QStringLiteral("DateCreated") : QStringLiteral("Overview"));
    addImageTypeParameters(wire);
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT episodesReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::fetchSimilarItems(const QString &itemId, int limit, int channel)
{
    if (m_state != State::Authenticated || !isValidJellyfinId(itemId) || limit <= 0)
        return false;
    const QString family = QStringLiteral("Similar");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Items/") + itemId + QStringLiteral("/Similar"));
    QUrlQuery wire;
    wire.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT similarItemsReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::fetchGenres(const QString &parentId, int startIndex, int limit,
                                 const QString &includeItemTypes, int channel)
{
    if (m_state != State::Authenticated || (!parentId.isEmpty() && !isValidJellyfinId(parentId))
        || startIndex < -1 || limit < -1
        || (!includeItemTypes.isEmpty() && !validKindList(includeItemTypes)))
        return false;
    const QString family = QStringLiteral("Genres");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Genres"));
    QUrlQuery wire;
    if (!parentId.isEmpty())
        wire.addQueryItem(QStringLiteral("parentId"), parentId);
    if (!includeItemTypes.isEmpty())
        wire.addQueryItem(QStringLiteral("includeItemTypes"), includeItemTypes);
    if (startIndex >= 0)
        wire.addQueryItem(QStringLiteral("startIndex"), QString::number(startIndex));
    if (limit >= 0)
        wire.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT genresReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::fetchFilters(const QString &parentId, const QString &includeItemTypes,
                                  int channel)
{
    if (m_state != State::Authenticated || (!parentId.isEmpty() && !isValidJellyfinId(parentId))
        || (!includeItemTypes.isEmpty() && !validKindList(includeItemTypes)))
        return false;
    const QString family = QStringLiteral("Filters");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Items/Filters2"));
    QUrlQuery wire;
    if (!parentId.isEmpty())
        wire.addQueryItem(QStringLiteral("parentId"), parentId);
    if (!includeItemTypes.isEmpty()) {
        // The kinds of a library sit below its folders: ask the whole tree
        wire.addQueryItem(QStringLiteral("includeItemTypes"), includeItemTypes);
        wire.addQueryItem(QStringLiteral("recursive"), QStringLiteral("true"));
    }
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonObject filters;
        if (!listingObject(finished, family, channel, &filters))
            return;
        Q_EMIT filtersReady(filters, channel);
    });
    return true;
}

bool JellyfinClient::fetchLegacyFilters(const QString &parentId, const QString &includeItemTypes,
                                        int channel)
{
    if (m_state != State::Authenticated || (!parentId.isEmpty() && !isValidJellyfinId(parentId))
        || (!includeItemTypes.isEmpty() && !validKindList(includeItemTypes)))
        return false;
    const QString family = QStringLiteral("FiltersLegacy");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Items/Filters"));
    QUrlQuery wire;
    if (!parentId.isEmpty())
        wire.addQueryItem(QStringLiteral("parentId"), parentId);
    if (!includeItemTypes.isEmpty())
        wire.addQueryItem(QStringLiteral("includeItemTypes"), includeItemTypes);
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            Q_EMIT legacyFiltersFailed(status, QStringLiteral("Authentication expired"), channel);
            return;
        }
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            Q_EMIT legacyFiltersFailed(status, QStringLiteral("Listing request failed"), channel);
            return;
        }
        QJsonObject filters;
        if (!parseObject(finished, &filters)) {
            Q_EMIT legacyFiltersFailed(status, QStringLiteral("Invalid listing response"), channel);
            return;
        }
        Q_EMIT legacyFiltersReady(filters, channel);
    });
    return true;
}

bool JellyfinClient::fetchMovieRecommendations(const QString &parentId, int categoryLimit,
                                               int itemLimit, int channel)
{
    if (m_state != State::Authenticated || (!parentId.isEmpty() && !isValidJellyfinId(parentId))
        || categoryLimit < 1 || categoryLimit > MaxRecommendationCategories || itemLimit < 1
        || itemLimit > MaxRecommendationItems)
        return false;
    const QString family = QStringLiteral("MovieRecommendations");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Movies/Recommendations"));
    QUrlQuery wire;
    if (!parentId.isEmpty())
        wire.addQueryItem(QStringLiteral("parentId"), parentId);
    wire.addQueryItem(QStringLiteral("categoryLimit"), QString::number(categoryLimit));
    wire.addQueryItem(QStringLiteral("itemLimit"), QString::number(itemLimit));
    wire.addQueryItem(QStringLiteral("fields"),
                      QStringLiteral("PrimaryImageAspectRatio,DateCreated"));
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray categories;
        if (!listingArray(finished, family, channel, &categories))
            return;
        Q_EMIT movieRecommendationsReady(categories, channel);
    });
    return true;
}

bool JellyfinClient::fetchSuggestions(const QString &type, int startIndex, int limit, int channel)
{
    if (m_state != State::Authenticated || !isValidBaseItemKindToken(type) || startIndex < -1
        || limit < 1 || limit > MaxSuggestions)
        return false;
    const QString family = QStringLiteral("Suggestions");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Items/Suggestions"));
    QUrlQuery wire;
    wire.addQueryItem(QStringLiteral("type"), type);
    if (startIndex >= 0)
        wire.addQueryItem(QStringLiteral("startIndex"), QString::number(startIndex));
    wire.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    // The route does not count by default, and the envelope check needs a TotalRecordCount
    wire.addQueryItem(QStringLiteral("enableTotalRecordCount"), QStringLiteral("true"));
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT suggestionsReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::fetchUpcomingEpisodes(const QString &parentId, int startIndex, int limit,
                                           int channel)
{
    if (m_state != State::Authenticated || (!parentId.isEmpty() && !isValidJellyfinId(parentId))
        || startIndex < -1 || limit < 1 || limit > MaxUpcomingEpisodes)
        return false;
    const QString family = QStringLiteral("Upcoming");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Shows/Upcoming"));
    QUrlQuery wire;
    if (!parentId.isEmpty())
        wire.addQueryItem(QStringLiteral("parentId"), parentId);
    addRowImageParameters(wire, QStringLiteral(",DateCreated"));
    if (startIndex >= 0)
        wire.addQueryItem(QStringLiteral("startIndex"), QString::number(startIndex));
    wire.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT upcomingReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::fetchStudios(const QString &parentId, const QString &includeItemTypes,
                                  int startIndex, int limit, int channel)
{
    if (m_state != State::Authenticated || (!parentId.isEmpty() && !isValidJellyfinId(parentId))
        || startIndex < -1 || limit < -1
        || (!includeItemTypes.isEmpty() && !validKindList(includeItemTypes)))
        return false;
    const QString family = QStringLiteral("Studios");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Studios"));
    QUrlQuery wire;
    if (!parentId.isEmpty())
        wire.addQueryItem(QStringLiteral("parentId"), parentId);
    if (!includeItemTypes.isEmpty())
        wire.addQueryItem(QStringLiteral("includeItemTypes"), includeItemTypes);
    if (startIndex >= 0)
        wire.addQueryItem(QStringLiteral("startIndex"), QString::number(startIndex));
    if (limit >= 0)
        wire.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    addRowImageParameters(wire);
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT studiosReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::fetchSessions(bool controllableByUserOnly)
{
    if (m_state != State::Authenticated)
        return false;
    const QString family = QStringLiteral("Sessions");
    const quint64 serial = supersedeListing(family);
    QUrl url = endpoint(QStringLiteral("/Sessions"));
    QUrlQuery wire;
    if (controllableByUserOnly)
        wire.addQueryItem(QStringLiteral("controllableByUserId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(family, reply);
    attach(reply, m_generation, [this, family, serial](QNetworkReply *finished) {
        if (listingSuperseded(family, serial))
            return;
        QJsonArray sessions;
        if (!listingArray(finished, family, 0, &sessions))
            return;
        Q_EMIT sessionsReady(sessions);
    });
    return true;
}

bool JellyfinClient::fetchPlaylistEntries(const QString &playlistId, int startIndex, int limit,
                                          int channel)
{
    if (m_state != State::Authenticated || !isValidJellyfinId(playlistId)
        || !isValidJellyfinId(m_userId) || startIndex < 0 || limit <= 0)
        return false;
    const QString family = QStringLiteral("PlaylistEntries");
    const QString key = listingKey(family, channel);
    const quint64 serial = supersedeListing(key);
    QUrl url = endpoint(QStringLiteral("/Playlists/") + playlistId + QStringLiteral("/Items"));
    QUrlQuery wire;
    wire.addQueryItem(QStringLiteral("startIndex"), QString::number(startIndex));
    wire.addQueryItem(QStringLiteral("limit"), QString::number(qMin(limit, MaxPlaylistPageSize)));
    addRowImageParameters(wire, QStringLiteral(",DateCreated"));
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(key, reply);
    attach(reply, m_generation, [this, family, key, channel, serial](QNetworkReply *finished) {
        if (listingSuperseded(key, serial))
            return;
        QJsonArray items;
        int total = 0;
        if (!listingEnvelope(finished, family, channel, &items, &total))
            return;
        Q_EMIT playlistEntriesReady(items, total, channel);
    });
    return true;
}

bool JellyfinClient::createPlaylist(const QString &name, const QStringList &itemIds)
{
    const QString trimmed = normalizedPlaylistName(name);
    if (!isAuthenticated() || trimmed.isEmpty() || !validPlaylistIds(itemIds, true)
        || !isValidJellyfinId(m_userId))
        return false;
    const QJsonObject body{{QStringLiteral("Name"), trimmed},
                           {QStringLiteral("Ids"), QJsonArray::fromStringList(itemIds)},
                           {QStringLiteral("UserId"), m_userId},
                           {QStringLiteral("MediaType"), QStringLiteral("Video")},
                           {QStringLiteral("IsPublic"), false}};
    return sendPlaylistWrite(PlaylistOp::Create, {}, QByteArrayLiteral("POST"),
                             endpoint(QStringLiteral("/Playlists")),
                             QJsonDocument(body).toJson(QJsonDocument::Compact));
}

bool JellyfinClient::addToPlaylist(const QString &playlistId, const QStringList &itemIds)
{
    if (!isAuthenticated() || !isValidJellyfinId(playlistId) || !validPlaylistIds(itemIds, false)
        || !isValidJellyfinId(m_userId))
        return false;
    QUrl url = endpoint(QStringLiteral("/Playlists/") + playlistId + QStringLiteral("/Items"));
    QUrlQuery wire;
    wire.addQueryItem(QStringLiteral("ids"), itemIds.join(QLatin1Char(',')));
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(wire);
    return sendPlaylistWrite(PlaylistOp::Add, playlistId, QByteArrayLiteral("POST"), url);
}

bool JellyfinClient::removePlaylistEntries(const QString &playlistId, const QStringList &entryIds)
{
    if (!isAuthenticated() || !isValidJellyfinId(playlistId) || !validPlaylistIds(entryIds, false))
        return false;
    QUrl url = endpoint(QStringLiteral("/Playlists/") + playlistId + QStringLiteral("/Items"));
    QUrlQuery wire;
    wire.addQueryItem(QStringLiteral("entryIds"), entryIds.join(QLatin1Char(',')));
    url.setQuery(wire);
    return sendPlaylistWrite(PlaylistOp::Remove, playlistId, QByteArrayLiteral("DELETE"), url);
}

bool JellyfinClient::movePlaylistEntry(const QString &playlistId, const QString &entryId,
                                       int newIndex)
{
    if (!isAuthenticated() || !isValidJellyfinId(playlistId) || !isValidJellyfinId(entryId)
        || newIndex < 0)
        return false;
    const QUrl url = endpoint(QStringLiteral("/Playlists/") + playlistId + QStringLiteral("/Items/")
                              + entryId + QStringLiteral("/Move/") + QString::number(newIndex));
    return sendPlaylistWrite(PlaylistOp::Move, playlistId, QByteArrayLiteral("POST"), url);
}

bool JellyfinClient::renamePlaylist(const QString &playlistId, const QString &name)
{
    const QString trimmed = normalizedPlaylistName(name);
    if (!isAuthenticated() || !isValidJellyfinId(playlistId) || trimmed.isEmpty())
        return false;
    const QJsonObject body{{QStringLiteral("Name"), trimmed}};
    return sendPlaylistWrite(PlaylistOp::Rename, playlistId, QByteArrayLiteral("POST"),
                             endpoint(QStringLiteral("/Playlists/") + playlistId),
                             QJsonDocument(body).toJson(QJsonDocument::Compact));
}

bool JellyfinClient::deletePlaylist(const QString &playlistId)
{
    if (!isAuthenticated() || !isValidJellyfinId(playlistId) || !isValidJellyfinId(m_userId))
        return false;
    // DELETE /Items/{id} removes any item and cannot be undone: the id is read first and must name a playlist, the slot is held across both requests
    const QString key = playlistWriteKey(false, playlistId);
    if (m_playlistPending.contains(key))
        return false;
    m_playlistPending.insert(key);
    QUrl url = endpoint(QStringLiteral("/Items/") + playlistId);
    QUrlQuery wire;
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    // The route answers every field by default (inferred): asked for in case it does not
    wire.addQueryItem(QStringLiteral("fields"), QStringLiteral("CanDelete"));
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    attach(reply, m_generation, [this, playlistId, key](QNetworkReply *finished) {
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject item;
        const bool isPlaylist =
            status == 200 && finished->error() == QNetworkReply::NoError
            && parseObject(finished, &item)
            && item.value(QStringLiteral("Type")).toString() == QLatin1String("Playlist")
            && playlistKey(item.value(QStringLiteral("Id")).toString()) == playlistKey(playlistId);
        // The server answers a DELETE the user may not make with 401, which would end the session: only a playlist it says the user can delete is deleted
        const bool deletable = isPlaylist && item.value(QStringLiteral("CanDelete")).toBool(false);
        if (deletable) {
            dispatchPlaylistWrite(PlaylistOp::Delete, playlistId, QByteArrayLiteral("DELETE"),
                                  endpoint(QStringLiteral("/Items/") + playlistId), {});
            return;
        }
        m_playlistPending.remove(key);
        if (status == 401)
            invalidateSession();
        Q_EMIT playlistWriteFailed(PlaylistOp::Delete, playlistId, isPlaylist ? 403 : status);
    });
    return true;
}

bool JellyfinClient::sendPlaylistWrite(PlaylistOp op, const QString &playlistId,
                                       const QByteArray &method, const QUrl &url,
                                       const QByteArray &body)
{
    const QString key = playlistWriteKey(op == PlaylistOp::Create, playlistId);
    if (m_playlistPending.contains(key))
        return false;
    m_playlistPending.insert(key);
    dispatchPlaylistWrite(op, playlistId, method, url, body);
    return true;
}

void JellyfinClient::dispatchPlaylistWrite(PlaylistOp op, const QString &playlistId,
                                           const QByteArray &method, const QUrl &url,
                                           const QByteArray &body)
{
    const QString key = playlistWriteKey(op == PlaylistOp::Create, playlistId);
    QNetworkRequest request = requestFor(url, true);
    QNetworkReply *reply = nullptr;
    if (method == "POST") {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        reply = m_network.post(request, body);
    } else {
        reply = m_network.sendCustomRequest(request, method);
    }
    attach(reply, m_generation, [this, op, playlistId, key](QNetworkReply *finished) {
        m_playlistPending.remove(key);
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401)
            invalidateSession();
        // Only the documented status counts: 200 with a body for a create, 204 for every other write
        const int expected = op == PlaylistOp::Create ? 200 : 204;
        bool ok = finished->error() == QNetworkReply::NoError && status == expected;
        QString createdId;
        if (ok && op == PlaylistOp::Create) {
            QJsonObject data;
            createdId = parseObject(finished, &data) ? data.value(QStringLiteral("Id")).toString()
                                                     : QString();
            ok = isValidJellyfinId(createdId);
        }
        if (ok)
            Q_EMIT playlistWriteSucceeded(op, playlistId, createdId);
        else
            Q_EMIT playlistWriteFailed(op, playlistId, status);
    });
}

bool JellyfinClient::sendSessionPlaystate(const QString &sessionId, const QString &command)
{
    if (m_state != State::Authenticated || !isValidJellyfinId(sessionId)
        || (command != QLatin1String("Pause") && command != QLatin1String("Unpause")
            && command != QLatin1String("Stop")))
        return false;
    const QUrl url =
        endpoint(QStringLiteral("/Sessions/") + sessionId + QStringLiteral("/Playing/") + command);
    QNetworkReply *reply = postEmpty(m_network, requestFor(url, true));
    attach(reply, m_generation, [this, sessionId, command](QNetworkReply *finished) {
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401)
            invalidateSession();
        if ((status == 204 || status == 200) && finished->error() == QNetworkReply::NoError)
            Q_EMIT sessionPlaystateSucceeded(sessionId, command);
        else
            Q_EMIT sessionPlaystateFailed(sessionId, command, status);
    });
    return true;
}

bool JellyfinClient::fetchSyncPlayGroups()
{
    if (m_state != State::Authenticated)
        return false;
    // The serial moves first: an abort may finish the old reply at once, and it must find itself superseded
    const quint64 serial = ++m_syncPlayGroupsSerial;
    if (m_syncPlayGroupsReply)
        m_syncPlayGroupsReply->abort();
    QNetworkReply *reply =
        m_network.get(requestFor(endpoint(QStringLiteral("/SyncPlay/List")), true));
    m_syncPlayGroupsReply = reply;
    attach(reply, m_generation, [this, serial](QNetworkReply *finished) {
        if (serial != m_syncPlayGroupsSerial)
            return;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401)
            invalidateSession();
        QJsonArray array;
        if (status != 200 || finished->error() != QNetworkReply::NoError
            || !parseArray(finished, &array)) {
            Q_EMIT syncPlayGroupsFailed(status);
            return;
        }
        Q_EMIT syncPlayGroupsReady(
            SyncPlay::parseGroupList(array).value_or(QList<SyncPlay::GroupInfo>()));
    });
    return true;
}

bool JellyfinClient::postSyncPlay(SyncPlay::Route route, const QJsonObject &body)
{
    if (m_state != State::Authenticated || !SyncPlay::isValidRequest(route, body)
        || m_syncPlayQueue.size() >= MaxQueuedSyncPlayRequests)
        return false;
    m_syncPlayQueue.append({route, QJsonDocument(body).toJson(QJsonDocument::Compact)});
    if (!m_syncPlayBusy)
        sendNextSyncPlayRequest();
    return true;
}

void JellyfinClient::sendNextSyncPlayRequest()
{
    if (m_syncPlayQueue.isEmpty() || m_state != State::Authenticated)
        return;
    m_syncPlayBusy = true;
    const SyncPlay::Route route = m_syncPlayQueue.constFirst().route;
    QNetworkRequest request =
        requestFor(endpoint(QStringLiteral("/SyncPlay/") + SyncPlay::routeName(route)), true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_network.post(request, m_syncPlayQueue.constFirst().body);
    attach(reply, m_generation, [this, route](QNetworkReply *finished) {
        m_syncPlayQueue.removeFirst();
        m_syncPlayBusy = false;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // The session's end empties the queue: nothing is sent after it
        if (status == 401)
            invalidateSession();
        const bool delivered = status == 204 || (status == 200 && route == SyncPlay::Route::New);
        Q_EMIT syncPlayRequestFinished(
            route, delivered && finished->error() == QNetworkReply::NoError, status);
        if (!m_syncPlayBusy)
            sendNextSyncPlayRequest();
    });
}

bool JellyfinClient::fetchUtcTime()
{
    if (m_state != State::Authenticated)
        return false;
    // The serial moves first: an abort may finish the old reply at once, and it must find itself superseded
    const quint64 serial = ++m_utcTimeSerial;
    if (m_utcTimeReply)
        m_utcTimeReply->abort();
    QNetworkReply *reply = m_network.get(requestFor(endpoint(QStringLiteral("/GetUtcTime")), true));
    m_utcTimeReply = reply;
    attach(reply, m_generation, [this, serial](QNetworkReply *finished) {
        if (serial != m_utcTimeSerial)
            return;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401)
            invalidateSession();
        QJsonObject object;
        std::optional<SyncPlay::UtcTime> time;
        if (status == 200 && finished->error() == QNetworkReply::NoError
            && parseObject(finished, &object))
            time = SyncPlay::parseUtcTime(object);
        if (time)
            Q_EMIT utcTimeReady(time->receptionMs, time->transmissionMs);
        else
            Q_EMIT utcTimeFailed(status);
    });
    return true;
}

bool JellyfinClient::setItemFavorite(const QString &itemId, bool favorite)
{
    if (!isAuthenticated() || !isValidJellyfinId(itemId))
        return false;
    // A GUID may be written dashed or dashless, and in either case.
    const QString key = QString(itemId).remove(QLatin1Char('-')).toLower();
    if (m_favoritePending.contains(key))
        return false;
    m_favoritePending.insert(key);

    const QNetworkRequest request =
        requestFor(endpoint(QStringLiteral("/UserFavoriteItems/") + itemId), true);
    QNetworkReply *reply = favorite
                               ? postEmpty(m_network, request)
                               : m_network.sendCustomRequest(request, QByteArrayLiteral("DELETE"));
    attach(reply, m_generation, [this, itemId, key, favorite](QNetworkReply *finished) {
        m_favoritePending.remove(key);
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            return;
        }
        bool valid = finished->error() == QNetworkReply::NoError && status == 204;
        if (status == 200 && finished->error() == QNetworkReply::NoError) {
            QJsonObject data;
            if (parseObject(finished, &data) && data.value(QStringLiteral("IsFavorite")).isBool()
                && data.value(QStringLiteral("IsFavorite")).toBool() == favorite) {
                const QJsonValue id = data.value(QStringLiteral("ItemId"));
                valid = id.isUndefined()
                        || (id.isString()
                            && QString(id.toString()).remove(QLatin1Char('-')).toLower() == key);
            }
        }
        if (valid)
            Q_EMIT itemFavoriteUpdated(itemId, favorite);
        else
            Q_EMIT itemFavoriteUpdateFailed(itemId, favorite, status);
    });
    return true;
}

bool JellyfinClient::setItemPlayed(const QString &itemId, bool played)
{
    if (!isAuthenticated() || !isValidJellyfinId(itemId) || !isValidJellyfinId(m_userId))
        return false;
    const QString key = QString(itemId).remove(QLatin1Char('-')).toLower();
    if (m_playedPending.contains(key))
        return false;
    m_playedPending.insert(key);

    QUrl url = endpoint(QStringLiteral("/UserPlayedItems/") + itemId);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(query);
    const QNetworkRequest request = requestFor(url, true);
    QNetworkReply *reply = played
                               ? postEmpty(m_network, request)
                               : m_network.sendCustomRequest(request, QByteArrayLiteral("DELETE"));
    attach(reply, m_generation, [this, itemId, key, played](QNetworkReply *finished) {
        m_playedPending.remove(key);
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            return;
        }
        // Jellyfin 12.4 answers 200 with UserItemDataDto; accept empty 204 too,
        // but never treat a 200 with a malformed/mismatching body as success.
        bool valid = finished->error() == QNetworkReply::NoError && status == 204;
        if (status == 200 && finished->error() == QNetworkReply::NoError) {
            QJsonObject data;
            if (parseObject(finished, &data) && data.value(QStringLiteral("Played")).isBool()
                && data.value(QStringLiteral("Played")).toBool() == played) {
                const QJsonValue id = data.value(QStringLiteral("ItemId"));
                valid = id.isUndefined()
                        || (id.isString()
                            && QString(id.toString()).remove(QLatin1Char('-')).toLower() == key);
            }
        }
        if (valid)
            Q_EMIT itemPlayedUpdated(itemId, played);
        else
            Q_EMIT itemPlayedUpdateFailed(itemId, played, status);
    });
    return true;
}

bool JellyfinClient::searchRemoteSubtitles(const QString &itemId, const QString &language)
{
    static const QRegularExpression languagePattern(QStringLiteral("\\A[A-Za-z]{2,3}\\z"));
    if (!isAuthenticated() || !canManageSubtitles() || !isValidJellyfinId(itemId)
        || !languagePattern.match(language).hasMatch())
        return false;
    const QUrl url = endpoint(QStringLiteral("/Items/") + itemId
                              + QStringLiteral("/RemoteSearch/Subtitles/") + language);
    QNetworkRequest request = requestFor(url, true);
    request.setTransferTimeout(RemoteSubtitleTimeoutMs);
    QNetworkReply *reply = m_network.get(request);
    attach(reply, m_generation, [this, itemId, language](QNetworkReply *finished) {
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            return;
        }
        if (finished->error() != QNetworkReply::NoError || status != 200) {
            Q_EMIT remoteSubtitlesFailed(itemId, language, status);
            return;
        }
        QJsonArray subtitles;
        if (!parseArray(finished, &subtitles)) {
            Q_EMIT remoteSubtitlesFailed(itemId, language, status);
            return;
        }
        Q_EMIT remoteSubtitlesReady(itemId, language, subtitles);
    });
    return true;
}

bool JellyfinClient::downloadRemoteSubtitle(const QString &itemId, const QString &subtitleId)
{
    const auto safeId = [](const QString &id) {
        // A path segment of dots alone would be a dot segment of the route
        if (id.isEmpty() || id.size() > 128 || id == QLatin1String(".")
            || id == QLatin1String(".."))
            return false;
        return std::all_of(id.cbegin(), id.cend(), [](QChar character) {
            const char16_t value = character.unicode();
            return (value >= u'0' && value <= u'9') || (value >= u'a' && value <= u'z')
                   || (value >= u'A' && value <= u'Z') || value == u'-' || value == u'_'
                   || value == u'.';
        });
    };
    if (!isAuthenticated() || !canManageSubtitles() || !isValidJellyfinId(itemId)
        || !safeId(subtitleId))
        return false;
    const QUrl url = endpoint(QStringLiteral("/Items/") + itemId
                              + QStringLiteral("/RemoteSearch/Subtitles/") + subtitleId);
    QNetworkRequest request = requestFor(url, true);
    request.setTransferTimeout(RemoteSubtitleTimeoutMs);
    QNetworkReply *reply = postEmpty(m_network, request);
    attach(reply, m_generation, [this, itemId, subtitleId](QNetworkReply *finished) {
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            return;
        }
        if (finished->error() == QNetworkReply::NoError && (status == 200 || status == 204))
            Q_EMIT remoteSubtitleDownloaded(itemId, subtitleId);
        else
            Q_EMIT remoteSubtitleDownloadFailed(itemId, subtitleId, status);
    });
    return true;
}

QUrl JellyfinClient::imageUrl(const QString &itemId, const QString &imageType, int maxWidth,
                              const QString &tag, int quality, const QString &format) const
{
    if (m_base.isEmpty() || !isValidJellyfinId(itemId) || !isValidImageTypeToken(imageType)
        || maxWidth <= 0 || maxWidth > 4096 || quality < 1 || quality > 100
        || (!format.isEmpty() && !isValidImageFormatToken(format))
        || (!tag.isEmpty() && !isValidImageTagToken(tag)))
        return {};
    QUrl url =
        endpoint(QStringLiteral("/Items/") + itemId + QStringLiteral("/Images/") + imageType);
    QUrlQuery wire;
    wire.addQueryItem(QStringLiteral("maxWidth"), QString::number(maxWidth));
    wire.addQueryItem(QStringLiteral("quality"), QString::number(quality));
    if (!tag.isEmpty())
        wire.addQueryItem(QStringLiteral("tag"), tag);
    if (!format.isEmpty())
        wire.addQueryItem(QStringLiteral("format"), format);
    url.setQuery(wire);
    return url;
}

QUrl JellyfinClient::userImageUrl(const QString &userId, const QString &tag) const
{
    if (m_base.isEmpty() || !isValidJellyfinId(userId)
        || (!tag.isEmpty() && !isValidUserImageTag(tag)))
        return {};
    QUrl url = endpoint(QStringLiteral("/UserImage"));
    QUrlQuery wire;
    wire.addQueryItem(QStringLiteral("userId"), userId);
    if (!tag.isEmpty())
        wire.addQueryItem(QStringLiteral("tag"), tag);
    url.setQuery(wire);
    return url;
}

bool JellyfinClient::isServerImageUrl(const QUrl &base, const QUrl &url)
{
    if (base.isEmpty() || !url.isValid() || url.scheme() != base.scheme()
        || url.host().compare(base.host(), Qt::CaseInsensitive) != 0
        || url.port(-1) != base.port(-1) || !url.userInfo().isEmpty() || url.hasFragment())
        return false;
    QString basePath = base.path(QUrl::FullyDecoded);
    while (basePath.endsWith(QLatin1Char('/')))
        basePath.chop(1);
    // An encoded slash or backslash would let a decoded path pretend to a different structure than the one the server routes
    const QString encoded = url.path(QUrl::FullyEncoded).toLower();
    if (encoded.contains(QStringLiteral("%2f")) || encoded.contains(QStringLiteral("%5c")))
        return false;
    const QString path = url.path(QUrl::FullyDecoded);
    if (path == basePath + QStringLiteral("/UserImage")) {
        // Exactly the query userImageUrl() writes: a valid userId and an optional valid tag, nothing else, nothing twice
        bool hasUser = false;
        bool hasTag = false;
        for (const auto &[key, value] : QUrlQuery(url).queryItems(QUrl::FullyDecoded)) {
            if (key == QLatin1String("userId") && !hasUser && isValidJellyfinId(value))
                hasUser = true;
            else if (key == QLatin1String("tag") && !hasTag && isValidUserImageTag(value))
                hasTag = true;
            else
                return false;
        }
        return hasUser;
    }
    if (!path.startsWith(basePath + QStringLiteral("/Items/")))
        return false;
    const QStringList parts = path.mid(basePath.size() + 1).split(QLatin1Char('/'));
    return parts.size() == 4 && parts.at(0) == QLatin1String("Items")
           && isValidJellyfinId(parts.at(1)) && parts.at(2) == QLatin1String("Images")
           && isValidImageTypeToken(parts.at(3));
}

bool JellyfinClient::fetchPublicUsers()
{
    if (m_state == State::Disconnected || m_state == State::Probing)
        return false;
    const QString family = QStringLiteral("PublicUsers");
    const quint64 serial = supersedeListing(family);
    QNetworkReply *reply =
        m_network.get(requestFor(endpoint(QStringLiteral("/Users/Public")), false));
    m_listingReplies.insert(family, reply);
    attach(reply, m_generation, [this, family, serial](QNetworkReply *finished) {
        if (listingSuperseded(family, serial))
            return;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            Q_EMIT publicUsersFailed(status, QStringLiteral("Public users request failed"));
            return;
        }
        QJsonArray entries;
        if (!parseArray(finished, &entries)) {
            Q_EMIT publicUsersFailed(status, QStringLiteral("Invalid public users response"));
            return;
        }
        QList<PublicUser> users;
        for (const QJsonValue &entry : entries) {
            if (users.size() >= MaxPublicUsers)
                break;
            const QJsonObject object = entry.toObject();
            const QString id = object.value(QStringLiteral("Id")).toString();
            const QString name = validUserName(object.value(QStringLiteral("Name")));
            if (!isValidJellyfinId(id) || name.isEmpty())
                continue;
            users.append(
                {id, name, validUserImageTag(object.value(QStringLiteral("PrimaryImageTag")))});
        }
        Q_EMIT publicUsersReady(users);
    });
    return true;
}

bool JellyfinClient::fetchUserViews()
{
    if (m_state != State::Authenticated)
        return false;
    QUrl url = endpoint(QStringLiteral("/UserViews"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(query);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    attach(reply, m_generation, [this](QNetworkReply *finished) {
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            Q_EMIT requestFailed(status, QStringLiteral("Authentication expired"));
            return;
        }
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            Q_EMIT requestFailed(status, QStringLiteral("Library request failed"));
            return;
        }
        QJsonObject data;
        if (!parseObject(finished, &data) || !data.value(QStringLiteral("Items")).isArray()
            || !data.value(QStringLiteral("TotalRecordCount")).isDouble()) {
            Q_EMIT requestFailed(status, QStringLiteral("Invalid library response"));
            return;
        }
        const int total = data.value(QStringLiteral("TotalRecordCount")).toInt(-1);
        const QJsonArray items = data.value(QStringLiteral("Items")).toArray();
        if (total < items.size()) {
            Q_EMIT requestFailed(status, QStringLiteral("Invalid library pagination"));
            return;
        }
        Q_EMIT userViewsReady(items, total);
    });
    return true;
}

bool JellyfinClient::requestPlaybackInfo(const QString &itemId, qint64 startTimeTicks,
                                         const QJsonObject &deviceProfile,
                                         qint64 maxStreamingBitrate, const StreamChoice &streams)
{
    if (m_state != State::Authenticated || !isValidJellyfinId(itemId) || startTimeTicks < 0
        || maxStreamingBitrate <= 0 || deviceProfile.isEmpty())
        return false;
    if ((!streams.mediaSourceId.isEmpty() && !isValidJellyfinId(streams.mediaSourceId))
        || (streams.audioStreamIndex && *streams.audioStreamIndex < 0)
        || (streams.subtitleStreamIndex && *streams.subtitleStreamIndex < -1))
        return false;
    cancelPlaybackInfo();
    const quint64 serial = m_playbackSerial;
    QJsonObject body{{QStringLiteral("UserId"), m_userId},
                     {QStringLiteral("StartTimeTicks"), startTimeTicks},
                     {QStringLiteral("MaxStreamingBitrate"), maxStreamingBitrate},
                     {QStringLiteral("DeviceProfile"), deviceProfile}};
    if (!streams.mediaSourceId.isEmpty())
        body.insert(QStringLiteral("MediaSourceId"), streams.mediaSourceId);
    if (streams.audioStreamIndex)
        body.insert(QStringLiteral("AudioStreamIndex"), *streams.audioStreamIndex);
    if (streams.subtitleStreamIndex)
        body.insert(QStringLiteral("SubtitleStreamIndex"), *streams.subtitleStreamIndex);
    QNetworkRequest request = requestFor(
        endpoint(QStringLiteral("/Items/") + itemId + QStringLiteral("/PlaybackInfo")), true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply =
        m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_playbackReply = reply;
    attach(reply, m_generation, [this, serial, itemId](QNetworkReply *finished) {
        if (serial != m_playbackSerial)
            return;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            Q_EMIT playbackInfoFailed(itemId, status, QStringLiteral("Authentication expired"));
            return;
        }
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            Q_EMIT playbackInfoFailed(itemId, status, QStringLiteral("Playback request failed"));
            return;
        }
        QJsonObject data;
        if (!parseObject(finished, &data)) {
            Q_EMIT playbackInfoFailed(itemId, status, QStringLiteral("Invalid playback response"));
            return;
        }
        const SourceSelection selection = selectPlaybackSource(data, itemId, m_base, m_token);
        if (!selection.source) {
            Q_EMIT playbackInfoFailed(itemId, status, selection.failure);
            return;
        }
        Q_EMIT playbackInfoReady(*selection.source);
    });
    return true;
}

bool JellyfinClient::fetchMediaSegments(const QString &itemId)
{
    if (m_state != State::Authenticated || !isValidJellyfinId(itemId))
        return false;
    const quint64 serial = ++m_segmentsSerial;
    QUrl url = endpoint(QStringLiteral("/MediaSegments/") + itemId);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("includeSegmentTypes"), QStringLiteral("Intro,Recap,Outro"));
    url.setQuery(query);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    attach(reply, m_generation, [this, itemId, serial](QNetworkReply *finished) {
        if (serial != m_segmentsSerial)
            return;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            return;
        }
        if (status != 200 || finished->error() != QNetworkReply::NoError)
            return; // Older servers without MediaSegments simply offer no Skip button.
        QJsonObject root;
        if (!parseObject(finished, &root))
            return;
        const QJsonArray raw = root.value(QStringLiteral("Items")).toArray();
        QJsonArray safe;
        for (const QJsonValue &entry : raw) {
            if (safe.size() == 100)
                break;
            const QJsonObject item = entry.toObject();
            const QString type = item.value(QStringLiteral("Type")).toString();
            if (type != QLatin1String("Intro") && type != QLatin1String("Recap")
                && type != QLatin1String("Outro"))
                continue;
            const qint64 start = item.value(QStringLiteral("StartTicks")).toVariant().toLongLong();
            const qint64 end = item.value(QStringLiteral("EndTicks")).toVariant().toLongLong();
            if (start < 0 || end <= start || end > 24LL * 60 * 60 * 10'000'000)
                continue;
            safe.append(QJsonObject{{QStringLiteral("type"), type},
                                    {QStringLiteral("start"), start},
                                    {QStringLiteral("end"), end}});
        }
        Q_EMIT mediaSegmentsReady(itemId, safe);
    });
    return true;
}

namespace {
// The API must never use the access token on an untrusted or redirected URL.
// Subtitle URLs are supplied solely by MediaSourceSelection after it has
// stripped token query parameters and validated the relative route.
bool allowedPlayerFile(const QUrl &base, const QUrl &url)
{
    QString prefix = base.path();
    while (prefix.endsWith(QLatin1Char('/')))
        prefix.chop(1);
    prefix += QStringLiteral("/Videos/");
    return url.isValid() && url.scheme() == base.scheme()
           && url.host().compare(base.host(), Qt::CaseInsensitive) == 0 && url.port() == base.port()
           && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment()
           && url.path().startsWith(prefix) && !url.path().contains(QStringLiteral("/../"));
}
} // namespace

bool JellyfinClient::downloadExternalSubtitle(const QUrl &url)
{
    if (m_state != State::Authenticated || !allowedPlayerFile(m_base, url)
        || !url.path().contains(QStringLiteral("/Subtitles/")))
        return false;
    const quint64 serial = ++m_subtitleSerial;
    if (m_subtitleReply)
        m_subtitleReply->abort();
    QNetworkRequest request = requestFor(url, true);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *reply = m_network.get(request);
    m_subtitleReply = reply;
    attach(reply, m_generation, [this, url, serial](QNetworkReply *finished) {
        if (serial != m_subtitleSerial)
            return;
        m_subtitleReply.clear();
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            return;
        }
        if (status != 200 || finished->error() != QNetworkReply::NoError
            || finished->bytesAvailable() > 1024 * 1024) {
            Q_EMIT externalSubtitleFailed(url);
            return;
        }
        const QByteArray bytes = finished->read(1024 * 1024 + 1);
        if (bytes.isEmpty() || bytes.size() > 1024 * 1024 || bytes.contains('\0')) {
            Q_EMIT externalSubtitleFailed(url);
            return;
        }
        Q_EMIT externalSubtitleDownloaded(url, bytes);
    });
    return true;
}

bool JellyfinClient::fetchPlaybackItem(const QString &itemId)
{
    if (m_state != State::Authenticated || !isValidJellyfinId(itemId))
        return false;
    const quint64 serial = ++m_playbackItemSerial;
    QUrl url = endpoint(QStringLiteral("/Users/") + m_userId + QStringLiteral("/Items/") + itemId);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("Trickplay,Chapters"));
    url.setQuery(query);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    attach(reply, m_generation, [this, itemId, serial](QNetworkReply *finished) {
        if (serial != m_playbackItemSerial)
            return;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            return;
        }
        QJsonObject item;
        if (status == 200 && finished->error() == QNetworkReply::NoError
            && parseObject(finished, &item))
            Q_EMIT playbackItemReady(itemId, item);
    });
    return true;
}

bool JellyfinClient::fetchTrickplayTile(const QString &itemId, const QString &mediaSourceId,
                                        int width, int tileIndex)
{
    if (m_state != State::Authenticated || !isValidJellyfinId(itemId)
        || !isValidJellyfinId(mediaSourceId) || width < 16 || width > 640 || tileIndex < 0
        || tileIndex > 100'000)
        return false;
    const quint64 serial = ++m_trickplayTileSerial;
    if (m_trickplayTileReply)
        m_trickplayTileReply->abort();
    QUrl url = endpoint(QStringLiteral("/Videos/") + itemId + QStringLiteral("/Trickplay/")
                        + QString::number(width) + QLatin1Char('/') + QString::number(tileIndex)
                        + QStringLiteral(".jpg"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("mediaSourceId"), mediaSourceId);
    url.setQuery(query);
    QNetworkRequest request = requestFor(url, true);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *reply = m_network.get(request);
    m_trickplayTileReply = reply;
    attach(reply, m_generation, [this, itemId, width, tileIndex, serial](QNetworkReply *finished) {
        if (serial != m_trickplayTileSerial)
            return;
        m_trickplayTileReply.clear();
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            return;
        }
        if (status != 200 || finished->error() != QNetworkReply::NoError
            || finished->bytesAvailable() > 2 * 1024 * 1024) {
            Q_EMIT trickplayTileFailed(itemId, width, tileIndex);
            return;
        }
        const QByteArray bytes = finished->read(2 * 1024 * 1024 + 1);
        if (bytes.size() > 4 && bytes.size() <= 2 * 1024 * 1024
            && bytes.startsWith(QByteArray::fromHex("ffd8")))
            Q_EMIT trickplayTileReady(itemId, width, tileIndex, bytes);
        else
            Q_EMIT trickplayTileFailed(itemId, width, tileIndex);
    });
    return true;
}

namespace {
QString reportRoute(PlaybackReport::Kind kind)
{
    switch (kind) {
    case PlaybackReport::Kind::Start:
        return QStringLiteral("/Sessions/Playing");
    case PlaybackReport::Kind::Progress:
        return QStringLiteral("/Sessions/Playing/Progress");
    case PlaybackReport::Kind::Stopped:
        break;
    }
    return QStringLiteral("/Sessions/Playing/Stopped");
}

bool retryableStatus(int status)
{
    return status == 0 || status == 502 || status == 503 || status == 504;
}
} // namespace

bool JellyfinClient::reportPlayback(const PlaybackReport &report)
{
    const std::optional<QJsonObject> body = playbackReportBody(report);
    if (m_state != State::Authenticated || !body)
        return false;
    // The first queued report is in flight, or waiting for its retry: only the ones behind it may be coalesced
    const qsizetype waiting = (m_reportBusy || m_reportRetryTimer.isActive()) ? 1 : 0;
    if (report.kind == PlaybackReport::Kind::Stopped) {
        for (qsizetype i = m_reportQueue.size() - 1; i >= waiting; --i) {
            if (m_reportQueue.at(i).report.kind == PlaybackReport::Kind::Progress)
                m_reportQueue.removeAt(i);
        }
    } else if (report.kind == PlaybackReport::Kind::Progress && m_reportQueue.size() > waiting
               && m_reportQueue.constLast().report.kind == PlaybackReport::Kind::Progress) {
        m_reportQueue.removeLast();
    }
    m_reportQueue.append({report, QJsonDocument(*body).toJson(QJsonDocument::Compact), 0});
    sendNextPlaybackReport();
    return true;
}

bool JellyfinClient::hasPendingPlaybackReports() const
{
    return !m_reportQueue.isEmpty();
}

void JellyfinClient::setPlaybackReportRetryDelay(std::chrono::milliseconds delay)
{
    m_reportRetryDelay = delay;
}

void JellyfinClient::sendNextPlaybackReport()
{
    // One request at a time, and nothing while the first report waits for its retry
    if (m_state != State::Authenticated || m_reportQueue.isEmpty() || m_reportBusy
        || m_reportRetryTimer.isActive())
        return;
    m_reportBusy = true;
    const QueuedReport &next = m_reportQueue.constFirst();
    QNetworkRequest request = requestFor(endpoint(reportRoute(next.report.kind)), true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_network.post(request, next.body);
    attach(reply, m_generation,
           [this](QNetworkReply *finished) { finishPlaybackReport(finished); });
}

void JellyfinClient::finishPlaybackReport(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401) {
        invalidateSession(); // drops the queue
        return;
    }
    QueuedReport &current = m_reportQueue.first();
    const bool delivered =
        status >= 200 && status < 300 && reply->error() == QNetworkReply::NoError;
    if (!delivered && current.report.kind == PlaybackReport::Kind::Stopped && current.attempts == 0
        && retryableStatus(status)) {
        // The one retry of a Stopped: ordering holds, nothing else is sent meanwhile
        current.attempts = 1;
        m_reportBusy = false;
        m_reportRetryTimer.start(m_reportRetryDelay);
        return;
    }
    m_reportQueue.removeFirst();
    m_reportBusy = false;
    if (m_reportQueue.isEmpty())
        Q_EMIT playbackReportsSettled();
    else
        sendNextPlaybackReport();
}

bool JellyfinClient::reportCapabilities()
{
    if (m_state != State::Authenticated)
        return false;
    QNetworkRequest request =
        requestFor(endpoint(QStringLiteral("/Sessions/Capabilities/Full")), true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply =
        m_network.post(request, QJsonDocument(clientCapabilitiesBody(m_mediaControlAnnounced))
                                    .toJson(QJsonDocument::Compact));
    attach(reply, m_generation, [this](QNetworkReply *finished) {
        if (finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401)
            invalidateSession();
    });
    return true;
}

void JellyfinClient::cancelPlaybackInfo()
{
    // Bumping the serial makes the handler of the aborted reply a no-op: no signal for a dropped request
    ++m_playbackSerial;
    if (m_playbackReply)
        m_playbackReply->abort();
    m_playbackReply.clear();
}

QByteArray JellyfinClient::streamAuthorization() const
{
    if (m_state != State::Authenticated)
        return {};
    return tokenAuthorization();
}

QUrl JellyfinClient::downloadUrl(const QString &itemId) const
{
    if (m_state != State::Authenticated || !isValidJellyfinId(itemId))
        return {};
    return endpoint(QStringLiteral("/Items/") + itemId + QStringLiteral("/Download"));
}

QUrl JellyfinClient::subtitleFileUrl(const QString &itemId, const QString &mediaSourceId, int index,
                                     const QString &extension) const
{
    // In step with subtitleExtension in DownloadsController.cpp and localSubtitles in PlayerController.cpp
    static const QStringList extensions{QStringLiteral("srt"), QStringLiteral("ass"),
                                        QStringLiteral("ssa"), QStringLiteral("vtt")};
    if (m_state != State::Authenticated || !isValidJellyfinId(itemId)
        || !isValidJellyfinId(mediaSourceId) || index < 0 || index > 9999
        || !extensions.contains(extension))
        return {};
    return endpoint(QStringLiteral("/Videos/") + itemId + QLatin1Char('/') + mediaSourceId
                    + QStringLiteral("/Subtitles/") + QString::number(index)
                    + QStringLiteral("/0/Stream.") + extension);
}

QNetworkRequest JellyfinClient::webSocketRequest() const
{
    if (m_state != State::Authenticated || m_base.isEmpty())
        return {};
    QUrl url = m_base;
    url.setPath(m_base.path() + QStringLiteral("/socket"));
    // The authenticated transport convention: https upgrades, and plain ws only where credentials may travel in the clear (loopback or the explicit opt-in). Authentication itself is impossible otherwise
    url.setScheme(url.scheme() == QLatin1String("https") ? QStringLiteral("wss")
                                                         : QStringLiteral("ws"));
    if (url.scheme() == QLatin1String("ws") && !transportAllowsAuthentication())
        return {};
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", tokenAuthorization());
    return request;
}

bool JellyfinClient::verifySession()
{
    if (m_state != State::Authenticated || m_verifyInFlight)
        return false;
    m_verifyInFlight = true;
    QNetworkReply *reply = m_network.get(requestFor(endpoint(QStringLiteral("/Users/Me")), true));
    attach(reply, m_generation, [this](QNetworkReply *finished) {
        m_verifyInFlight = false;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            return;
        }
        QJsonObject me;
        if (status != 200 || finished->error() != QNetworkReply::NoError
            || !parseObject(finished, &me))
            return;
        // The token belongs to another user than the one restored: the stored session is not this user's
        if (me.value(QStringLiteral("Id")).toString().compare(m_userId, Qt::CaseInsensitive) != 0) {
            invalidateSession();
            return;
        }
        const QString name = validUserName(me.value(QStringLiteral("Name")));
        if (!name.isEmpty())
            m_userName = name;
        m_userImageTag = validUserImageTag(me.value(QStringLiteral("PrimaryImageTag")));
        m_isAdministrator = userIsAdministrator(me);
        m_canDownload = userCanDownload(me);
        m_canManageSubtitles = userCanManageSubtitles(me);
        m_syncPlayAccess = userSyncPlayAccess(me);
        Q_EMIT userProfileRefreshed(m_userId, m_userName, m_userImageTag);
    });
    return true;
}

bool JellyfinClient::acceptAccountReply(QNetworkReply *finished, bool allowNoContent,
                                        const QString &failure, int *status, QString *reason)
{
    *status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (*status == 401) {
        invalidateSession();
        *reason = QStringLiteral("Authentication expired");
        return false;
    }
    const bool statusOk = *status == 200 || (allowNoContent && *status == 204);
    if (!statusOk || finished->error() != QNetworkReply::NoError) {
        *reason = failure;
        return false;
    }
    return true;
}

bool JellyfinClient::fetchCurrentUserConfiguration()
{
    if (m_state != State::Authenticated)
        return false;
    const QString family = QStringLiteral("UserConfiguration");
    const quint64 serial = supersedeListing(family);
    QNetworkReply *reply = m_network.get(requestFor(endpoint(QStringLiteral("/Users/Me")), true));
    m_listingReplies.insert(family, reply);
    attach(reply, m_generation, [this, family, serial](QNetworkReply *finished) {
        if (listingSuperseded(family, serial))
            return;
        int status = 0;
        QString reason;
        if (!acceptAccountReply(finished, false, QStringLiteral("User request failed"), &status,
                                &reason)) {
            Q_EMIT currentUserConfigurationFailed(status, reason);
            return;
        }
        QJsonObject user;
        if (!parseObject(finished, &user)) {
            Q_EMIT currentUserConfigurationFailed(status, QStringLiteral("Invalid user response"));
            return;
        }
        Q_EMIT currentUserConfigurationReady(user);
    });
    return true;
}

bool JellyfinClient::updateCurrentUserConfiguration(const QJsonObject &configuration)
{
    if (m_state != State::Authenticated || configuration.isEmpty())
        return false;
    const QByteArray body = QJsonDocument(configuration).toJson(QJsonDocument::Compact);
    if (body.size() > MaxResponseBytes)
        return false;
    const QString family = QStringLiteral("UserConfigurationUpdate");
    const quint64 serial = supersedeListing(family);
    QNetworkRequest request = requestFor(endpoint(QStringLiteral("/Users/Configuration")), true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_network.post(request, body);
    m_listingReplies.insert(family, reply);
    // The generation guard of attach() is what keeps a late 204 from reaching a later session
    attach(reply, m_generation, [this, family, serial](QNetworkReply *finished) {
        if (listingSuperseded(family, serial))
            return;
        int status = 0;
        QString reason;
        if (!acceptAccountReply(finished, true, QStringLiteral("Configuration update failed"),
                                &status, &reason)) {
            Q_EMIT userConfigurationUpdateFailed(status, reason);
            return;
        }
        Q_EMIT userConfigurationUpdated();
    });
    return true;
}

bool JellyfinClient::fetchCultures()
{
    if (m_state != State::Authenticated)
        return false;
    const QString family = QStringLiteral("Cultures");
    const quint64 serial = supersedeListing(family);
    QNetworkReply *reply =
        m_network.get(requestFor(endpoint(QStringLiteral("/Localization/Cultures")), true));
    m_listingReplies.insert(family, reply);
    attach(reply, m_generation, [this, family, serial](QNetworkReply *finished) {
        if (listingSuperseded(family, serial))
            return;
        int status = 0;
        QString reason;
        if (!acceptAccountReply(finished, false, QStringLiteral("Cultures request failed"), &status,
                                &reason)) {
            Q_EMIT culturesFailed(status, reason);
            return;
        }
        QJsonArray cultures;
        if (!parseArray(finished, &cultures) || cultures.size() > MaxCultures) {
            Q_EMIT culturesFailed(status, QStringLiteral("Invalid cultures response"));
            return;
        }
        Q_EMIT culturesReady(cultures);
    });
    return true;
}

bool JellyfinClient::fetchDisplayHomeSections()
{
    if (m_state != State::Authenticated)
        return false;
    const QString family = QStringLiteral("DisplayHomeSections");
    const quint64 serial = supersedeListing(family);
    QUrl url = endpoint(QStringLiteral("/DisplayPreferences/usersettings"));
    QUrlQuery wire;
    wire.addQueryItem(QStringLiteral("userId"), m_userId);
    // The namespace jellyfin-web writes its home sections to
    wire.addQueryItem(QStringLiteral("client"), QStringLiteral("emby"));
    url.setQuery(wire);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    m_listingReplies.insert(family, reply);
    attach(reply, m_generation, [this, family, serial](QNetworkReply *finished) {
        if (listingSuperseded(family, serial))
            return;
        int status = 0;
        QString reason;
        if (!acceptAccountReply(finished, false,
                                QStringLiteral("Display preferences request failed"), &status,
                                &reason)) {
            Q_EMIT displayHomeSectionsFailed(status, reason);
            return;
        }
        QJsonObject preferences;
        if (!parseObject(finished, &preferences)
            || !preferences.value(QStringLiteral("CustomPrefs")).isObject()
            || preferences.value(QStringLiteral("CustomPrefs")).toObject().size()
                   > MaxDisplayPreferenceEntries) {
            Q_EMIT displayHomeSectionsFailed(
                status, QStringLiteral("Invalid display preferences response"));
            return;
        }
        Q_EMIT displayHomeSectionsReady(
            preferences.value(QStringLiteral("CustomPrefs")).toObject());
    });
    return true;
}

QString JellyfinClient::listingKey(const QString &family, int channel)
{
    return channel == 0 ? family : family + QLatin1Char('#') + QString::number(channel);
}

void JellyfinClient::releaseListingChannel(int channel)
{
    if (channel == 0)
        return;
    for (const QString &family :
         {QStringLiteral("Items"), QStringLiteral("Resume"), QStringLiteral("Latest"),
          QStringLiteral("NextUp"), QStringLiteral("Similar"), QStringLiteral("Seasons"),
          QStringLiteral("Episodes"), QStringLiteral("Genres"), QStringLiteral("Filters"),
          QStringLiteral("FiltersLegacy"), QStringLiteral("MovieRecommendations"),
          QStringLiteral("Suggestions"), QStringLiteral("Upcoming"), QStringLiteral("Studios"),
          QStringLiteral("PlaylistEntries")}) {
        const QString key = listingKey(family, channel);
        if (!m_listingSerials.contains(key))
            continue;
        // Bump first, as supersedeListing does: the aborted reply's handler must see itself superseded
        supersedeListing(key);
        m_listingSerials.remove(key);
        m_listingReplies.remove(key);
    }
}

quint64 JellyfinClient::supersedeListing(const QString &family)
{
    // Bump first: abort() fires `finished` synchronously, and the handler of the aborted reply must already see itself superseded (same order as cancelPlaybackInfo)
    const quint64 serial = ++m_listingSerials[family];
    if (!m_listingReplies.value(family).isNull())
        m_listingReplies.take(family)->abort();
    return serial;
}

bool JellyfinClient::listingSuperseded(const QString &family, quint64 serial) const
{
    return m_listingSerials.value(family) != serial;
}

bool JellyfinClient::listingEnvelope(QNetworkReply *finished, const QString &family, int channel,
                                     QJsonArray *items, int *total)
{
    const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401) {
        invalidateSession();
        Q_EMIT listingFailed(family, status, QStringLiteral("Authentication expired"), channel);
        return false;
    }
    if (status != 200 || finished->error() != QNetworkReply::NoError) {
        Q_EMIT listingFailed(family, status, QStringLiteral("Listing request failed"), channel);
        return false;
    }
    QJsonObject data;
    if (!parseObject(finished, &data) || !data.value(QStringLiteral("Items")).isArray()
        || !data.value(QStringLiteral("TotalRecordCount")).isDouble()) {
        Q_EMIT listingFailed(family, status, QStringLiteral("Invalid listing response"), channel);
        return false;
    }
    *total = data.value(QStringLiteral("TotalRecordCount")).toInt(-1);
    *items = data.value(QStringLiteral("Items")).toArray();
    if (*total < items->size()) {
        Q_EMIT listingFailed(family, status, QStringLiteral("Invalid listing pagination"), channel);
        return false;
    }
    return true;
}

bool JellyfinClient::listingArray(QNetworkReply *finished, const QString &family, int channel,
                                  QJsonArray *items)
{
    const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401) {
        invalidateSession();
        Q_EMIT listingFailed(family, status, QStringLiteral("Authentication expired"), channel);
        return false;
    }
    if (status != 200 || finished->error() != QNetworkReply::NoError) {
        Q_EMIT listingFailed(family, status, QStringLiteral("Listing request failed"), channel);
        return false;
    }
    if (!parseArray(finished, items)) {
        Q_EMIT listingFailed(family, status, QStringLiteral("Invalid listing response"), channel);
        return false;
    }
    return true;
}

bool JellyfinClient::listingObject(QNetworkReply *finished, const QString &family, int channel,
                                   QJsonObject *object)
{
    const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401) {
        invalidateSession();
        Q_EMIT listingFailed(family, status, QStringLiteral("Authentication expired"), channel);
        return false;
    }
    if (status != 200 || finished->error() != QNetworkReply::NoError) {
        Q_EMIT listingFailed(family, status, QStringLiteral("Listing request failed"), channel);
        return false;
    }
    if (!parseObject(finished, object)) {
        Q_EMIT listingFailed(family, status, QStringLiteral("Invalid listing response"), channel);
        return false;
    }
    return true;
}

void JellyfinClient::attach(QNetworkReply *reply, quint64 generation,
                            const std::function<void(QNetworkReply *)> &handler)
{
    m_pending.insert(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, handler] {
        m_pending.remove(reply);
        reply->deleteLater();
        if (generation == m_generation)
            handler(reply);
    });
}

void JellyfinClient::abortPending()
{
    ++m_generation;
    m_favoritePending.clear();
    m_playedPending.clear();
    m_playlistPending.clear();
    // Reports die with the pending requests, quietly: the generation bump keeps their handlers from running. The same goes for a session re-check whose flag would otherwise stay set forever
    m_reportQueue.clear();
    m_reportBusy = false;
    m_reportRetryTimer.stop();
    m_syncPlayQueue.clear();
    m_syncPlayBusy = false;
    m_verifyInFlight = false;
    const auto pending = m_pending;
    m_pending.clear();
    for (QNetworkReply *reply : pending)
        reply->abort();
}

void JellyfinClient::clearToken()
{
    const bool hadToken = !m_token.isEmpty();
    m_token.fill('\0');
    m_token.clear();
    m_userId.clear();
    m_userName.clear();
    m_userImageTag.clear();
    m_isAdministrator = false;
    m_canDownload = false;
    m_canManageSubtitles = false;
    m_syncPlayAccess.clear();
    if (hadToken)
        Q_EMIT credentialsCleared();
}

void JellyfinClient::setMediaControlAnnounced(bool announced)
{
    m_mediaControlAnnounced = announced;
}

void JellyfinClient::invalidateSession()
{
    if (m_state != State::Authenticated)
        return;
    const QString expiredUser = m_userId;
    abortPending();
    clearToken();
    m_state = State::Ready;
    Q_EMIT sessionExpired(expiredUser);
}

void JellyfinClient::failProbe(int status, const QString &reason, bool certificateRejected)
{
    abortPending();
    clearToken();
    clearQuickConnectSecret();
    m_state = State::Disconnected;
    m_serverId.clear();
    m_base = {};
    if (certificateRejected)
        Q_EMIT serverCertificateRejected();
    else
        Q_EMIT requestFailed(status, reason);
}

void JellyfinClient::disconnectFromServer()
{
    abortPending();
    clearToken();
    clearQuickConnectSecret();
    m_serverId.clear();
    m_base = {};
    m_state = State::Disconnected;
    m_allowInsecureHttp = false;
}

void JellyfinClient::forgetSession()
{
    abortPending();
    clearToken();
    m_state = State::Ready;
}

QUrl JellyfinClient::endpoint(const QString &path) const
{
    QUrl url = m_base;
    url.setPath(m_base.path() + path);
    return url;
}

QNetworkRequest JellyfinClient::requestFor(const QUrl &url, bool authenticated) const
{
    QNetworkRequest request(url);
    // Tokens are never propagated by Qt's automatic redirect handling
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    request.setRawHeader("Accept", "application/json");
    if (authenticated) {
        request.setRawHeader("Authorization", tokenAuthorization());
    }
    return request;
}

bool JellyfinClient::isAuthenticated() const
{
    return m_state == State::Authenticated;
}
bool JellyfinClient::isPlainHttpRemote() const
{
    return m_base.scheme() == QLatin1String("http") && !isLoopback(m_base);
}
QUrl JellyfinClient::serverUrl() const
{
    return m_base;
}
QString JellyfinClient::serverId() const
{
    return m_serverId;
}
QString JellyfinClient::userId() const
{
    return m_userId;
}
QString JellyfinClient::userName() const
{
    return m_userName;
}
QString JellyfinClient::userImageTag() const
{
    return m_userImageTag;
}
bool JellyfinClient::isAdministrator() const
{
    return m_isAdministrator;
}

bool JellyfinClient::canManageSubtitles() const
{
    return m_state == State::Authenticated && m_canManageSubtitles;
}

bool JellyfinClient::canDownload() const
{
    return m_state == State::Authenticated && m_canDownload;
}

QString JellyfinClient::syncPlayAccess() const
{
    return m_state == State::Authenticated ? m_syncPlayAccess : QString();
}
QString JellyfinClient::deviceId() const
{
    return m_deviceId;
}
QString JellyfinClient::deviceName() const
{
    return m_deviceName;
}
QString JellyfinClient::accessToken() const
{
    return m_state == State::Authenticated ? QString::fromUtf8(m_token) : QString();
}

bool JellyfinClient::canSendCredentials() const
{
    return transportAllowsAuthentication();
}

bool JellyfinClient::restoreSession(const QString &userId, const QString &token)
{
    if (m_state != State::Ready || m_deviceId.isEmpty() || userId.isEmpty() || token.isEmpty()
        || !validHeaderField(token) || !transportAllowsAuthentication())
        return false;
    m_token = token.toUtf8();
    m_userId = userId;
    m_state = State::Authenticated;
    Q_EMIT loginSucceeded(m_userId);
    return true;
}

} // namespace Kinefin
