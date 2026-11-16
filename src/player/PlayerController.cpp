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
#include <QQuickWindow>
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
// An item that played for less wall-clock time than this since it loaded is not repeated
constexpr qint64 MinimumPlayedMs = 2000;
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

// What names an item: for an episode the series, with the numbered episode below; else the item's name alone
std::pair<QString, QString> itemTitles(const ItemSummary &item)
{
    const bool episode = item.type == QLatin1String("Episode") && !item.seriesName.isEmpty();
    return {episode ? item.seriesName : item.name, episode ? episodeSubtitle(item) : QString()};
}

// A track of the file as a choice to remember: its language, forced flag and place among its kind
std::optional<TrackChoice> choiceOf(const QList<TrackFact> &tracks, int id)
{
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks.at(i).id == id)
            return TrackChoice{tracks.at(i).lang, tracks.at(i).forced, i};
    }
    return std::nullopt;
}

int indexOfItem(const QStringList &ids, const QString &id)
{
    for (int i = 0; i < ids.size(); ++i) {
        if (sameItemId(ids.at(i), id))
            return i;
    }
    return -1;
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

QList<ChapterSummary> playableChapters(QList<ChapterSummary> chapters, qint64 durationTicks)
{
    std::stable_sort(chapters.begin(), chapters.end(),
                     [](const ChapterSummary &a, const ChapterSummary &b) {
                         return a.startTicks < b.startTicks;
                     });
    QList<ChapterSummary> result;
    for (const ChapterSummary &chapter : std::as_const(chapters)) {
        if (durationTicks > 0 && chapter.startTicks >= durationTicks)
            break;
        if (result.isEmpty() || result.constLast().startTicks != chapter.startTicks)
            result.append(chapter);
    }
    return result.size() < 2 ? QList<ChapterSummary>() : result;
}

int chapterIndexAt(const QList<ChapterSummary> &chapters, qint64 ticks)
{
    const auto after = std::upper_bound(
        chapters.cbegin(), chapters.cend(), ticks,
        [](qint64 value, const ChapterSummary &chapter) { return value < chapter.startTicks; });
    return int(after - chapters.cbegin()) - 1;
}

qint64 chapterStepTarget(const QList<ChapterSummary> &chapters, qint64 positionTicks, int direction)
{
    if (chapters.isEmpty() || direction == 0)
        return -1;
    // A seek can land a frame or so before the chapter start it asked for: that start counts as reached
    const int current = chapterIndexAt(chapters, positionTicks + TicksPerSecond);
    if (direction > 0)
        return current + 1 < chapters.size() ? chapters.at(current + 1).startTicks : -1;
    if (current >= 0 && positionTicks - chapters.at(current).startTicks > 3 * TicksPerSecond)
        return chapters.at(current).startTicks;
    return current > 0 ? chapters.at(current - 1).startTicks : 0;
}

RememberedTracks rememberedTracks(const QJsonObject &item, const SeriesChoice &series,
                                  const QList<TrackFact> &audio, const QList<TrackFact> &subtitles,
                                  bool rememberAudio, bool rememberSubtitles)
{
    const auto listed = [](const QList<TrackFact> &tracks, int id) {
        return id > 0 && std::any_of(tracks.cbegin(), tracks.cend(), [id](const TrackFact &track) {
                   return track.id == id;
               });
    };
    RememberedTracks result;
    if (rememberAudio) {
        // The item's own id, when this source has it; else the series' choice
        const int id = item.value(QStringLiteral("audio")).toInt();
        if (listed(audio, id))
            result.audio = id;
        else if (series.audio)
            result.audio = pickTrack(audio, *series.audio);
    }
    if (rememberSubtitles) {
        const QJsonValue stored = item.value(QStringLiteral("subtitle"));
        if (stored.isDouble() && stored.toInt() == 0)
            result.subtitleOff = true;
        else if (listed(subtitles, stored.toInt()))
            result.subtitle = stored.toInt();
        else if (series.subtitleOff)
            result.subtitleOff = true;
        else if (series.subtitle)
            result.subtitle = pickTrack(subtitles, *series.subtitle);
    }
    return result;
}

std::optional<UpNextWindow> upNextWindow(qint64 durationTicks, const QVariantList &segments)
{
    if (durationTicks < 10 * 60 * TicksPerSecond)
        return std::nullopt;
    const qint64 lead = 30 * TicksPerSecond;
    for (const QVariant &value : segments) {
        const QVariantMap segment = value.toMap();
        const qint64 start = segment.value(QStringLiteral("start")).toLongLong();
        const qint64 end = segment.value(QStringLiteral("end")).toLongLong();
        // An Outro followed by a scene does not end the item, nor does one that starts in its first half or more than 15 minutes before its end
        if (segment.value(QStringLiteral("type")).toString() == QLatin1String("Outro")
            && end >= durationTicks - lead && start < durationTicks
            && start >= std::max(durationTicks / 2, durationTicks - 15 * 60 * TicksPerSecond))
            return UpNextWindow{start, std::min(start + 10 * TicksPerSecond, durationTicks)};
    }
    return UpNextWindow{durationTicks - lead, durationTicks};
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
              // A group's item ignores the local repeat and shuffle
              state.repeatMode = m_groupItem ? 0 : m_repeat;
              state.shuffle = !m_groupItem && m_shuffle;
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
                for (const qint64 ticks : std::exchange(m_earlySeeks, {}))
                    keepIntrosAt(ticks);
                updateSkip();
                updateUpNext();
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
        updateCurrentChapter();
        updateUpNext();
        Q_EMIT changed();
    });
    connect(&m_engine, &PlaybackEngine::durationChanged, this, [this] {
        updateChapters();
        updateUpNext();
        Q_EMIT changed();
    });
    connect(&m_engine, &PlaybackEngine::canSeekChanged, this, &PlayerController::changed);
    connect(&m_engine, &PlaybackEngine::seekingChanged, this, &PlayerController::changed);
    connect(&m_engine, &PlaybackEngine::rateChanged, this, [this] {
        updateUpNext();
        Q_EMIT changed();
    });
    m_seriesPreferences.setParent(this);
    if (const QString data = profileDataDirectory(profileId); !data.isEmpty())
        m_seriesPreferences.setDirectory(data + QStringLiteral("/series"));
    connect(&m_seriesPreferences, &SeriesPreferences::changed, this,
            &PlayerController::seriesChoicesChanged);
    connect(&m_engine, &PlaybackEngine::fileLoaded, this, [this] {
        if (m_resumeTicks > 0) {
            const qint64 ticks = m_resumeTicks;
            m_resumeTicks = 0;
            m_engine.seekTo(ticks);
            Q_EMIT seeked(ticks);
        }
        m_trackRestored = false;
        m_trackTimer.start();
        m_playedClock.start();
        m_mpvSettings.applyLoadedAspect();
        m_fileLoaded = true;
        m_trackChosen = false;
        applySeriesPlayback();
        refreshTracks();
        // Opened paused, the Start went out as playing: a paused Progress follows once the position is known
        if (m_groupItem)
            m_seekReport.start();
        // A paused file may have told its only position before this: a group's Ready must not wait for another
        Q_EMIT changed();
    });
    connect(&m_engine, &PlaybackEngine::videoParamsChanged, &m_mpvSettings,
            &PlayerSettings::applyLoadedAspect);
    connect(&m_engine, &PlaybackEngine::streamEnded, this, &PlayerController::onEnd);
    m_sleep.setSingleShot(true);
    connect(&m_sleep, &QTimer::timeout, this, [this] { stop(); });
    m_seekReport.setSingleShot(true);
    m_seekReport.setInterval(250);
    connect(&m_seekReport, &QTimer::timeout, this, [this] { m_reporter.reportNow(); });
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
    // The display rate is the one of the screen the video is on
    m_mpvSettings.setWindow(item->window());
    connect(item, &QQuickItem::windowChanged, this,
            [this](QQuickWindow *window) { m_mpvSettings.setWindow(window); });
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
    disconnect(item, &QQuickItem::windowChanged, this, nullptr);
    m_mpvSettings.setWindow(nullptr);
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
    m_primaryTag.clear();
    m_queue.setCurrentPlaybackItemId({});
    m_pendingSubtitle.clear();
    clearPreview();
    m_itemChapters.clear();
    updateChapters();
    resetUpNext();
    m_seriesPreferences.flush();
    if (!m_itemSeriesId.isEmpty()) {
        m_itemSeriesId.clear();
        Q_EMIT seriesChoicesChanged();
    }
    Q_EMIT changed();
}

bool PlayerController::playItem(const QString &id, qint64 resumeTicks)
{
    if (!isValidJellyfinId(id) || resumeTicks < 0 || m_client.userId().isEmpty())
        return false;
    if (grouped())
        return resumeTicks <= 24LL * 3600 * TicksPerSecond
               && m_group->requestQueue({id}, 0, resumeTicks);
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

bool PlayerController::openGroupItem(const QString &id, qint64 startTicks)
{
    if (!isValidJellyfinId(id) || m_client.userId().isEmpty() || startTicks < 0
        || startTicks > 24LL * 3600 * TicksPerSecond)
        return false;
    // The saved queue keeps where it was: the group's queue lives in the group
    persistQueueCursor();
    dropSeriesPlaylist();
    m_playlist.clear();
    m_queueIndex = -1;
    m_queue.setCurrentPlaybackItemId({});
    return startItem(id, startTicks, true);
}

bool PlayerController::startItem(const QString &id, qint64 resumeTicks, bool grouped)
{
    if (!isValidJellyfinId(id) || resumeTicks < 0 || m_client.userId().isEmpty())
        return false;
    const bool sameItem = id == m_itemId;
    // Preserve the prior item's cursor before the new id replaces it.
    if (!m_itemId.isEmpty() && !sameItem)
        persistQueueCursor();
    // A different item cannot be opened until the old mpv end-file arrived.
    // The pending id survives that stop and is negotiated exactly once.
    m_client.cancelPlaybackInfo();
    cancelSeriesRequest();
    m_seriesPlaylistWanted = false;
    m_groupItem = grouped;
    m_mpvSettings.setSpeedHeld(grouped);
    m_itemId = id;
    m_title.clear();
    m_subtitle.clear();
    m_primaryTag.clear();
    m_resumeTicks = resumeTicks;
    m_resumedFrom = resumeTicks;
    m_rateDecided = false;
    m_fileLoaded = false;
    m_seriesApplied = false;
    if (!m_itemSeriesId.isEmpty()) {
        m_itemSeriesId.clear();
        Q_EMIT seriesChoicesChanged();
    }
    m_errorText.clear();
    m_loading = true;
    m_stopping = false;
    m_skipLabel.clear();
    m_segments.clear();
    m_source = PlaybackSource{};
    // The same item again (a repeat) keeps the aspect and the rate the user chose; a group plays at 1.0
    m_keptRate.reset();
    if (sameItem && !grouped)
        m_keptRate = m_engine.playbackRate();
    else
        m_mpvSettings.setAspectOverride({});
    m_externalSubtitles.clear();
    m_subtitleOptions.clear();
    m_pendingSubtitle.clear();
    m_trickplay.reset();
    m_trickplayInfo = {};
    m_itemChapters.clear();
    updateChapters();
    resetUpNext();
    m_requestedFrame.reset();
    m_lastTile = {};
    m_cachedTileIndex = -1;
    m_pendingTileIndex = -1;
    clearPreview();
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    m_audioFacts.clear();
    m_subtitleFacts.clear();
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

bool PlayerController::playQueue(int index, qint64 startTicks)
{
    if (index < 0 || index >= m_queue.count() || startTicks < 0
        || startTicks > 24LL * 3600 * TicksPerSecond)
        return false;
    if (grouped())
        return m_group->requestQueue(m_queue.itemIds(), index, startTicks);
    dropSeriesPlaylist();
    m_playlist = m_queue.itemIds();
    m_queueIndex = index;
    if (!startItem(m_playlist.at(index), startTicks)) {
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
    if (grouped())
        return m_group->requestQueue(ids, index, ticks);
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
    if (m_groupItem) {
        m_engine.setRate(1.0);
        m_rateDecided = true;
    } else if (m_keptRate) {
        m_engine.setRate(*m_keptRate);
        m_keptRate.reset();
        m_rateDecided = true;
    }
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
    if (!m_engine.open(source, m_groupItem)) {
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
        std::tie(m_title, m_subtitle) = itemTitles(summary);
        m_primaryTag = summary.primaryTag;
        if (summary.type == QLatin1String("Episode") && isValidJellyfinId(summary.seriesId)) {
            seriesStore();
            m_itemSeriesId = summary.seriesId;
            Q_EMIT seriesChoicesChanged();
            applySeriesPlayback();
            // Tracks restored before the series was known get its choices now, unless the user chose since
            if (m_trackRestored && !m_trackChosen)
                restoreTracks();
        }
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
    m_itemChapters = parseChapters(item.value(QStringLiteral("Chapters")));
    updateChapters();
    Q_EMIT changed();
}

void PlayerController::updateTrickplayLayout()
{
    if (!m_trickplayInfo.isEmpty() && !m_source.mediaSourceId.isEmpty())
        m_trickplay = parseTrickplayLayout(m_trickplayInfo, m_source.mediaSourceId);
}

void PlayerController::updateChapters()
{
    m_chapters = playableChapters(m_itemChapters, durationTicks());
    QVariantList list;
    list.reserve(m_chapters.size());
    for (int i = 0; i < m_chapters.size(); ++i) {
        const ChapterSummary &chapter = m_chapters.at(i);
        const QString name = chapter.name.trimmed();
        list.append(QVariantMap{
            {QStringLiteral("name"),
             name.isEmpty() ? i18nc("@item a chapter the server gives no name", "Chapter %1", i + 1)
                            : name},
            {QStringLiteral("startTicks"), chapter.startTicks},
            {QStringLiteral("time"), timeText(chapter.startTicks)}});
    }
    if (list != m_chapterList) {
        m_chapterList = list;
        Q_EMIT chaptersChanged();
    }
    updateCurrentChapter();
}

void PlayerController::updateCurrentChapter()
{
    const int chapter = chapterIndexAt(m_chapters, positionTicks());
    if (chapter != m_currentChapter) {
        m_currentChapter = chapter;
        Q_EMIT currentChapterChanged();
    }
}

void PlayerController::jumpToChapter(int index)
{
    if (index >= 0 && index < m_chapters.size())
        seek(m_chapters.at(index).startTicks);
}

void PlayerController::stepChapter(int direction)
{
    const qint64 target = chapterStepTarget(m_chapters, positionTicks(), direction);
    if (target >= 0)
        seek(target);
}

int PlayerController::pendingAdvance(bool pick) const
{
    if (!advancesAtEnd())
        return -1;
    const int index = nextIndex(1, pick);
    return index == m_queueIndex ? -1 : index;
}

void PlayerController::updateUpNext()
{
    const std::optional<qint64> position = m_engine.knownPositionTicks();
    const qint64 duration = durationTicks();
    std::optional<UpNextWindow> window;
    if (!m_itemId.isEmpty() && !m_loading && position && pendingAdvance(false) >= 0)
        window = upNextWindow(duration, m_segments);
    bool show = window && !m_upNextCancelled && *position >= window->showAt;
    if (show && window->advanceAt < duration && *position >= window->advanceAt) {
        // Played into the early advance with the card up, the next item starts; reached any other way (a resume), the card stays down and the end moves on as usual (a seek past it is a Cancel, in seek())
        if (m_upNextShown) {
            playUpNext();
            return;
        }
        show = false;
    }
    QString id;
    if (show) {
        // Under shuffle the step drawn when the card came up holds while it is still in the playlist
        const bool keep = m_shuffle && m_upNextShown && indexOfItem(m_playlist, m_upNextId) >= 0
                          && !sameItemId(m_upNextId, m_itemId);
        const int index = keep ? -1 : pendingAdvance(true);
        id = keep ? m_upNextId : index >= 0 ? m_playlist.at(index) : QString();
        show = !id.isEmpty();
    }
    const QVariantMap info = !show                                         ? QVariantMap()
                             : m_upNextShown && sameItemId(id, m_upNextId) ? m_upNext
                                                                           : upNextInfo(id);
    const double rate = std::max(0.01, this->rate());
    const int seconds = show ? std::max(0, int(std::ceil(double(window->advanceAt - *position)
                                                         / TicksPerSecond / rate)))
                             : 0;
    m_upNextId = id;
    if (show != m_upNextShown || seconds != m_upNextSeconds || info != m_upNext) {
        m_upNextShown = show;
        m_upNextSeconds = seconds;
        m_upNext = info;
        Q_EMIT upNextChanged();
    }
}

void PlayerController::resetUpNext()
{
    m_upNextId.clear();
    m_upNextCancelled = false;
    m_handledIntros.clear();
    m_earlySeeks.clear();
    if (m_upNextShown || !m_upNext.isEmpty() || m_upNextSeconds != 0) {
        m_upNextShown = false;
        m_upNext.clear();
        m_upNextSeconds = 0;
        Q_EMIT upNextChanged();
    }
}

QVariantMap PlayerController::upNextInfo(const QString &id) const
{
    if (m_playlistEphemeral)
        return m_seriesInfo.value(id.toLower());
    const int row = indexOfItem(m_queue.itemIds(), id);
    if (row < 0)
        return {};
    const QModelIndex index = m_queue.index(row);
    ItemSummary item;
    item.id = id;
    item.name = index.data(PlayQueue::NameRole).toString();
    item.type = index.data(PlayQueue::TypeRole).toString();
    item.seriesName = index.data(PlayQueue::SeriesNameRole).toString();
    item.seasonNumber = index.data(PlayQueue::SeasonNumberRole).toInt();
    item.episodeNumber = index.data(PlayQueue::EpisodeNumberRole).toInt();
    const auto [title, subtitle] = itemTitles(item);
    return {{QStringLiteral("title"), title},
            {QStringLiteral("subtitle"), subtitle},
            {QStringLiteral("imageUrl"), index.data(PlayQueue::ImageUrlRole).toUrl()}};
}

void PlayerController::playUpNext()
{
    // A refused start takes the card down rather than trying again with every position
    if (m_upNextShown && !next()) {
        m_upNextCancelled = true;
        updateUpNext();
    }
}

void PlayerController::cancelUpNext()
{
    if (!m_upNextShown)
        return;
    m_upNextCancelled = true;
    updateUpNext();
}

void PlayerController::keepIntrosAt(qint64 ticks)
{
    if (m_segments.isEmpty()) {
        if (m_earlySeeks.size() < 32)
            m_earlySeeks.append(ticks);
        return;
    }
    for (const QVariant &value : std::as_const(m_segments)) {
        const QVariantMap segment = value.toMap();
        const qint64 start = segment.value(QStringLiteral("start")).toLongLong();
        if (segment.value(QStringLiteral("type")).toString() == QLatin1String("Intro")
            && ticks >= start && ticks < segment.value(QStringLiteral("end")).toLongLong()
            && !m_handledIntros.contains(start))
            m_handledIntros.append(start);
    }
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
        const auto [title, subtitle] = itemTitles(episode);
        m_pendingSeriesInfo.insert(episode.id.toLower(),
                                   {{QStringLiteral("title"), title},
                                    {QStringLiteral("subtitle"), subtitle},
                                    {QStringLiteral("imageUrl"), m_queue.imageUrlFor(episode)}});
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
    QHash<QString, QVariantMap> info = std::exchange(m_pendingSeriesInfo, {});
    cancelSeriesRequest();
    // An episode the listing does not hold plays alone, as before
    if (index < 0)
        return;
    m_playlist = ids;
    m_queueIndex = index;
    m_playlistEphemeral = true;
    m_seriesInfo = std::move(info);
    updateUpNext();
    Q_EMIT changed();
}

void PlayerController::cancelSeriesRequest()
{
    m_seriesEpisodes.clear();
    m_seriesEpisodeIndex = -1;
    m_pendingSeriesInfo.clear();
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
    m_seriesInfo.clear();
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
    // The group decides what follows its item: nothing moves on or plays again here
    if (m_groupItem) {
        m_playedClock.invalidate();
        clearCurrent();
        Q_EMIT finished();
        Q_EMIT changed();
        return;
    }
    const bool ended = reason == QLatin1String("eof") && !m_stopping;
    // Cancel on the Up Next card ends this playback here, without the advance or the replay of a repeat; a repeat mode chosen after it that replays the item wins
    const bool cancelled = m_upNextCancelled && pendingAdvance(false) >= 0;
    const bool playedLongEnough =
        m_playedClock.isValid() && m_playedClock.elapsed() >= MinimumPlayedMs;
    m_playedClock.invalidate();
    if (ended && !cancelled && advancesAtEnd() && next())
        return;
    // An item with nothing to advance to plays again when either repeat mode is on, after a real end: a stream that stops at once or far from its end does not loop
    const QString again = m_itemId;
    if (ended && !cancelled && m_repeat != 0 && m_engine.nearEnd() && playedLongEnough
        && (m_playlist.isEmpty() || m_playlistEphemeral) && startItem(again, 0))
        return;
    // The end of the queue forgets where it was; a Cancel keeps it, the queue has more to play
    if (reason == QLatin1String("eof") && !cancelled && !m_playlist.isEmpty()
        && !m_playlistEphemeral && !m_client.serverId().isEmpty() && !m_client.userId().isEmpty()) {
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
    m_primaryTag.clear();
    m_errorText.clear();
    m_loading = false;
    m_stopping = false;
    m_resumeTicks = 0;
    m_skipLabel.clear();
    m_skipEnd = 0;
    m_segments.clear();
    m_source = PlaybackSource{};
    m_keptRate.reset();
    m_mpvSettings.setAspectOverride({});
    m_itemChapters.clear();
    updateChapters();
    resetUpNext();
    m_fileLoaded = false;
    m_groupItem = false;
    m_mpvSettings.setSpeedHeld(false);
    if (!m_itemSeriesId.isEmpty()) {
        m_itemSeriesId.clear();
        Q_EMIT seriesChoicesChanged();
    }
}

void PlayerController::pause()
{
    if (grouped())
        m_group->requestPause();
    else
        m_engine.pause();
}
void PlayerController::resume()
{
    if (grouped())
        m_group->requestUnpause();
    else
        m_engine.resume();
}
void PlayerController::applyPause()
{
    m_engine.pause();
}
void PlayerController::applyUnpause()
{
    m_engine.resume();
}
void PlayerController::applySeek(qint64 ticks)
{
    seekLocally(ticks);
}
void PlayerController::applyStop()
{
    stopLocally();
}
void PlayerController::releaseGroupItem()
{
    m_groupItem = false;
    m_mpvSettings.setSpeedHeld(false);
}
bool PlayerController::groupItemLoaded(const QString &id) const
{
    return m_groupItem && id == m_itemId && m_fileLoaded && !m_loading
           && (m_session.state() == PlaybackSession::State::Paused
               || m_session.state() == PlaybackSession::State::Playing
               || m_session.state() == PlaybackSession::State::Buffering);
}
bool PlayerController::groupItemSettled(const QString &id) const
{
    return groupItemLoaded(id) && m_resumeTicks == 0 && m_engine.knownPositionTicks()
           && !m_engine.seeking();
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
    stopLocally();
}
void PlayerController::stopLocally()
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
    if (grouped())
        return false;
    // While the Up Next card is up, Next starts what it offers (under shuffle, the step it drew)
    int next = m_upNextId.isEmpty() ? -1 : indexOfItem(m_playlist, m_upNextId);
    if (next < 0)
        next = nextIndex(1);
    if (next < 0)
        return false;
    // Moving on from the card ends this item: reported as played to its end, as the natural end is
    if (m_upNextShown)
        m_engine.markEnded();
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
    if (grouped())
        return false;
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

PlayerController::Status PlayerController::status() const
{
    if (m_loading)
        return Status::Playing;
    switch (m_session.state()) {
    case PlaybackSession::State::Opening:
    case PlaybackSession::State::Playing:
    case PlaybackSession::State::Buffering:
        return Status::Playing;
    case PlaybackSession::State::Paused:
        return Status::Paused;
    default:
        return Status::Stopped;
    }
}

QUrl PlayerController::artworkUrl() const
{
    if (m_itemId.isEmpty() || m_primaryTag.isEmpty())
        return {};
    return m_client.imageUrl(m_itemId, QStringLiteral("Primary"), 512, m_primaryTag);
}

void PlayerController::seek(qint64 ticks)
{
    // While the next item loads, the engine still holds the previous one's stream
    if (!grouped()) {
        seekLocally(ticks);
        return;
    }
    if (m_engine.canSeek() && active() && !m_loading && ticks >= 0)
        m_group->requestSeek(
            m_engine.durationTicks() > 0 ? std::min(ticks, m_engine.durationTicks()) : ticks);
}
void PlayerController::seekLocally(qint64 ticks)
{
    if (!m_engine.canSeek() || !active() || m_loading || ticks < 0)
        return;
    const qint64 target =
        m_engine.durationTicks() > 0 ? std::min(ticks, m_engine.durationTicks()) : ticks;
    // Back before the Up Next card arms it again; past its early advance counts as its Cancel
    if (const auto window = upNextWindow(durationTicks(), m_segments);
        window && pendingAdvance(false) >= 0) {
        if (target < window->showAt)
            m_upNextCancelled = false;
        else if (window->advanceAt < durationTicks() && target > window->advanceAt)
            m_upNextCancelled = true;
    }
    keepIntrosAt(target);
    m_engine.seekTo(target);
    Q_EMIT seeked(target);
    m_seekReport.start();
}
void PlayerController::seekRelative(int seconds)
{
    seek(std::max<qint64>(0, positionTicks() + qint64(seconds) * TicksPerSecond));
}
void PlayerController::setRate(double value)
{
    if (grouped() || !std::isfinite(value) || value < MinimumRate || value > MaximumRate)
        return;
    m_engine.setRate(value);
    m_rateDecided = true;
    // The series keeps an explicit speed; the default speed is no choice
    if (!m_itemSeriesId.isEmpty()) {
        const double standard =
            m_settings.value(QStringLiteral("video"), QStringLiteral("default_playback_speed"))
                .toDouble();
        seriesStore().setRate(m_itemSeriesId, qFuzzyCompare(value, standard)
                                                  ? std::nullopt
                                                  : std::optional<double>(value));
    }
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
    if (grouped())
        return;
    m_shuffle = enabled;
    updateUpNext();
    Q_EMIT changed();
}
void PlayerController::setAspectMode(const QString &mode)
{
    if (!PlayerSettings::isAspectMode(mode))
        return;
    m_mpvSettings.setAspectOverride(mode);
    // The series keeps it, unless it is the setting's own value
    if (!m_itemSeriesId.isEmpty())
        seriesStore().setAspect(
            m_itemSeriesId,
            mode == m_settings.value(QStringLiteral("video"), QStringLiteral("aspect")).toString()
                ? QString()
                : mode);
    Q_EMIT changed();
}

QVariantMap PlayerController::playbackInfo() const
{
    QVariantMap info;
    if (m_source.itemId.isEmpty() || m_source.itemId != m_itemId)
        return info;
    const PlaybackMediaInfo &media = m_source.media;
    const auto put = [&info](const char *key, const QVariant &value) {
        const bool empty = value.typeId() == QMetaType::QString ? value.toString().isEmpty()
                           : value.typeId() == QMetaType::QStringList
                               ? value.toStringList().isEmpty()
                               : value.toDouble() <= 0;
        if (!empty)
            info.insert(QString::fromLatin1(key), value);
    };
    const bool direct = m_source.method == PlaybackSource::Method::Direct;
    info.insert(QStringLiteral("playMethod"),
                direct ? QStringLiteral("DirectPlay") : QStringLiteral("Transcode"));
    put("streamType", media.streamType);
    put("protocol", media.protocol);
    put("targetVideoCodec", media.targetVideoCodec);
    put("targetAudioCodec", media.targetAudioCodec);
    put("transcodeReasons", media.transcodeReasons);
    put("container", media.source.container);
    put("sizeBytes", media.source.size);
    put("bitrate", media.source.bitrate);
    if (const StreamSummary *video = pickStream(media.source, u"Video")) {
        put("videoCodec", video->codec);
        put("videoProfile", video->profile);
        put("videoLevel", video->level);
        put("videoBitrate", video->bitrate);
        put("videoFps", video->frameRate);
        put("videoBitDepth", video->bitDepth);
        put("videoPixelFormat", video->pixelFormat);
        put("videoRange",
            video->videoRangeType.isEmpty() ? video->videoRange : video->videoRangeType);
        put("colorSpace", video->colorSpace);
        put("colorPrimaries", video->colorPrimaries);
        put("colorTransfer", video->colorTransfer);
    }
    // A transcode carries the one track the server chose: mpv's stream index says nothing about the source's
    const int audioIndex = direct && m_selectedAudioIndex ? *m_selectedAudioIndex : -1;
    if (const StreamSummary *audio = pickStream(media.source, u"Audio", audioIndex)) {
        put("audioCodec", audio->codec);
        put("audioChannels", audio->channels);
        put("audioBitrate", audio->bitrate);
        put("audioSampleRate", audio->sampleRate);
    }
    info.insert(m_engine.videoFacts());
    return info;
}

void PlayerController::setRepeatMode(int mode)
{
    if (grouped() || mode < 0 || mode > 2)
        return;
    m_repeat = mode;
    updateUpNext();
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
    m_audioFacts.clear();
    m_subtitleFacts.clear();
    m_selectedAudioIndex.reset();
    m_selectedSubtitleIndex.reset();
    const bool direct = directPlay();
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
        // The server's stream index is mpv's only for an embedded track of a direct play: a transcode or an external file has its own numbering
        if (selected && direct && !map.value(QStringLiteral("external")).toBool()
            && indexValue.isValid() && indexValue.toInt() >= 0) {
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
        (type == QLatin1String("audio") ? m_audioFacts : m_subtitleFacts)
            .append({id, trackLanguage(map.value(QStringLiteral("lang")).toString()),
                     map.value(QStringLiteral("forced")).toBool()});
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
    m_trackChosen = true;
    rememberTracks(id, m_selectedSubtitleId);
    if (const auto choice = choiceOf(m_audioFacts, id); choice && keepsSeriesTracks())
        seriesStore().setAudio(m_itemSeriesId, choice);
    // The report carries the new stream index (a remote controller shows it at once)
    m_selectedAudioIndex = streamIndexOfTrack(QStringLiteral("audio"), id);
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
    m_trackChosen = true;
    m_selectedSubtitleIndex =
        id == 0 ? std::nullopt : streamIndexOfTrack(QStringLiteral("sub"), id);
    rememberTracks(m_selectedAudioId, id);
    if (keepsSeriesTracks()) {
        const auto choice = choiceOf(m_subtitleFacts, id);
        if (id == 0 || choice)
            seriesStore().setSubtitle(m_itemSeriesId, id == 0, choice);
    }
    QTimer::singleShot(0, this, &PlayerController::refreshTracks);
    m_reporter.reportNow();
}
bool PlayerController::directPlay() const
{
    // A transcode carries the one track the server chose: mpv's stream index says nothing about the source's
    return active() && m_engine.isInitialized() && !m_source.itemId.isEmpty()
           && m_source.itemId == m_itemId && m_source.method == PlaybackSource::Method::Direct;
}
std::optional<int> PlayerController::streamIndexOfTrack(const QString &type, int id) const
{
    if (!directPlay())
        return std::nullopt;
    const QVariantList tracks = m_engine.mpvProperty(QStringLiteral("track-list")).toList();
    for (const QVariant &entry : tracks) {
        const QVariantMap map = entry.toMap();
        const QVariant indexValue = map.value(QStringLiteral("ff-index"));
        if (map.value(QStringLiteral("type")).toString() == type
            && map.value(QStringLiteral("id")).toInt() == id
            && !map.value(QStringLiteral("external")).toBool() && indexValue.isValid()
            && indexValue.toInt() >= 0)
            return indexValue.toInt();
    }
    return std::nullopt;
}
int PlayerController::trackOfStreamIndex(const QString &type, int index) const
{
    if (!directPlay())
        return 0;
    const QVariantList tracks = m_engine.mpvProperty(QStringLiteral("track-list")).toList();
    for (const QVariant &entry : tracks) {
        const QVariantMap map = entry.toMap();
        const QVariant indexValue = map.value(QStringLiteral("ff-index"));
        const int id = map.value(QStringLiteral("id")).toInt();
        // An external track's ff-index counts in its own file, not in the server's media source
        if (map.value(QStringLiteral("type")).toString() == type && id > 0 && id <= 1024
            && !map.value(QStringLiteral("external")).toBool() && indexValue.isValid()
            && indexValue.toInt() == index)
            return id;
    }
    return 0;
}
bool PlayerController::selectAudioStreamIndex(int index)
{
    if (index < 0 || index > 1024)
        return false;
    const int id = trackOfStreamIndex(QStringLiteral("audio"), index);
    if (id == 0)
        return false;
    selectAudioTrack(id);
    return true;
}
bool PlayerController::selectSubtitleStreamIndex(int index)
{
    if (index < -1 || index > 1024)
        return false;
    // Subtitles off needs no track, only the direct play the indexes belong to
    if (index == -1) {
        if (!directPlay())
            return false;
        selectSubtitleTrack(0);
        return true;
    }
    const int id = trackOfStreamIndex(QStringLiteral("sub"), index);
    if (id == 0)
        return false;
    selectSubtitleTrack(id);
    return true;
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
        // Skip Intros Automatically: each Intro once, from a position mpv reported, never one the user seeked into or one before where the playback resumed, never one that runs to the end, and only once a seek can be made
        // Never in a group: every member would ask for the same seek
        if (type == QLatin1String("Intro") && !grouped() && !m_handledIntros.contains(start)
            && start >= m_resumedFrom && m_engine.knownPositionTicks() && canSeek() && !m_loading
            && durationTicks() > 0 && finish <= durationTicks() - 5 * TicksPerSecond
            && m_settings.value(QStringLiteral("video"), QStringLiteral("skip_intros")).toBool()) {
            m_handledIntros.append(start);
            seek(finish);
            return;
        }
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
    // Past its cap the track memory starts again, on disk too
    if (m_memory.size() > MaxRememberedItems)
        m_memory = {};
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
    const SeriesChoice series =
        m_itemSeriesId.isEmpty() ? SeriesChoice() : seriesStore().choice(m_itemSeriesId);
    const RememberedTracks tracks =
        rememberedTracks(m_memory.value(memoryKey()).toObject(), series, m_audioFacts,
                         m_subtitleFacts, m_rememberAudio, m_rememberSubtitles);
    if (tracks.audio > 0)
        m_engine.setMpvProperty(QStringLiteral("aid"), tracks.audio);
    if (tracks.subtitleOff)
        m_engine.setMpvProperty(QStringLiteral("sid"), QStringLiteral("no"));
    else if (tracks.subtitle > 0)
        m_engine.setMpvProperty(QStringLiteral("sid"), tracks.subtitle);
}

SeriesPreferences &PlayerController::seriesStore()
{
    m_seriesPreferences.setOwner(m_client.serverId(), m_client.userId());
    return m_seriesPreferences;
}

void PlayerController::applySeriesPlayback()
{
    if (!m_fileLoaded || m_seriesApplied || m_itemSeriesId.isEmpty())
        return;
    m_seriesApplied = true;
    const SeriesChoice choice = seriesStore().choice(m_itemSeriesId);
    // The speed a repeat kept or the user chose wins over the series', which wins over the default speed applied already; a group's item plays at 1.0
    if (choice.rate && !m_rateDecided && !m_groupItem)
        m_engine.setRate(*choice.rate);
    // An aspect chosen for this playback wins over the series', which wins over the setting
    if (!choice.aspect.isEmpty() && !m_mpvSettings.hasAspectOverride()) {
        m_mpvSettings.setAspectOverride(choice.aspect);
        Q_EMIT changed();
    }
}

void PlayerController::forgetSeriesChoices()
{
    if (!m_itemSeriesId.isEmpty())
        seriesStore().forget(m_itemSeriesId);
}

void PlayerController::forgetStoredUser(const QString &serverId, const QString &userId)
{
    if (!isValidJellyfinId(serverId) || !isValidJellyfinId(userId))
        return;
    m_seriesPreferences.forgetUser(serverId, userId);
    pruneMemory(serverId.toLower() + QLatin1Char('/') + userId.toLower());
}

void PlayerController::forgetStoredServer(const QString &serverId)
{
    if (!isValidJellyfinId(serverId))
        return;
    m_seriesPreferences.forgetServer(serverId);
    pruneMemory(serverId.toLower());
}

void PlayerController::pruneMemory(const QString &owner)
{
    for (QJsonObject *memory : {&m_memory, &m_queueMemory}) {
        const QStringList keys = memory->keys();
        for (const QString &key : keys) {
            if (key == owner || key.startsWith(owner + QLatin1Char('/')))
                memory->remove(key);
        }
    }
    // Written even when nothing was in memory: the file is what has to lose the owner
    saveMemory();
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
    updateUpNext();
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
