// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cast/CastRenderer.h"
#include "cast/Soap.h"
#include "cast/StreamProbe.h"
#include "cast/UpnpDescription.h"

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>

class QNetworkReply;

namespace Kinefin {

// One DLNA renderer: its own network manager without credentials, cookies, proxy or redirects, one SOAP action at a time with user commands ahead of polls, a 3 s timeout and a 1 s poll
class DlnaRenderer final : public CastRenderer {
    Q_OBJECT

  public:
    // No PLAYING this long after a load is a refusal of the stream
    static constexpr int StartTimeoutMs = 30'000;
    // PLAYING this long without the position moving is a refusal of a direct play's stream: a renderer that cannot fetch the URL stays PLAYING at 0 and reports no error
    static constexpr int StallTimeoutMs = 20'000;
    // The token-less range probe of a direct play
    static constexpr int ProbeTimeoutMs = 5000;
    // SetAVTransportURI and Play: many renderers fetch the stream before they answer
    static constexpr int LoadTimeoutMs = 15'000;

    explicit DlnaRenderer(RendererInfo info, QObject *parent = nullptr);

    const RendererInfo &info() const { return m_info; }
    QString id() const override { return m_info.udn; }
    bool hasVolume() const override { return m_info.hasVolume(); }
    bool canPause() const override { return m_info.canPause(); }
    // A direct play seeks with REL_TIME; a transcode seeks by loading its URL at another start
    bool canSeek() const override { return !m_direct || m_info.canSeekRelTime(); }
    bool direct() const { return m_direct; }

    // Loads and plays `url`, an http or https URL under `serverBase`: a direct play first checks with one token-less range request that the server lets the renderer in, then seeks to `startMs` when the renderer lists REL_TIME; a transcode URL already starts at `startMs`, and its positions count from there. The transport state comes through stateChanged
    bool load(const QUrl &url, const QString &mime, const QString &title, qint64 startMs,
              qint64 durationMs, const QUrl &serverBase, bool direct);
    // The same transcode again from `url`, which starts at `startMs`: how a transcode seeks
    bool load(const CastLoad &request) override
    {
        return load(request.url, request.mime, request.title, request.startMs, request.durationMs,
                    request.serverBase, request.direct);
    }
    bool reload(const QUrl &url, qint64 startMs, const QUrl &subtitleUrl = {}) override;
    bool play() override;
    bool pause() override;
    // A direct play only: a transcode seeks by loading the same URL at another start
    bool seek(qint64 positionMs) override;
    bool stop() override;
    bool setVolume(int volume) override;
    bool setMuted(bool muted) override;
    bool refreshVolume();
    // The item's runtime once the server named it: the renderer's own duration of a transcode is not trusted
    void setDurationMs(qint64 durationMs) override;

    void startPolling();
    void stopPolling();
    bool polling() const { return m_pollTimer.isActive(); }
    qint64 positionMs() const override { return m_positionMs; }
    qint64 durationMs() const override { return m_durationMs; }
    std::optional<Transport> transport() const override { return m_reportedTransport; }
    // -1 until the renderer answered GetVolume
    int volume() const override { return m_volume; }
    bool muted() const override { return m_muted; }
    QUrl url() const { return m_url; }
    // For the tests: a shorter StallTimeoutMs
    void setStallTimeoutMs(int ms) { m_stallTimer.setInterval(ms); }

  private:
    struct Command {
        UpnpService service;
        QString action;
        QList<SoapArgument> arguments;
        QStringList expected;
        std::function<void(const SoapResult &)> done;
        bool poll = false;
        // A fault on it fails the load: SetAVTransportURI and the Play of a load
        bool fatal = false;
        int timeoutMs = kSoapTimeoutMs;
        quint64 generation = 0;
    };

    Command avTransportCommand(const QString &action, QList<SoapArgument> arguments = {}) const;
    void startTransport();
    void probe();
    void onProbeFinished(int status);
    void enqueue(Command command);
    void sendNext();
    void send(const Command &command);
    void onFinished();
    void onTimeout();
    void finish(const SoapResult &result);
    void poll();
    void handleTransport(const SoapResult &result);
    void handlePosition(const SoapResult &result);
    void handleVolume(const SoapResult &result);
    void updateState(Transport state);
    void watchStall(qint64 rendererMs);
    void resetStall();
    bool isOwnTrack(const QString &track) const;
    void abortProbe();
    void fail(Failure failure);

    RendererInfo m_info;
    QNetworkAccessManager m_network;
    QQueue<Command> m_commands;
    QQueue<Command> m_polls;
    std::optional<Command> m_current;
    QNetworkReply *m_reply = nullptr;
    StreamProbe m_probe{&m_network};
    QByteArray m_response;
    int m_responseLimit = kMaxSoapResponseBytes;
    QTimer m_timeout;
    QTimer m_pollTimer;
    QTimer m_startTimer;
    QTimer m_stallTimer;
    // The first position the renderer gave while PLAYING for this load: the stall timer runs from it
    std::optional<qint64> m_stallBaseMs;
    // The position moved since: the load plays, and the stall watchdog is off for it
    bool m_progressSeen = false;
    // Every load and stop starts a new generation: an answer to a command of an earlier one is dropped
    quint64 m_generation = 0;
    QUrl m_url;
    QUrl m_serverBase;
    QString m_mime;
    QString m_title;
    qint64 m_startMs = 0;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    std::optional<Transport> m_reportedTransport;
    // PLAYING or PAUSED_PLAYBACK seen since the load
    bool m_started = false;
    // The start of a direct play was checked once it played: a renderer that refused the Seek before Play gets it again
    bool m_startChecked = false;
    int m_pollFailures = 0;
    int m_volume = -1;
    bool m_muted = false;
    bool m_direct = true;
    bool m_stopping = false;
    int m_pollCount = 0;
};

} // namespace Kinefin
