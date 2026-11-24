// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/DlnaRenderer.h"

#include "cast/Didl.h"

#include <QLoggingCategory>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>

Q_LOGGING_CATEGORY(rendererLog, "kinefin.cast.renderer")

namespace Kinefin {
namespace {
constexpr int kPollIntervalMs = 1000;
constexpr int kPollFailuresBeforeFailed = 3;
constexpr qint64 kEndWindowMs = 10'000;
constexpr qint64 kMaxDurationMs = 24LL * 3600 * 1000;
// How far before its wanted start the first position of a direct play may be before it is sought again
constexpr qint64 kStartSlackMs = 2'000;
constexpr qint64 kStallProgressMs = 500;

DlnaRenderer::Transport transportFrom(const QString &state)
{
    const auto is = [&state](QLatin1StringView name) {
        return state.compare(name, Qt::CaseInsensitive) == 0;
    };
    if (is(QLatin1StringView("PLAYING")))
        return DlnaRenderer::Transport::Playing;
    if (is(QLatin1StringView("PAUSED_PLAYBACK")) || is(QLatin1StringView("PAUSED_RECORDING")))
        return DlnaRenderer::Transport::Paused;
    if (is(QLatin1StringView("STOPPED")))
        return DlnaRenderer::Transport::Stopped;
    if (is(QLatin1StringView("NO_MEDIA_PRESENT")))
        return DlnaRenderer::Transport::NoMedia;
    // A state this code does not know (vendor values) is not an end: a Stopped would close a started session
    return DlnaRenderer::Transport::Transitioning;
}

// No cookie is sent or kept, whatever a renderer or the server sets
void withoutCookies(QNetworkRequest &request)
{
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
}

} // namespace

DlnaRenderer::DlnaRenderer(RendererInfo info, QObject *parent)
    : CastRenderer(parent), m_info(std::move(info))
{
    m_network.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    connect(&m_probe, &StreamProbe::finished, this, &DlnaRenderer::onProbeFinished);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(kSoapTimeoutMs);
    connect(&m_timeout, &QTimer::timeout, this, &DlnaRenderer::onTimeout);
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &DlnaRenderer::poll);
    m_startTimer.setSingleShot(true);
    m_startTimer.setInterval(StartTimeoutMs);
    connect(&m_startTimer, &QTimer::timeout, this, [this] {
        qCWarning(rendererLog) << "no PLAYING in time from" << m_info.udn;
        fail(Failure::Refused);
    });
    m_stallTimer.setSingleShot(true);
    m_stallTimer.setInterval(StallTimeoutMs);
    connect(&m_stallTimer, &QTimer::timeout, this, [this] {
        qCWarning(rendererLog) << "no progress while PLAYING from" << m_info.udn;
        fail(Failure::Refused);
    });
}

DlnaRenderer::Command DlnaRenderer::avTransportCommand(const QString &action,
                                                       QList<SoapArgument> arguments) const
{
    Command command;
    command.service = m_info.avTransport;
    command.action = action;
    command.arguments = {{QStringLiteral("InstanceID"), QStringLiteral("0")}};
    command.arguments.append(std::move(arguments));
    command.generation = m_generation;
    return command;
}

bool DlnaRenderer::load(const QUrl &url, const QString &mime, const QString &title, qint64 startMs,
                        qint64 durationMs, const QUrl &serverBase, bool direct)
{
    const bool web =
        url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https");
    if (!url.isValid() || !web || !url.userInfo().isEmpty() || !isSameOrigin(url, serverBase)
        || startMs < 0 || startMs > kMaxUpnpHours * 3600 * 1000) {
        return false;
    }
    if (didlItem(title, url.toString(QUrl::FullyEncoded), mime).isEmpty())
        return false;
    ++m_generation;
    abortProbe();
    stopPolling();
    resetStall();
    m_commands.clear();
    m_url = url;
    m_serverBase = serverBase;
    m_mime = mime;
    m_title = title;
    m_startMs = startMs;
    m_durationMs = std::clamp<qint64>(durationMs, 0, kMaxDurationMs);
    m_positionMs = startMs;
    m_direct = direct;
    m_stopping = false;
    m_started = false;
    m_startChecked = false;
    m_pollFailures = 0;
    m_reportedTransport.reset();
    m_startTimer.start();
    if (direct)
        probe();
    else
        startTransport();
    return true;
}

void DlnaRenderer::probe()
{
    // The renderer fetches the URL with no credential: the same request without one tells whether the server lets it in, before anything reaches the renderer
    m_probe.start(m_url);
}

void DlnaRenderer::onProbeFinished(int status)
{
    if (status != 206) {
        qCWarning(rendererLog) << "the stream answered" << status << "without credentials for"
                               << m_info.udn;
        // A sign-in asked for is the server's rule; anything else is a stream that may still play another way
        fail(status == 401 || status == 403 ? Failure::ServerRefused : Failure::Refused);
        return;
    }
    startTransport();
}

void DlnaRenderer::abortProbe()
{
    m_probe.abort();
}

void DlnaRenderer::startTransport()
{
    // Never empty metadata: a Fire TV renderer refuses SetAVTransportURI without a DIDL item with HTTP 500
    const QString metadata = didlItem(m_title, m_url.toString(QUrl::FullyEncoded), m_mime);
    Command setUri =
        avTransportCommand(QStringLiteral("SetAVTransportURI"),
                           {{QStringLiteral("CurrentURI"), m_url.toString(QUrl::FullyEncoded)},
                            {QStringLiteral("CurrentURIMetaData"), metadata}});
    setUri.fatal = true;
    setUri.timeoutMs = LoadTimeoutMs;
    enqueue(setUri);
    if (m_direct && m_startMs > 0 && m_info.canSeekRelTime()) {
        enqueue(avTransportCommand(QStringLiteral("Seek"),
                                   {{QStringLiteral("Unit"), QStringLiteral("REL_TIME")},
                                    {QStringLiteral("Target"), upnpTime(m_startMs)}}));
    }
    Command play = avTransportCommand(QStringLiteral("Play"),
                                      {{QStringLiteral("Speed"), QStringLiteral("1")}});
    play.fatal = true;
    play.timeoutMs = LoadTimeoutMs;
    enqueue(play);
    startPolling();
    // The OSD shows the renderer's volume from the start, not after the first periodic refresh
    refreshVolume();
}

bool DlnaRenderer::reload(const QUrl &url, qint64 startMs, const QUrl &)
{
    if (m_direct || m_url.isEmpty())
        return false;
    return load(url, m_mime, m_title, startMs, m_durationMs, m_serverBase, false);
}

bool DlnaRenderer::play()
{
    enqueue(avTransportCommand(QStringLiteral("Play"),
                               {{QStringLiteral("Speed"), QStringLiteral("1")}}));
    return true;
}

bool DlnaRenderer::pause()
{
    if (!canPause())
        return false;
    enqueue(avTransportCommand(QStringLiteral("Pause")));
    return true;
}

bool DlnaRenderer::seek(qint64 positionMs)
{
    if (!m_direct || !m_info.canSeekRelTime() || positionMs < 0)
        return false;
    if (m_durationMs > 0)
        positionMs = std::min(positionMs, m_durationMs);
    m_positionMs = positionMs;
    Command seek = avTransportCommand(QStringLiteral("Seek"),
                                      {{QStringLiteral("Unit"), QStringLiteral("REL_TIME")},
                                       {QStringLiteral("Target"), upnpTime(positionMs)}});
    seek.done = [this](const SoapResult &result) {
        if (result.status == SoapStatus::Ok)
            Q_EMIT positionChanged(m_positionMs, m_durationMs);
    };
    enqueue(seek);
    Q_EMIT positionChanged(m_positionMs, m_durationMs);
    return true;
}

bool DlnaRenderer::stop()
{
    if (!m_info.avTransport.isValid())
        return false;
    ++m_generation;
    abortProbe();
    m_startTimer.stop();
    resetStall();
    m_stopping = true;
    stopPolling();
    m_commands.clear();
    Command stop = avTransportCommand(QStringLiteral("Stop"));
    // A device that refuses the Stop is still no longer driven from here
    stop.done = [this](const SoapResult &) {
        m_stopping = false;
        Q_EMIT stopped();
    };
    enqueue(stop);
    return true;
}

bool DlnaRenderer::setVolume(int volume)
{
    if (!hasVolume() || volume < 0 || volume > 100)
        return false;
    m_volume = volume;
    Command set;
    set.service = m_info.renderingControl;
    set.action = QStringLiteral("SetVolume");
    set.arguments = {{QStringLiteral("InstanceID"), QStringLiteral("0")},
                     {QStringLiteral("Channel"), QStringLiteral("Master")},
                     {QStringLiteral("DesiredVolume"), QString::number(volume)}};
    set.generation = m_generation;
    enqueue(set);
    Q_EMIT volumeChanged(m_volume, m_muted);
    return true;
}

bool DlnaRenderer::setMuted(bool muted)
{
    if (!hasVolume())
        return false;
    m_muted = muted;
    Command set;
    set.service = m_info.renderingControl;
    set.action = QStringLiteral("SetMute");
    set.arguments = {
        {QStringLiteral("InstanceID"), QStringLiteral("0")},
        {QStringLiteral("Channel"), QStringLiteral("Master")},
        {QStringLiteral("DesiredMute"), muted ? QStringLiteral("1") : QStringLiteral("0")}};
    set.generation = m_generation;
    enqueue(set);
    Q_EMIT volumeChanged(m_volume, m_muted);
    return true;
}

bool DlnaRenderer::refreshVolume()
{
    if (!hasVolume())
        return false;
    Command getVolume;
    getVolume.service = m_info.renderingControl;
    getVolume.action = QStringLiteral("GetVolume");
    getVolume.arguments = {{QStringLiteral("InstanceID"), QStringLiteral("0")},
                           {QStringLiteral("Channel"), QStringLiteral("Master")}};
    getVolume.expected = {QStringLiteral("CurrentVolume")};
    getVolume.done = [this](const SoapResult &result) { handleVolume(result); };
    getVolume.poll = true;
    getVolume.generation = m_generation;
    enqueue(getVolume);
    Command getMute = getVolume;
    getMute.action = QStringLiteral("GetMute");
    getMute.expected = {QStringLiteral("CurrentMute")};
    enqueue(getMute);
    return true;
}

void DlnaRenderer::setDurationMs(qint64 durationMs)
{
    if (durationMs <= 0 || durationMs > kMaxDurationMs || durationMs == m_durationMs)
        return;
    m_durationMs = durationMs;
    m_positionMs = std::min(m_positionMs, m_durationMs);
    Q_EMIT positionChanged(m_positionMs, m_durationMs);
}

void DlnaRenderer::startPolling()
{
    if (!m_pollTimer.isActive())
        m_pollTimer.start();
}

void DlnaRenderer::stopPolling()
{
    m_pollTimer.stop();
    m_polls.clear();
}

void DlnaRenderer::enqueue(Command command)
{
    if (command.poll)
        m_polls.enqueue(std::move(command));
    else
        m_commands.enqueue(std::move(command));
    sendNext();
}

void DlnaRenderer::sendNext()
{
    if (m_current || m_reply)
        return;
    if (m_commands.isEmpty() && m_polls.isEmpty())
        return;
    Command next = m_commands.isEmpty() ? m_polls.dequeue() : m_commands.dequeue();
    m_current = std::move(next);
    send(*m_current);
}

void DlnaRenderer::send(const Command &command)
{
    const QByteArray body =
        soapRequestBody(command.service.serviceType, command.action, command.arguments);
    const QByteArray actionHeader = soapActionHeader(command.service.serviceType, command.action);
    if (body.isEmpty() || actionHeader.isEmpty() || !command.service.controlUrl.isValid()) {
        finish(SoapResult{});
        return;
    }
    QNetworkRequest request(command.service.controlUrl);
    withoutCookies(request);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(kSoapContentType));
    request.setRawHeader("SOAPACTION", actionHeader);
    request.setTransferTimeout(command.timeoutMs);
    m_response.clear();
    m_responseLimit = soapResponseLimit(command.action);
    m_reply = m_network.post(request, body);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (!m_reply)
            return;
        m_response.append(m_reply->readAll());
        if (m_response.size() > m_responseLimit) {
            qCWarning(rendererLog) << "response past the limit from" << m_info.udn;
            m_reply->abort();
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, &DlnaRenderer::onFinished);
    m_timeout.start(command.timeoutMs);
}

void DlnaRenderer::onFinished()
{
    if (!m_reply || !m_current)
        return;
    m_response.append(m_reply->readAll());
    const bool tooLarge = m_response.size() > m_responseLimit;
    m_reply->deleteLater();
    m_reply = nullptr;
    m_timeout.stop();
    if (tooLarge) {
        finish(SoapResult{});
        return;
    }
    const Command command = *m_current;
    finish(parseSoapResponse(m_response, command.action, command.expected));
}

void DlnaRenderer::onTimeout()
{
    if (!m_reply)
        return;
    qCWarning(rendererLog) << "timeout from" << m_info.udn;
    m_reply->abort();
}

void DlnaRenderer::finish(const SoapResult &result)
{
    if (!m_current)
        return;
    const Command command = std::move(*m_current);
    m_current.reset();
    m_response.clear();
    QTimer::singleShot(0, this, &DlnaRenderer::sendNext);
    // An answer for a load or stop that has been replaced since
    if (command.generation != m_generation)
        return;
    if (result.status != SoapStatus::Ok && !command.poll) {
        qCWarning(rendererLog) << command.action << "failed with" << int(result.status)
                               << result.errorCode << "from" << m_info.udn;
        // Only the load itself is fatal: a refused Pause, Seek or volume leaves the playback as the next poll finds it
        if (command.fatal) {
            fail(result.status == SoapStatus::Fault ? Failure::Refused : Failure::NotResponding);
            return;
        }
    }
    if (command.done)
        command.done(result);
}

void DlnaRenderer::poll()
{
    // A slow renderer gets the next poll once the last one answered, never a growing queue
    if (m_stopping || !m_polls.isEmpty() || (m_current && m_current->poll))
        return;
    ++m_pollCount;
    Command transport = avTransportCommand(QStringLiteral("GetTransportInfo"));
    transport.expected = {QStringLiteral("CurrentTransportState")};
    transport.done = [this](const SoapResult &result) { handleTransport(result); };
    transport.poll = true;
    enqueue(transport);
    Command position = avTransportCommand(QStringLiteral("GetPositionInfo"));
    position.expected = {QStringLiteral("RelTime"), QStringLiteral("TrackDuration"),
                         QStringLiteral("TrackURI")};
    position.done = [this](const SoapResult &result) { handlePosition(result); };
    position.poll = true;
    enqueue(position);
    if (hasVolume() && m_pollCount % 5 == 0)
        refreshVolume();
}

void DlnaRenderer::handleTransport(const SoapResult &result)
{
    if (result.status != SoapStatus::Ok) {
        if (++m_pollFailures >= kPollFailuresBeforeFailed)
            fail(Failure::NotResponding);
        return;
    }
    m_pollFailures = 0;
    updateState(transportFrom(result.values.value(QStringLiteral("CurrentTransportState"))));
}

void DlnaRenderer::handlePosition(const SoapResult &result)
{
    // The transport poll of the same round counts the failures: one per round
    if (result.status != SoapStatus::Ok)
        return;
    // The item's runtime wins; the renderer's own duration only stands in for a direct play whose runtime is unknown
    if (const auto duration = parseUpnpTime(result.values.value(QStringLiteral("TrackDuration")));
        m_direct && m_durationMs <= 0 && duration && *duration > 0 && *duration <= kMaxDurationMs)
        m_durationMs = *duration;
    // A transcode starts at its own start, and the renderer counts from there; until it plays, the start stands (a renderer still TRANSITIONING says 0, and the Start report would carry it)
    if (const auto position = parseUpnpTime(result.values.value(QStringLiteral("RelTime")));
        position && m_started) {
        watchStall(*position);
        qint64 value = std::max<qint64>(0, *position) + (m_direct ? 0 : m_startMs);
        if (m_durationMs > 0)
            value = std::min(value, m_durationMs);
        // Many renderers refuse a Seek before Play: the first position of a resumed direct play far before its start asks once more, and the start stands until the renderer moved, so no report takes the resume point back to zero
        if (m_direct && m_started && !m_startChecked) {
            m_startChecked = true;
            if (m_startMs >= kStartSlackMs && value < m_startMs - kStartSlackMs
                && m_info.canSeekRelTime()) {
                enqueue(avTransportCommand(QStringLiteral("Seek"),
                                           {{QStringLiteral("Unit"), QStringLiteral("REL_TIME")},
                                            {QStringLiteral("Target"), upnpTime(m_startMs)}}));
                value = m_startMs;
            }
        }
        m_positionMs = value;
    }
    if (m_started && !isOwnTrack(result.values.value(QStringLiteral("TrackURI")))) {
        qCInfo(rendererLog) << "another control point took" << m_info.udn;
        ++m_generation;
        stopPolling();
        m_startTimer.stop();
        resetStall();
        Q_EMIT stopped();
        return;
    }
    Q_EMIT positionChanged(m_positionMs, m_durationMs);
}

void DlnaRenderer::watchStall(qint64 rendererMs)
{
    // A transcode is a chunked stream without a length, on which renderers are known to keep RelTime at 0 while they play
    if (!m_direct || m_progressSeen || m_reportedTransport != Transport::Playing)
        return;
    if (!m_stallBaseMs) {
        m_stallBaseMs = rendererMs;
        m_stallTimer.start();
    } else if (qAbs(rendererMs - *m_stallBaseMs) >= kStallProgressMs) {
        m_progressSeen = true;
        m_stallTimer.stop();
    }
}

void DlnaRenderer::resetStall()
{
    m_stallTimer.stop();
    m_stallBaseMs.reset();
    m_progressSeen = false;
}

bool DlnaRenderer::isOwnTrack(const QString &track) const
{
    // Renderers echo the URI unescaped, re-escaped or not at all: the server, the item's path and the absence of another track are what count
    const QUrl other(track.trimmed());
    if (track.trimmed().isEmpty() || !other.isValid() || other.host().isEmpty())
        return true;
    return other.host().compare(m_url.host(), Qt::CaseInsensitive) == 0
           && other.port(-1) == m_url.port(-1)
           && other.path().compare(m_url.path(), Qt::CaseInsensitive) == 0;
}

void DlnaRenderer::handleVolume(const SoapResult &result)
{
    if (result.status != SoapStatus::Ok)
        return;
    if (const QString volume = result.values.value(QStringLiteral("CurrentVolume"));
        !volume.isEmpty()) {
        bool ok = false;
        const int value = volume.toInt(&ok);
        if (ok && value >= 0 && value <= 100)
            m_volume = value;
    }
    if (const QString mute = result.values.value(QStringLiteral("CurrentMute")); !mute.isEmpty())
        m_muted = mute == QLatin1String("1")
                  || mute.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
    Q_EMIT volumeChanged(m_volume, m_muted);
}

void DlnaRenderer::updateState(Transport state)
{
    if (m_reportedTransport && *m_reportedTransport == state)
        return;
    const bool wasTransitioning =
        m_reportedTransport && *m_reportedTransport == Transport::Transitioning;
    m_reportedTransport = state;
    if (state == Transport::Playing && m_stallBaseMs && !m_progressSeen)
        m_stallTimer.start();
    else
        m_stallTimer.stop();
    if (state == Transport::Playing || state == Transport::Paused) {
        m_started = true;
        m_startTimer.stop();
    }
    if (state == Transport::Stopped || state == Transport::NoMedia) {
        if (m_started) {
            ++m_generation;
            stopPolling();
            if (m_durationMs > 0 && m_positionMs >= m_durationMs - kEndWindowMs)
                Q_EMIT ended();
            else
                Q_EMIT stopped();
            return;
        }
        // A stop after the renderer tried and before it ever played: it could not play the stream. A plain STOPPED before it tried is the state it was left in, and the start timeout covers a renderer that never moves
        if (wasTransitioning) {
            fail(Failure::Refused);
            return;
        }
    }
    Q_EMIT stateChanged(state, m_positionMs, m_durationMs);
}

void DlnaRenderer::fail(Failure failure)
{
    ++m_generation;
    abortProbe();
    stopPolling();
    m_startTimer.stop();
    resetStall();
    m_commands.clear();
    m_current.reset();
    m_timeout.stop();
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    // A renderer that was given the stream may still start it late: it is asked to stop, and its answer is not waited for
    if (failure != Failure::ServerRefused && m_info.avTransport.isValid())
        enqueue(avTransportCommand(QStringLiteral("Stop")));
    Q_EMIT failed(failure);
}

} // namespace Kinefin
