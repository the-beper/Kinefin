// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// An in-process fake Jellyfin server and the helpers shared by the API client tests. Header-only on purpose: every test target includes it (KINEFIN_SOURCE_DIR must be defined for the fixtures)
#pragma once

#include "api/JellyfinClient.h"

#include "FakeSocketConnection.h"

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>

#include <algorithm>
#include <optional>

namespace {

// A valid 1x1 PNG, the body of every image route
const QByteArray kTinyPng =
    QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwG"
                           "A60e6kgAAAABJRU5ErkJggg==");

inline QByteArray fixture(const QString &name)
{
    QFile file(QString::fromUtf8(KINEFIN_SOURCE_DIR) + QStringLiteral("/tests/fixtures/") + name
               + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

class FakeJellyfin {
  public:
    FakeJellyfin()
    {
        listingFixtures.insert(QStringLiteral("Items"), QStringLiteral("items_page"));
        listingFixtures.insert(QStringLiteral("Resume"), QStringLiteral("user_items_resume"));
        listingFixtures.insert(QStringLiteral("Latest"), QStringLiteral("items_latest"));
        listingFixtures.insert(QStringLiteral("NextUp"), QStringLiteral("shows_next_up"));
        listingFixtures.insert(QStringLiteral("Seasons"), QStringLiteral("shows_seasons"));
        listingFixtures.insert(QStringLiteral("Episodes"), QStringLiteral("episodes_page"));
        listingFixtures.insert(QStringLiteral("Similar"), QStringLiteral("items_similar"));
        listingFixtures.insert(QStringLiteral("Genres"), QStringLiteral("genres_page"));
        listingFixtures.insert(QStringLiteral("Filters"), QStringLiteral("items_filters2"));
        listingFixtures.insert(QStringLiteral("FiltersLegacy"),
                               QStringLiteral("items_filters_legacy"));
        listingFixtures.insert(QStringLiteral("MovieRecommendations"),
                               QStringLiteral("movies_recommendations"));
        listingFixtures.insert(QStringLiteral("Suggestions"), QStringLiteral("items_page"));
        listingFixtures.insert(QStringLiteral("Upcoming"), QStringLiteral("shows_next_up"));
        listingFixtures.insert(QStringLiteral("Studios"), QStringLiteral("studios_page"));
        listingFixtures.insert(QStringLiteral("Sessions"), QStringLiteral("sessions_list"));
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket] { readRequest(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                    m_buffers.remove(socket);
                    for (qsizetype i = heldDownloads.size() - 1; i >= 0; --i) {
                        if (heldDownloads.at(i).socket == socket)
                            heldDownloads.removeAt(i);
                    }
                    // A later socket may be allocated at this address and must not inherit the WebSocket role
                    m_wsTaken.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    bool listen(const QHostAddress &address = QHostAddress::LocalHost)
    {
        return m_server.listen(address);
    }

    ~FakeJellyfin() { qDeleteAll(m_wsConnections); }

    QUrl url(const QString &base = QString()) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(base));
    }

    // The login response body, with the access token substituted on later successful logins when rotation is on. With a user table, the body carries that user's token, id, name and avatar tag (the first user when `username` is empty or unknown)
    QByteArray authenticateBody(const QString &username = {}) const
    {
        auto document = QJsonDocument::fromJson(fixture(QStringLiteral("authenticate_success")));
        QJsonObject object = document.object();
        const FakeUser *found = userByName(username);
        const FakeUser *account = found ? found : (users.isEmpty() ? nullptr : &users.constFirst());
        QJsonObject policedUser = object.value(QStringLiteral("User")).toObject();
        QJsonObject policy = policedUser.value(QStringLiteral("Policy")).toObject();
        policy.insert(QStringLiteral("IsAdministrator"), administratorValue(account));
        policy.insert(QStringLiteral("EnableContentDownloading"), canDownloadValue());
        policy.insert(QStringLiteral("EnableSubtitleManagement"), canManageSubtitlesValue());
        if (!syncPlayAccess.isEmpty())
            policy.insert(QStringLiteral("SyncPlayAccess"), syncPlayAccess);
        policedUser.insert(QStringLiteral("Policy"), policy);
        object.insert(QStringLiteral("User"), policedUser);
        if (account) {
            const FakeUser &user = *account;
            QJsonObject userObject = object.value(QStringLiteral("User")).toObject();
            userObject.insert(QStringLiteral("Id"), user.id);
            userObject.insert(QStringLiteral("Name"), user.name);
            userObject.remove(QStringLiteral("PrimaryImageTag"));
            if (!user.tag.isEmpty())
                userObject.insert(QStringLiteral("PrimaryImageTag"), user.tag);
            object.insert(QStringLiteral("User"), userObject);
            object.insert(QStringLiteral("AccessToken"), QString::fromUtf8(user.token));
        } else if (rotateSecondLoginToken && successfulLogins >= 2) {
            object.insert(QStringLiteral("AccessToken"), QString::fromUtf8(secondLoginToken));
        }
        document.setObject(object);
        return document.toJson(QJsonDocument::Compact);
    }

    // The user table: when not empty, a login names one of these (any password), /Users/Me answers by the token of the request, /Users/Public lists the visible ones and /UserImage serves the ones with a tag. Empty keeps the single fixture user
    struct FakeUser {
        QString id;
        QByteArray token;
        QString name;
        QString tag; // empty: no avatar (the field is absent and /UserImage answers 404)
        bool hidden = false;
        bool administrator = true;
    };
    QList<FakeUser> users;
    // `Policy.IsAdministrator` of the single fixture user (the login reply and /Users/Me); a user of the table has its own. `administratorRaw`, when set, replaces the value for everyone and is sent as is (an Undefined value leaves the field out)
    bool administrator = true;
    std::optional<QJsonValue> administratorRaw;
    QJsonValue administratorValue(const FakeUser *user) const
    {
        if (administratorRaw)
            return *administratorRaw;
        return user ? user->administrator : administrator;
    }
    // `Policy.EnableContentDownloading` of every user (the login reply and /Users/Me); `canDownloadRaw` replaces the value as is
    bool canDownload = true;
    std::optional<QJsonValue> canDownloadRaw;
    QJsonValue canDownloadValue() const
    {
        return canDownloadRaw ? *canDownloadRaw : QJsonValue(canDownload);
    }
    // `Policy.EnableSubtitleManagement` of every user (the login reply and /Users/Me); `canManageSubtitlesRaw` replaces the value as is
    bool canManageSubtitles = true;
    std::optional<QJsonValue> canManageSubtitlesRaw;
    QJsonValue canManageSubtitlesValue() const
    {
        return canManageSubtitlesRaw ? *canManageSubtitlesRaw : QJsonValue(canManageSubtitles);
    }
    // `Policy.SyncPlayAccess` of every user (the login reply and /Users/Me); empty leaves the field out
    QString syncPlayAccess;
    QJsonObject policyOf(const FakeUser *user) const
    {
        QJsonObject policy{{QStringLiteral("IsAdministrator"), administratorValue(user)},
                           {QStringLiteral("EnableContentDownloading"), canDownloadValue()},
                           {QStringLiteral("EnableSubtitleManagement"), canManageSubtitlesValue()}};
        if (!syncPlayAccess.isEmpty())
            policy.insert(QStringLiteral("SyncPlayAccess"), syncPlayAccess);
        return policy;
    }
    // Tokens /Users/Me answers 401 to (the per-user deny switch)
    QSet<QByteArray> deniedUsersMeTokens;
    const FakeUser *userByName(const QString &name) const
    {
        for (const FakeUser &user : users) {
            if (user.name == name)
                return &user;
        }
        return nullptr;
    }
    const FakeUser *userByToken(const QByteArray &token) const
    {
        for (const FakeUser &user : users) {
            if (user.token == token)
                return &user;
        }
        return nullptr;
    }
    // /Users/Public: a status or a raw body replaces the generated list
    int publicUsersRequests = 0;
    QByteArray lastPublicUsersAuth;
    int publicUsersStatus = 200;
    QByteArray publicUsersRawBody;
    // /UserImage
    int userImageRequests = 0;
    QByteArray lastUserImageRoute;
    QByteArray lastUserImageAuth;

    int publicInfoRequests = 0;
    // TLS ClientHellos received on the plain port: a real HTTP server closes on them, and so does this one
    int tlsAttempts = 0;
    QByteArray lastPublicInfoAuth;
    bool invalidIdentity = false;
    bool denyLogin = false;
    bool denyViews = false;
    bool redirectPublicOffHost = false;
    bool relocatePublicOnSameHost = false;
    bool redirectViewsOffHost = false;
    bool holdViews = false;
    // A body to answer /UserViews with instead of the fixture
    QByteArray viewsRawBody;
    bool disableQuickConnect = false;
    bool invalidQuickConnectCode = false;
    bool rejectQuickConnectStatus = false;
    bool mismatchQuickConnectSecret = false;
    bool approveQuickConnect = false;
    bool denyQuickConnectExchange = false;
    bool holdQuickConnectPoll = false;
    int quickInitiationRequests = 0;
    int quickStatusRequests = 0;
    int quickExchangeRequests = 0;
    QByteArray lastQuickExchangeBody;
    QByteArray lastQuickExchangeAuth;
    QByteArray lastQuickPollAuth;
    QString lastQuickPollSecret;
    int viewRequests = 0;
    // Images: GET .../Images/... answers a 1x1 PNG, or imageStatus, or a redirect off the host
    int imageRequests = 0;
    int imageStatus = 200;
    bool redirectImages = false;
    QByteArray imageBody; // overrides the PNG when not empty
    QByteArray lastImageRoute;
    QByteArray lastImageAuth;
    QByteArray lastImageCookie;
    // GET /Items/{id}/Download: a deterministic body per id, Range/If-Range honoured unless a knob says otherwise
    int downloadRequests = 0;
    QByteArray lastDownloadRoute;
    QByteArray lastDownloadAuth;
    QByteArray lastDownloadRange;
    QByteArray lastDownloadIfRange;
    int downloadStatus = 0;
    bool redirectDownload = false;
    bool holdDownload = false;
    bool downloadIgnoreRange = false;
    QByteArray downloadBody;
    // The HTTP ETag header, independent of the item's own MediaSources ETag; empty sends none, and Last-Modified then serves as the validator
    QByteArray downloadEtag = QByteArrayLiteral("\"etag-1\"");
    QByteArray downloadLastModified;
    // Answers 206 even without a Range header (from byte 0)
    bool downloadForce206 = false;
    // Shifts the start of every 206 by this many bytes and says so in Content-Range
    qint64 downloadStartShift = 0;
    // The totals of the next 206 Content-Range headers, one per request, instead of the real size
    QList<qint64> downloadClaimedTotals;
    // Sends no Content-Length, so the body ends with the connection
    bool downloadOmitLength = false;
    // Remote subtitle search/download through the server plugin
    QByteArray remoteSubtitleSearchBody = "[]";
    int remoteSubtitleSearchStatus = 0;
    int remoteSubtitleDownloadStatus = 204;
    QList<QByteArray> remoteSubtitleSearches;
    QList<QByteArray> remoteSubtitleDownloads;
    struct HeldDownload {
        QTcpSocket *socket = nullptr;
        int status = 0;
        QByteArray body;
        QByteArray extra;
    };
    QList<HeldDownload> heldDownloads;
    QByteArray downloadBodyFor(const QByteArray &id) const
    {
        if (!downloadBody.isEmpty())
            return downloadBody;
        QByteArray body(64 * 1024, '\0');
        for (qsizetype i = 0; i < body.size(); ++i)
            body[i] = id.at(i % id.size());
        return body;
    }
    void releaseDownloads()
    {
        const QList<HeldDownload> held = heldDownloads;
        heldDownloads.clear();
        for (const HeldDownload &entry : held)
            respondBytes(entry.socket, entry.status, entry.body, entry.extra);
    }
    // Every plain HTTP request that was parsed, whatever the route (a refused image request must leave this at 0)
    int requestCount = 0;
    QByteArray lastViewsAuth;
    QByteArray lastLoginAuth;
    // PlaybackInfo: the fixture to answer with, or a status / raw body override
    QString playbackInfoFixture = QStringLiteral("playbackinfo_direct");
    int playbackInfoStatus = 0;
    QByteArray playbackInfoRawBody;
    bool holdPlaybackInfo = false;
    bool redirectPlaybackInfo = false;
    int playbackInfoRequests = 0;
    QByteArray lastPlaybackInfoRoute;
    QByteArray lastPlaybackInfoAuth;
    QByteArray lastPlaybackInfoBody;
    QByteArray lastPlaybackInfoContentType;
    // Optional player endpoints, with controllable failure and redirect responses.
    int mediaSegmentsRequests = 0;
    int mediaSegmentsStatus = 200;
    QByteArray mediaSegmentsBody = "{\"Items\":[]}";
    QByteArray mediaSegmentsAuth;
    int subtitleDownloadRequests = 0;
    QByteArray subtitleDownloadAuth;
    QByteArray subtitleDownloadBody = "1\n00:00:00,000 --> 00:00:01,000\nHello\n";
    int subtitleDownloadStatus = 200;
    bool redirectSubtitleDownload = false;
    int trickplayMetadataRequests = 0;
    // The request line's target of the last playback item request
    QByteArray trickplayMetadataRoute;
    QByteArray trickplayMetadataBody = "{\"Trickplay\":{}}";
    int trickplayMetadataStatus = 200;
    int trickplayTileRequests = 0;
    QByteArray trickplayTileAuth;
    QByteArray trickplayTileBody = QByteArray::fromHex("ffd8ffe00010ffd9");
    int trickplayTileStatus = 200;
    // Playback reports: an ordered log of what arrived, a status for all of them, an optional different status for the first Stopped, and a hold that keeps the answers back until releaseHeld()
    struct ReportLog {
        QByteArray route;
        QByteArray body;
        QByteArray auth;
        QByteArray contentType;
    };
    QList<ReportLog> reportLog;
    int reportStatus = 204;
    // Remote playstate commands: authenticated POSTs to a listed session.
    QList<QByteArray> remoteCommandRoutes;
    QList<QByteArray> remoteCommandAuthorizations;
    int remoteCommandStatus = 204;
    bool redirectRemoteCommands = false;
    bool holdRemoteCommands = false;
    // SyncPlay: `GET /SyncPlay/List` answers syncPlayListStatus with syncPlayListBody (the syncplay_group_list fixture when empty), `GET /GetUtcTime` utcTimeStatus with utcTimeBody (the get_utc_time fixture when empty); every `POST /SyncPlay/<route>` is recorded and answers syncPlayStatus, New with the syncplay_group fixture when that is 200
    int syncPlayListStatus = 200;
    QByteArray syncPlayListBody;
    int syncPlayListRequests = 0;
    int utcTimeStatus = 200;
    QByteArray utcTimeBody;
    int utcTimeRequests = 0;
    // `GET /GetUtcTime` answers with the current time (for tests that time commands)
    bool utcTimeNow = false;
    int syncPlayStatus = 204;
    // SyncPlay posts are answered on releaseHeld() (with reportStatus)
    bool holdSyncPlay = false;
    struct SyncPlayRequest {
        QString route;
        QJsonObject body;
        QByteArray authorization;
        QByteArray contentType;
    };
    QList<SyncPlayRequest> syncPlayRequests;
    struct FavoriteWrite {
        QByteArray method;
        QByteArray route;
        QByteArray authorization;
    };
    QList<FavoriteWrite> favoriteWrites;
    int favoriteWriteStatus = 200;
    QByteArray favoriteWriteRawBody;
    bool holdFavoriteWrites = false;
    bool redirectFavoriteWrites = false;
    struct PlayedWrite {
        QByteArray method;
        QByteArray route;
        QByteArray authorization;
    };
    QList<PlayedWrite> playedWrites;
    int playedWriteStatus = 200;
    QByteArray playedWriteRawBody;
    bool holdPlayedWrites = false;
    bool redirectPlayedWrites = false;
    int firstStoppedStatus = 0;
    int stoppedRequests = 0;
    bool holdReports = false;
    bool redirectReports = false;
    int capabilitiesRequests = 0;
    QByteArray lastCapabilitiesBody;
    QByteArray lastCapabilitiesAuth;
    // WebSocket upgrade: the attempt is recorded at the header boundary before any accept decision, so tests assert the handshake itself (`Token` only, never `api_key` or a query)
    int upgradeAttempts = 0;
    QString lastSocketRoute;
    QByteArray lastSocketAuth;
    // Everything after the '?' of the upgrade request line; must stay empty
    QString lastSocketQuery;
    // 0 accepts the 101; otherwise the upgrade is answered with this status as plain HTTP
    int socketUpgradeStatus = 0;
    // Keep the TCP connection open but answer nothing: the client-side connect timeout shape
    bool holdSocketUpgrade = false;
    // Login tokens: the fixture one on the first success, a different one on later successes
    bool rotateSecondLoginToken = false;
    QByteArray secondLoginToken = "f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7";
    int successfulLogins = 0;
    // GET /Users/Me (the session re-verification): counts and identity, with a deny switch
    int usersMeRequests = 0;
    QByteArray lastUsersMeAuth;
    bool denyUsersMe = false;
    // GET /Users/Me: a status (non-zero) or a raw body replaces the generated user, after the deny switches
    int usersMeStatus = 0;
    QByteArray usersMeRawBody;
    // The account preference routes: POST /Users/Configuration, GET /Localization/Cultures and GET /DisplayPreferences/usersettings. Every request to them is logged; `holdAccount` keeps the answers back until releaseHeld(); a status (non-zero) or a raw body replaces the default answer of a route
    struct AccountCall {
        QByteArray method;
        QByteArray route;
        QByteArray auth;
        QByteArray contentType;
        QByteArray body;
    };
    QList<AccountCall> accountCalls;
    bool holdAccount = false;
    int userConfigurationStatus = 204;
    int culturesStatus = 0;
    QByteArray culturesRawBody;
    int displayPreferencesStatus = 0;
    QByteArray displayPreferencesRawBody;
    // Called before GET /Users/Me is answered (so a test can change usersMeRawBody per request) and with the body of a POST /Users/Configuration before its answer (so a test can store it, or end the session in the middle of the write)
    std::function<void()> onUsersMe;
    std::function<void(const QByteArray &)> onUserConfigurationPost;
    // Every plain HTTP request in arrival order, method and route (the whole-client check that a route is never written to)
    QList<QPair<QByteArray, QByteArray>> requestLog;

    // The listing routes (fetchItems and family). `holdFirstListings` holds that many next listing replies suppressed until releaseHeld(), `holdListings` holds every one of them, `denyListings` answers 401, `redirectListings` answers 302 off-host. The family (a key of the two last* hashes) is classified from the route fragment, so a test can assert the exact query the client sent. Fixture bodies come from `listingFixtures` (the ctor fills the defaults; a test may override a family)
    int holdFirstListings = 0;
    bool holdListings = false;
    // Families whose requests are held like holdListings does, the others are answered
    QSet<QString> heldFamilies;
    bool denyListings = false;
    bool redirectListings = false;
    QHash<QString, QString> listingFixtures;
    // Raw body per family, overriding the fixture when set (like playbackInfoRawBody)
    QHash<QString, QByteArray> listingRawBody;
    int listingRequests = 0;
    // Status per family answered instead of the body (a 5xx for a failing page)
    QHash<QString, int> listingStatus;
    // When >= 0 the Items family is generated from startIndex/limit: `generatedItemTotal` is the TotalRecordCount, `generatedAvailable` (when >= 0) the rows that really exist, so a page past it comes back short; ids, names and image tags follow the row index
    int generatedItemTotal = -1;
    int generatedAvailable = -1;
    // When >= 0 the Resume family is generated from startIndex/limit like the Items family: this is the TotalRecordCount
    int generatedResumeTotal = -1;
    // When >= 0 the Episodes family is generated from startIndex/limit, like generatedItemTotal does for Items: this is the TotalRecordCount
    int generatedEpisodeTotal = -1;
    QList<QByteArray> itemsRoutes; // every Items request in arrival order
    // A test's own answer to an Items request, from its route: a status of 0 passes to the other answers
    std::function<std::pair<int, QByteArray>(const QByteArray &route)> itemsResponder;
    // Items by lower-case id, each a JSON object: when not empty, an Items request with `ids=` answers the entries it finds (an empty envelope when none), whatever the listing fixture says
    QHash<QString, QByteArray> itemsById;
    // A search answers by the one type it asks for: `includeItemTypes` value -> fixture name. Only a request with a searchTerm is answered this way
    QHash<QString, QString> itemsByType;
    // A genre rail answers by the one genre id it asks for: `genreIds` value -> fixture name. Only a request with a genreIds is answered this way
    QHash<QString, QString> itemsByGenre;
    // When >= 0 the Genres family is generated from startIndex/limit: this is the TotalRecordCount
    int generatedGenreTotal = -1;
    // An ids= answer in the reverse of the order asked: the server does not promise to keep it
    bool reverseIdsAnswer = false;
    // The first this many generated episodes are played
    int generatedEpisodesPlayed = 0;
    QHash<QString, QByteArray> lastListingRoute;     // family -> "path?query"
    QHash<QString, QList<QByteArray>> listingRoutes; // family -> every route in arrival order
    QHash<QString, QByteArray> lastListingAuth;      // family -> the Authorization header

    // The playlist routes over an in-memory store, answering as the 12.4 server did (every one answers 401 to an Authorization without the token of the session): create answers 200 `{Id}`; entries carry a `PlaylistItemId` and the copies of one item share it, so removing it removes every copy; a move names the entry, an unknown entry is a silent 204 and an index past the end answers 500; removing an unknown entry is a silent 204; rename and delete answer 204. A write for an unknown playlist answers 404, a create without a name 400. `GET /Items/{id}` answers the item's `Type` (a playlist, with its `CanDelete`, or an entry of `typedItems`), 404 otherwise
    struct FakePlaylistEntry {
        QString entryId;
        QString itemId;
    };
    struct FakePlaylist {
        QString id;
        QString name;
        QList<FakePlaylistEntry> entries;
    };
    struct PlaylistRequest {
        QByteArray method;
        QByteArray route;
        QByteArray body;
        QByteArray authorization;
        QByteArray contentType;
    };
    QList<FakePlaylist> playlists;
    // Every request to a /Playlists route, and every DELETE /Items/{id}, in arrival order
    QList<PlaylistRequest> playlistRequests;
    // Not 0: every write (not the entries read) answers with this status and leaves the store alone
    int playlistWriteStatus = 0;
    // Writes are applied on arrival but answered on releasePlaylistWrites()
    bool holdPlaylistWrites = false;
    // When not empty, the body of the 200 a create answers instead of `{Id}` (the playlist is still created)
    QByteArray playlistCreateRawBody;
    // Not playlists: item id -> Type, answered by GET /Items/{id} (a DELETE of one still answers 404)
    QHash<QString, QString> typedItems;
    // GET /Items/{id}: a non-zero status, or a non-empty raw body, replaces the answer
    int playlistItemStatus = 0;
    // The `CanDelete` GET /Items/{id} answers for a playlist (not sent for other items)
    bool playlistsDeletable = true;
    QByteArray playlistItemRawBody;
    // Writes are applied, then the connection is closed with no answer (a transport error for the client)
    bool dropPlaylistWrites = false;
    qsizetype heldPlaylistCount() const { return m_heldPlaylistWrites.size(); }
    // Not 0: every entries read (`GET /Playlists/{id}/Items`) of a known playlist answers with this status
    int playlistEntriesStatus = 0;
    // Entries reads are answered on releasePlaylistEntries()
    bool holdPlaylistEntries = false;
    qsizetype heldPlaylistEntriesCount() const { return m_heldPlaylistEntries.size(); }

    void releasePlaylistEntries()
    {
        const auto held = m_heldPlaylistEntries;
        m_heldPlaylistEntries.clear();
        for (const auto &read : held) {
            if (read.socket)
                respond(read.socket, read.status, read.body);
        }
    }

    void releasePlaylistWrites()
    {
        const auto held = m_heldPlaylistWrites;
        m_heldPlaylistWrites.clear();
        for (const auto &write : held) {
            if (write.socket)
                respond(write.socket, write.status, write.body);
        }
    }

    // A playlist made without a request: its id
    QString seedPlaylist(const QString &name, const QStringList &itemIds)
    {
        FakePlaylist playlist{newFakeId(QLatin1Char('b')), name, {}};
        appendPlaylistItems(&playlist, itemIds);
        playlists.append(playlist);
        return playlist.id;
    }

    FakePlaylist *playlistById(const QString &id)
    {
        for (FakePlaylist &playlist : playlists) {
            if (playlist.id.compare(id, Qt::CaseInsensitive) == 0)
                return &playlist;
        }
        return nullptr;
    }

    // The entry id of an item in a playlist, empty when it is not there
    QString entryIdOf(const QString &playlistId, const QString &itemId)
    {
        if (const FakePlaylist *playlist = playlistById(playlistId)) {
            for (const FakePlaylistEntry &entry : playlist->entries) {
                if (entry.itemId == itemId)
                    return entry.entryId;
            }
        }
        return {};
    }

    // The item ids of a playlist in order
    QStringList playlistItemIds(const QString &playlistId)
    {
        QStringList ids;
        if (const FakePlaylist *playlist = playlistById(playlistId)) {
            for (const FakePlaylistEntry &entry : playlist->entries)
                ids.append(entry.itemId);
        }
        return ids;
    }

    // The most recent accepted WebSocket connection (nullptr until one handshake completes)
    FakeSocketConnection *lastConnection() const
    {
        return m_wsConnections.isEmpty() ? nullptr : m_wsConnections.constLast();
    }

    // How many replies are being held; a test waits for it before releasing, as the request may not have reached the server yet
    qsizetype heldCount() const { return m_held.size(); }

    // `snapshotItemsReads` answers each /Items read from the state at request time, but only on releaseSnapshots(): a read the server served before a concurrent write was applied, delivered after that write's answer
    bool snapshotItemsReads = false;
    qsizetype snapshotCount() const { return m_snapshots.size(); }

    void releaseSnapshots()
    {
        const auto snapshots = m_snapshots;
        m_snapshots.clear();
        for (const auto &snapshot : snapshots) {
            if (snapshot.first)
                respond(snapshot.first, 200, snapshot.second);
        }
    }

    void releaseHeld()
    {
        const auto held = m_held;
        m_held.clear();
        for (const QPointer<QTcpSocket> &socket : held) {
            if (socket)
                respond(socket, reportStatus, {});
        }
    }

    // The listing-family fragment of a request route, or "" when it is not one. The specific fragments come first so `/Items/Latest` never reads as the generic /Items
    static QString classifyListingRoute(const QByteArray &route)
    {
        if (route.contains("/UserItems/Resume?"))
            return QStringLiteral("Resume");
        if (route.contains("/Items/Latest?"))
            return QStringLiteral("Latest");
        if (route.contains("/Shows/NextUp?"))
            return QStringLiteral("NextUp");
        if (route.contains("/Seasons?"))
            return QStringLiteral("Seasons");
        if (route.contains("/Episodes?"))
            return QStringLiteral("Episodes");
        if (route.contains("/Similar?"))
            return QStringLiteral("Similar");
        if (route.contains("/Genres?"))
            return QStringLiteral("Genres");
        if (route.contains("/Filters2?"))
            return QStringLiteral("Filters");
        if (route.contains("/Items/Filters?"))
            return QStringLiteral("FiltersLegacy");
        if (route.contains("/Movies/Recommendations?"))
            return QStringLiteral("MovieRecommendations");
        if (route.contains("/Items/Suggestions?"))
            return QStringLiteral("Suggestions");
        if (route.contains("/Shows/Upcoming?"))
            return QStringLiteral("Upcoming");
        if (route.contains("/Studios?"))
            return QStringLiteral("Studios");
        if (route.endsWith("/Sessions") || route.contains("/Sessions?"))
            return QStringLiteral("Sessions");
        if (route.contains("/Items?"))
            return QStringLiteral("Items");
        return {};
    }

    // The full query of a captured listing request: tests dissect it with QUrlQuery
    static QUrlQuery capturedListingQuery(const QByteArray &route)
    {
        QByteArray full = QByteArrayLiteral("http://127.0.0.1");
        full += route;
        return QUrlQuery(QUrl::fromEncoded(full));
    }

  private:
    struct HeldPlaylistWrite {
        QPointer<QTcpSocket> socket;
        int status = 204;
        QByteArray body;
    };

    QString newFakeId(QChar fill)
    {
        return QString::number(++m_nextFakeId, 16).rightJustified(32, fill);
    }

    // The copies of one item share the entry id of its first one
    void appendPlaylistItems(FakePlaylist *playlist, const QStringList &itemIds)
    {
        for (const QString &itemId : itemIds) {
            QString entryId;
            for (const FakePlaylistEntry &entry : std::as_const(playlist->entries)) {
                if (entry.itemId == itemId)
                    entryId = entry.entryId;
            }
            playlist->entries.append(
                {entryId.isEmpty() ? newFakeId(QLatin1Char('c')) : entryId, itemId});
        }
    }

    // The playlist listing (`includeItemTypes=Playlist`) over the store, by name, paged by `limit`
    QByteArray playlistsListingBody(const QByteArray &route) const
    {
        QList<FakePlaylist> sorted = playlists;
        std::sort(sorted.begin(), sorted.end(), [](const FakePlaylist &a, const FakePlaylist &b) {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
        const QUrlQuery query = capturedListingQuery(route);
        const int limit = query.hasQueryItem(QStringLiteral("limit"))
                              ? query.queryItemValue(QStringLiteral("limit")).toInt()
                              : int(sorted.size());
        QJsonArray items;
        for (qsizetype row = 0; row < sorted.size() && row < limit; ++row)
            items.append(
                QJsonObject{{QStringLiteral("Id"), sorted.at(row).id},
                            {QStringLiteral("Name"), sorted.at(row).name},
                            {QStringLiteral("Type"), QStringLiteral("Playlist")},
                            {QStringLiteral("MediaType"), QStringLiteral("Video")},
                            {QStringLiteral("ChildCount"), int(sorted.at(row).entries.size())}});
        return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                         {QStringLiteral("StartIndex"), 0},
                                         {QStringLiteral("TotalRecordCount"), int(sorted.size())}})
            .toJson(QJsonDocument::Compact);
    }

    QJsonObject playlistEntryJson(const FakePlaylistEntry &entry) const
    {
        const QByteArray raw = itemsById.value(entry.itemId.toLower());
        QJsonObject item = raw.isEmpty()
                               ? QJsonObject{{QStringLiteral("Id"), entry.itemId},
                                             {QStringLiteral("Name"),
                                              QString(QStringLiteral("Item ") + entry.itemId)},
                                             {QStringLiteral("Type"), QStringLiteral("Movie")}}
                               : QJsonDocument::fromJson(raw).object();
        item.insert(QStringLiteral("PlaylistItemId"), entry.entryId);
        return item;
    }

    // Whether an Authorization header carries the token the fake issued to the session
    bool hasSessionToken(const QByteArray &authorization) const
    {
        const qsizetype tokenAt = authorization.indexOf("Token=\"");
        if (tokenAt < 0)
            return false;
        const QByteArray token = authorization.mid(tokenAt + 7).split('"').value(0);
        if (token.isEmpty())
            return false;
        if (!users.isEmpty())
            return userByToken(token) != nullptr;
        if (rotateSecondLoginToken && successfulLogins >= 2)
            return token == secondLoginToken;
        return token
               == QJsonDocument::fromJson(fixture(QStringLiteral("authenticate_success")))
                      .object()
                      .value(QStringLiteral("AccessToken"))
                      .toString()
                      .toUtf8();
    }

    // Answers a write, now or on releasePlaylistWrites()
    void answerPlaylistWrite(QTcpSocket *socket, int status, const QByteArray &body = {})
    {
        if (dropPlaylistWrites)
            socket->disconnectFromHost();
        else if (holdPlaylistWrites)
            m_heldPlaylistWrites.append({socket, status, body});
        else
            respond(socket, status, body);
    }

    // GET/POST /Items/{id}/RemoteSearch/Subtitles/{language|id}; false when the request is another route
    bool handleRemoteSubtitleRoute(QTcpSocket *socket, const QByteArray &method,
                                   const QByteArray &route, const QByteArray &authorization)
    {
        const QList<QByteArray> parts = route.split('?').first().split('/');
        const qsizetype at = parts.indexOf("RemoteSearch");
        if (at < 2 || at + 2 >= parts.size() || parts.at(at + 1) != "Subtitles")
            return false;
        if (!hasSessionToken(authorization)) {
            respond(socket, 401, {});
            return true;
        }
        if (method == "GET") {
            remoteSubtitleSearches.append(parts.last());
            if (remoteSubtitleSearchStatus != 0) {
                respond(socket, remoteSubtitleSearchStatus, {});
                return true;
            }
            respondBytes(socket, 200, remoteSubtitleSearchBody,
                         "Content-Type: application/json\r\n");
            return true;
        }
        if (method == "POST") {
            remoteSubtitleDownloads.append(parts.last());
            respond(socket, remoteSubtitleDownloadStatus, {});
            return true;
        }
        return false;
    }

    // GET /Items/{id}/Download; false when the request is another route
    bool handleDownloadRoute(QTcpSocket *socket, const QByteArray &method, const QByteArray &route,
                             const QByteArray &range, const QByteArray &ifRange,
                             const QByteArray &authorization)
    {
        if (method != "GET")
            return false;
        const QList<QByteArray> parts = route.split('?').first().split('/');
        if (parts.size() < 4 || parts.at(parts.size() - 3) != "Items" || parts.last() != "Download")
            return false;
        const QByteArray id = parts.at(parts.size() - 2);
        if (id.size() != 32 || !std::all_of(id.cbegin(), id.cend(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            }))
            return false;
        ++downloadRequests;
        lastDownloadRoute = route;
        lastDownloadAuth = authorization;
        lastDownloadRange = range;
        lastDownloadIfRange = ifRange;
        if (!hasSessionToken(authorization)) {
            respond(socket, 401, {});
            return true;
        }
        if (downloadStatus != 0) {
            respond(socket, downloadStatus, {});
            return true;
        }
        if (redirectDownload) {
            respond(socket, 302, {}, "Location: https://unexpected.invalid/Items/x/Download\r\n");
            return true;
        }
        QByteArray body = downloadBodyFor(id);
        QByteArray extra;
        if (!downloadEtag.isEmpty())
            extra += "ETag: " + downloadEtag + "\r\n";
        if (!downloadLastModified.isEmpty())
            extra += "Last-Modified: " + downloadLastModified + "\r\n";
        extra += "Accept-Ranges: bytes\r\n";
        const QByteArray validator = downloadEtag.isEmpty() ? downloadLastModified : downloadEtag;
        const bool ifRangeMismatch =
            !range.isEmpty() && !ifRange.isEmpty() && !validator.isEmpty() && ifRange != validator;
        if (!downloadForce206 && (range.isEmpty() || downloadIgnoreRange || ifRangeMismatch)) {
            answerDownload(socket, 200, body, extra);
            return true;
        }
        qint64 start = 0;
        if (!range.isEmpty()) {
            const QByteArray spec = range.mid(range.indexOf("bytes=") + 6);
            if (!range.startsWith("bytes=") || !spec.endsWith('-')) {
                answerDownload(socket, 416, {}, extra);
                return true;
            }
            bool ok = false;
            start = spec.chopped(1).toLongLong(&ok);
            if (!ok || start < 0) {
                answerDownload(socket, 416, {}, extra);
                return true;
            }
        }
        const qint64 total = body.size();
        if (start >= total) {
            extra += "Content-Range: bytes */" + QByteArray::number(total) + "\r\n";
            answerDownload(socket, 416, {}, extra);
            return true;
        }
        start = qBound<qint64>(0, start + downloadStartShift, total - 1);
        const qint64 claimed =
            downloadClaimedTotals.isEmpty() ? total : downloadClaimedTotals.takeFirst();
        extra += "Content-Range: bytes " + QByteArray::number(start) + "-"
                 + QByteArray::number(total - 1) + "/" + QByteArray::number(claimed) + "\r\n";
        answerDownload(socket, 206, body.mid(start), extra);
        return true;
    }

    void answerDownload(QTcpSocket *socket, int status, const QByteArray &body,
                        const QByteArray &extra)
    {
        if (holdDownload) {
            heldDownloads.append({socket, status, body, extra});
            return;
        }
        if (downloadOmitLength) {
            socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\n"
                          + "Content-Type: application/octet-stream\r\n" + extra
                          + "Connection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
            return;
        }
        respondBytes(socket, status, body, extra);
    }

    // The /Playlists routes and DELETE /Items/{id}; false when the request is none of them
    bool handlePlaylistRoute(QTcpSocket *socket, const QByteArray &method, const QByteArray &route,
                             const QByteArray &body, const QByteArray &authorization,
                             const QByteArray &contentType)
    {
        const QList<QByteArray> parts = route.split('?').first().split('/');
        const qsizetype at = parts.indexOf("Playlists");
        const bool itemRoute = at < 0 && parts.size() >= 2 && parts.at(parts.size() - 2) == "Items"
                               && !parts.contains("Users");
        const bool deleteItem = itemRoute && method == "DELETE";
        // The listing routes under /Items/ (Latest, Filters, ...) are not ids: a GUID is 32 hex digits, dashed or not
        const QByteArray lastPart = parts.last();
        const bool getItem = itemRoute && method == "GET" && lastPart.size() >= 32
                             && lastPart.size() <= 36
                             && std::all_of(lastPart.cbegin(), lastPart.cend(), [](char c) {
                                    return (c >= '0' && c <= '9') || c == '-'
                                           || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                                });
        if (at < 0 && !deleteItem && !getItem)
            return false;
        if (!hasSessionToken(authorization)) {
            respond(socket, 401, {});
            return true;
        }
        const QList<QByteArray> rest = at < 0 ? QList<QByteArray>() : parts.mid(at + 1);
        const bool read = method == "GET";
        playlistRequests.append({method, route, body, authorization, contentType});
        if (getItem) {
            const QString id = QString::fromUtf8(lastPart);
            const FakePlaylist *playlist = playlistById(id);
            const QString type = playlist ? QStringLiteral("Playlist") : typedItems.value(id);
            if (playlistItemStatus != 0)
                respond(socket, playlistItemStatus, {});
            else if (!playlistItemRawBody.isEmpty())
                respond(socket, 200, playlistItemRawBody);
            else if (type.isEmpty())
                respond(socket, 404, {});
            else {
                QJsonObject item{{QStringLiteral("Id"), id},
                                 {QStringLiteral("Name"), QStringLiteral("Item")},
                                 {QStringLiteral("Type"), type}};
                if (playlist)
                    item.insert(QStringLiteral("CanDelete"), playlistsDeletable);
                respond(socket, 200, QJsonDocument(item).toJson(QJsonDocument::Compact));
            }
            return true;
        }
        if (!read && playlistWriteStatus != 0) {
            answerPlaylistWrite(socket, playlistWriteStatus);
            return true;
        }
        const QUrlQuery query = capturedListingQuery(route);
        const QJsonObject json = QJsonDocument::fromJson(body).object();
        if (deleteItem) {
            const QString id = QString::fromUtf8(parts.last());
            for (qsizetype i = 0; i < playlists.size(); ++i) {
                if (playlists.at(i).id.compare(id, Qt::CaseInsensitive) == 0) {
                    playlists.removeAt(i);
                    answerPlaylistWrite(socket, 204);
                    return true;
                }
            }
            answerPlaylistWrite(socket, 404);
            return true;
        }
        if (rest.isEmpty() && method == "POST") {
            const QString name = json.value(QStringLiteral("Name")).toString();
            if (name.isEmpty()) {
                answerPlaylistWrite(socket, 400);
                return true;
            }
            QStringList ids;
            for (const QJsonValue &id : json.value(QStringLiteral("Ids")).toArray())
                ids.append(id.toString());
            const QString id = seedPlaylist(name, ids);
            answerPlaylistWrite(socket, 200,
                                playlistCreateRawBody.isEmpty()
                                    ? QJsonDocument(QJsonObject{{QStringLiteral("Id"), id}})
                                          .toJson(QJsonDocument::Compact)
                                    : playlistCreateRawBody);
            return true;
        }
        if (rest.isEmpty() || (rest.size() != 1 && rest.size() != 2 && rest.size() != 5))
            return false;
        FakePlaylist *playlist = playlistById(QString::fromUtf8(rest.at(0)));
        if (!playlist) {
            if (read)
                respond(socket, 404, {});
            else
                answerPlaylistWrite(socket, 404);
            return true;
        }
        if (rest.size() == 1 && method == "POST") {
            if (json.value(QStringLiteral("Name")).isString())
                playlist->name = json.value(QStringLiteral("Name")).toString();
            answerPlaylistWrite(socket, 204);
            return true;
        }
        if (rest.size() == 2 && rest.at(1) == "Items") {
            if (read) {
                if (playlistEntriesStatus != 0) {
                    respond(socket, playlistEntriesStatus, {});
                    return true;
                }
                const int start = query.queryItemValue(QStringLiteral("startIndex")).toInt();
                const int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
                QJsonArray items;
                for (qsizetype row = start; row < start + limit && row < playlist->entries.size();
                     ++row)
                    items.append(playlistEntryJson(playlist->entries.at(row)));
                const QByteArray page =
                    QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                              {QStringLiteral("StartIndex"), start},
                                              {QStringLiteral("TotalRecordCount"),
                                               int(playlist->entries.size())}})
                        .toJson(QJsonDocument::Compact);
                if (holdPlaylistEntries)
                    m_heldPlaylistEntries.append({socket, 200, page});
                else
                    respond(socket, 200, page);
                return true;
            }
            if (method == "POST") {
                appendPlaylistItems(playlist,
                                    query.queryItemValue(QStringLiteral("ids"), QUrl::FullyDecoded)
                                        .split(QLatin1Char(','), Qt::SkipEmptyParts));
                answerPlaylistWrite(socket, 204);
                return true;
            }
            if (method == "DELETE") {
                const QStringList entryIds =
                    query.queryItemValue(QStringLiteral("entryIds"), QUrl::FullyDecoded)
                        .split(QLatin1Char(','), Qt::SkipEmptyParts);
                playlist->entries.removeIf([&entryIds](const FakePlaylistEntry &entry) {
                    return entryIds.contains(entry.entryId);
                });
                answerPlaylistWrite(socket, 204);
                return true;
            }
        }
        if (rest.size() == 5 && rest.at(1) == "Items" && rest.at(3) == "Move" && method == "POST") {
            const QString entryId = QString::fromUtf8(rest.at(2));
            const int newIndex = rest.at(4).toInt();
            qsizetype from = -1;
            for (qsizetype i = 0; i < playlist->entries.size() && from < 0; ++i) {
                if (playlist->entries.at(i).entryId == entryId)
                    from = i;
            }
            if (from < 0) {
                answerPlaylistWrite(socket, 204);
            } else if (newIndex >= playlist->entries.size()) {
                answerPlaylistWrite(socket, 500);
            } else {
                playlist->entries.move(from, newIndex);
                answerPlaylistWrite(socket, 204);
            }
            return true;
        }
        return false;
    }

    QByteArray generatedGenresBody(const QByteArray &route) const
    {
        const QUrlQuery query = capturedListingQuery(route);
        const int start = query.queryItemValue(QStringLiteral("startIndex")).toInt();
        const int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
        QJsonArray items;
        for (int row = start; row < start + limit && row < generatedGenreTotal; ++row) {
            items.append(QJsonObject{
                {QStringLiteral("Id"), QStringLiteral("%1").arg(row + 1, 32, 16, QLatin1Char('d'))},
                {QStringLiteral("Name"), QStringLiteral("Genre %1").arg(row + 1)},
                {QStringLiteral("Type"), QStringLiteral("Genre")}});
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                         {QStringLiteral("StartIndex"), start},
                                         {QStringLiteral("TotalRecordCount"), generatedGenreTotal}})
            .toJson(QJsonDocument::Compact);
    }

    QByteArray generatedEpisodesBody(const QByteArray &route) const
    {
        const QUrlQuery query = capturedListingQuery(route);
        const int start = query.queryItemValue(QStringLiteral("startIndex")).toInt();
        const int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
        QJsonArray items;
        for (int row = start; row < start + limit && row < generatedEpisodeTotal; ++row) {
            items.append(QJsonObject{
                {QStringLiteral("Id"), QStringLiteral("%1").arg(row + 1, 32, 16, QLatin1Char('e'))},
                {QStringLiteral("Name"), QStringLiteral("Episode %1").arg(row + 1)},
                {QStringLiteral("Type"), QStringLiteral("Episode")},
                {QStringLiteral("SeriesId"), QStringLiteral("aa11bb22cc33dd44ee55ff6677889900")},
                {QStringLiteral("ParentIndexNumber"), 1},
                {QStringLiteral("IndexNumber"), row + 1},
                {QStringLiteral("RunTimeTicks"), qint64(18'000'000'000)},
                {QStringLiteral("UserData"),
                 QJsonObject{{QStringLiteral("Played"), row < generatedEpisodesPlayed}}},
                {QStringLiteral("ImageTags"),
                 QJsonObject{{QStringLiteral("Primary"), QStringLiteral("ep%1").arg(row)}}}});
        }
        return QJsonDocument(
                   QJsonObject{{QStringLiteral("Items"), items},
                               {QStringLiteral("StartIndex"), start},
                               {QStringLiteral("TotalRecordCount"), generatedEpisodeTotal}})
            .toJson(QJsonDocument::Compact);
    }

    QByteArray itemsByIdBody(const QByteArray &route) const
    {
        const QStringList ids = capturedListingQuery(route)
                                    .queryItemValue(QStringLiteral("ids"))
                                    .split(QLatin1Char(','));
        QJsonArray items;
        for (const QString &id : ids) {
            const QByteArray raw = itemsById.value(id.toLower());
            if (!raw.isEmpty())
                items.append(QJsonDocument::fromJson(raw).object());
        }
        if (reverseIdsAnswer) {
            QJsonArray reversed;
            for (qsizetype i = items.size() - 1; i >= 0; --i)
                reversed.append(items.at(i));
            items = reversed;
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                         {QStringLiteral("StartIndex"), 0},
                                         {QStringLiteral("TotalRecordCount"), items.size()}})
            .toJson(QJsonDocument::Compact);
    }

    static QByteArray generatedItemsBody(const QByteArray &route, int total, int available)
    {
        const QUrlQuery query(QUrl(QString::fromUtf8(route)));
        const int start = query.queryItemValue(QStringLiteral("startIndex")).toInt();
        const int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
        QJsonArray items;
        for (int row = start; row < start + limit && row < available; ++row) {
            // A 32-digit hex id built from the row index
            items.append(QJsonObject{
                {QStringLiteral("Id"), QStringLiteral("%1").arg(row + 1, 32, 16, QLatin1Char('0'))},
                {QStringLiteral("Name"), QStringLiteral("Item %1").arg(row)},
                {QStringLiteral("Type"), QStringLiteral("Movie")},
                {QStringLiteral("ProductionYear"), 2000 + row % 25},
                {QStringLiteral("ImageTags"),
                 QJsonObject{{QStringLiteral("Primary"), QStringLiteral("tag%1").arg(row)}}}});
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                         {QStringLiteral("StartIndex"), start},
                                         {QStringLiteral("TotalRecordCount"), total}})
            .toJson(QJsonDocument::Compact);
    }

    static void respondBytes(QTcpSocket *socket, int status, const QByteArray &body,
                             const QByteArray &extraHeaders = {})
    {
        QByteArray result = "HTTP/1.1 " + QByteArray::number(status)
                            + (status == 200 ? " OK\r\n" : " Response\r\n");
        result += "Content-Type: application/octet-stream\r\nContent-Length: "
                  + QByteArray::number(body.size()) + "\r\n";
        result += extraHeaders + "Connection: close\r\n\r\n" + body;
        socket->write(result);
        socket->disconnectFromHost();
    }

    static void respond(QTcpSocket *socket, int status, const QByteArray &body,
                        const QByteArray &extraHeaders = {})
    {
        QByteArray result = "HTTP/1.1 " + QByteArray::number(status)
                            + (status == 200 ? " OK\r\n" : " Response\r\n");
        result += "Content-Type: application/json\r\nContent-Length: "
                  + QByteArray::number(body.size()) + "\r\n";
        result += extraHeaders + "Connection: close\r\n\r\n" + body;
        socket->write(result);
        socket->disconnectFromHost();
    }

    void readRequest(QTcpSocket *socket)
    {
        // Once a WebSocket handshake is accepted, the bytes belong to the frame parser
        if (FakeSocketConnection *connection = m_wsTaken.value(socket)) {
            connection->drain();
            return;
        }
        if (m_wsTaken.contains(socket))
            return;
        QByteArray &input = m_buffers[socket];
        input.append(socket->readAll());
        if (input.startsWith('\x16')) {
            ++tlsAttempts;
            m_buffers.remove(socket);
            socket->disconnectFromHost();
            return;
        }
        const qsizetype end = input.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        const QByteArray headers = input.left(end + 2);
        const QList<QByteArray> lines = headers.split('\n');
        int contentLength = 0;
        QByteArray authorization;
        QByteArray cookie;
        QByteArray contentType;
        QByteArray websocketKey;
        QByteArray range;
        QByteArray ifRange;
        bool upgradeRequest = false;
        for (const QByteArray &raw : lines) {
            const QByteArray line = raw.trimmed();
            if (line.toLower().startsWith("content-length:"))
                contentLength = line.mid(15).trimmed().toInt();
            if (line.toLower().startsWith("authorization:"))
                authorization = line.mid(14).trimmed();
            if (line.toLower().startsWith("cookie:"))
                cookie = line.mid(7).trimmed();
            if (line.toLower().startsWith("content-type:"))
                contentType = line.mid(13).trimmed();
            if (line.toLower().startsWith("range:"))
                range = line.mid(6).trimmed();
            if (line.toLower().startsWith("if-range:"))
                ifRange = line.mid(9).trimmed();
            if (line.toLower().startsWith("sec-websocket-key:"))
                websocketKey = line.mid(18).trimmed();
            if (line.toLower().startsWith("upgrade:")
                && line.mid(8).trimmed().toLower() == "websocket")
                upgradeRequest = true;
        }
        if (contentLength < 0 || contentLength > 65536) {
            respond(socket, 413, {});
            return;
        }
        const qsizetype bodyStart = end + 4;
        if (input.size() - bodyStart < contentLength)
            return;
        const QByteArray route = lines.constFirst().trimmed().split(' ').value(1);
        const QByteArray method = lines.constFirst().trimmed().split(' ').value(0);
        const QByteArray body = input.mid(bodyStart, contentLength);
        m_buffers.remove(socket);
        ++requestCount;
        requestLog.append({method, route});
        if (upgradeRequest) {
            // Guard first so bytes arriving while the handshake is written can never leak back into the HTTP parser
            m_wsTaken.insert(socket, nullptr);
            ++upgradeAttempts;
            lastSocketRoute = QString::fromUtf8(route);
            lastSocketAuth = authorization;
            const qsizetype query = route.indexOf('?');
            lastSocketQuery =
                query < 0 ? QString() : QString::fromUtf8(route.mid(qsizetype(query) + 1));
            if (holdSocketUpgrade)
                return;
            if (socketUpgradeStatus != 0) {
                respond(socket, socketUpgradeStatus, {},
                        "Location: http://127.0.0.1:9/socket-relocated\r\n");
                return;
            }
            auto *connection = new FakeSocketConnection(socket, websocketKey);
            m_wsTaken.insert(socket, connection);
            m_wsConnections.append(connection);
            return;
        }
        if (method == "GET" && route.endsWith("/System/Info/Public")) {
            ++publicInfoRequests;
            lastPublicInfoAuth = authorization;
            if (redirectPublicOffHost) {
                respond(socket, 302, {},
                        "Location: https://unexpected.invalid/System/Info/Public\r\n");
            } else if (relocatePublicOnSameHost && route.startsWith("/jellyfin/")) {
                respond(socket, 302, {}, "Location: /jellyfin-proxy/System/Info/Public\r\n");
            } else if (invalidIdentity) {
                respond(socket, 200, "{\"Id\":null}");
            } else {
                respond(socket, 200, fixture(QStringLiteral("system_info_public")));
            }
            return;
        }
        if (method == "POST" && route.endsWith("/Users/AuthenticateByName")) {
            lastLoginAuth = authorization;
            if (!QJsonDocument::fromJson(body).isObject()) {
                respond(socket, 400, {});
            } else if (denyLogin) {
                respond(socket, 401, {});
            } else {
                const QString username = QJsonDocument::fromJson(body)
                                             .object()
                                             .value(QStringLiteral("Username"))
                                             .toString();
                if (!users.isEmpty() && !userByName(username)) {
                    respond(socket, 401, {});
                    return;
                }
                ++successfulLogins;
                respond(socket, 200, authenticateBody(username));
            }
            return;
        }
        if (method == "POST" && route.endsWith("/QuickConnect/Initiate")) {
            ++quickInitiationRequests;
            if (disableQuickConnect) {
                respond(socket, 401, {});
            } else if (invalidQuickConnectCode) {
                respond(socket, 200, "{\"Code\":\"AB12CD\"}");
            } else {
                respond(socket, 200, fixture(QStringLiteral("quick_connect_initiate")));
            }
            return;
        }
        if (method == "GET" && route.contains("/QuickConnect/Connect?")) {
            ++quickStatusRequests;
            lastQuickPollAuth = authorization;
            const QUrl fullUrl =
                QUrl::fromEncoded(QByteArray(QByteArray("http://127.0.0.1") + route));
            lastQuickPollSecret = QUrlQuery(fullUrl).queryItemValue(QStringLiteral("secret"));
            if (holdQuickConnectPoll)
                return;
            if (rejectQuickConnectStatus
                || lastQuickPollSecret != QStringLiteral("fixture-quick-secret")) {
                respond(socket, 404, {});
            } else if (mismatchQuickConnectSecret) {
                respond(socket, 200, "{\"Authenticated\":true,\"Secret\":\"wrong-secret\"}");
            } else if (approveQuickConnect) {
                respond(socket, 200, fixture(QStringLiteral("quick_connect_approved")));
            } else {
                respond(socket, 200, fixture(QStringLiteral("quick_connect_pending")));
            }
            return;
        }
        if (method == "POST" && route.endsWith("/Users/AuthenticateWithQuickConnect")) {
            ++quickExchangeRequests;
            lastQuickExchangeBody = body;
            lastQuickExchangeAuth = authorization;
            if (denyQuickConnectExchange
                || QJsonDocument::fromJson(body).object().value(QStringLiteral("Secret")).toString()
                       != QStringLiteral("fixture-quick-secret")) {
                respond(socket, 401, {});
            } else {
                ++successfulLogins;
                respond(socket, 200, authenticateBody());
            }
            return;
        }
        if (method == "GET" && route.endsWith("/Users/Me")) {
            ++usersMeRequests;
            lastUsersMeAuth = authorization;
            if (onUsersMe)
                onUsersMe();
            const qsizetype tokenAt = authorization.indexOf("Token=\"");
            const QByteArray token =
                tokenAt < 0 ? QByteArray() : authorization.mid(tokenAt + 7).split('"').value(0);
            if (denyUsersMe || deniedUsersMeTokens.contains(token)) {
                respond(socket, 401, {});
            } else if (usersMeStatus != 0) {
                respond(socket, usersMeStatus, {});
            } else if (!usersMeRawBody.isEmpty()) {
                respond(socket, 200, usersMeRawBody);
            } else if (!users.isEmpty()) {
                const FakeUser *user = userByToken(token);
                if (!user) {
                    respond(socket, 401, {});
                    return;
                }
                QJsonObject me{{QStringLiteral("Id"), user->id},
                               {QStringLiteral("Name"), user->name},
                               {QStringLiteral("Policy"), policyOf(user)},
                               {QStringLiteral("Configuration"), QJsonObject()}};
                if (!user->tag.isEmpty())
                    me.insert(QStringLiteral("PrimaryImageTag"), user->tag);
                respond(socket, 200, QJsonDocument(me).toJson(QJsonDocument::Compact));
            } else {
                const QJsonObject me{
                    {QStringLiteral("Id"), QStringLiteral("d5ed5d4e2e5447b58e9f2b6f7bd5c96a")},
                    {QStringLiteral("Name"), QStringLiteral("bepi")},
                    {QStringLiteral("Policy"), policyOf(nullptr)},
                    {QStringLiteral("Configuration"), QJsonObject()}};
                respond(socket, 200, QJsonDocument(me).toJson(QJsonDocument::Compact));
            }
            return;
        }
        if (method == "GET" && route.endsWith("/Users/Public")) {
            ++publicUsersRequests;
            lastPublicUsersAuth = authorization;
            if (publicUsersStatus != 200) {
                respond(socket, publicUsersStatus, {});
            } else if (!publicUsersRawBody.isEmpty()) {
                respond(socket, 200, publicUsersRawBody);
            } else {
                QJsonArray list;
                for (const FakeUser &user : users) {
                    if (user.hidden)
                        continue;
                    QJsonObject entry{{QStringLiteral("Id"), user.id},
                                      {QStringLiteral("Name"), user.name},
                                      {QStringLiteral("HasPassword"), true}};
                    if (!user.tag.isEmpty())
                        entry.insert(QStringLiteral("PrimaryImageTag"), user.tag);
                    list.append(entry);
                }
                respond(socket, 200, QJsonDocument(list).toJson(QJsonDocument::Compact));
            }
            return;
        }
        if (method == "GET" && (route.contains("/UserImage?") || route.endsWith("/UserImage"))) {
            ++userImageRequests;
            lastUserImageRoute = route;
            lastUserImageAuth = authorization;
            const QString id = capturedListingQuery(route).queryItemValue(QStringLiteral("userId"));
            bool hasAvatar = true;
            if (!users.isEmpty()) {
                hasAvatar = false;
                for (const FakeUser &user : users)
                    hasAvatar = hasAvatar || (user.id == id && !user.tag.isEmpty());
            }
            if (hasAvatar)
                respond(socket, 200, kTinyPng, "Cache-Control: public, max-age=31536000\r\n");
            else
                respond(socket, 404, {});
            return;
        }
        if ((method == "POST" && route.endsWith("/Users/Configuration"))
            || (method == "GET" && route.endsWith("/Localization/Cultures"))
            || route.contains("/DisplayPreferences/")) {
            accountCalls.append({method, route, authorization, contentType, body});
            if (holdAccount) {
                m_held.append(socket);
                return;
            }
            if (method == "POST") {
                if (onUserConfigurationPost)
                    onUserConfigurationPost(body);
                respond(socket, userConfigurationStatus, {});
            } else if (route.contains("/Localization/Cultures")) {
                if (culturesStatus != 0)
                    respond(socket, culturesStatus, {});
                else
                    respond(socket, 200,
                            culturesRawBody.isEmpty()
                                ? QByteArrayLiteral(
                                      "[{\"Name\":\"English\",\"DisplayName\":\"English\","
                                      "\"TwoLetterISOLanguageName\":\"en\","
                                      "\"ThreeLetterISOLanguageName\":\"eng\"},"
                                      "{\"Name\":\"Italian\",\"DisplayName\":\"Italian\","
                                      "\"TwoLetterISOLanguageName\":\"it\","
                                      "\"ThreeLetterISOLanguageName\":\"ita\"}]")
                                : culturesRawBody);
            } else if (displayPreferencesStatus != 0) {
                respond(socket, displayPreferencesStatus, {});
            } else {
                respond(socket, 200,
                        displayPreferencesRawBody.isEmpty()
                            ? QByteArrayLiteral(
                                  "{\"Id\":\"usersettings\",\"Client\":\"emby\","
                                  "\"CustomPrefs\":{\"homesection0\":\"smalllibrarytiles\","
                                  "\"homesection1\":\"resume\"}}")
                            : displayPreferencesRawBody);
            }
            return;
        }
        if (handleRemoteSubtitleRoute(socket, method, route, authorization))
            return;
        if (handleDownloadRoute(socket, method, route, range, ifRange, authorization))
            return;
        // Before the listing routes: `/Playlists/{id}/Items?` would read as the Items family
        if (handlePlaylistRoute(socket, method, route, body, authorization, contentType))
            return;
        // The listing routes classify by fragment; `listingFamily` keeps the order so the more specific fragments win (/Items/Latest before /Items, /Sessions before the POST-only playback routes never match a GET)
        const QString listingFamily = classifyListingRoute(route);
        if (method == "GET" && !listingFamily.isEmpty()) {
            ++listingRequests;
            lastListingRoute.insert(listingFamily, route);
            listingRoutes[listingFamily].append(route);
            lastListingAuth.insert(listingFamily, authorization);
            if (listingFamily == QLatin1String("Items"))
                itemsRoutes.append(route);
            if (listingFamily == QLatin1String("Items") && snapshotItemsReads) {
                m_snapshots.append({socket, itemsByIdBody(route)});
                return;
            }
            if (holdListings || heldFamilies.contains(listingFamily) || holdFirstListings-- > 0) {
                m_held.append(socket);
                return;
            }
            const std::pair<int, QByteArray> custom =
                listingFamily == QLatin1String("Items") && itemsResponder
                    ? itemsResponder(route)
                    : std::pair<int, QByteArray>{};
            if (listingStatus.contains(listingFamily)) {
                respond(socket, listingStatus.value(listingFamily), {});
            } else if (custom.first != 0) {
                respond(socket, custom.first, custom.second);
            } else if (listingFamily == QLatin1String("Items")
                       && route.contains("includeItemTypes=Playlist") && !denyListings
                       && !redirectListings) {
                respond(socket, 200, playlistsListingBody(route));
            } else if (listingFamily == QLatin1String("Episodes") && generatedEpisodeTotal >= 0
                       && !denyListings && !redirectListings) {
                respond(socket, 200, generatedEpisodesBody(route));
            } else if (listingFamily == QLatin1String("Items") && route.contains("genreIds=")
                       && itemsByGenre.contains(capturedListingQuery(route).queryItemValue(
                           QStringLiteral("genreIds"), QUrl::FullyDecoded))) {
                respond(socket, 200,
                        fixture(itemsByGenre.value(capturedListingQuery(route).queryItemValue(
                            QStringLiteral("genreIds"), QUrl::FullyDecoded))));
            } else if (listingFamily == QLatin1String("Genres") && generatedGenreTotal >= 0
                       && !denyListings && !redirectListings) {
                respond(socket, 200, generatedGenresBody(route));
            } else if (listingFamily == QLatin1String("Items") && route.contains("searchTerm=")
                       && itemsByType.contains(capturedListingQuery(route).queryItemValue(
                           QStringLiteral("includeItemTypes")))) {
                respond(socket, 200,
                        fixture(itemsByType.value(capturedListingQuery(route).queryItemValue(
                            QStringLiteral("includeItemTypes")))));
            } else if (listingFamily == QLatin1String("Items") && !itemsById.isEmpty()
                       && route.contains("ids=") && !denyListings && !redirectListings) {
                respond(socket, 200, itemsByIdBody(route));
            } else if (listingFamily == QLatin1String("Items") && generatedItemTotal >= 0
                       && !denyListings && !redirectListings) {
                respond(socket, 200,
                        generatedItemsBody(route, generatedItemTotal,
                                           generatedAvailable >= 0 ? generatedAvailable
                                                                   : generatedItemTotal));
            } else if (listingFamily == QLatin1String("Resume") && generatedResumeTotal >= 0
                       && !denyListings && !redirectListings) {
                respond(socket, 200,
                        generatedItemsBody(route, generatedResumeTotal, generatedResumeTotal));
            } else if (redirectListings) {
                respond(socket, 302, {},
                        QByteArrayLiteral("Location: https://unexpected.invalid/") + route.mid(1)
                            + QByteArrayLiteral("\r\n"));
            } else if (denyListings) {
                respond(socket, 401, {});
            } else if (listingRawBody.contains(listingFamily)) {
                respond(socket, 200, listingRawBody.value(listingFamily));
            } else {
                respond(socket, 200, fixture(listingFixtures.value(listingFamily)));
            }
            return;
        }
        if (method == "POST" && route.endsWith("/Sessions/Capabilities/Full")) {
            ++capabilitiesRequests;
            lastCapabilitiesBody = body;
            lastCapabilitiesAuth = authorization;
            respond(socket, 204, {});
            return;
        }
        if ((method == "POST" || method == "DELETE") && route.contains("/UserFavoriteItems/")) {
            favoriteWrites.append({method, route, authorization});
            if (holdFavoriteWrites) {
                m_held.append(socket);
                return;
            }
            if (redirectFavoriteWrites) {
                respond(socket, 302, {}, "Location: https://unexpected.invalid/favorite\r\n");
                return;
            }
            if (favoriteWriteStatus != 200 && favoriteWriteStatus != 204) {
                respond(socket, favoriteWriteStatus, {});
                return;
            }
            const QByteArray encodedId = route.split('/').last();
            const QString id = QString::fromUtf8(encodedId).toLower();
            const bool isFavorite = method == "POST";
            if (itemsById.contains(id)) {
                QJsonObject item = QJsonDocument::fromJson(itemsById.value(id)).object();
                QJsonObject userData = item.value(QStringLiteral("UserData")).toObject();
                userData.insert(QStringLiteral("IsFavorite"), isFavorite);
                item.insert(QStringLiteral("UserData"), userData);
                itemsById.insert(id, QJsonDocument(item).toJson(QJsonDocument::Compact));
            }
            if (favoriteWriteStatus == 204) {
                respond(socket, 204, {});
            } else if (!favoriteWriteRawBody.isEmpty()) {
                respond(socket, 200, favoriteWriteRawBody);
            } else {
                respond(socket, 200,
                        QJsonDocument(QJsonObject{{QStringLiteral("ItemId"), id},
                                                  {QStringLiteral("IsFavorite"), isFavorite}})
                            .toJson(QJsonDocument::Compact));
            }
            return;
        }
        if ((method == "POST" || method == "DELETE") && route.contains("/UserPlayedItems/")) {
            playedWrites.append({method, route, authorization});
            if (holdPlayedWrites) {
                m_held.append(socket);
                return;
            }
            if (redirectPlayedWrites) {
                respond(socket, 302, {}, "Location: https://unexpected.invalid/played\r\n");
                return;
            }
            if (playedWriteStatus != 200 && playedWriteStatus != 204) {
                respond(socket, playedWriteStatus, {});
                return;
            }
            const QString id =
                QString::fromUtf8(route.split('?').first().split('/').last()).toLower();
            const bool isPlayed = method == "POST";
            if (itemsById.contains(id)) {
                QJsonObject item = QJsonDocument::fromJson(itemsById.value(id)).object();
                QJsonObject userData = item.value(QStringLiteral("UserData")).toObject();
                userData.insert(QStringLiteral("Played"), isPlayed);
                userData.insert(QStringLiteral("PlaybackPositionTicks"), 0);
                userData.insert(QStringLiteral("PlayedPercentage"), isPlayed ? 100 : 0);
                item.insert(QStringLiteral("UserData"), userData);
                itemsById.insert(id, QJsonDocument(item).toJson(QJsonDocument::Compact));
            }
            if (playedWriteStatus == 204) {
                respond(socket, 204, {});
            } else if (!playedWriteRawBody.isEmpty()) {
                respond(socket, 200, playedWriteRawBody);
            } else {
                respond(socket, 200,
                        QJsonDocument(QJsonObject{{QStringLiteral("ItemId"), id},
                                                  {QStringLiteral("Played"), isPlayed}})
                            .toJson(QJsonDocument::Compact));
            }
            return;
        }
        if (method == "POST"
            && (route.endsWith("/Sessions/Playing") || route.endsWith("/Sessions/Playing/Progress")
                || route.endsWith("/Sessions/Playing/Stopped"))) {
            reportLog.append({route, body, authorization, contentType});
            const bool stopped = route.endsWith("/Stopped");
            if (stopped)
                ++stoppedRequests;
            if (holdReports) {
                m_held.append(socket);
                return;
            }
            if (redirectReports) {
                respond(socket, 302, {},
                        "Location: https://unexpected.invalid/Sessions/Playing\r\n");
            } else if (stopped && stoppedRequests == 1 && firstStoppedStatus != 0) {
                respond(socket, firstStoppedStatus, {});
            } else {
                respond(socket, reportStatus, {});
            }
            return;
        }
        if (method == "POST" && route.contains("/Sessions/")
            && (route.endsWith("/Playing/Pause") || route.endsWith("/Playing/Unpause")
                || route.endsWith("/Playing/Stop"))) {
            remoteCommandRoutes.append(route);
            remoteCommandAuthorizations.append(authorization);
            if (holdRemoteCommands)
                m_held.append(socket);
            else if (redirectRemoteCommands)
                respond(socket, 302, {}, "Location: https://unexpected.invalid/remote\r\n");
            else
                respond(socket, remoteCommandStatus, {});
            return;
        }
        if (method == "GET" && route.endsWith("/SyncPlay/List")) {
            ++syncPlayListRequests;
            respond(socket, syncPlayListStatus,
                    syncPlayListBody.isEmpty() ? fixture(QStringLiteral("syncplay_group_list"))
                                               : syncPlayListBody);
            return;
        }
        if (method == "GET" && route.endsWith("/GetUtcTime")) {
            ++utcTimeRequests;
            if (utcTimeNow && utcTimeStatus == 200) {
                const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                respond(
                    socket, 200,
                    QJsonDocument(QJsonObject{{QStringLiteral("RequestReceptionTime"), now},
                                              {QStringLiteral("ResponseTransmissionTime"), now}})
                        .toJson(QJsonDocument::Compact));
                return;
            }
            respond(socket, utcTimeStatus,
                    utcTimeBody.isEmpty() ? fixture(QStringLiteral("get_utc_time")) : utcTimeBody);
            return;
        }
        if (method == "POST" && route.contains("/SyncPlay/")) {
            const QString name = QString::fromUtf8(route.mid(route.lastIndexOf('/') + 1));
            syncPlayRequests.append(
                {name, QJsonDocument::fromJson(body).object(), authorization, contentType});
            if (holdSyncPlay) {
                m_held.append(socket);
                return;
            }
            const bool groupReply = syncPlayStatus == 200 && name == QLatin1String("New");
            respond(socket, syncPlayStatus,
                    groupReply ? fixture(QStringLiteral("syncplay_group")) : QByteArray());
            return;
        }
        if (method == "GET" && route.contains("/MediaSegments/")) {
            ++mediaSegmentsRequests;
            mediaSegmentsAuth = authorization;
            respond(socket, mediaSegmentsStatus,
                    mediaSegmentsStatus == 200 ? mediaSegmentsBody : QByteArray());
            return;
        }
        if (method == "GET" && route.contains("/Videos/") && route.contains("/Subtitles/")) {
            ++subtitleDownloadRequests;
            subtitleDownloadAuth = authorization;
            if (redirectSubtitleDownload)
                respond(socket, 302, {}, "Location: https://unexpected.invalid/leak.srt\r\n");
            else
                respond(socket, subtitleDownloadStatus,
                        subtitleDownloadStatus == 200 ? subtitleDownloadBody : QByteArray());
            return;
        }
        if (method == "GET" && route.contains("/Videos/") && route.contains("/Trickplay/")) {
            ++trickplayTileRequests;
            trickplayTileAuth = authorization;
            respond(socket, trickplayTileStatus,
                    trickplayTileStatus == 200 ? trickplayTileBody : QByteArray());
            return;
        }
        if (method == "GET" && route.contains("/Users/") && route.contains("/Items/")
            && route.contains("fields=Trickplay")) {
            ++trickplayMetadataRequests;
            trickplayMetadataRoute = route;
            respond(socket, trickplayMetadataStatus,
                    trickplayMetadataStatus == 200 ? trickplayMetadataBody : QByteArray());
            return;
        }
        if (method == "POST" && route.endsWith("/PlaybackInfo")) {
            ++playbackInfoRequests;
            lastPlaybackInfoRoute = route;
            lastPlaybackInfoAuth = authorization;
            lastPlaybackInfoBody = body;
            lastPlaybackInfoContentType = contentType;
            if (holdPlaybackInfo)
                return;
            if (redirectPlaybackInfo) {
                respond(socket, 302, {},
                        "Location: https://unexpected.invalid/Items/x/PlaybackInfo\r\n");
            } else if (playbackInfoStatus != 0) {
                respond(socket, playbackInfoStatus, {});
            } else if (!playbackInfoRawBody.isEmpty()) {
                respond(socket, 200, playbackInfoRawBody);
            } else {
                respond(socket, 200, fixture(playbackInfoFixture));
            }
            return;
        }
        if (method == "GET" && route.contains("/UserViews?")) {
            ++viewRequests;
            lastViewsAuth = authorization;
            if (holdViews)
                return;
            if (redirectViewsOffHost) {
                respond(socket, 302, {}, "Location: https://unexpected.invalid/UserViews\r\n");
            } else if (denyViews) {
                respond(socket, 401, {});
            } else {
                respond(socket, 200,
                        viewsRawBody.isEmpty() ? fixture(QStringLiteral("user_views"))
                                               : viewsRawBody);
            }
            return;
        }
        if (method == "GET" && route.contains("/Images/")) {
            ++imageRequests;
            lastImageRoute = route;
            lastImageAuth = authorization;
            lastImageCookie = cookie;
            if (redirectImages) {
                respond(socket, 302, {}, "Location: https://unexpected.invalid/image.png\r\n");
            } else if (imageStatus != 200) {
                respond(socket, imageStatus, {});
            } else {
                // Cache-Control mirrors what the server sends for a tagged image
                respond(socket, 200, imageBody.isEmpty() ? kTinyPng : imageBody,
                        "Cache-Control: public, max-age=31536000\r\n");
            }
            return;
        }
        respond(socket, 404, {});
    }

    // The server comes last so it is destroyed first: its child sockets may still emit `disconnected` while it dies, and the slots touch the members above
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QTcpSocket *, FakeSocketConnection *> m_wsTaken;
    QList<QPointer<QTcpSocket>> m_held;
    QList<QPair<QPointer<QTcpSocket>, QByteArray>> m_snapshots;
    QList<HeldPlaylistWrite> m_heldPlaylistWrites;
    QList<HeldPlaylistWrite> m_heldPlaylistEntries;
    quint64 m_nextFakeId = 0;
    QTcpServer m_server;
    QList<FakeSocketConnection *> m_wsConnections;
};

inline bool prepare(Kinefin::JellyfinClient &client, const FakeJellyfin &server)
{
    if (!client.setDeviceIdentity(QStringLiteral("Kinefin test"), QStringLiteral("stable-test-id"),
                                  QStringLiteral("0.0.1")))
        return false;
    return client.connectToServer(server.url(QStringLiteral("/jellyfin")));
}

const QString kPlayItem = QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
// The access token of tests/fixtures/authenticate_success.json
const QByteArray kFixtureToken = "f2f4b7c1ee8a4d0bb2a5f9d1f9e0e2ab";
const QByteArray kTokenField = QByteArray("Token=\"") + kFixtureToken + QByteArray("\"");

// Connects and logs in; QTRY_* macros cannot live in a helper that returns a value
inline bool login(Kinefin::JellyfinClient &client, const FakeJellyfin &server)
{
    QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
    QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
    const bool prepared = prepare(client, server);
    if (!prepared)
        qWarning() << "login: prepare failed";
    if (!QTest::qWaitFor([&] { return ready.size() == 1; }, 5000)) {
        qWarning() << "login: serverReady never arrived; prepared =" << prepared
                   << "requests seen by the fake:" << server.upgradeAttempts << server.viewRequests
                   << server.capabilitiesRequests;
        return false;
    }
    if (!client.authenticate(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"))) {
        qWarning() << "login: authenticate refused by the client";
        return false;
    }
    if (!QTest::qWaitFor([&] { return loggedIn.size() == 1; }, 5000)) {
        qWarning() << "login: loginSucceeded never arrived; authenticated"
                   << client.isAuthenticated()
                   << "logins seen by the fake:" << server.successfulLogins;
        return false;
    }
    return true;
}

// A valid direct-play response plus a padding field: the only thing wrong with it is its size
inline QByteArray paddedPlaybackInfo(int paddingBytes)
{
    QJsonObject object =
        QJsonDocument::fromJson(fixture(QStringLiteral("playbackinfo_direct"))).object();
    object.insert(QStringLiteral("Padding"), QString(paddingBytes, QLatin1Char('a')));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

} // namespace
