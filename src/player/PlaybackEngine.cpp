// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlaybackEngine.h"

#include <cmath>

#include <MpvController>

#include "api/JellyfinClient.h"

Q_LOGGING_CATEGORY(playerLog, "kinefin.player.engine")

namespace Kinefin {

// One tick is 100 ns (server time base, parity §9); mpv reports seconds.
static qint64 secondsToTicks(double seconds)
{
    return seconds <= 0.0 ? 0 : static_cast<qint64>(std::llround(seconds * 1e7));
}

PlaybackEngine::PlaybackEngine(JellyfinClient &client, PlaybackSession &session, QObject *parent)
    : QObject(parent), m_client(client), m_session(session)
{
}

PlaybackEngine::~PlaybackEngine() = default;

bool PlaybackEngine::init(const QStringList &options)
{
    if (m_controller != nullptr) {
        return false;
    }
    m_controller = new MpvController(this);
    connect(m_controller, &MpvController::fileLoaded, this, [this] {
        qCDebug(playerLog) << "file loaded";
        m_session.opened();
    });
    connect(m_controller, &MpvController::endFile, this, &PlaybackEngine::onEndFile);
    connect(m_controller, &MpvController::propertyChanged, this,
            &PlaybackEngine::onPropertyChanged);
    m_controller->init();

    // MpvQt pins vo=libmpv for its QML item; an option from the caller replaces it
    // (later wins), so the offscreen container gets vo=null/ao=null and P5 attaches
    // the real window. A bounded network timeout keeps a failed open from hanging
    // into the injection tests (INT-02).
    m_controller->setProperty(QStringLiteral("network-timeout"), 15);
    for (const QString &option : options) {
        const qsizetype equals = option.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            qCWarning(playerLog) << "ignoring malformed mpv option" << option;
            continue;
        }
        const QString name = option.left(equals);
        const QString value = option.mid(equals + 1);
        if (const int error = m_controller->setProperty(name, value); error < 0) {
            qCWarning(playerLog) << "mpv option" << name << "was refused:" << error;
        }
    }

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
        m_controller->observeProperty(QString::fromLatin1(entry.name), entry.format, entry.id);
    }
    return true;
}

void PlaybackEngine::applyStreamAuthorization()
{
    if (m_controller == nullptr) {
        return;
    }
    // One mpv list entry with the full authorization value. The MediaBrowser header
    // is full of the characters mpv's option list parser treats specially (commas,
    // quotes): mpv's API string-form set slices the value on every comma (verified
    // against libmpv 0.41), so the property must be set through a node array of one
    // entry instead, which stores the whole value uninterpreted. The token lives
    // only inside this property; neither the URL nor any log line sees it.
    const QByteArray authorization = m_client.streamAuthorization();
    if (authorization.isEmpty()) {
        // Never leave a stale header behind when the identity went away.
        m_controller->setProperty(QStringLiteral("http-header-fields"), QVariantList{});
        return;
    }
    const QByteArray entry = QByteArrayLiteral("Authorization: ") + authorization;
    m_controller->setProperty(QStringLiteral("http-header-fields"),
                              QVariantList{QString::fromUtf8(entry)});
    // The exact value is never logged (project note: no tokens in URLs, signals or logs).
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
    applyStreamAuthorization();
    qCDebug(playerLog) << "opening"
                       << (route == PlaybackSession::Route::Transcoded ? "a transcoded stream"
                                                                       : "a direct stream");
    const QVariant load = m_controller->command(
        {QStringLiteral("loadfile"), source.url.toString(QUrl::FullyEncoded)});
    if (load.canConvert<ErrorReturn>()) {
        // mpv refused the command outright: no stream ever started, so the session
        // drops the phantom server session without a report (plan P2x gate).
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
    m_controller->setProperty(QStringLiteral("pause"), true);
}

void PlaybackEngine::resume()
{
    if (m_controller == nullptr || !m_session.resume()) {
        return;
    }
    m_controller->setProperty(QStringLiteral("pause"), false);
}

void PlaybackEngine::stop()
{
    if (m_controller == nullptr) {
        return;
    }
    switch (m_session.state()) {
    case PlaybackSession::State::Opening: {
        // Stopping during the load: mpv answers by going idle WITHOUT an end-file
        // event while the file never completed its load (verified against libmpv
        // 0.41), so the session settles here instead of waiting for one. If the
        // load had already gone through, the follow-up end-file("stop") lands in
        // an Idle session and is ignored.
        m_session.stop();
        m_session.stopped();
        m_controller->command({QStringLiteral("stop")});
        break;
    }
    case PlaybackSession::State::Playing:
    case PlaybackSession::State::Paused:
    case PlaybackSession::State::Buffering: {
        // The file is loaded: mpv answers the stop with end-file("stop") and the
        // session settles there.
        m_session.stop();
        const QVariant stopped = m_controller->command({QStringLiteral("stop")});
        if (stopped.canConvert<ErrorReturn>()) {
            qCWarning(playerLog) << "mpv refused the stop command:"
                                 << stopped.value<ErrorReturn>().error;
        }
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
        m_controller->setProperty(QStringLiteral("speed"), rate);
    }
}

void PlaybackEngine::seekTo(qint64 ticks)
{
    if (m_controller == nullptr || ticks < 0) {
        return;
    }
    const double seconds = static_cast<double>(ticks) / 1e7;
    m_controller->command(
        {QStringLiteral("seek"), QString::number(seconds, 'f', 3), QStringLiteral("absolute")});
}

void PlaybackEngine::settleFailure()
{
    if (m_session.state() != PlaybackSession::State::Failed) {
        return;
    }
    // The best-effort remote stop for the actually started stream is the reporter's
    // own doing on the Stopping transition; an opening failure reports nothing.
    m_session.stop();
    m_session.stopped();
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
        // Cache stalls only mean Buffering around an actually Playing session: the
        // state machine refuses every other path silently.
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
        // A natural end and a real failure are different reports: the item's end is
        // the ordinary one-Stop path (parity §9), a dead stream is failed().
        if (eofish) {
            m_session.stop();
        } else {
            m_session.failed();
        }
        break;
    case PlaybackSession::State::Opening:
        if (reason == QLatin1String("stop")) {
            // Our own stop() drive reached mpv before the stream ever produced a
            // fileLoaded: an ordinary teardown, not a failure (no server session
            // was ever opened).
            m_session.stop();
        } else {
            // The load never produced a stream: an opening failure, which the gate
            // says must not reach the server as a stop report.
            m_session.failed();
        }
        break;
    case PlaybackSession::State::Stopping:
        // A stop() drive was handed to mpv before this; the file is actually gone.
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
    Q_EMIT streamEnded(reason);
}
} // namespace Kinefin
