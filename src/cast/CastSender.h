// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cast/CastChannel.h"
#include "cast/CastRenderer.h"
#include "cast/Mdns.h"
#include "cast/StreamProbe.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

class QNetworkReply;

namespace Kinefin {

// One Google Cast device playing on the Default Media Receiver (CC1AD845): the channel, the LAUNCH and LOAD, the controls, the mapping of MEDIA_STATUS to the CastRenderer states, the position extrapolated between statuses, the reconnect and the teardown. The device's own network traffic stays in its CastChannel; the media probe uses a network manager of its own with no credentials
class CastSender final : public CastRenderer {
    Q_OBJECT

  public:
    // The defaults are the production values; the tests shorten the times and inject the resolvers
    struct Options {
        CastChannel::Options channel;
        // From the LAUNCH to a RECEIVER_STATUS with our app and its transportId
        int launchTimeoutMs = 10'000;
        // From the LOAD to a PLAYING with a position above 0; a LOAD that carries a text track gets 180 s more, the device fetches the track first and the server may need minutes to extract it
        int startTimeoutMs = 30'000;
        // The reply PLAYING at position 0 that is not followed by a moving position
        int stuckMs = 10'000;
        // A media GET_STATUS this often while a media session exists
        int statusPollMs = 5000;
        // positionChanged() this often while playing
        int positionTickMs = 1000;
        int reconnectAttempts = 3;
        int reconnectDelayMs = 5000;
        // The media STOP of stop() is waited for this long before the app STOP and the close
        int stopTimeoutMs = 1500;
        // Finds the device again for a reconnect and calls `done` with it, or nullopt. Empty: a one-shot MdnsSearch of 3 s under the same address policy
        std::function<void(const QString &deviceId,
                           std::function<void(const std::optional<CastDeviceInfo> &)> done)>
            resolveDevice;
        // Turns the host name of the server URL into an address for the device (the Chromecast ignores the network's DNS). Empty: QHostInfo::lookupHost
        std::function<void(const QString &host, std::function<void(const QHostAddress &)> done)>
            resolveHost;
        // The address the media URLs may name once resolved
        AddressPolicy mediaAddressPolicy = isAllowedRendererAddress;
    };

    CastSender(CastDeviceInfo info, const Options &options, QObject *parent = nullptr);
    explicit CastSender(CastDeviceInfo info, QObject *parent = nullptr);
    ~CastSender() override;

    // `cast:` followed by the device id: what the picker lists the device as
    static QString targetId(const CastDeviceInfo &info);
    const CastDeviceInfo &info() const { return m_info; }
    bool direct() const { return m_direct; }

    QString id() const override { return targetId(m_info); }
    // False without anything sent when a check fails: the URL is not http or https, has user info, is not under `serverBase`, carries a token (a `carriesToken`-style check: `api_key`, `apikey` or an access token in the query), is over 1024 bytes as a contentId, or `startMs` is outside 0 to 24 hours; and when a load is already running without a stop. A track or image URL that fails the same rules is left out and the video loads without it. Otherwise the load goes on asynchronously: the DNS rule (an `http` server whose host name resolves to an address passing `mediaAddressPolicy` is sent to the device as that address), a token-less GET of the text track, which only makes the server extract it before the device asks, the token-less range probe of a direct play (a 401 or 403 is Failure::ServerRefused, any other status but 206 Failure::Refused), the channel, the LAUNCH and the LOAD
    bool load(const CastLoad &request) override;
    // A transcode seeks by loading its URL again at `startMs`: a new LOAD on the running session, the position counting from `startMs`; `subtitleUrl` is the track at that start. Also accepted while the previous LOAD is still unanswered: the newer one replaces it
    bool reload(const QUrl &url, qint64 startMs, const QUrl &subtitleUrl = {}) override;
    bool play() override;
    bool pause() override;
    // A direct play only (SEEK); false for a transcode
    bool seek(qint64 positionMs) override;
    // Media STOP, then app STOP when the last RECEIVER_STATUS still lists our sessionId, then CLOSE; stopped() follows when that is done (as for a DLNA renderer), unless a load started meanwhile
    bool stop() override;
    bool setVolume(int volume) override;
    bool setMuted(bool muted) override;
    void setDurationMs(qint64 durationMs) override;

    bool hasVolume() const override;
    bool canPause() const override { return true; }
    bool canSeek() const override { return true; }
    qint64 positionMs() const override;
    qint64 durationMs() const override { return m_durationMs; }
    std::optional<Transport> transport() const override { return m_transport; }
    int volume() const override { return m_volume; }
    bool muted() const override { return m_muted; }

  private:
    enum class Phase {
        Idle,
        // The DNS rule and the range probe
        Preparing,
        // The channel is opening and authenticating
        Connecting,
        // LAUNCH sent, waiting for our app and its transport
        Launching,
        // LOAD sent, no media session yet
        Loading,
        // A media session exists
        Active,
        Reconnecting,
        Stopping,
    };
    enum class Resume {
        None,
        Resolving,
        Opening,
        ReceiverStatus,
        MediaStatus,
    };
    struct App {
        QString appId;
        QString sessionId;
        QString transportId;
    };

    static const char *phaseName(Phase phase);
    void prepare();
    void lookupHost(const QString &host, std::function<void(const QHostAddress &)> done);
    void findDevice(const QString &deviceId,
                    std::function<void(const std::optional<CastDeviceInfo> &)> done);
    void applyResolved(const QHostAddress &address);
    QUrl rewritten(const QUrl &url) const;
    void proceedAfterResolve();
    void onProbeFinished(int status);
    void warmTrack(const QUrl &url);
    void abortWarmup();

    void openChannel();
    void createChannel(const QHostAddress &address, quint16 port);
    void closeChannel();
    void onChannelOpened();
    void onChannelFailed(CastChannel::FailReason reason, const QString &detail);
    void onMessage(const QString &source, const QString &ns, const QJsonObject &json);
    void onVirtualConnectionClosed(const QString &source);

    void sendLaunch();
    void onReceiverMessage(const QJsonObject &json);
    void handleLaunchError(const QJsonObject &json);
    QList<App> readApplications(const QJsonObject &status) const;
    void applyVolume(const QJsonObject &volume);
    void joinApp(const App &app);
    bool sendLoad();
    QJsonObject loadBody() const;

    void onMediaMessage(const QJsonObject &json);
    void handleMediaStatus(const QJsonObject &json);
    void handleEntry(const QJsonObject &entry, quint32 requestId);
    void logStatus(const QJsonObject &entry, const QString &state, const QString &reason);
    bool trackInLoad() const;
    void announceWarmTrack();
    void handleIdle(const QString &reason, bool loading);
    void handlePlaying(const QJsonObject &entry);
    void adoptSession(qint64 id);
    void markStarted();
    void applyPosition(const QJsonObject &entry);
    void setTransport(Transport state);

    bool canControl() const;
    bool sendMediaCommand(QLatin1StringView type, QJsonObject extra = {});
    bool sendVolume(const QJsonObject &volume);
    void pollStatus();
    void tick();
    int startDeadlineMs() const;
    void onStartTimeout();
    void onStepTimeout();
    bool sendMediaStop();
    void sendAppStop();
    void proceedStop();
    void announceOwnStop();

    void beginReconnect();
    void reconnectAttempt();
    void attemptFailed();
    void handleResumeReceiverStatus(const QList<App> &apps);
    void handleResumeMediaStatus(const QJsonObject &json);

    void fail(Failure failure);
    void endedByDevice();
    void stoppedByDevice(const char *reason);
    void finishSession();
    void freezePosition();

    CastDeviceInfo m_info;
    Options m_options;
    QNetworkAccessManager m_network;
    StreamProbe m_probe{&m_network};
    // The token-less request that makes the server extract the text track before the device asks for it
    QPointer<QNetworkReply> m_warmup;
    CastChannel *m_channel = nullptr;
    QTimer m_pollTimer;
    QTimer m_tickTimer;
    QTimer m_startTimer;
    QTimer m_stuckTimer;
    // The wait for our app after LAUNCH, and for each answer of a reconnect
    QTimer m_stepTimer;
    QTimer m_stopTimer;
    QTimer m_reconnectTimer;
    QElapsedTimer m_statusClock;
    std::optional<Transport> m_transport;
    Phase m_phase = Phase::Idle;
    Resume m_resume = Resume::None;
    // Every load, stop and end starts a new one: the answer of a resolver that outlived its load is dropped
    quint64 m_generation = 0;

    // What the device was asked to play: the URLs as sent (the DNS rule applied) and the server URL they must stay under
    QUrl m_originalBase;
    QString m_baseHost;
    QString m_resolvedHost;
    QUrl m_url;
    QUrl m_trackUrl;
    QUrl m_imageUrl;
    QString m_contentId;
    QString m_mime;
    QString m_title;
    QString m_subtitle;
    QString m_trackLanguage;
    QString m_trackName;
    double m_loadStartSec = 0;

    qint64 m_durationMs = 0;
    // The position at the last status, the transcode start included; positionMs() adds the time since m_statusClock while playing
    qint64 m_baseMs = 0;
    // What the reported time of a transcode counts from
    qint64 m_offsetMs = 0;
    double m_rate = 1.0;
    // PLAYING with a moving position or PAUSED seen since the load
    bool m_started = false;
    // The state and idle reason of the last media status logged: a buffering device sends several statuses a second
    QString m_loggedStatus;
    // The server answered the text track of this load (its warm-up request)
    bool m_trackWarm = false;
    // The last LOAD went out without the text track, which comes with a reload once it is warm
    bool m_trackLeftOut = false;

    QString m_sessionId;
    QString m_transportId;
    // The last RECEIVER_STATUS still listed our sessionId
    bool m_sessionListed = false;
    quint32 m_launchRequest = 0;
    // The GET_STATUS whose answer a reconnect judges
    quint32 m_resumeRequest = 0;
    bool m_launchRetried = false;
    quint32 m_loadRequestId = 0;
    // The LOAD has not been answered by a PLAYING, BUFFERING or PAUSED with its requestId
    bool m_loadInFlight = false;
    qint64 m_mediaSessionId = 0;
    // The session a reload replaces
    qint64 m_oldMediaSessionId = 0;
    QByteArray m_firstLeaf;
    // The device a reconnect found: m_info takes it only once its leaf is the first one
    QHostAddress m_candidateAddress;
    quint16 m_candidatePort = 0;
    int m_attempt = 0;

    int m_volume = -1;
    bool m_muted = false;
    bool m_volumeSeen = false;
    bool m_volumeFixed = false;
    bool m_direct = true;
};

} // namespace Kinefin
