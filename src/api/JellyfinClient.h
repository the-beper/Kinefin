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

// An asynchronous P2a REST client: bootstrap, login, Quick Connect, library views and PlaybackInfo negotiation. It also owns the ordered queue of playback reports and the capabilities POST. It does not own profile persistence or the WebSocket. The Quick Connect pairing secret and access token remain in memory, never in logs or signals.
// The rule the client applies to every identity field it puts into an Authorization header (no control characters, quotes or backslashes, at most 128 characters).
bool isValidHeaderField(const QString &field);

class JellyfinClient final : public QObject {
    Q_OBJECT

  public:
    // No limit: the same value as the profile's MaxStaticBitrate (parity §3).
    static constexpr qint64 DefaultMaxStreamingBitrate = 1'000'000'000;

    explicit JellyfinClient(QObject *parent = nullptr);
    ~JellyfinClient() override;

    // A stable DeviceId must be supplied by the profile layer, not generated afresh on every login. Reject fields that could inject auth directives.
    bool setDeviceIdentity(const QString &name, const QString &id, const QString &version);
    void setAllowInsecureHttp(bool allowed); // explicit user opt-in for non-loopback HTTP

    bool connectToServer(const QUrl &baseUrl);
    bool authenticate(const QString &username, const QString &password);
    bool initiateQuickConnect();
    // The UI may poll every ~5 seconds; no timers or secrets are exposed here.
    bool pollQuickConnect();
    void cancelQuickConnect();
    bool isQuickConnectPending() const;
    bool fetchUserViews();
    // Asks the server how to play `itemId` and emits playbackInfoReady with the chosen source (media URL without the token) or playbackInfoFailed. Everything goes in the JSON body (the query parameters of this route are deprecated in 12.4); `maxStreamingBitrate` must be sent, the server default is 8 Mbps. Exactly one of the two signals follows a request, except that a newer request, cancelPlaybackInfo(), disconnectFromServer() and a session lost elsewhere (a 401 on another call) end it with no signal of its own; a 401 on this very request emits sessionExpired and then playbackInfoFailed. Returns false, without a request, when not authenticated or on invalid input.
    bool requestPlaybackInfo(const QString &itemId, qint64 startTimeTicks,
                             const QJsonObject &deviceProfile,
                             qint64 maxStreamingBitrate = DefaultMaxStreamingBitrate);
    void cancelPlaybackInfo();
    // Queues one report for the server, one request at a time and in order (an old session's Stopped always precedes a new session's Start). A newer Progress replaces a queued one, a Stopped drops the queued Progress, only a Stopped is retried (once). Reports never emit a failure signal: playback must not depend on them. Returns false, without a request, when not authenticated or the report is invalid. A 401 expires the session and drops the queue; disconnectFromServer() drops it silently.
    bool reportPlayback(const PlaybackReport &report);
    bool hasPendingPlaybackReports() const;
    // For tests; the default is 2 s.
    void setPlaybackReportRetryDelay(std::chrono::milliseconds delay);
    // Announces this client to the server; sent automatically after every login, never retried.
    bool reportCapabilities();
    // The `Authorization` header value the player must send with the media URL; empty unless authenticated. It carries the access token: never log it or put it in a URL, and check that the player does not forward it to another host on redirects.
    QByteArray streamAuthorization() const;
    // The request the WebSocket component opens for `<base>/socket` (https→wss, ws only on a transport that may carry credentials): a fresh attempt asks every time, so the token is never copied anywhere. Empty and invalid unless authenticated. The token travels only in the `Authorization` header, never in a query parameter.
    [[nodiscard]] QNetworkRequest webSocketRequest() const;
    // Re-checks a live session against the server (`GET /Users/Me`): a 401 expires it, everything else is silent. One request in flight at a time. Returns false when it cannot even be sent.
    bool verifySession();
    // Reattaches a persisted session without a password: only in the Ready state (the public identity probe succeeded), on a transport that may carry credentials, with a header-safe token. Emits loginSucceeded like a fresh login; the caller should follow with verifySession(), whose 401 expires it. Returns false, changing nothing, otherwise.
    bool restoreSession(const QString &userId, const QString &token);
    // Whether credentials may be sent to the probed server (HTTPS, loopback, or the explicit insecure-HTTP opt-in): what restoreSession() and authenticate() require of the transport.
    bool canSendCredentials() const;
    void disconnectFromServer();

    // Internal, only the WebSocket component flips it: remote media control is announced while an authenticated socket exists, and every socket entry re-posts the capabilities.
    void setMediaControlAnnounced(bool announced);

    bool isAuthenticated() const;
    QUrl serverUrl() const;
    QString serverId() const;
    QString userId() const;
    QString deviceId() const;
    QString deviceName() const;
    // For the credential store only (ServerCredentialSync): it is the one place allowed to put the token on disk. Empty unless authenticated; never log it.
    QString accessToken() const;

  Q_SIGNALS:
    void serverReady(const QString &serverId);
    void loginSucceeded(const QString &userId);
    // Emitted when an existing access token left memory (401, logout, reconnect, destructor): the WebSocket component closes and stops. It also fires at teardown, so persistence must not forget on it (see ServerCredentialSync).
    void credentialsCleared();
    void quickConnectCodeReady(const QString &code);
    void quickConnectAwaitingApproval();
    void userViewsReady(const QJsonArray &items, int totalRecordCount);
    void requestFailed(int httpStatus, const QString &reason);
    void sessionExpired();
    // Emitted once when the last queued report was delivered or dropped and nothing is left to send; not after an abort.
    void playbackReportsSettled();
    void playbackInfoReady(const Kinefin::PlaybackSource &source);
    // `reason` is a fixed English string: never a URL, a body or a path from the server.
    void playbackInfoFailed(const QString &itemId, int httpStatus, const QString &reason);

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
    // Flipped by the WebSocket component while an authenticated socket exists; `reportCapabilities()` carries it.
    bool m_mediaControlAnnounced = false;
    bool m_verifyInFlight = false;
};

} // namespace Kinefin
