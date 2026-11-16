// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <functional>
#include <optional>

#include "api/MediaSourceSelection.h"
#include "api/PlaybackReport.h"
#include "api/SyncPlayProtocol.h"

class QNetworkReply;

namespace Kinefin {

bool isValidHeaderField(const QString &field);

// A text value of a pipe-delimited filter (tag, official rating): non-empty, at most MaxFilterValueChars, one line without control characters, and no '|' (the delimiter, which the server could not tell apart)
bool isValidFilterText(const QString &text);

// The most genre ids one query may carry
constexpr int MaxGenreIds = 50;

// The most values of one multi-value filter (tags, official ratings, years, video types, series status, studio ids) one query may carry, and the longest text value
constexpr int MaxFilterValues = 50;
constexpr int MaxFilterValueChars = 128;

// The longest encoded query of an Items listing: below the 8 KiB request line the server and the usual proxies accept. fetchItems refuses a longer one without a request; the filter UI keeps its selections within it by asking itemsQueryChars()
constexpr int MaxItemsQueryChars = 7000;

// The longest playlist name (in Unicode code points), the most ids one create, add or remove call may carry, and the largest page fetchPlaylistEntries asks for (a larger `limit` is cut to it)
constexpr int MaxPlaylistNameChars = 128;
constexpr int MaxPlaylistWriteIds = 100;
constexpr int MaxPlaylistPageSize = 100;

// The playlist name `name` stands for: trimmed, 1 to MaxPlaylistNameChars code points, at least one visible (non-space) character, valid UTF-16 with no control, format (bidi and zero-width), line or paragraph separator, surrogate, unassigned or noncharacter code point; empty when it does not qualify
QString normalizedPlaylistName(const QString &name);

// Random sorting is requested as one page of this many items, never a later one: the order differs on every request
constexpr int RandomPageSize = 100;

// Bounds of the user fields taken from the server
constexpr int MaxUserNameChars = 128;
constexpr int MaxPublicUsers = 100;

struct ItemQuery {
    QString parentId;
    QString includeItemTypes; // comma-separated BaseItemKind tokens
    QString excludeItemTypes;
    // comma-separated MediaType tokens: Video, Audio, Photo, Book (a playlist is listed by its own)
    QString mediaTypes;
    QStringList itemIds;
    QString searchTerm;
    QString sortBy;    // comma-separated sorting keys (shape-checked letters/digits/underscores)
    QString sortOrder; // "Ascending" or "Descending"
    int startIndex = -1;
    int limit = -1;
    bool recursive = false;
    QString filters;    // comma-separated ItemFilter tokens (e.g. "IsResumable")
    QStringList fields; // ItemFields tokens
    // Also ask for the image types a card shows (one tag per type)
    bool cardImages = false;
    // With `cardImages`, one more image type whose tag is wanted: "Banner", "Disc" or "Logo" (the card's Primary, Backdrop and Thumb tags are always asked for); empty asks for nothing more
    QString cardImageType;
    // Genre ids (hex ids from /Genres or Items/Filters2, never names): an item matches any of them. Written pipe-delimited, at most MaxGenreIds
    QStringList genreIds;
    // The server-side filters of a library grid (see LibraryFilterState.h for the composer): sections combine with AND, the values of one list with OR. A flag or optional that is false/unset sends nothing; `isHd` false is the SD choice. Written only for a listing, never for an ids= resolution. `videoTypes` ("BluRay", "Dvd"), `seriesStatus` ("Continuing", "Ended", "Unreleased") and `years` (1 to 9999) are comma-separated on the wire; `tags` (text from Items/Filters2), `officialRatings` (text from Items/Filters; each isValidFilterText) and `studioIds` (hex ids) are pipe-delimited; every list holds at most MaxFilterValues. `parentIndexNumber` 0 selects the specials, -1 sends none
    bool hasSubtitles = false;
    bool hasTrailer = false;
    bool hasSpecialFeature = false;
    bool hasThemeSong = false;
    bool hasThemeVideo = false;
    std::optional<bool> isHd;
    bool is4K = false;
    bool is3D = false;
    QStringList videoTypes;
    QStringList seriesStatus;
    QStringList tags;
    QStringList officialRatings;
    QList<int> years;
    QStringList studioIds;
    int parentIndexNumber = -1;
    bool isMissing = false;
    bool isUnaired = false;
};

// One entry of `GET /Users/Public`, already validated: a Jellyfin id, a bounded single-line name and a safe avatar tag (empty when the user has no avatar)
struct PublicUser {
    QString id;
    QString name;
    QString imageTag;
};

class JellyfinClient final : public QObject {
    Q_OBJECT

  public:
    // The writes of a playlist: the `op` of playlistWriteSucceeded and playlistWriteFailed
    enum class PlaylistOp { Create, Add, Remove, Move, Rename, Delete };
    Q_ENUM(PlaylistOp)

    // No limit: the same value as the profile's MaxStaticBitrate
    static constexpr qint64 DefaultMaxStreamingBitrate = 1'000'000'000;

    explicit JellyfinClient(QObject *parent = nullptr);
    ~JellyfinClient() override;

    // A stable DeviceId must be supplied by the profile layer, not generated afresh on every login. Reject fields that could inject auth directives
    bool setDeviceIdentity(const QString &name, const QString &id, const QString &version);
    void setAllowInsecureHttp(bool allowed); // explicit user opt-in for non-loopback HTTP

    bool connectToServer(const QUrl &baseUrl);
    bool authenticate(const QString &username, const QString &password);
    bool initiateQuickConnect();
    // The UI may poll every ~5 seconds; no timers or secrets are exposed here
    bool pollQuickConnect();
    void cancelQuickConnect();
    bool isQuickConnectPending() const;
    bool fetchUserViews();
    // The universal listing of `/Items`: library grids, search and ids= resolution. The members of `query` are validated and composed into the camelCase query the 12.4 routes accept; the ids better than one item are joined comma-only. `channel` names the consumer: a family keeps one request in flight per channel, a new call on the same channel aborts the pending one, and every signal of the call carries the channel back, so independent consumers (the library grid, the home rows) pass distinct channels and ignore the others' replies. Only the families with several consumers (Items, Resume, Latest, NextUp, Similar, Seasons, Episodes, Genres, Filters) take a channel; the others have the single channel 0
    bool fetchItems(const ItemQuery &query, int channel = 0);
    // The length of the encoded query fetchItems would send for `query` (the current user's id included), to compare with MaxItemsQueryChars; the validity of the members is not checked here
    int itemsQueryChars(const ItemQuery &query) const;
    // The Continue Watching row: `GET /UserItems/Resume` (the route the upstream resume section speaks on the 12.x generation), video items; the envelope's total is still required and validated and `resumeItemsReady` carries it. A `startIndex` of 0 or more pages the list, -1 sends none
    bool fetchResumeItems(int limit, int channel = 0, int startIndex = -1);
    // One library's Recently Added row: `GET /Items/Latest` — the bare-array endpoint. An empty `parentId` spans the whole library set. An `isPlayed` that is set rides as `isPlayed=true|false` (false asks for unplayed items only, the user's HidePlayedInLatest), unset sends nothing
    bool fetchLatestItems(const QString &parentId, int limit, int channel = 0,
                          std::optional<bool> isPlayed = std::nullopt);
    // The Next Up row: `GET /Shows/NextUp`. An empty `seriesId` spans every series of the parent (or the whole library). `nextUpDateCutoff` is the upstream user setting's `yyyy-MM-dd` date string the server should not look behind; `enableRewatching` rides as true or false
    bool fetchNextUp(const QString &seriesId, const QString &parentId,
                     const QString &nextUpDateCutoff, bool enableRewatching, int limit,
                     int channel = 0);
    // One series' seasons, and its episodes per season (`seasonId` empty = every season, paged by `startIndex`/`limit`; `startIndex` < 0 lets the server default)
    bool fetchSeasons(const QString &seriesId, int channel = 0);
    bool fetchEpisodes(const QString &seriesId, const QString &seasonId, int startIndex, int limit,
                       int channel = 0);
    // Similar items of any item kind (the generic `/Items/{itemId}/Similar` route)
    bool fetchSimilarItems(const QString &itemId, int limit, int channel = 0);
    // Drops the pending requests of one channel (silently, like a superseded one) and forgets its bookkeeping: for a consumer that is going away, or one that abandons what it asked for and may use the channel again later. Channel 0 is not releasable
    void releaseListingChannel(int channel);
    // Genre rails: `/Genres` validated as an envelope (the items carry Name and Id). An empty `parentId` sends none (every library of the kinds)
    bool fetchGenres(const QString &parentId, int startIndex, int limit,
                     const QString &includeItemTypes = QString(), int channel = 0);
    // The browse filter values: `GET /Items/Filters2`. The known arrays (`Genres` rows of Name/Id, `Tags` strings) and any other QueryFilters member pass through as-is. An empty `parentId` sends none
    bool fetchFilters(const QString &parentId, const QString &includeItemTypes = QString(),
                      int channel = 0);
    // The legacy filter values: `GET /Items/Filters` (OfficialRatings and Years, which Filters2 lacks), family "FiltersLegacy" with its own signals so that an endpoint missing on the server (a 404 in legacyFiltersFailed) only hides those sections. The object is passed through as-is, legacyFacetsFrom() bounds it. An empty `parentId` sends none
    bool fetchLegacyFilters(const QString &parentId, const QString &includeItemTypes = QString(),
                            int channel = 0);
    // The movie recommendation rows: `GET /Movies/Recommendations` (bare array of RecommendationDto, each with its own Items). An empty `parentId` spans every library. `categoryLimit` 1 to 20, `itemLimit` 1 to 100
    bool fetchMovieRecommendations(const QString &parentId, int categoryLimit, int itemLimit,
                                   int channel = 0);
    // The server's suggestions: `GET /Items/Suggestions`. The route has no parentId, so the answer is always for the whole library set of the user. `type` is one BaseItemKind, `limit` 1 to 100
    bool fetchSuggestions(const QString &type, int startIndex, int limit, int channel = 0);
    // Upcoming episodes: `GET /Shows/Upcoming` (not NextUp). An empty `parentId` spans every library. `startIndex` -1 sends none, `limit` is 1 to 100
    bool fetchUpcomingEpisodes(const QString &parentId, int startIndex, int limit, int channel = 0);
    // The studios (the networks of a series library): `GET /Studios`, items with Name and Id. An empty `parentId` sends none; `startIndex` and `limit` -1 send none
    bool fetchStudios(const QString &parentId, const QString &includeItemTypes, int startIndex,
                      int limit, int channel = 0);
    // The connected sessions of the server (`GET /Sessions`): the Remote page's data. With `controllableByUserOnly` the server is asked for the sessions this user may control
    bool fetchSessions(bool controllableByUserOnly);
    // Control another session returned by /Sessions. The session id uses the same
    // bounded path-token grammar as other Jellyfin ids; the command is restricted
    // to Pause, Unpause and Stop. No arbitrary path, URL or JSON reaches the wire.
    bool sendSessionPlaystate(const QString &sessionId, const QString &command);
    // Update the signed-in user's favorite flag for one item. Only one write per
    // item may be in flight; 401 expires the session and all other failures
    // leave the displayed state unchanged. Replies after logout are ignored.
    bool setItemFavorite(const QString &itemId, bool favorite);
    // Mark/unmark a video as watched for the authenticated user. Deduplicate
    // writes by canonical GUID; the state changes only after a confirmed reply.
    // The endpoint requires userId on 12.4, not the legacy /Users/... route.
    bool setItemPlayed(const QString &itemId, bool played);
    // The entries of one playlist (`GET /Playlists/{id}/Items`), paged: each item carries its `PlaylistItemId` (copies of one item share one), `playlistEntriesReady` carries the page and the `TotalRecordCount`. `startIndex` is 0 or more, `limit` 1 or more and is cut to MaxPlaylistPageSize. Same channel rules as fetchItems; failures arrive as listingFailed("PlaylistEntries")
    bool fetchPlaylistEntries(const QString &playlistId, int startIndex, int limit,
                              int channel = 0);
    // The playlist writes: no channel, one write per playlist in flight (a call for a playlist with a write pending returns false, whatever the spelling of its id; so does a second create while one is pending), the outcome comes as playlistWriteSucceeded or playlistWriteFailed. Every call returns false without a request when not authenticated or when its input is refused: an id that is not a Jellyfin id, a name that normalizedPlaylistName() refuses, more than MaxPlaylistWriteIds ids, an empty id list for add and remove, a negative index. A 401 expires the session and still ends with playlistWriteFailed(401). A create succeeds only on a 200 with a valid `Id`, every other write only on a 204: any other status, a 200 included, is a failure carrying that status. A failure with status 0 (transport error, timeout) may still have been applied by the server: a consumer reloads after any failure and never retries a create blindly. Writes in flight are dropped without any signal when the session ends (sessionExpired, credentialsCleared), so a consumer resets its per-write state on those signals, as with favorites. Create makes a private video playlist holding `itemIds` (may be empty): the new id comes back as `createdId`
    bool createPlaylist(const QString &name, const QStringList &itemIds);
    bool addToPlaylist(const QString &playlistId, const QStringList &itemIds);
    // `entryIds` are PlaylistItemIds, not item ids: the server removes every copy of an entry and ignores an unknown one
    bool removePlaylistEntries(const QString &playlistId, const QStringList &entryIds);
    // `newIndex` is sent as given: the caller clamps it to the playlist, the server answers 500 past the end
    bool movePlaylistEntry(const QString &playlistId, const QString &entryId, int newIndex);
    bool renamePlaylist(const QString &playlistId, const QString &name);
    // `DELETE /Items/{id}` removes any item for good, so the item is read first (`GET /Items/{id}`) and deleted only when its `Type` is exactly "Playlist" and its `CanDelete` is true: a playlist without that ends as playlistWriteFailed(Delete, 403) with no DELETE (the server answers 401 to a delete the user may not make, which would end the session); any other type, a failed read or an invalid reply ends as playlistWriteFailed(Delete) with the status of the read, and nothing is deleted. The playlist's write slot is held across both requests
    bool deletePlaylist(const QString &playlistId);
    // The token-free URL of one item image (`GET /Items/{itemId}/Images/{type}` has no `security` requirement in the 12.4 spec, so the URL carries no credentials and the image fetch may stay unauthenticated). `imageType` is one of the spec's ImageType names; `format` may be empty (the server negotiates) or one of "jpg", "gif", "png", "webp"; `tag` is an opaque alphanumeric cache buster. Invalid input yields an empty URL, never a partially valid one
    QUrl imageUrl(const QString &itemId, const QString &imageType, int maxWidth, const QString &tag,
                  int quality = 90, const QString &format = {}) const;
    // The token-free URL of one user's avatar: `GET /UserImage?userId=&tag=` answers anonymously (404 without an avatar) and takes no size: the page decodes at the size it needs. `tag` is the user's `PrimaryImageTag` (a cache buster, omitted from the URL when empty). An invalid id or tag yields an empty URL
    QUrl userImageUrl(const QString &userId, const QString &tag) const;
    // Whether `url` is one of the image URLs imageUrl() or userImageUrl() build for the server at `base`: same scheme, host and port, no credentials, and either the path `<base path>/Items/<id>/Images/<type>` with a valid id and type, or `<base path>/UserImage` with a query of only `userId` (valid id) and `tag` (valid). The one place that recognises what the builders produce, for the image fetcher's allowlist
    static bool isServerImageUrl(const QUrl &base, const QUrl &url);
    // The users the server lists on its sign-in screen (`GET /Users/Public`, no credentials, so it works in any state after the probe except while probing): publicUsersReady carries at most MaxPublicUsers validated entries (the others are dropped), publicUsersFailed a transport or protocol failure. A new call supersedes a pending one. Returns false, without a request, before the probe succeeded
    bool fetchPublicUsers();
    // SyncPlay, watching together (the wire format is SyncPlayProtocol.h). The group list (`GET /SyncPlay/List`): syncPlayGroupsReady carries the valid groups, syncPlayGroupsFailed the status (0 for a transport error, an unusable reply keeps its 200). A newer call supersedes a pending one silently. Returns false, without a request, when not authenticated
    bool fetchSyncPlayGroups();
    // One group request, `POST /SyncPlay/<route>`, with a body SyncPlay::isValidRequest accepts for the route. The requests go out one at a time in call order (on separate connections a Ready could overtake the Buffering before it), at most MaxQueuedSyncPlayRequests waiting; each ends with syncPlayRequestFinished, `accepted` for a 204 (a 200 for New, which answers with the group). The server answers 204 and refuses on the socket (a group update to this session), so accepted means delivered; a user without SyncPlay access presumably gets a 403 *(not observed)*. A 401 expires the session; the requests waiting or in flight when the session ends are dropped without a signal. Returns false, without a request, when not authenticated, the body does not fit the route or the queue is full
    static constexpr int MaxQueuedSyncPlayRequests = 32;
    bool postSyncPlay(Kinefin::SyncPlay::Route route, const QJsonObject &body);
    // The server's time (`GET /GetUtcTime`) for the SyncPlay clock: utcTimeReady carries the server's reception and transmission times (ms since the epoch), utcTimeFailed the status. A newer call supersedes a pending one silently. Returns false, without a request, when not authenticated
    bool fetchUtcTime();

    // Asks the server how to play `itemId` and emits playbackInfoReady with the chosen source (media URL without the token) or playbackInfoFailed. Everything goes in the JSON body (the query parameters of this route are deprecated in 12.4); `maxStreamingBitrate` must be sent, the server default is 8 Mbps. Exactly one of the two signals follows a request, except that a newer request, cancelPlaybackInfo(), disconnectFromServer() and a session lost elsewhere (a 401 on another call) end it with no signal of its own; a 401 on this very request emits sessionExpired and then playbackInfoFailed. Returns false, without a request, when not authenticated or on invalid input
    bool requestPlaybackInfo(const QString &itemId, qint64 startTimeTicks,
                             const QJsonObject &deviceProfile,
                             qint64 maxStreamingBitrate = DefaultMaxStreamingBitrate);
    void cancelPlaybackInfo();
    // Optional skip-intro/recap/outro data. Stale replies are discarded.
    bool fetchMediaSegments(const QString &itemId);
    // Authenticated subtitle files are downloaded via Qt, never opened by mpv
    // over the network (which could forward player headers on redirects).
    bool downloadExternalSubtitle(const QUrl &url);
    // The item being played (`GET /Users/{id}/Items/{id}`): its title fields, chapters and trickplay metadata; a stale reply is dropped and a failed one is silent
    bool fetchPlaybackItem(const QString &itemId);
    // Trickplay thumbnails are authenticated on the same origin and bounded in
    // size. Superseded image requests never update the currently playing item.
    bool fetchTrickplayTile(const QString &itemId, const QString &mediaSourceId, int width,
                            int tileIndex);
    // Queues one report for the server, one request at a time and in order (an old session's Stopped always precedes a new session's Start). A newer Progress replaces a queued one, a Stopped drops the queued Progress, only a Stopped is retried (once). Reports never emit a failure signal: playback must not depend on them. Returns false, without a request, when not authenticated or the report is invalid. A 401 expires the session and drops the queue; disconnectFromServer() drops it silently
    bool reportPlayback(const PlaybackReport &report);
    bool hasPendingPlaybackReports() const;
    // For tests; the default is 2 s
    void setPlaybackReportRetryDelay(std::chrono::milliseconds delay);
    // Announces this client to the server; sent automatically after every login, never retried
    bool reportCapabilities();
    // The `Authorization` header value the player must send with the media URL; empty unless authenticated. It carries the access token: never log it or put it in a URL, and check that the player does not forward it to another host on redirects
    QByteArray streamAuthorization() const;
    // The request the WebSocket component opens for `<base>/socket` (https→wss, ws only on a transport that may carry credentials): a fresh attempt asks every time, so the token is never copied anywhere. Empty and invalid unless authenticated. The token travels only in the `Authorization` header, never in a query parameter
    [[nodiscard]] QNetworkRequest webSocketRequest() const;
    // Re-checks a live session against the server (`GET /Users/Me`): a 401 expires it, anything else that fails is silent; a good reply for the session's own user refreshes userName(), userImageTag() and isAdministrator() and emits userProfileRefreshed, one for another user is treated like a 401. One request in flight at a time. Returns false when it cannot even be sent
    bool verifySession();
    // The most entries of the lists the account preference calls take from the server: a longer list fails the call
    static constexpr int MaxCultures = 1000;
    static constexpr int MaxDisplayPreferenceEntries = 256;
    // The signed-in user's whole `UserDto` (`GET /Users/Me`) for the preferences editor, unmodified (unknown fields, nulls and nested objects survive): the caller compares `Id` with the signed-in user. Dedicated family, so neither verifySession()'s outcome nor the listing signals are involved. A new call supersedes a pending one silently. Returns false, without a request, when not authenticated
    bool fetchCurrentUserConfiguration();
    // Replaces the current user's whole configuration (`POST /Users/Configuration`, the object as the JSON body, no user id: the token names the user). Success is 204 (200 is accepted too) and emits userConfigurationUpdated(), anything else userConfigurationUpdateFailed(); a reply that arrives after the session was lost emits nothing. A new call supersedes a pending one silently. Returns false, without a request, when not authenticated or when the object is empty or its body exceeds the response size cap
    bool updateCurrentUserConfiguration(const QJsonObject &configuration);
    // The server's language catalog (`GET /Localization/Cultures`): at most MaxCultures entries, else the call fails
    bool fetchCultures();
    // The `CustomPrefs` of the current user's web display preferences (`GET /DisplayPreferences/usersettings?userId=&client=emby`), values as the server sent them, at most MaxDisplayPreferenceEntries. Read only: nothing in this client ever writes display preferences
    bool fetchDisplayHomeSections();
    // Reattaches a persisted session without a password: only in the Ready state (the public identity probe succeeded), on a transport that may carry credentials, with a header-safe token. Emits loginSucceeded like a fresh login; the caller should follow with verifySession(), whose 401 expires it. Returns false, changing nothing, otherwise
    bool restoreSession(const QString &userId, const QString &token);
    // Whether credentials may be sent to the probed server (HTTPS, loopback, or the explicit insecure-HTTP opt-in): what restoreSession() and authenticate() require of the transport
    bool canSendCredentials() const;
    void disconnectFromServer();
    // Drops the session in memory without disconnecting: the server stays probed (Ready), only the token goes. What Switch User needs: the sign-in page of the same server is next
    void forgetSession();

    // Internal, only the WebSocket component flips it: remote media control is announced while an authenticated socket exists, and every socket entry re-posts the capabilities
    void setMediaControlAnnounced(bool announced);

    bool isAuthenticated() const;
    QUrl serverUrl() const;
    QString serverId() const;
    QString userId() const;
    // The signed-in user's display name and avatar tag as the server last reported them (login reply, then `/Users/Me`); empty when unknown or invalid (a name longer than MaxUserNameChars or with a line break or control character, a tag outside letters, digits and '-'). Empty unless authenticated; a restored session knows neither until verifySession() answers. The name is remote text: show it as plain text
    QString userName() const;
    QString userImageTag() const;
    // `Policy.IsAdministrator` of the signed-in user as the server last reported it (login reply, then `/Users/Me`); false when unknown, absent or not a bool, and unless authenticated (a restored session knows it only once verifySession() answers). A convenience for hiding entries: the server enforces the rights
    bool isAdministrator() const;
    // `Policy.SyncPlayAccess` as the server last reported it (login reply, then `/Users/Me`): "CreateAndJoinGroups", "JoinGroups" or "None"; empty when unknown and unless authenticated
    QString syncPlayAccess() const;
    QString deviceId() const;
    QString deviceName() const;
    // For the credential store only (ServerCredentialSync): it is the one place allowed to put the token on disk. Empty unless authenticated; never log it
    QString accessToken() const;

  Q_SIGNALS:
    void serverReady(const QString &serverId);
    // userName(), userImageTag() and isAdministrator() are already set when this fires
    void loginSucceeded(const QString &userId);
    // A `/Users/Me` reply for the current session's user: `userName` is the last known good name, `imageTag` empty when the user has no avatar
    void userProfileRefreshed(const QString &userId, const QString &userName,
                              const QString &imageTag);
    // The account preference calls: the failures carry the HTTP status (0 for a transport error) and a fixed English reason, a 401 also expires the session. None of them goes through requestFailed or listingFailed
    void currentUserConfigurationReady(const QJsonObject &user);
    void currentUserConfigurationFailed(int httpStatus, const QString &reason);
    void userConfigurationUpdated();
    void userConfigurationUpdateFailed(int httpStatus, const QString &reason);
    void culturesReady(const QJsonArray &cultures);
    void culturesFailed(int httpStatus, const QString &reason);
    void displayHomeSectionsReady(const QJsonObject &customPrefs);
    void displayHomeSectionsFailed(int httpStatus, const QString &reason);
    void publicUsersReady(const QList<Kinefin::PublicUser> &users);
    void publicUsersFailed(int httpStatus, const QString &reason);
    // Emitted when an existing access token left memory (401, logout, reconnect, destructor): the WebSocket component closes and stops. It also fires at teardown, so persistence must not forget on it (see ServerCredentialSync)
    void credentialsCleared();
    void quickConnectCodeReady(const QString &code);
    void quickConnectAwaitingApproval();
    void userViewsReady(const QJsonArray &items, int totalRecordCount);
    void requestFailed(int httpStatus, const QString &reason);
    // A listing family failed (status 0 for a transport error): `family` is the fixed name of the fetcher ("Items", "Resume", "Latest", "NextUp", "Seasons", "Episodes", "Similar", "Genres", "Filters", "MovieRecommendations", "Suggestions", "Upcoming", "Studios", "PlaylistEntries", "Sessions"), the reason a fixed English string; a 401 also expires the session. Listings never emit requestFailed (fetchUserViews, the libraries call, still does), so the Home page error stays about the libraries only. "FiltersLegacy" never emits it: legacyFiltersFailed is its failure
    void listingFailed(const QString &family, int httpStatus, const QString &reason, int channel);
    // `userId` is the user whose session ended (userId() is already empty by then)
    void sessionExpired(const QString &userId);
    // Emitted once when the last queued report was delivered or dropped and nothing is left to send; not after an abort
    void playbackReportsSettled();
    void playbackInfoReady(const Kinefin::PlaybackSource &source);
    void mediaSegmentsReady(const QString &itemId, const QJsonArray &segments);
    void externalSubtitleDownloaded(const QUrl &url, const QByteArray &bytes);
    void externalSubtitleFailed(const QUrl &url);
    void playbackItemReady(const QString &itemId, const QJsonObject &item);
    void trickplayTileReady(const QString &itemId, int width, int index, const QByteArray &jpeg);
    void trickplayTileFailed(const QString &itemId, int width, int index);
    // `reason` is a fixed English string: never a URL, a body or a path from the server
    void playbackInfoFailed(const QString &itemId, int httpStatus, const QString &reason);
    // The listing results: envelope families carry the items and the `TotalRecordCount` the server echoed, the two bare-array families (`/Items/Latest`, `/Sessions`) carry only the array; nothing here ever contains an access token or a server-provided error string
    void itemsReady(const QJsonArray &items, int totalRecordCount, int channel);
    void resumeItemsReady(const QJsonArray &items, int totalRecordCount, int channel);
    void latestItemsReady(const QJsonArray &items, int channel);
    void nextUpReady(const QJsonArray &items, int totalRecordCount, int channel);
    void seasonsReady(const QJsonArray &items, int totalRecordCount, int channel);
    void episodesReady(const QJsonArray &items, int totalRecordCount, int channel);
    void similarItemsReady(const QJsonArray &items, int totalRecordCount, int channel);
    void genresReady(const QJsonArray &items, int totalRecordCount, int channel);
    void filtersReady(const QJsonObject &filters, int channel);
    void legacyFiltersReady(const QJsonObject &filters, int channel);
    // `httpStatus` is 0 for a transport error, 404 for a server without the endpoint; a 401 also expires the session. `reason` is a fixed English string
    void legacyFiltersFailed(int httpStatus, const QString &reason, int channel);
    // The recommendation categories as the server sent them (objects with `Items`, `RecommendationType`, `BaselineItemName`, `CategoryId`): the entries are not validated beyond being objects
    void movieRecommendationsReady(const QJsonArray &categories, int channel);
    void suggestionsReady(const QJsonArray &items, int totalRecordCount, int channel);
    void upcomingReady(const QJsonArray &items, int totalRecordCount, int channel);
    void studiosReady(const QJsonArray &items, int totalRecordCount, int channel);
    void playlistEntriesReady(const QJsonArray &items, int totalRecordCount, int channel);
    // `createdId` is the new playlist's id for Create and empty otherwise (`playlistId` is empty for Create); `httpStatus` is 0 for a transport error, the status of the reply otherwise (so 200 for a create reply without a valid id, and for a delete that named no playlist), and 403 for a delete of a playlist the user may not delete
    void playlistWriteSucceeded(Kinefin::JellyfinClient::PlaylistOp op, const QString &playlistId,
                                const QString &createdId);
    void playlistWriteFailed(Kinefin::JellyfinClient::PlaylistOp op, const QString &playlistId,
                             int httpStatus);
    void sessionsReady(const QJsonArray &sessions);
    void syncPlayGroupsReady(const QList<Kinefin::SyncPlay::GroupInfo> &groups);
    void syncPlayGroupsFailed(int httpStatus);
    void syncPlayRequestFinished(Kinefin::SyncPlay::Route route, bool accepted, int httpStatus);
    void utcTimeReady(qint64 receptionMs, qint64 transmissionMs);
    void utcTimeFailed(int httpStatus);
    void sessionPlaystateSucceeded(const QString &sessionId, const QString &command);
    void sessionPlaystateFailed(const QString &sessionId, const QString &command, int httpStatus);
    void itemFavoriteUpdated(const QString &itemId, bool favorite);
    void itemFavoriteUpdateFailed(const QString &itemId, bool favorite, int httpStatus);
    void itemPlayedUpdated(const QString &itemId, bool played);
    void itemPlayedUpdateFailed(const QString &itemId, bool played, int httpStatus);

  private:
    enum class State {
        Disconnected,
        Probing,
        Ready,
        Authenticating,
        Authenticated,
        QuickStarting,
        QuickPending,
        QuickAuthenticating
    };

    void startProbe(const QUrl &endpoint, quint64 generation, int redirectCount);
    void invalidateSession();
    void finishAuthentication(QNetworkReply *reply);
    void exchangeQuickConnectSecret();
    void clearQuickConnectSecret();
    bool transportAllowsAuthentication() const;
    QByteArray identityAuthorization() const;
    QByteArray tokenAuthorization() const;
    void abortPending();
    void sendNextPlaybackReport();
    void sendNextSyncPlayRequest();
    void finishPlaybackReport(QNetworkReply *reply);
    void clearToken();
    void failProbe(int httpStatus, const QString &reason);
    // The shared status handling of the account preference calls: true for a 200 (or a 204 when `allowNoContent`) without a transport error; otherwise false with the status and the fixed reason (`failure`, or "Authentication expired" after a 401 that also expired the session)
    bool acceptAccountReply(QNetworkReply *finished, bool allowNoContent, const QString &failure,
                            int *status, QString *reason);
    // The shared plumbing of the playlist writes: the one-per-playlist guard, the request and the outcome signals. `body` (JSON) rides only a POST
    bool sendPlaylistWrite(PlaylistOp op, const QString &playlistId, const QByteArray &method,
                           const QUrl &url, const QByteArray &body = {});
    // The request of a write whose slot is already taken
    void dispatchPlaylistWrite(PlaylistOp op, const QString &playlistId, const QByteArray &method,
                               const QUrl &url, const QByteArray &body);
    QUrl endpoint(const QString &path) const;
    QNetworkRequest requestFor(const QUrl &url, bool authenticated) const;
    void attach(QNetworkReply *reply, quint64 generation,
                const std::function<void(QNetworkReply *)> &handler);
    // The listing families: one request in flight per family; a new call of the same family aborts the pending one and arms a fresh serial so its late reply drops silently. `family` names are the fixed strings the fetchers pass
    static QString listingKey(const QString &family, int channel);
    quint64 supersedeListing(const QString &key);
    bool listingSuperseded(const QString &key, quint64 serial) const;
    // The shared response plumbing of the listing families: the status/401 handling with the fixed `listingFailed` emissions, then the envelope, bare-array or object paragraph of the endpoint. They return false (with the failure already emitted) whenever the reply is unusable; the fetcher then emits its Ready signal
    bool listingEnvelope(QNetworkReply *finished, const QString &family, int channel,
                         QJsonArray *items, int *total);
    bool listingArray(QNetworkReply *finished, const QString &family, int channel,
                      QJsonArray *items);
    bool listingObject(QNetworkReply *finished, const QString &family, int channel,
                       QJsonObject *object);

    QNetworkAccessManager m_network;
    QSet<QNetworkReply *> m_pending;
    QSet<QString> m_favoritePending;
    QSet<QString> m_playedPending;
    QSet<QString> m_playlistPending;
    QUrl m_base;
    QString m_serverId;
    QString m_userId;
    QString m_userName;
    QString m_userImageTag;
    bool m_isAdministrator = false;
    QString m_syncPlayAccess;
    QByteArray m_token;
    QByteArray m_quickConnectSecret;
    struct QueuedReport {
        PlaybackReport report;
        QByteArray body;
        int attempts = 0;
    };
    QList<QueuedReport>
        m_reportQueue; // the first one is in flight (m_reportBusy) or waits for its retry (timer active)
    bool m_reportBusy = false;
    QTimer m_reportRetryTimer;
    std::chrono::milliseconds m_reportRetryDelay{2000};
    QPointer<QNetworkReply> m_playbackReply;
    quint64 m_playbackSerial = 0;
    quint64 m_segmentsSerial = 0;
    quint64 m_subtitleSerial = 0;
    quint64 m_playbackItemSerial = 0;
    quint64 m_trickplayTileSerial = 0;
    quint64 m_syncPlayGroupsSerial = 0;
    quint64 m_utcTimeSerial = 0;
    QPointer<QNetworkReply> m_syncPlayGroupsReply;
    struct QueuedSyncPlayRequest {
        SyncPlay::Route route;
        QByteArray body;
    };
    // The first one is in flight while m_syncPlayBusy
    QList<QueuedSyncPlayRequest> m_syncPlayQueue;
    bool m_syncPlayBusy = false;
    QPointer<QNetworkReply> m_utcTimeReply;
    QPointer<QNetworkReply> m_subtitleReply;
    QPointer<QNetworkReply> m_trickplayTileReply;
    QString m_deviceName;
    QString m_deviceId;
    QString m_version;
    quint64 m_generation = 0;
    State m_state = State::Disconnected;
    bool m_allowInsecureHttp = false;
    bool m_quickPollInFlight = false;
    // Flipped by the WebSocket component while an authenticated socket exists; `reportCapabilities()` carries it
    bool m_mediaControlAnnounced = false;
    bool m_verifyInFlight = false;
    // The listing families: `Items`, `Resume`, `Latest`, `NextUp`, `Seasons`, `Episodes`, `Similar`, `Genres`, `Filters`, `FiltersLegacy`, `MovieRecommendations`, `Suggestions`, `Upcoming`, `Studios`, `PlaylistEntries`, `Sessions` — the serial each accepted request holds and the reply of the one in flight (for supersede aborts)
    QHash<QString, quint64> m_listingSerials;
    QHash<QString, QPointer<QNetworkReply>> m_listingReplies;
};

} // namespace Kinefin
