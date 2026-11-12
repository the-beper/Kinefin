// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlaybackEngine.h"

#include <cmath>

#include <QTimer>

#include <MpvAbstractItem>
#include <MpvController>

#include "api/JellyfinClient.h"

Q_LOGGING_CATEGORY(playerLog, "kinefin.player.engine")

namespace Kinefin {

// One tick is 100 ns (server time base, parity §9); mpv reports seconds
static qint64 secondsToTicks(double seconds)
{
    return seconds <= 0.0 ? 0 : static_cast<qint64>(std::llround(seconds * 1e7));
}

// An eof within this distance of the duration is the item's end: the last time-pos may fall a few ms short, while a dropped connection also ends in eof far from the end and must keep its position
constexpr qint64 kEofSnapTicks = 5 * 10'000'000;
// How long a stop() waits for mpv's end-file before the session settles without it
constexpr int kStopWatchdogMs = 3000;

PlaybackEngine::PlaybackEngine(JellyfinClient &client, PlaybackSession &session, QObject *parent)
    : QObject(parent), m_client(client), m_session(session)
{
    m_stopWatchdog.setSingleShot(true);
    m_stopWatchdog.setInterval(kStopWatchdogMs);
    // The stream's Stopped report went out on the transition to Stopping: settling here only releases the session when mpv never answers (a refused stop, a dropped end-file reason, mpv gone)
    connect(&m_stopWatchdog, &QTimer::timeout, this, [this] {
        if (m_session.state() == PlaybackSession::State::Stopping) {
            qCWarning(playerLog) << "mpv never ended the stopped file, settling the session";
            m_session.stopped();
        }
    });
}

PlaybackEngine::~PlaybackEngine() = default;

bool PlaybackEngine::init(const QStringList &options)
{
    if (m_controller != nullptr) {
        return false;
    }
    m_controller = new MpvController(this);
    m_standalone = true;
    m_controller->init();
    wireController();
    // Standalone playback is used by the headless engine integration tests.
    setProperty(QStringLiteral("network-timeout"), 15);
    for (const QString &option : options) {
        const qsizetype equals = option.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            qCWarning(playerLog) << "ignoring malformed mpv option" << option;
            continue;
        }
        const QString name = option.left(equals);
        const QString value = option.mid(equals + 1);
        setProperty(name, value);
    }
    return true;
}

bool PlaybackEngine::attach(MpvAbstractItem *item, MpvController *controller)
{
    if (!item || !controller || m_controller || m_session.state() != PlaybackSession::State::Idle)
        return false;
    m_renderer = item;
    m_controller = controller;
    wireController();
    setProperty(QStringLiteral("network-timeout"), 15);
    return true;
}

void PlaybackEngine::detach(MpvAbstractItem *item)
{
    if (!item || item != m_renderer)
        return;
    // The renderer's controller dies on its worker thread when the page closes.
    // Report Stopped before it does and do not keep its raw pointer afterwards.
    if (m_session.state() != PlaybackSession::State::Idle) {
        m_session.stop();
        m_session.stopped();
    }
    m_stopWatchdog.stop();
    disconnect(m_controller, nullptr, this, nullptr);
    m_controller = nullptr;
    m_renderer = nullptr;
}

void PlaybackEngine::setProperty(const QString &name, const QVariant &value)
{
    if (!m_controller)
        return;
    if (m_renderer)
        m_renderer->setProperty(name, value);
    else
        m_controller->setProperty(name, value);
}

void PlaybackEngine::setMpvProperty(const QString &name, const QVariant &value)
{
    // Names are supplied exclusively by the local settings map and player UI,
    // not by the server or the external deep-link parser.
    setProperty(name, value);
}

QVariant PlaybackEngine::mpvProperty(const QString &name) const
{
    if (!m_controller)
        return {};
    return m_renderer ? m_renderer->getProperty(name) : m_controller->getProperty(name);
}

QVariant PlaybackEngine::command(const QStringList &arguments)
{
    if (!m_controller)
        return {};
    return m_renderer ? m_renderer->commandBlocking(arguments) : m_controller->command(arguments);
}

bool PlaybackEngine::mpvCommand(const QStringList &arguments)
{
    return m_controller && !command(arguments).canConvert<ErrorReturn>();
}

void PlaybackEngine::wireController()
{
    connect(m_controller, &MpvController::fileStarted, this, &PlaybackEngine::onFileStarted);
    connect(m_controller, &MpvController::fileLoaded, this, [this] {
        qCDebug(playerLog) << "file loaded";
        m_session.opened();
        Q_EMIT fileLoaded();
    });
    connect(m_controller, &MpvController::endFile, this, &PlaybackEngine::onEndFile);
    connect(m_controller, &MpvController::propertyChanged, this,
            &PlaybackEngine::onPropertyChanged);
    struct Observed {
        const char *name = nullptr;
        mpv_format format = MPV_FORMAT_NONE;
        quint64 id = 0;
    };
    static const Observed observed[] = {
        {"time-pos", MPV_FORMAT_DOUBLE, 1}, {"duration", MPV_FORMAT_DOUBLE, 2},
        {"seekable", MPV_FORMAT_FLAG, 3},   {"paused-for-cache", MPV_FORMAT_FLAG, 4},
        {"speed", MPV_FORMAT_DOUBLE, 5},
    };
    for (const Observed &entry : observed) {
        if (m_renderer)
            m_renderer->observeProperty(QString::fromLatin1(entry.name), entry.format, entry.id);
        else
            m_controller->observeProperty(QString::fromLatin1(entry.name), entry.format, entry.id);
    }
    // This function wires both the standalone controller and the video renderer.
}

void PlaybackEngine::applyStreamAuthorization()
{
    if (m_controller == nullptr) {
        return;
    }
    // One mpv list entry with the full authorization value. The MediaBrowser header is full of the characters mpv's option list parser treats specially (commas, quotes): mpv's API string-form set slices the value on every comma (verified against libmpv 0.41), so the property must be set through a node array of one entry instead, which stores the whole value uninterpreted. The token lives only inside this property; neither the URL nor any log line sees it
    const QByteArray authorization = m_client.streamAuthorization();
    if (authorization.isEmpty()) {
        // Never leave a stale header behind when the identity went away
        setProperty(QStringLiteral("http-header-fields"), QVariantList{});
        return;
    }
    const QByteArray entry = QByteArrayLiteral("Authorization: ") + authorization;
    setProperty(QStringLiteral("http-header-fields"), QVariantList{QString::fromUtf8(entry)});
    // The exact value is never logged (project note: no tokens in URLs, signals or logs)
}

bool PlaybackEngine::open(const PlaybackSource &source)
{
    if (m_controller == nullptr || m_session.state() != PlaybackSession::State::Idle) {
        return false;
    }
    if (source.itemId.isEmpty() || source.playSessionId.isEmpty() || source.url.isEmpty()) {
        return false;
    }
    PlaybackSession::Identity identity;
    identity.itemId = source.itemId.toStdString();
    identity.mediaSourceId = source.mediaSourceId.toStdString();
    identity.playSessionId = source.playSessionId.toStdString();
    const PlaybackSession::Route route = source.method == PlaybackSource::Method::Transcode
                                             ? PlaybackSession::Route::Transcoded
                                             : PlaybackSession::Route::Direct;
    if (!m_session.open(std::move(identity), route)) {
        return false;
    }
    m_positionTicks = 0;
    m_positionKnown = false;
    m_loadStarted = false;
    m_stopPending = false;
    m_stopWatchdog.stop();
    if (m_durationTicks != 0) {
        m_durationTicks = 0;
        Q_EMIT durationChanged(m_durationTicks);
    }
    if (m_canSeek) {
        m_canSeek = false;
        Q_EMIT canSeekChanged(m_canSeek);
    }
    applyStreamAuthorization();
    qCDebug(playerLog) << "opening"
                       << (route == PlaybackSession::Route::Transcoded ? "a transcoded stream"
                                                                       : "a direct stream");
    const QVariant load =
        command({QStringLiteral("loadfile"), source.url.toString(QUrl::FullyEncoded)});
    if (load.canConvert<ErrorReturn>()) {
        // mpv refused the command outright: no stream ever started, so the session drops the phantom server session without a report
        m_session.failed();
        settleFailure();
        return false;
    }
    return true;
}

void PlaybackEngine::pause()
{
    if (m_controller == nullptr || !m_session.pause()) {
        return;
    }
    setProperty(QStringLiteral("pause"), true);
}

void PlaybackEngine::resume()
{
    if (m_controller == nullptr || !m_session.resume()) {
        return;
    }
    setProperty(QStringLiteral("pause"), false);
}

void PlaybackEngine::stop()
{
    if (m_controller == nullptr) {
        return;
    }
    switch (m_session.state()) {
    case PlaybackSession::State::Opening:
        // A stop sent before mpv began the load is dropped with no end-file at all, while a started load always ends with one (observed against libmpv 0.41): the stop waits for the start-file, and the session stays Stopping until that end-file, so a next open() can never meet a stale event of this load
        m_session.stop();
        m_stopWatchdog.start();
        if (m_loadStarted) {
            sendStop();
        } else {
            m_stopPending = true;
        }
        break;
    case PlaybackSession::State::Playing:
    case PlaybackSession::State::Paused:
    case PlaybackSession::State::Buffering: {
        // The file is loaded: mpv answers the stop with end-file("stop") and the session settles there
        m_session.stop();
        m_stopWatchdog.start();
        sendStop();
        break;
    }
    case PlaybackSession::State::Idle:
    case PlaybackSession::State::Stopping:
    case PlaybackSession::State::Failed:
        break;
    }
}

void PlaybackEngine::setRate(qreal rate)
{
    if (m_controller != nullptr) {
        setProperty(QStringLiteral("speed"), rate);
    }
}

void PlaybackEngine::seekTo(qint64 ticks)
{
    if (m_controller == nullptr || ticks < 0) {
        return;
    }
    const double seconds = static_cast<double>(ticks) / 1e7;
    command({QStringLiteral("seek"), QString::number(seconds, 'f', 3), QStringLiteral("absolute")});
}

void PlaybackEngine::settleFailure()
{
    if (m_session.state() != PlaybackSession::State::Failed) {
        return;
    }
    // The best-effort remote stop for the actually started stream is the reporter's own doing on the Stopping transition; an opening failure reports nothing
    m_session.stop();
    m_session.stopped();
}

void PlaybackEngine::sendStop()
{
    const QVariant stopped = command({QStringLiteral("stop")});
    if (stopped.canConvert<ErrorReturn>()) {
        qCWarning(playerLog) << "mpv refused the stop command:"
                             << stopped.value<ErrorReturn>().error;
    }
}

void PlaybackEngine::onFileStarted()
{
    m_loadStarted = true;
    if (m_stopPending) {
        m_stopPending = false;
        sendStop();
    }
}

void PlaybackEngine::onPropertyChanged(const QString &property, const QVariant &value)
{
    const bool isDouble = value.metaType() == QMetaType::fromType<double>();
    const bool isFlag = value.metaType() == QMetaType::fromType<bool>();
    if (property == QLatin1String("time-pos")) {
        if (!isDouble) {
            return; // absent past a file end or before a file starts
        }
        m_positionTicks = secondsToTicks(value.toDouble());
        m_positionKnown = true;
        Q_EMIT positionChanged(m_positionTicks);
    } else if (property == QLatin1String("duration")) {
        if (isDouble) {
            m_durationTicks = secondsToTicks(value.toDouble());
            Q_EMIT durationChanged(m_durationTicks);
        }
    } else if (property == QLatin1String("speed")) {
        if (isDouble) {
            m_rate = value.toDouble();
            Q_EMIT rateChanged(m_rate);
        }
    } else if (property == QLatin1String("seekable")) {
        if (isFlag) {
            m_canSeek = value.toBool();
            Q_EMIT canSeekChanged(m_canSeek);
        }
    } else if (property == QLatin1String("paused-for-cache")) {
        // Cache stalls only mean Buffering around an actually Playing session: the state machine refuses every other path silently
        if (isFlag && value.toBool()) {
            m_session.buffering();
        } else if (isFlag) {
            m_session.bufferRecovered();
        }
    }
}

void PlaybackEngine::onEndFile(const QString &reason)
{
    qCDebug(playerLog) << "end file:" << reason;
    const bool eofish = reason == QLatin1String("eof");
    switch (m_session.state()) {
    case PlaybackSession::State::Playing:
    case PlaybackSession::State::Paused:
    case PlaybackSession::State::Buffering:
        // Only an error is a failure: a natural end and a stop not driven by stop() (an external mpv command) are the ordinary one-Stop path (parity §9)
        if (reason == QLatin1String("error")) {
            m_session.failed();
            break;
        }
        if (eofish && m_positionKnown && m_durationTicks - m_positionTicks <= kEofSnapTicks) {
            m_positionTicks = m_durationTicks;
            Q_EMIT positionChanged(m_positionTicks);
        }
        m_session.stop();
        break;
    case PlaybackSession::State::Opening:
        if (reason == QLatin1String("stop")) {
            // A stop not driven by stop() reached mpv before the stream produced a fileLoaded: an ordinary teardown, not a failure (no server session was ever opened)
            m_session.stop();
        } else {
            // The load never produced a stream: an opening failure, which the gate says must not reach the server as a stop report
            m_session.failed();
        }
        break;
    case PlaybackSession::State::Stopping:
        // A stop() drive was handed to mpv before this; the file is actually gone
        m_session.stopped();
        break;
    case PlaybackSession::State::Failed:
    case PlaybackSession::State::Idle:
        break;
    }
    if (m_session.state() == PlaybackSession::State::Stopping) {
        m_session.stopped();
    } else {
        settleFailure();
    }
    m_stopWatchdog.stop();
    Q_EMIT streamEnded(reason);
}
} // namespace Kinefin
