// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cast/Soap.h"
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
class DlnaRenderer final : public QObject {
    Q_OBJECT

  public:
    enum class Transport {
        Stopped,
        Playing,
        Paused,
        Transitioning,
        NoMedia,
    };
    Q_ENUM(Transport)

    enum class Failure {
        // The renderer would not play the stream it was given: a fault on the load, no PLAYING in time, a stop before it ever played. Another stream may still play
        Refused,
        // No answer to the load or to three polls in a row
        NotResponding,
        // The server asks the renderer for a credential: nothing reached the renderer
        ServerRefused,
    };
    Q_ENUM(Failure)

    // No PLAYING this long after a load is a refusal of the stream
    static constexpr int StartTimeoutMs = 30'000;
    // The token-less range probe of a direct play
    static constexpr int ProbeTimeoutMs = 5000;
    // SetAVTransportURI and Play: many renderers fetch the stream before they answer
    static constexpr int LoadTimeoutMs = 15'000;

    explicit DlnaRenderer(RendererInfo info, QObject *parent = nullptr);

    const RendererInfo &info() const { return m_info; }
    bool hasVolume() const { return m_info.hasVolume(); }
    bool canPause() const { return m_info.canPause(); }
    // A direct play seeks with REL_TIME; a transcode seeks by loading its URL at another start
    bool canSeek() const { return !m_direct || m_info.canSeekRelTime(); }
    bool direct() const { return m_direct; }

    // Loads and plays `url`, an http or https URL under `serverBase`: a direct play first checks with one token-less range request that the server lets the renderer in, then seeks to `startMs` when the renderer lists REL_TIME; a transcode URL already starts at `startMs`, and its positions count from there. The transport state comes through stateChanged
    bool load(const QUrl &url, const QString &mime, const QString &title, qint64 startMs,
              qint64 durationMs, const QUrl &serverBase, bool direct);
    // The same transcode again from `url`, which starts at `startMs`: how a transcode seeks
    bool reload(const QUrl &url, qint64 startMs);
    bool play();
    bool pause();
    // A direct play only: a transcode seeks by loading the same URL at another start
    bool seek(qint64 positionMs);
    bool stop();
    bool setVolume(int volume);
    bool setMuted(bool muted);
    bool refreshVolume();
    // The item's runtime once the server named it: the renderer's own duration of a transcode is not trusted
    void setDurationMs(qint64 durationMs);

    void startPolling();
    void stopPolling();
    bool polling() const { return m_pollTimer.isActive(); }
    qint64 positionMs() const { return m_positionMs; }
    qint64 durationMs() const { return m_durationMs; }
    std::optional<Transport> transport() const { return m_reportedTransport; }
    // -1 until the renderer answered GetVolume
    int volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    QUrl url() const { return m_url; }

  Q_SIGNALS:
    void stateChanged(Kinefin::DlnaRenderer::Transport state, qint64 positionMs, qint64 durationMs);
    void positionChanged(qint64 positionMs, qint64 durationMs);
    void volumeChanged(int volume, bool muted);
    // The renderer answered STOPPED at the end of the item
    void ended();
    // The renderer stopped on its own, or another control point took its track
    void stopped();
    void failed(Kinefin::DlnaRenderer::Failure failure);

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
    bool isOwnTrack(const QString &track) const;
    void abortProbe();
    void fail(Failure failure);

    RendererInfo m_info;
    QNetworkAccessManager m_network;
    QQueue<Command> m_commands;
    QQueue<Command> m_polls;
    std::optional<Command> m_current;
    QNetworkReply *m_reply = nullptr;
    QPointer<QNetworkReply> m_probe;
    QByteArray m_response;
    int m_responseLimit = kMaxSoapResponseBytes;
    QTimer m_timeout;
    QTimer m_pollTimer;
    QTimer m_startTimer;
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
