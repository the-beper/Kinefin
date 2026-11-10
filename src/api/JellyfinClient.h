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

#include "api/MediaSourceSelection.h"
#include "api/PlaybackReport.h"

class QNetworkReply;

namespace Kinefin {

bool isValidHeaderField(const QString &field);

struct ItemQuery {
    QString parentId;
    QString includeItemTypes; // comma-separated BaseItemKind tokens
    QString excludeItemTypes;
    QStringList itemIds;
    QString searchTerm;
    QString sortBy;    // comma-separated sorting keys (shape-checked letters/digits/underscores)
    QString sortOrder; // "Ascending" or "Descending"
    int startIndex = -1;
    int limit = -1;
    bool recursive = false;
    QString filters;    // comma-separated ItemFilter tokens (e.g. "IsResumable")
    QStringList fields; // ItemFields tokens
};

class JellyfinClient final : public QObject {
    Q_OBJECT

  public:
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
    // The universal listing of `/Items`: library grids, search and ids= resolution. The members of `query` are validated and composed into the camelCase query the 12.4 routes accept; the ids better than one item are joined comma-only. `channel` names the consumer: a family keeps one request in flight per channel, a new call on the same channel aborts the pending one, and every signal of the call carries the channel back, so independent consumers (the library grid, the home rows) pass distinct channels and ignore the others' replies. Only the seven families the browse and details pages use (Items, Resume, Latest, NextUp, Similar, Seasons, Episodes) take a channel; the others have the single channel 0
    bool fetchItems(const ItemQuery &query, int channel = 0);
    // The Continue Watching row: `GET /UserItems/Resume` (the route the upstream resume section speaks on the 12.x generation), video items; the envelope's total is still required and validated
    bool fetchResumeItems(int limit, int channel = 0);
    // One library's Recently Added row: `GET /Items/Latest` — the bare-array endpoint. An empty `parentId` spans the whole library set
    bool fetchLatestItems(const QString &parentId, int limit, int channel = 0);
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
    // Drops the pending requests of one channel (silently, like a superseded one) and forgets its bookkeeping: for a consumer that is going away and will not use the channel again. Channel 0 is not releasable
    void releaseListingChannel(int channel);
    // Genre rails: `/Genres` validated as an envelope (the items carry Name and Id)
    bool fetchGenres(const QString &parentId, int startIndex, int limit);
    // The browse filter values: `GET /Items/Filters2`. The known arrays (`Genres` rows of Name/Id, `Tags` strings) and any other QueryFilters member pass through as-is
    bool fetchFilters(const QString &parentId);
    // The connected sessions of the server (`GET /Sessions`): the Remote page's data. With `controllableByUserOnly` the server is asked for the sessions this user may control
    bool fetchSessions(bool controllableByUserOnly);
    // The token-free URL of one item image (`GET /Items/{itemId}/Images/{type}` has no `security` requirement in the 12.4 spec, so the URL carries no credentials and the image fetch may stay unauthenticated). `imageType` is one of the spec's ImageType names; `format` may be empty (the server negotiates) or one of "jpg", "gif", "png", "webp"; `tag` is an opaque alphanumeric cache buster. Invalid input yields an empty URL, never a partially valid one
    QUrl imageUrl(const QString &itemId, const QString &imageType, int maxWidth, const QString &tag,
                  int quality = 90, const QString &format = {}) const;
    // Whether `url` is one of the image URLs imageUrl() builds for the server at `base`: same scheme, host and port, no credentials, and the path `<base path>/Items/<id>/Images/<type>` with a valid id and type. The one place that recognises what imageUrl() builds, for the image fetcher's allowlist
    static bool isServerImageUrl(const QUrl &base, const QUrl &url);

    // Asks the server how to play `itemId` and emits playbackInfoReady with the chosen source (media URL without the token) or playbackInfoFailed. Everything goes in the JSON body (the query parameters of this route are deprecated in 12.4); `maxStreamingBitrate` must be sent, the server default is 8 Mbps. Exactly one of the two signals follows a request, except that a newer request, cancelPlaybackInfo(), disconnectFromServer() and a session lost elsewhere (a 401 on another call) end it with no signal of its own; a 401 on this very request emits sessionExpired and then playbackInfoFailed. Returns false, without a request, when not authenticated or on invalid input
    bool requestPlaybackInfo(const QString &itemId, qint64 startTimeTicks,
                             const QJsonObject &deviceProfile,
                             qint64 maxStreamingBitrate = DefaultMaxStreamingBitrate);
    void cancelPlaybackInfo();
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
    // Re-checks a live session against the server (`GET /Users/Me`): a 401 expires it, everything else is silent. One request in flight at a time. Returns false when it cannot even be sent
    bool verifySession();
    // Reattaches a persisted session without a password: only in the Ready state (the public identity probe succeeded), on a transport that may carry credentials, with a header-safe token. Emits loginSucceeded like a fresh login; the caller should follow with verifySession(), whose 401 expires it. Returns false, changing nothing, otherwise
    bool restoreSession(const QString &userId, const QString &token);
    // Whether credentials may be sent to the probed server (HTTPS, loopback, or the explicit insecure-HTTP opt-in): what restoreSession() and authenticate() require of the transport
    bool canSendCredentials() const;
    void disconnectFromServer();

    // Internal, only the WebSocket component flips it: remote media control is announced while an authenticated socket exists, and every socket entry re-posts the capabilities
    void setMediaControlAnnounced(bool announced);

    bool isAuthenticated() const;
    QUrl serverUrl() const;
    QString serverId() const;
    QString userId() const;
    QString deviceId() const;
    QString deviceName() const;
    // For the credential store only (ServerCredentialSync): it is the one place allowed to put the token on disk. Empty unless authenticated; never log it
    QString accessToken() const;

  Q_SIGNALS:
    void serverReady(const QString &serverId);
    void loginSucceeded(const QString &userId);
    // Emitted when an existing access token left memory (401, logout, reconnect, destructor): the WebSocket component closes and stops. It also fires at teardown, so persistence must not forget on it (see ServerCredentialSync)
    void credentialsCleared();
    void quickConnectCodeReady(const QString &code);
    void quickConnectAwaitingApproval();
    void userViewsReady(const QJsonArray &items, int totalRecordCount);
    void requestFailed(int httpStatus, const QString &reason);
    // A listing family failed (status 0 for a transport error): `family` is the fixed name of the fetcher ("Items", "Resume", "Latest", "NextUp", "Seasons", "Episodes", "Similar", "Genres", "Filters", "Sessions"), the reason a fixed English string; a 401 also expires the session. Listings never emit requestFailed (fetchUserViews, the libraries call, still does), so the Home page error stays about the libraries only
    void listingFailed(const QString &family, int httpStatus, const QString &reason, int channel);
    void sessionExpired();
    // Emitted once when the last queued report was delivered or dropped and nothing is left to send; not after an abort
    void playbackReportsSettled();
    void playbackInfoReady(const Kinefin::PlaybackSource &source);
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
    void genresReady(const QJsonArray &items, int totalRecordCount);
    void filtersReady(const QJsonObject &filters);
    void sessionsReady(const QJsonArray &sessions);

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
    void finishPlaybackReport(QNetworkReply *reply);
    void clearToken();
    void failProbe(int httpStatus, const QString &reason);
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
    QUrl m_base;
    QString m_serverId;
    QString m_userId;
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
    // The listing families: `Items`, `Resume`, `Latest`, `NextUp`, `Seasons`, `Episodes`, `Similar`, `Genres`, `Filters`, `Sessions` — the serial each accepted request holds and the reply of the one in flight (for supersede aborts)
    QHash<QString, quint64> m_listingSerials;
    QHash<QString, QPointer<QNetworkReply>> m_listingReplies;
};

} // namespace Kinefin
