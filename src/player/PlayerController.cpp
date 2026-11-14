// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/PlayerController.h"

#include "api/DeviceProfile.h"
#include "api/JellyfinClient.h"
#include "models/ItemSummary.h"
#include "models/PlayQueue.h"
#include "settings/ClientSettings.h"
#include "settings/ProfilePaths.h"

#include <KLocalizedString>
#include <MpvAbstractItem>
#include <MpvController>

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace Kinefin {
namespace {
constexpr qint64 TicksPerSecond = 10'000'000;
constexpr int MaxRememberedItems = 1000;
// The most episodes of one series the player lists to step through
constexpr int MaxSeriesEpisodes = 2000;
// A page of a series' episodes: a whole long series in one answer would pass the client's reply cap
constexpr int SeriesPageSize = 100;

// `S2:E6 — Weightless`: the same wording as the details page and the episode rows
QString episodeSubtitle(const ItemSummary &episode)
{
    QString label;
    if (episode.episodeNumber >= 0) {
        const bool range = episode.episodeNumberEnd > episode.episodeNumber;
        if (episode.seasonNumber >= 0)
            label = range ? i18nc("season and episode range", "S%1:E%2–E%3", episode.seasonNumber,
                                  episode.episodeNumber, episode.episodeNumberEnd)
                          : i18nc("season and episode number", "S%1:E%2", episode.seasonNumber,
                                  episode.episodeNumber);
        else
            label = range ? i18nc("episode number range", "E%1–E%2", episode.episodeNumber,
                                  episode.episodeNumberEnd)
                          : i18nc("episode number", "E%1", episode.episodeNumber);
    }
    if (label.isEmpty())
        return episode.name;
    return episode.name.isEmpty()
               ? label
               : i18nc("episode label and episode title", "%1 — %2", label, episode.name);
}
} // namespace

QString playbackTimeText(qint64 ticks, bool withHours)
{
    const qint64 seconds = std::max<qint64>(0, ticks) / TicksPerSecond;
    const auto part = [](qint64 value) {
        return QString::number(value).rightJustified(2, QLatin1Char('0'));
    };
    const QString minutes = part(withHours ? seconds / 60 % 60 : seconds / 60);
    const QString text = minutes + QLatin1Char(':') + part(seconds % 60);
    return withHours ? part(seconds / 3600) + QLatin1Char(':') + text : text;
}

PlayerController::PlayerController(JellyfinClient &client, PlayQueue &queue,
                                   ClientSettings &settings, const QString &profileId,
                                   QObject *parent)
    : QObject(parent), m_client(client), m_queue(queue), m_settings(settings),
      m_memoryFile(profileDataDirectory(profileId).isEmpty()
                       ? QString()
                       : profileDataDirectory(profileId) + QStringLiteral("/storage.json")),
      m_engine(client, m_session, this),
      m_reporter(
          m_session, client,
          [this] {
              PlaybackReporter::PlayerState state;
              state.positionTicks = m_engine.knownPositionTicks();
              state.canSeek = m_engine.canSeek();
              state.audioStreamIndex = m_selectedAudioIndex;
              state.subtitleStreamIndex = m_selectedSubtitleIndex;
              state.volume = m_volume;
              state.muted = m_muted;
              // The episodes of a series played alone are not a queue the server hears of
              if (!m_playlistEphemeral)
                  state.nowPlayingQueue = m_playlist;
              // The item id is the stable queue identity for this session. It is
              // valid even after a user reordered the saved queue in another view.
              state.playlistItemId =
                  m_queueIndex >= 0 && !m_playlistEphemeral ? m_itemId : QString();
              state.repeatMode = m_repeat;
              state.shuffle = m_shuffle;
              return state;
          },
          std::chrono::seconds(10), this),
      m_mpvSettings(settings, m_engine, this)
{
    m_profileDataDir = profileDataDirectory(profileId);
    m_observerId = m_session.addObserver(
        [this](PlaybackSession::State from, PlaybackSession::State to) { onState(from, to); });
    connect(&client, &JellyfinClient::playbackInfoReady, this, &PlayerController::onSource);
    connect(&client, &JellyfinClient::externalSubtitleDownloaded, this,
            [this](const QUrl &url, const QByteArray &bytes) {
                if (!active() || url != m_pendingSubtitle || !m_subtitleDir.isValid())
                    return;
                const QString extension = url.path().section(QLatin1Char('.'), -1).toLower();
                const QString filename = m_subtitleDir.path() + QStringLiteral("/subtitle-")
                                         + QString::number(QDateTime::currentMSecsSinceEpoch())
                                         + QLatin1Char('.') + extension;
                QSaveFile file(filename);
                if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
                    && file.commit()) {
                    m_engine.mpvCommand(
                        {QStringLiteral("sub-add"), filename, QStringLiteral("select")});
                    QTimer::singleShot(250, this, &PlayerController::refreshTracks);
                }
                m_pendingSubtitle.clear();
            });
    connect(&client, &JellyfinClient::externalSubtitleFailed, this, [this](const QUrl &url) {
        if (url == m_pendingSubtitle) {
            m_pendingSubtitle.clear();
            m_errorText = i18n("Could not download the subtitle.");
            Q_EMIT changed();
        }
    });
    connect(&client, &JellyfinClient::playbackItemReady, this, &PlayerController::onPlaybackItem);
    connect(&client, &JellyfinClient::episodesReady, this,
            [this](const QJsonArray &items, int total, int channel) {
                onSeriesEpisodes(items, total, channel);
            });
    // A page that fails ends the listing: what was read so far still serves
    connect(&client, &JellyfinClient::listingFailed, this,
            [this](const QString &, int, const QString &, int channel) {
                if (channel == EpisodesChannel && !m_seriesListItem.isEmpty())
                    finishSeriesPlaylist();
            });
    connect(&client, &JellyfinClient::trickplayTileFailed, this,
            [this](const QString &id, int width, int index) {
                if (id == m_itemId && m_trickplay && width == m_trickplay->width
                    && index == m_pendingTileIndex)
                    m_pendingTileIndex = -1;
            });
    connect(&client, &JellyfinClient::trickplayTileReady, this,
            [this](const QString &id, int width, int index, const QByteArray &jpeg) {
                if (!active() || id != m_itemId || !m_trickplay || width != m_trickplay->width
                    || index != m_pendingTileIndex || !m_requestedFrame
                    || m_requestedFrame->tileIndex != index)
                    return;
                QBuffer buffer;
                buffer.setData(jpeg);
                if (!buffer.open(QIODevice::ReadOnly))
                    return;
                QImageReader reader(&buffer, "jpeg");
                const QSize size = reader.size();
                if (!size.isValid() || qint64(size.width()) * size.height() > 12'000'000
                    || size.width() < m_trickplay->width * m_trickplay->columns
                    || size.height() < m_trickplay->height * m_trickplay->rows)
                    return;
                m_lastTile = reader.read();
                if (m_lastTile.isNull())
                    return;
                m_pendingTileIndex = -1;
                m_cachedTileIndex = index;
                displayTileFrame(m_lastTile, *m_requestedFrame);
            });
    connect(&m_queue, &PlayQueue::edited, this, &PlayerController::updatePlaylist);
    connect(&client, &JellyfinClient::mediaSegmentsReady, this,
            [this](const QString &itemId, const QJsonArray &segments) {
                if (itemId != m_itemId || !active())
                    return;
                m_segments.clear();
                for (const QJsonValue &segment : segments)
                    m_segments.append(segment.toObject().toVariantMap());
                updateSkip();
            });
    connect(&client, &JellyfinClient::playbackInfoFailed, this,
            [this](const QString &itemId, int, const QString &) {
                if (!m_loading || itemId != m_itemId)
                    return;
                m_loading = false;
                m_errorText = i18n("The video could not be opened. Try again.");
                Q_EMIT changed();
            });
    connect(&client, &JellyfinClient::sessionExpired, this, [this] { onAccountLeaving(); });
    connect(&client, &JellyfinClient::credentialsCleared, this, [this] { onAccountLeaving(); });
    connect(&m_engine, &PlaybackEngine::positionChanged, this, [this] {
        updateSkip();
        Q_EMIT changed();
    });
    connect(&m_engine, &PlaybackEngine::durationChanged, this, &PlayerController::changed);
    connect(&m_engine, &PlaybackEngine::canSeekChanged, this, &PlayerController::changed);
    connect(&m_engine, &PlaybackEngine::rateChanged, this, &PlayerController::changed);
    connect(&m_engine, &PlaybackEngine::fileLoaded, this, [this] {
        if (m_resumeTicks > 0) {
            const qint64 ticks = m_resumeTicks;
            m_resumeTicks = 0;
            m_engine.seekTo(ticks);
        }
        m_trackRestored = false;
        m_trackTimer.start();
        refreshTracks();
    });
    connect(&m_engine, &PlaybackEngine::streamEnded, this, &PlayerController::onEnd);
    m_sleep.setSingleShot(true);
    connect(&m_sleep, &QTimer::timeout, this, [this] { stop(); });
    m_clock.setInterval(30'000);
    connect(&m_clock, &QTimer::timeout, this, [this] {
        persistQueueCursor();
        Q_EMIT changed();
    });
    m_trackTimer.setInterval(2000);
    connect(&m_trackTimer, &QTimer::timeout, this, &PlayerController::refreshTracks);
    loadMemory();
}

PlayerController::~PlayerController()
{
    m_session.removeObserver(m_observerId);
    onAccountLeaving();
}

bool PlayerController::attachVideo(MpvAbstractItem *item, MpvController *controller)
{
    if (!item || m_video || !m_engine.attach(item, controller))
        return false;
    m_video = item;
    m_mpvSettings.apply();
    m_volume = qBound(0, m_engine.mpvProperty(QStringLiteral("volume")).toInt(), 100);
    m_muted = m_engine.mpvProperty(QStringLiteral("mute")).toBool();
    requestPending();
    return true;
}

void PlayerController::detachVideo(MpvAbstractItem *item)
{
    if (!item || item != m_video)
        return;
    // This is also the window-close path: the renderer must go before its
    // handle's worker thread. Stopped is queued once by the state observer.
    m_stopping = true;
    m_client.cancelPlaybackInfo();
    m_loading = false;
    m_trackTimer.stop();
    persistQueueCursor();
    m_queue.setCurrentPlaybackItemId({});
    m_engine.detach(item);
    m_video = nullptr;
    clearCurrent();
    Q_EMIT changed();
}

void PlayerController::onAccountLeaving()
{
    persistQueueCursor();
    m_stopping = true;
    m_client.cancelPlaybackInfo();
    m_loading = false;
    m_sleep.stop();
    m_clock.stop();
    if (m_engine.isInitialized())
        m_engine.stop();
    dropSeriesPlaylist();
    m_playlist.clear();
    m_queueIndex = -1;
    m_title.clear();
    m_subtitle.clear();
    m_queue.setCurrentPlaybackItemId({});
    m_pendingSubtitle.clear();
    clearPreview();
    Q_EMIT changed();
}

bool PlayerController::playItem(const QString &id, qint64 resumeTicks)
{
    if (!isValidJellyfinId(id) || resumeTicks < 0 || m_client.userId().isEmpty())
        return false;
    persistQueueCursor();
    m_playlist.clear();
    m_queueIndex = -1;
    m_playlistEphemeral = false;
    m_queue.setCurrentPlaybackItemId({});
    if (!startItem(id, resumeTicks))
        return false;
    // An episode started alone learns its series from the item reply and then lists the episodes to step through
    m_seriesPlaylistWanted = true;
    return true;
}

bool PlayerController::startItem(const QString &id, qint64 resumeTicks)
{
    if (!isValidJellyfinId(id) || resumeTicks < 0 || m_client.userId().isEmpty())
        return false;
    // Preserve the prior item's cursor before the new id replaces it.
    if (!m_itemId.isEmpty() && m_itemId != id)
        persistQueueCursor();
    // A different item cannot be opened until the old mpv end-file arrived.
    // The pending id survives that stop and is negotiated exactly once.
    m_client.cancelPlaybackInfo();
    cancelSeriesRequest();
    m_seriesPlaylistWanted = false;
    m_itemId = id;
    m_title.clear();
    m_subtitle.clear();
    m_resumeTicks = resumeTicks;
    m_errorText.clear();
    m_loading = true;
    m_stopping = false;
    m_skipLabel.clear();
    m_segments.clear();
    m_source = PlaybackSource{};
    m_externalSubtitles.clear();
    m_subtitleOptions.clear();
    m_pendingSubtitle.clear();
    m_trickplay.reset();
    m_trickplayInfo = {};
    m_requestedFrame.reset();
    m_lastTile = {};
    m_cachedTileIndex = -1;
    m_pendingTileIndex = -1;
    clearPreview();
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    m_selectedAudioIndex.reset();
    m_selectedSubtitleIndex.reset();
    m_selectedAudioId = 0;
    m_selectedSubtitleId = 0;
    Q_EMIT tracksChanged();
    // The title and the trickplay layout come from the item itself, asked alongside PlaybackInfo
    m_client.fetchPlaybackItem(id);
    if (m_session.state() != PlaybackSession::State::Idle)
        m_engine.stop();
    else
        requestPending();
    Q_EMIT changed();
    return true;
}

bool PlayerController::playQueue(int index)
{
    if (index < 0 || index >= m_queue.count())
        return false;
    dropSeriesPlaylist();
    m_playlist = m_queue.itemIds();
    m_queueIndex = index;
    if (!startItem(m_playlist.at(index), 0)) {
        m_playlist.clear();
        m_queueIndex = -1;
        return false;
    }
    m_queue.setCurrentPlaybackItemId(m_playlist.at(index));
    persistQueueCursor();
    return true;
}

bool PlayerController::resumeQueue()
{
    if (m_queue.count() == 0 || m_queue.busy())
        return false;
    const QStringList ids = m_queue.itemIds();
    const QString key =
        m_client.serverId().toLower() + QLatin1Char('/') + m_client.userId().toLower();
    const QJsonObject last = m_queueMemory.value(key).toObject();
    int index = ids.indexOf(last.value(QStringLiteral("itemId")).toString());
    qint64 ticks = 0;
    if (index >= 0) {
        ticks = last.value(QStringLiteral("positionTicks")).toVariant().toLongLong();
        if (ticks < 0 || ticks > 24LL * 3600 * TicksPerSecond)
            ticks = 0;
    } else {
        index = 0;
        for (int i = 0; i < m_queue.count(); ++i) {
            if (!m_queue.data(m_queue.index(i), PlayQueue::PlayedRole).toBool()) {
                index = i;
                break;
            }
        }
    }
    dropSeriesPlaylist();
    m_playlist = ids;
    m_queueIndex = index;
    if (!startItem(ids.at(index), ticks)) {
        m_playlist.clear();
        m_queueIndex = -1;
        return false;
    }
    m_queue.setCurrentPlaybackItemId(ids.at(index));
    persistQueueCursor();
    return true;
}

void PlayerController::requestPending()
{
    if (!m_loading || !m_engine.isInitialized() || m_itemId.isEmpty()
        || m_session.state() != PlaybackSession::State::Idle)
        return;
    m_mpvSettings.apply();
    const QJsonObject profile = buildDeviceProfile(m_settings.deviceProfileOptions());
    if (!m_client.requestPlaybackInfo(m_itemId, m_resumeTicks, profile)) {
        m_loading = false;
        m_errorText = i18n("Unable to request playback from the server.");
        Q_EMIT changed();
    }
}

void PlayerController::onSource(const PlaybackSource &source)
{
    if (!m_loading || m_stopping || source.itemId != m_itemId)
        return;
    if (!m_engine.isInitialized() || m_session.state() != PlaybackSession::State::Idle)
        return;
    m_loading = false;
    m_source = source;
    m_subtitleOptions = source.externalSubtitles;
    m_externalSubtitles.clear();
    for (const ExternalSubtitle &entry : m_subtitleOptions)
        m_externalSubtitles.append(QVariantMap{
            {QStringLiteral("index"), entry.index},
            {QStringLiteral("name"),
             entry.label.isEmpty() ? i18n("External subtitle %1").arg(entry.index) : entry.label}});
    Q_EMIT tracksChanged();
    if (!m_engine.open(source)) {
        m_errorText = i18n("The selected media source could not be opened.");
        Q_EMIT changed();
        return;
    }
    refreshSegments();
    updateTrickplayLayout();
    Q_EMIT changed();
}

void PlayerController::onPlaybackItem(const QString &id, const QJsonObject &item)
{
    // A reply for an item that is no longer the current one must not name or lay out this one
    if (id != m_itemId || !active())
        return;
    const ItemSummary summary = parseItemSummary(item);
    if (summary.valid) {
        const bool episode =
            summary.type == QLatin1String("Episode") && !summary.seriesName.isEmpty();
        m_title = episode ? summary.seriesName : summary.name;
        m_subtitle = episode ? episodeSubtitle(summary) : QString();
        if (m_seriesPlaylistWanted && episode && !summary.seriesId.isEmpty()) {
            m_seriesId = summary.seriesId;
            m_seriesEpisodes.clear();
            m_seriesEpisodeIndex = -1;
            m_seriesNextStart = 0;
            if (m_client.fetchEpisodes(m_seriesId, QString(), 0, SeriesPageSize, EpisodesChannel))
                m_seriesListItem = m_itemId;
        }
        m_seriesPlaylistWanted = false;
    }
    // The tile layout is picked by media source, which PlaybackInfo may deliver after this reply
    m_trickplayInfo =
        QJsonObject{{QStringLiteral("Trickplay"), item.value(QStringLiteral("Trickplay"))}};
    updateTrickplayLayout();
    Q_EMIT changed();
}

void PlayerController::updateTrickplayLayout()
{
    if (!m_trickplayInfo.isEmpty() && !m_source.mediaSourceId.isEmpty())
        m_trickplay = parseTrickplayLayout(m_trickplayInfo, m_source.mediaSourceId);
}

void PlayerController::onSeriesEpisodes(const QJsonArray &items, int total, int channel)
{
    if (channel != EpisodesChannel || m_seriesListItem.isEmpty() || m_seriesListItem != m_itemId)
        return;
    for (const QJsonValue &value : items) {
        const ItemSummary episode = parseItemSummary(value.toObject());
        // An episode the library lists without a file cannot be played
        if (!episode.valid || episode.virtualItem || m_seriesEpisodes.size() >= MaxSeriesEpisodes)
            continue;
        if (sameItemId(episode.id, m_itemId))
            m_seriesEpisodeIndex = m_seriesEpisodes.size();
        m_seriesEpisodes.append(episode.id);
    }
    m_seriesNextStart += items.size();
    const bool more = items.size() == SeriesPageSize && m_seriesNextStart < total
                      && m_seriesNextStart < MaxSeriesEpisodes;
    if (more
        && m_client.fetchEpisodes(m_seriesId, QString(), m_seriesNextStart, SeriesPageSize,
                                  EpisodesChannel))
        return;
    finishSeriesPlaylist();
}

void PlayerController::finishSeriesPlaylist()
{
    const QStringList ids = std::exchange(m_seriesEpisodes, {});
    const int index = std::exchange(m_seriesEpisodeIndex, -1);
    cancelSeriesRequest();
    // An episode the listing does not hold plays alone, as before
    if (index < 0)
        return;
    m_playlist = ids;
    m_queueIndex = index;
    m_playlistEphemeral = true;
    Q_EMIT changed();
}

void PlayerController::cancelSeriesRequest()
{
    m_seriesEpisodes.clear();
    m_seriesEpisodeIndex = -1;
    if (m_seriesListItem.isEmpty())
        return;
    m_seriesListItem.clear();
    m_client.releaseListingChannel(EpisodesChannel);
}

void PlayerController::dropSeriesPlaylist()
{
    cancelSeriesRequest();
    m_seriesPlaylistWanted = false;
    if (!m_playlistEphemeral)
        return;
    m_playlistEphemeral = false;
    m_playlist.clear();
    m_queueIndex = -1;
}

void PlayerController::onState(PlaybackSession::State, PlaybackSession::State to)
{
    if (to == PlaybackSession::State::Playing)
        m_clock.start();
    if (to == PlaybackSession::State::Idle) {
        m_trackTimer.stop();
        m_clock.stop();
        if (m_loading)
            QTimer::singleShot(0, this, &PlayerController::requestPending);
    }
    Q_EMIT changed();
}

void PlayerController::onEnd(const QString &reason)
{
    m_trackTimer.stop();
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    Q_EMIT tracksChanged();
    if (m_loading) {
        QTimer::singleShot(0, this, &PlayerController::requestPending);
        return;
    }
    // The saved queue always advances; the episodes of a series follow the user's autoplay preference, which never stops Repeat One
    const bool advance = !m_playlistEphemeral || m_nextEpisodeAutoPlay || m_repeat == 2;
    if (reason == QLatin1String("eof") && !m_stopping && advance && next())
        return;
    if (reason == QLatin1String("eof") && !m_playlist.isEmpty() && !m_playlistEphemeral
        && !m_client.serverId().isEmpty() && !m_client.userId().isEmpty()) {
        m_queueMemory.remove(m_client.serverId().toLower() + QLatin1Char('/')
                             + m_client.userId().toLower());
        saveMemory();
    }
    m_queue.setCurrentPlaybackItemId({});
    clearCurrent();
    Q_EMIT finished();
    Q_EMIT changed();
}

void PlayerController::clearCurrent()
{
    dropSeriesPlaylist();
    m_itemId.clear();
    m_title.clear();
    m_subtitle.clear();
    m_errorText.clear();
    m_loading = false;
    m_stopping = false;
    m_resumeTicks = 0;
    m_skipLabel.clear();
    m_skipEnd = 0;
    m_segments.clear();
}

void PlayerController::pause()
{
    m_engine.pause();
}
void PlayerController::resume()
{
    m_engine.resume();
}
void PlayerController::togglePause()
{
    if (paused())
        resume();
    else
        pause();
}
void PlayerController::stop()
{
    persistQueueCursor();
    m_stopping = true;
    m_client.cancelPlaybackInfo();
    m_loading = false;
    m_sleep.stop();
    if (m_session.state() == PlaybackSession::State::Idle) {
        m_queue.setCurrentPlaybackItemId({});
        clearCurrent();
        Q_EMIT finished();
    } else {
        m_engine.stop();
    }
    Q_EMIT changed();
}

int PlayerController::nextIndex(int direction, bool pick) const
{
    if (m_playlist.isEmpty() || m_queueIndex < 0 || m_queueIndex >= m_playlist.size())
        return -1;
    if (m_repeat == 2 && direction > 0)
        return m_queueIndex;
    // Shuffle is a queue mode: a series played alone keeps its airing order
    if (m_shuffle && !m_playlistEphemeral && direction > 0 && m_playlist.size() > 1) {
        const int offset =
            pick ? QRandomGenerator::global()->bounded(int(m_playlist.size()) - 1) + 1 : 1;
        return (m_queueIndex + offset) % m_playlist.size();
    }
    const int candidate = m_queueIndex + direction;
    if (candidate >= 0 && candidate < m_playlist.size())
        return candidate;
    if (m_repeat == 1)
        return direction > 0 ? 0 : m_playlist.size() - 1;
    return -1;
}

bool PlayerController::next()
{
    const int next = nextIndex(1);
    if (next < 0)
        return false;
    m_queueIndex = next;
    const QString id = m_playlist.at(next);
    if (!m_playlistEphemeral)
        m_queue.setCurrentPlaybackItemId(id);
    const bool started = startItem(id, 0);
    if (started)
        persistQueueCursor();
    return started;
}
bool PlayerController::previous()
{
    if (positionTicks() > 3 * TicksPerSecond) {
        seek(0);
        return true;
    }
    const int prev = nextIndex(-1);
    if (prev < 0)
        return false;
    m_queueIndex = prev;
    const QString id = m_playlist.at(prev);
    if (!m_playlistEphemeral)
        m_queue.setCurrentPlaybackItemId(id);
    const bool started = startItem(id, 0);
    if (started)
        persistQueueCursor();
    return started;
}

void PlayerController::seek(qint64 ticks)
{
    if (!m_engine.canSeek() || !active() || ticks < 0)
        return;
    m_engine.seekTo(m_engine.durationTicks() > 0 ? std::min(ticks, m_engine.durationTicks())
                                                 : ticks);
    QTimer::singleShot(250, this, [this] { m_reporter.reportNow(); });
}
void PlayerController::seekRelative(int seconds)
{
    seek(std::max<qint64>(0, positionTicks() + qint64(seconds) * TicksPerSecond));
}
void PlayerController::setRate(double value)
{
    if (std::isfinite(value) && value >= 0.5 && value <= 4.0)
        m_engine.setRate(value);
}
void PlayerController::setVolume(int level)
{
    if (!m_engine.isInitialized() || level < 0 || level > 100)
        return;
    m_volume = level;
    m_engine.setMpvProperty(QStringLiteral("volume"), level);
    Q_EMIT changed();
    m_reporter.reportNow();
}
void PlayerController::toggleMute()
{
    if (!m_engine.isInitialized())
        return;
    m_muted = !m_muted;
    m_engine.setMpvProperty(QStringLiteral("mute"), m_muted);
    Q_EMIT changed();
    m_reporter.reportNow();
}
void PlayerController::setShuffle(bool enabled)
{
    m_shuffle = enabled;
    Q_EMIT changed();
}
void PlayerController::setRepeatMode(int mode)
{
    if (mode < 0 || mode > 2)
        return;
    m_repeat = mode;
    Q_EMIT changed();
}

QString PlayerController::timeText(qint64 ticks) const
{
    return playbackTimeText(ticks, durationTicks() >= 3600 * TicksPerSecond);
}

void PlayerController::refreshTracks()
{
    if (!m_engine.isInitialized() || m_session.state() == PlaybackSession::State::Idle)
        return;
    const QVariantList tracks = m_engine.mpvProperty(QStringLiteral("track-list")).toList();
    QVariantList audio, subs;
    m_selectedAudioIndex.reset();
    m_selectedSubtitleIndex.reset();
    // Subtitles turned off leave no selected track: the remembered choice is then none, not the last one
    m_selectedSubtitleId = 0;
    for (const QVariant &entry : tracks) {
        const QVariantMap map = entry.toMap();
        const QString type = map.value(QStringLiteral("type")).toString();
        const int id = map.value(QStringLiteral("id")).toInt();
        if (id <= 0 || id > 1024
            || (type != QLatin1String("audio") && type != QLatin1String("sub")))
            continue;
        QString label = map.value(QStringLiteral("title")).toString().left(120);
        if (label.isEmpty())
            label = map.value(QStringLiteral("lang")).toString().left(24);
        if (label.isEmpty())
            label = type == QLatin1String("audio") ? i18n("Audio track %1").arg(id)
                                                   : i18n("Subtitle %1").arg(id);
        const bool selected = map.value(QStringLiteral("selected")).toBool();
        const QVariant indexValue = map.value(QStringLiteral("ff-index"));
        if (selected && indexValue.isValid() && indexValue.toInt() >= 0) {
            if (type == QLatin1String("audio"))
                m_selectedAudioIndex = indexValue.toInt();
            else
                m_selectedSubtitleIndex = indexValue.toInt();
        }
        if (selected) {
            if (type == QLatin1String("audio"))
                m_selectedAudioId = id;
            else
                m_selectedSubtitleId = id;
        }
        const QVariantMap result{{QStringLiteral("id"), id},
                                 {QStringLiteral("name"), label},
                                 {QStringLiteral("selected"), selected}};
        if (type == QLatin1String("audio"))
            audio.append(result);
        else
            subs.append(result);
    }
    if (audio != m_audioTracks || subs != m_subtitleTracks) {
        m_audioTracks = audio;
        m_subtitleTracks = subs;
        Q_EMIT tracksChanged();
    }
    if (!m_trackRestored && !tracks.isEmpty()) {
        m_trackRestored = true;
        restoreTracks();
    }
}
void PlayerController::selectAudioTrack(int id)
{
    if (id < 1 || id > 1024 || !active())
        return;
    m_engine.setMpvProperty(QStringLiteral("aid"), id);
    rememberTracks(id, m_selectedSubtitleId);
    QTimer::singleShot(0, this, &PlayerController::refreshTracks);
    m_reporter.reportNow();
}
void PlayerController::selectSubtitleTrack(int id)
{
    if (id < 0 || id > 1024 || !active())
        return;
    m_engine.setMpvProperty(QStringLiteral("sid"),
                            id == 0 ? QVariant(QStringLiteral("no")) : QVariant(id));
    m_selectedSubtitleId = id;
    rememberTracks(m_selectedAudioId, id);
    QTimer::singleShot(0, this, &PlayerController::refreshTracks);
    m_reporter.reportNow();
}
void PlayerController::adjustAudioDelay(double seconds)
{
    if (!std::isfinite(seconds) || seconds < -10.0 || seconds > 10.0)
        return;
    m_audioDelay = seconds;
    m_mpvSettings.setAudioOffset(seconds);
    Q_EMIT changed();
}
void PlayerController::adjustSubtitleDelay(double seconds)
{
    if (!std::isfinite(seconds) || seconds < -10.0 || seconds > 10.0)
        return;
    m_subtitleDelay = seconds;
    m_mpvSettings.setSubtitleOffset(seconds);
    Q_EMIT changed();
}
void PlayerController::setSleepTimer(int minutes)
{
    if (minutes < 0 || minutes > 240)
        return;
    if (minutes == 0)
        m_sleep.stop();
    else
        m_sleep.start(minutes * 60 * 1000);
    Q_EMIT changed();
}
int PlayerController::sleepRemainingMinutes() const
{
    return m_sleep.isActive() ? std::max(1, (m_sleep.remainingTime() + 59'999) / 60'000) : 0;
}
void PlayerController::screenshot()
{
    if (!active() || !m_engine.isInitialized() || m_profileDataDir.isEmpty())
        return;
    const QString directory = m_profileDataDir + QStringLiteral("/screenshots");
    if (!ensurePrivateDirectory(directory))
        return;
    const QString file =
        directory + QStringLiteral("/kinefin-")
        + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))
        + QStringLiteral(".png");
    if (!m_engine.mpvCommand(
            {QStringLiteral("screenshot-to-file"), file, QStringLiteral("video")})) {
        m_errorText = i18n("Could not save the screenshot.");
        Q_EMIT changed();
        return;
    }
    // The path is within the profile's private data directory, writable even
    // inside Flatpak. A portal export can be added independently in P7.
    m_screenshotPath = file;
    Q_EMIT changed();
}

void PlayerController::refreshSegments()
{
    m_segments.clear();
    m_skipLabel.clear();
    // The authenticated client owns MediaSegments requests; results for an older
    // item must not modify a new playback session.
    m_client.fetchMediaSegments(m_itemId);
}

void PlayerController::updateSkip()
{
    QString label;
    qint64 end = 0;
    for (const QVariant &value : m_segments) {
        const QVariantMap row = value.toMap();
        const qint64 start = row.value(QStringLiteral("start")).toLongLong();
        const qint64 finish = row.value(QStringLiteral("end")).toLongLong();
        if (positionTicks() < start || positionTicks() >= finish || finish <= start)
            continue;
        const QString type = row.value(QStringLiteral("type")).toString();
        label = type == QLatin1String("Intro")   ? i18n("Skip intro")
                : type == QLatin1String("Recap") ? i18n("Skip recap")
                : type == QLatin1String("Outro") ? i18n("Skip outro")
                                                 : QString();
        if (!label.isEmpty()) {
            end = finish;
            break;
        }
    }
    if (label != m_skipLabel || end != m_skipEnd) {
        m_skipLabel = label;
        m_skipEnd = end;
        Q_EMIT changed();
    }
}
void PlayerController::skipSegment()
{
    if (m_skipEnd > positionTicks() && !m_skipLabel.isEmpty())
        seek(m_skipEnd);
}

QString PlayerController::memoryKey() const
{
    return m_client.serverId().toLower() + QLatin1Char('/') + m_client.userId().toLower()
           + QLatin1Char('/') + m_itemId.toLower();
}
void PlayerController::loadMemory()
{
    if (m_memoryFile.isEmpty())
        return;
    QFile file(m_memoryFile);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024)
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject() || doc.object().value(QStringLiteral("version")).toInt() != 1)
        return;
    m_memory = doc.object().value(QStringLiteral("tracks")).toObject();
    m_queueMemory = doc.object().value(QStringLiteral("queue")).toObject();
}
void PlayerController::saveMemory()
{
    if (m_memoryFile.isEmpty())
        return;
    if (m_memory.size() > MaxRememberedItems) {
        m_memory = {};
        return;
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                       {QStringLiteral("tracks"), m_memory},
                                                       {QStringLiteral("queue"), m_queueMemory}})
                                 .toJson(QJsonDocument::Compact);
    if (bytes.size() > 256 * 1024)
        return;
    writePrivateFile(m_memoryFile, bytes);
}
void PlayerController::rememberTracks(int audioId, int subtitleId)
{
    if (m_itemId.isEmpty() || m_memoryFile.isEmpty())
        return;
    m_memory.insert(memoryKey(), QJsonObject{{QStringLiteral("audio"), audioId},
                                             {QStringLiteral("subtitle"), subtitleId}});
    saveMemory();
}
void PlayerController::restoreTracks()
{
    const QJsonObject stored = m_memory.value(memoryKey()).toObject();
    if (stored.isEmpty())
        return;
    // Only restore ids which actually exist on this media source.
    const int aid = stored.value(QStringLiteral("audio")).toInt();
    const int sid = stored.value(QStringLiteral("subtitle")).toInt();
    for (const QVariant &track : m_audioTracks)
        if (track.toMap().value(QStringLiteral("id")).toInt() == aid) {
            m_engine.setMpvProperty(QStringLiteral("aid"), aid);
            break;
        }
    if (sid == 0) {
        m_engine.setMpvProperty(QStringLiteral("sid"), QStringLiteral("no"));
    } else
        for (const QVariant &track : m_subtitleTracks) {
            if (track.toMap().value(QStringLiteral("id")).toInt() == sid) {
                m_engine.setMpvProperty(QStringLiteral("sid"), sid);
                break;
            }
        }
}

bool PlayerController::downloadSubtitle(int index)
{
    if (!active() || !m_subtitleDir.isValid() || !m_pendingSubtitle.isEmpty())
        return false;
    for (const ExternalSubtitle &subtitle : m_subtitleOptions) {
        if (subtitle.index != index)
            continue;
        if (m_client.downloadExternalSubtitle(subtitle.url)) {
            m_pendingSubtitle = subtitle.url;
            return true;
        }
        return false;
    }
    return false;
}

void PlayerController::displayTileFrame(const QImage &tile, const TrickplayFrame &frame)
{
    if (!m_trickplay || tile.isNull())
        return;
    const QRect crop(frame.column * m_trickplay->width, frame.row * m_trickplay->height,
                     m_trickplay->width, m_trickplay->height);
    if (!tile.rect().contains(crop))
        return;
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return;
    if (!tile.copy(crop).save(&buffer, "PNG"))
        return;
    m_previewImage =
        QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
    Q_EMIT previewChanged();
}

void PlayerController::previewAt(qint64 ticks)
{
    if (!m_trickplay || m_itemId.isEmpty() || !active() || !canSeek())
        return;
    const auto frame = trickplayFrameAt(*m_trickplay, ticks);
    if (!frame) {
        clearPreview();
        return;
    }
    m_requestedFrame = frame;
    if (m_cachedTileIndex == frame->tileIndex && !m_lastTile.isNull()) {
        displayTileFrame(m_lastTile, *frame);
        return;
    }
    if (m_pendingTileIndex == frame->tileIndex)
        return;
    if (m_client.fetchTrickplayTile(m_itemId, m_source.mediaSourceId, m_trickplay->width,
                                    frame->tileIndex))
        m_pendingTileIndex = frame->tileIndex;
}

void PlayerController::clearPreview()
{
    m_requestedFrame.reset();
    if (!m_previewImage.isEmpty()) {
        m_previewImage.clear();
        Q_EMIT previewChanged();
    }
}

void PlayerController::updatePlaylist()
{
    if (m_playlistEphemeral || m_playlist.isEmpty() || m_queueIndex < 0 || m_itemId.isEmpty())
        return;
    QStringList updated = m_queue.itemIds();
    const int current = updated.indexOf(m_itemId);
    if (current >= 0) {
        m_playlist = updated;
        m_queueIndex = current;
    } else {
        // Removing the current row does not interrupt the loaded file. Keep
        // an ephemeral entry so Next still advances to what follows it.
        const int priorIndex = qBound(0, m_queueIndex, int(updated.size()));
        updated.insert(priorIndex, m_itemId);
        m_playlist = updated;
        m_queueIndex = priorIndex;
    }
    Q_EMIT changed();
}

void PlayerController::persistQueueCursor()
{
    if (m_playlistEphemeral || m_playlist.isEmpty() || m_queueIndex < 0 || m_itemId.isEmpty()
        || m_client.serverId().isEmpty() || m_client.userId().isEmpty())
        return;
    // An item that left the queue, or one played before the queue started, has no cursor to keep there
    if (!std::any_of(m_playlist.cbegin(), m_playlist.cend(),
                     [this](const QString &id) { return sameItemId(id, m_itemId); }))
        return;
    const QString key =
        m_client.serverId().toLower() + QLatin1Char('/') + m_client.userId().toLower();
    const auto position = m_loading ? std::optional<qint64>{} : m_engine.knownPositionTicks();
    // A pending media source has no mpv position. Do not erase an earlier
    // resume point while buffering its next source.
    qint64 ticks = position.value_or(m_resumeTicks);
    if (ticks < 0 || ticks > 24LL * 3600 * TicksPerSecond)
        ticks = 0;
    if (m_queueMemory.size() >= 20 && !m_queueMemory.contains(key))
        m_queueMemory = {};
    m_queueMemory.insert(key, QJsonObject{{QStringLiteral("itemId"), m_itemId},
                                          {QStringLiteral("positionTicks"), ticks}});
    saveMemory();
}

} // namespace Kinefin
