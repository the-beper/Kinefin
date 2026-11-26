// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/PlayerController.h"

#include "api/DeviceProfile.h"
#include "api/JellyfinClient.h"
#include "downloads/ExportNames.h"
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
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <utility>

#include <algorithm>
#include <cmath>

namespace Kinefin {
namespace {
constexpr qint64 TicksPerSecond = 10'000'000;

// A transcode asked from another start: the server begins its job at StartTimeTicks, and the rest of the query stays as the server wrote it. `freshSession` gives it a new PlaySessionId: the server names the job's output file after that id and serves the earlier file again for the same id, whatever the start
QUrl withStartTicks(QUrl url, qint64 ticks, bool freshSession = false)
{
    QUrlQuery query(url);
    const auto items = query.queryItems(QUrl::FullyEncoded);
    QUrlQuery kept;
    for (const auto &[name, value] : items) {
        if (freshSession && name.compare(QLatin1String("PlaySessionId"), Qt::CaseInsensitive) == 0)
            kept.addQueryItem(name, QUuid::createUuid().toString(QUuid::Id128));
        else if (name.compare(QLatin1String("StartTimeTicks"), Qt::CaseInsensitive) != 0)
            kept.addQueryItem(name, value);
    }
    kept.addQueryItem(QStringLiteral("StartTimeTicks"),
                      QString::number(std::max<qint64>(0, ticks)));
    url.setQuery(kept.query(QUrl::FullyEncoded), QUrl::StrictMode);
    return url;
}
constexpr int MaxRememberedItems = 1000;
// Past the cap the items chosen longest ago go first, those stored before entries carried a time ("at") before all; `spare` is the choice just made, which stays whatever its time says
void evictOldestChoices(QJsonObject &memory, const QString &spare)
{
    if (memory.size() <= MaxRememberedItems)
        return;
    // Sorted once: a file far over the cap costs no more than its size
    QList<std::pair<qint64, QString>> order;
    order.reserve(memory.size());
    for (auto entry = memory.constBegin(); entry != memory.constEnd(); ++entry) {
        if (entry.key() != spare)
            order.append(
                {entry.value().toObject().value(QStringLiteral("at")).toInteger(), entry.key()});
    }
    std::sort(order.begin(), order.end());
    const qsizetype excess = memory.size() - MaxRememberedItems;
    for (qsizetype index = 0; index < excess && index < order.size(); ++index)
        memory.remove(order.at(index).second);
}
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
    // Only a text subtitle other than the primary can take the second slot
    const auto secondaryCandidates = [&subtitles](int primary) {
        QList<TrackFact> candidates;
        for (const TrackFact &track : subtitles) {
            if (track.text && track.id != primary)
                candidates.append(track);
        }
        return candidates;
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
        // The second subtitle: the item's own id first, then the series', never the primary nor a bitmap one
        const QList<TrackFact> candidates = secondaryCandidates(result.subtitle);
        const int storedSecond = item.value(QStringLiteral("subtitle2")).toInt();
        if (listed(candidates, storedSecond))
            result.secondary = storedSecond;
        else if (series.subtitle2)
            result.secondary = pickTrack(candidates, *series.subtitle2);
    }
    return result;
}

bool textSubtitleCodec(const QString &codec)
{
    static const QStringList text{QStringLiteral("subrip"),   QStringLiteral("ass"),
                                  QStringLiteral("ssa"),      QStringLiteral("webvtt"),
                                  QStringLiteral("mov_text"), QStringLiteral("text")};
    return text.contains(codec);
}

DumpTrackIds dumpTrackIds(const QVariantList &tracks, int audio, int subtitle,
                          int secondarySubtitle, const QStringList &subtitleFiles)
{
    // The selected subtitle streams of the file, in their order; files added with sub-add are no streams of it
    QList<int> streamSubtitles;
    QHash<int, QString> subtitleFile;
    bool audioInStream = false;
    for (const QVariant &value : tracks) {
        const QVariantMap track = value.toMap();
        const QString type = track.value(QStringLiteral("type")).toString();
        const int id = track.value(QStringLiteral("id")).toInt();
        const bool external = track.value(QStringLiteral("external")).toBool();
        if (type == QLatin1String("audio") && id == audio && audio > 0 && !external)
            audioInStream = true;
        if (type != QLatin1String("sub"))
            continue;
        if (external)
            subtitleFile.insert(id, track.value(QStringLiteral("external-filename")).toString());
        else if (track.value(QStringLiteral("selected")).toBool())
            streamSubtitles.append(id);
    }
    const auto map = [&](int id) {
        if (id <= 0)
            return 0;
        if (const qsizetype rank = streamSubtitles.indexOf(id); rank >= 0)
            return int(rank) + 1;
        const qsizetype file = subtitleFiles.indexOf(subtitleFile.value(id));
        return subtitleFile.contains(id) && file >= 0 ? int(streamSubtitles.size() + file) + 1 : 0;
    };
    return {audioInStream ? 1 : 0, map(subtitle), map(secondarySubtitle)};
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
              state.positionTicks = m_casting ? std::optional<qint64>(m_castPositionMs * 10'000)
                                              : m_engine.knownPositionTicks();
              if (m_casting)
                  state.method = m_source.method;
              state.canSeek = m_casting ? m_cast->canSeek() : m_engine.canSeek();
              if (m_casting) {
                  // The renderer picks its own tracks; its volume counts once it answered
                  if (m_cast->hasVolume() && m_cast->volume() >= 0) {
                      state.volume = m_cast->volume();
                      state.muted = m_cast->muted();
                  }
              } else {
                  state.audioStreamIndex = m_selectedAudioIndex;
                  // Subtitles the user turned off are -1, as jellyfin-web reports them
                  state.subtitleStreamIndex = m_selectedSubtitleIndex ? m_selectedSubtitleIndex
                                              : m_subtitlesOff        ? std::optional<int>(-1)
                                                                      : std::nullopt;
                  state.volume = m_volume;
                  state.muted = m_muted;
              }
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
    // What an export left behind when the app stopped during it: staging files, their `.part` and cache dumps
    if (!m_profileDataDir.isEmpty()) {
        QDir clips(m_profileDataDir + QStringLiteral("/clips"));
        for (const QString &name : clips.entryList({QStringLiteral("clip-*")}, QDir::Files))
            clips.remove(name);
    }
    m_cast = new CastController(this);
    m_castSeek.setSingleShot(true);
    m_castSeek.setInterval(400);
    connect(&m_castSeek, &QTimer::timeout, this, [this] {
        // Every reload carries a text track that is ready
        m_castTrackPending = false;
        if (!m_casting || m_source.method != PlaybackSource::Method::Transcode)
            return;
        // The reload for the track alone would undo a pause made meanwhile: it waits for the resume
        if (m_castSeekTarget < 0 && m_session.state() == PlaybackSession::State::Paused) {
            m_castTrackPending = true;
            return;
        }
        // A negative target is where the device is now (a reload that brings in the text track)
        const qint64 target =
            m_castSeekTarget >= 0
                ? m_castSeekTarget
                : std::max<qint64>(0, m_cast->positionMs().value_or(m_castPositionMs)) * 10'000;
        m_cast->reload(withStartTicks(m_source.url, target, true), target / 10'000,
                       castTextTrack(target).value_or(CastTextTrack{}).url);
    });
    connect(m_cast, &CastController::playing, this, &PlayerController::onCastPlaying);
    connect(m_cast, &CastController::paused, this, &PlayerController::onCastPaused);
    connect(m_cast, &CastController::buffering, this, &PlayerController::onCastBuffering);
    connect(m_cast, &CastController::ended, this, &PlayerController::onCastEnded);
    connect(m_cast, &CastController::stopped, this, &PlayerController::onCastStopped);
    connect(m_cast, &CastController::failed, this, &PlayerController::onCastFailed);
    connect(m_cast, &CastController::positionChanged, this, &PlayerController::onCastPosition);
    connect(m_cast, &CastController::volumeChanged, this, &PlayerController::onCastVolume);
    connect(m_cast, &CastController::textTrackReady, this, &PlayerController::onCastTextTrackReady);
    connect(m_cast, &CastController::targetsChanged, this, &PlayerController::changed);
    m_observerId = m_session.addObserver(
        [this](PlaybackSession::State from, PlaybackSession::State to) { onState(from, to); });
    connect(&client, &JellyfinClient::playbackInfoReady, this, &PlayerController::onSource);
    connect(&client, &JellyfinClient::externalSubtitleDownloaded, this,
            [this](const QUrl &url, const QByteArray &bytes) {
                if (!active() || url != m_pendingSubtitle || !m_subtitleDir.isValid())
                    return;
                const bool secondary = std::exchange(m_pendingSubtitleSecondary, false);
                const QString extension = url.path().section(QLatin1Char('.'), -1).toLower();
                const QString filename = m_subtitleDir.path() + QStringLiteral("/subtitle-")
                                         + QString::number(QDateTime::currentMSecsSinceEpoch())
                                         + QLatin1Char('-') + QString::number(++m_subtitleFileSeq)
                                         + QLatin1Char('.') + extension;
                QSaveFile file(filename);
                if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
                    && file.commit()) {
                    for (const ExternalSubtitle &subtitle : std::as_const(m_subtitleOptions)) {
                        if (subtitle.url == url) {
                            m_loadedSubtitles.insert(filename, subtitle.index);
                            m_loadedSubtitleOrder.append(filename);
                        }
                    }
                    if (secondary) {
                        // `auto` adds it without selecting it: the primary stays as it is
                        m_engine.mpvCommand(
                            {QStringLiteral("sub-add"), filename, QStringLiteral("auto")});
                        m_pendingSecondaryFile = filename;
                        QTimer::singleShot(250, this, &PlayerController::refreshTracks);
                    } else {
                        m_engine.mpvCommand(
                            {QStringLiteral("sub-add"), filename, QStringLiteral("select")});
                        m_subtitlesOff = false;
                        // The report carries the new index once the tracks are read again
                        QTimer::singleShot(250, this, [this] {
                            refreshTracks();
                            m_reporter.reportNow();
                        });
                        // A second subtitle the reopen could not ask for while this download was pending follows it
                        if (m_secondaryStreamIndex && m_secondarySubtitleId == 0)
                            QTimer::singleShot(500, this,
                                               &PlayerController::restoreSecondarySubtitle);
                    }
                }
                m_pendingSubtitle.clear();
            });
    connect(&client, &JellyfinClient::externalSubtitleFailed, this, [this](const QUrl &url) {
        if (url == m_pendingSubtitle) {
            const bool secondary = std::exchange(m_pendingSubtitleSecondary, false);
            m_pendingSubtitle.clear();
            m_errorText = secondary ? i18n("The secondary subtitles could not be downloaded")
                                    : i18n("Could not download the subtitle.");
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
    // The OSD's check and the Settings page's switch are one setting: each follows the other
    connect(&m_settings, &ClientSettings::valueChanged, this,
            [this](const QString &section, const QString &key) {
                if (section == QLatin1String("audio") && key == QLatin1String("night_mode"))
                    Q_EMIT changed();
            });
    connect(&m_queue, &PlayQueue::edited, this, &PlayerController::updatePlaylist);
    connect(&client, &JellyfinClient::mediaSegmentsReady, this,
            [this](const QString &itemId, const QJsonArray &segments) {
                if (itemId != m_itemId || !active() || m_localPlayback)
                    return;
                QVariantList known;
                for (const QJsonValue &segment : segments)
                    known.append(segment.toObject().toVariantMap());
                setSegments(known);
                for (const qint64 ticks : std::exchange(m_earlySeeks, {}))
                    keepIntrosAt(ticks);
                updateSkip();
                updateUpNext();
            });
    connect(&client, &JellyfinClient::playbackInfoFailed, this,
            [this](const QString &itemId, int, const QString &) {
                // The request for a lower stream failed: playback goes on as it was
                if (!m_loading && m_fallbackRequest.active && itemId == m_itemId) {
                    const bool manual = m_fallbackRequest.manual;
                    m_fallbackRequest = {};
                    if (manual) {
                        m_qualitySwitching = false;
                        Q_EMIT changed();
                        Q_EMIT qualityChangeFailed();
                    } else {
                        m_fallback.exhaust();
                        Q_EMIT qualityUnavailable();
                    }
                    return;
                }
                if (!m_loading || itemId != m_itemId)
                    return;
                m_loading = false;
                m_errorText = i18n("The video could not be opened. Try again.");
                Q_EMIT changed();
            });
    connect(&client, &JellyfinClient::sessionExpired, this, [this] { onAccountLeaving(); });
    connect(&client, &JellyfinClient::credentialsCleared, this, [this] { onAccountLeaving(); });
    connect(&m_engine, &PlaybackEngine::positionChanged, this, [this] {
        // A second played past where the switched stream resumed (or a later seek landed) shows it delivers
        if (m_switchSettleTicks >= 0 && !m_engine.seeking()
            && m_engine.positionTicks() >= m_switchSettleTicks + TicksPerSecond)
            settleSwitch();
        updateSkip();
        updateCurrentChapter();
        updateUpNext();
        // The first tick of a burst goes out at once, the rest wait for the timer: the properties of the shell that follow this signal are about fifty bindings, and a frame rate of them lags the page
        if (m_positionNotice.isActive()) {
            m_positionNoticePending = true;
        } else {
            Q_EMIT changed();
            m_positionNotice.start();
        }
    });
    connect(&m_engine, &PlaybackEngine::durationChanged, this, [this] {
        updateChapters();
        updateUpNext();
        Q_EMIT changed();
    });
    connect(&m_engine, &PlaybackEngine::canSeekChanged, this, &PlayerController::changed);
    connect(&m_engine, &PlaybackEngine::seekingChanged, this, &PlayerController::changed);
    // A seek that lands gives the stream a grace before its stalls count again
    connect(&m_engine, &PlaybackEngine::seekingChanged, this, [this] {
        if (m_engine.seeking())
            return;
        m_fallback.restarted();
        // Where a seek landed is the point the picked stream must play on from, with the whole wait again
        if (m_switchSettleTicks >= 0) {
            m_switchSettleTicks = m_engine.positionTicks();
            m_switchTimeout.start();
        }
    });
    connect(&m_fallback, &BitrateFallback::starving, this, &PlayerController::onStarving);
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
        // mpv may not keep the points of a loop across a reopen of the same item
        if (m_clipLooping) {
            m_loopSet = false;
            applyClipLoop();
        }
        const qint64 resumeAt = m_resumeTicks;
        if (m_resumeTicks > 0) {
            const qint64 ticks = m_resumeTicks;
            m_resumeTicks = 0;
            m_engine.seekTo(ticks);
            Q_EMIT seeked(ticks);
        }
        m_trackRestored = false;
        const bool manualSwitch = m_switchManual;
        // The seek of a picked stream starts the server's encoder again: the pick is done once the stream plays on from there; a paused reopen cannot tell, and the fallback's own steps keep waiting as they always did
        if (m_switchingStream && m_switchManual && resumeAt > 0
            && m_session.state() != PlaybackSession::State::Paused) {
            m_switchSettleTicks = resumeAt;
            m_switchTimeout.start();
        } else {
            m_switchTimeout.stop();
            m_switchManual = false;
        }
        m_trackTimer.start();
        m_playedClock.start();
        m_mpvSettings.applyLoadedAspect();
        m_fileLoaded = true;
        m_trackChosen = false;
        applySeriesPlayback();
        // A transcode delivers the text subtitle that played as an external one: it is asked for before the restore, so a second subtitle can follow once it lands
        const bool lowered = std::exchange(m_switchingStream, false);
        if (lowered) {
            if (const std::optional<int> subtitle = m_requestedStreams.subtitleStreamIndex;
                subtitle && *subtitle >= 0
                && std::any_of(m_subtitleOptions.cbegin(), m_subtitleOptions.cend(),
                               [&subtitle](const ExternalSubtitle &entry) {
                                   return entry.index == *subtitle;
                               }))
                downloadSubtitle(*subtitle);
        }
        // A download's subtitle files join the tracks before the remembered choice is restored; `auto` leaves the choice to it
        if (m_localPlayback) {
            for (const LocalSubtitle &file : std::as_const(m_localSubtitles))
                m_engine.mpvCommand({QStringLiteral("sub-add"), file.path, QStringLiteral("auto"),
                                     file.title, file.language});
        }
        // The subtitle files the viewer added by hand come back with the reopened item, in the order mpv lists them; the one that was selected is selected again once the choice is restored
        for (auto added = m_addedSubtitles.begin(); added != m_addedSubtitles.end();) {
            if (m_engine.mpvCommand({QStringLiteral("sub-add"), added->path, QStringLiteral("auto"),
                                     added->title})) {
                m_loadedSubtitleOrder.append(added->path);
                ++added;
            } else {
                added = m_addedSubtitles.erase(added);
            }
        }
        refreshTracks();
        // Opened paused (a group's item, or a reopen after a subtitle download), the Start went out as playing: a paused Progress follows once the position is known
        if (m_groupItem || m_session.state() == PlaybackSession::State::Paused)
            m_seekReport.start();
        // The stream that plays now, for the fallback: a lowered one completes its step here
        m_fallback.streamLoaded(currentStreamBitrate(), lowered);
        // The user's own pick is no slow connection
        if (lowered && !manualSwitch)
            Q_EMIT qualityLowered(m_requestedCap);
        // A paused file may have told its only position before this: a group's Ready must not wait for another
        Q_EMIT changed();
    });
    connect(&m_engine, &PlaybackEngine::videoParamsChanged, &m_mpvSettings,
            &PlayerSettings::applyLoadedAspect);
    connect(&m_engine, &PlaybackEngine::videoSizeChanged, this, [this] {
        // dwidth and dheight arrive one after the other: the aspect is read once both are in, not from a mix of the old size and the new
        if (m_videoAspectPending)
            return;
        m_videoAspectPending = true;
        QMetaObject::invokeMethod(
            this,
            [this] {
                m_videoAspectPending = false;
                const QVariantMap facts = m_engine.videoFacts();
                const double width = facts.value(QStringLiteral("videoWidth")).toDouble();
                const double height = facts.value(QStringLiteral("videoHeight")).toDouble();
                const qreal aspect = width > 0 && height > 0 ? width / height : 0;
                if (qFuzzyCompare(aspect + 1, m_videoAspect + 1))
                    return;
                m_videoAspect = aspect;
                Q_EMIT videoAspectChanged();
            },
            Qt::QueuedConnection);
    });
    connect(&m_engine, &PlaybackEngine::streamEnded, this, &PlayerController::onEnd);
    connect(&m_clipExporter, &ClipExporter::progress, this, [this](double fraction) {
        m_clipProgress = fraction;
        Q_EMIT clipStateChanged();
    });
    connect(&m_clipExporter, &ClipExporter::finished, this, &PlayerController::onClipFinished);
    connect(&m_clipExporter, &ClipExporter::failed, this, &PlayerController::onClipFailed);
    connect(&m_client, &JellyfinClient::remoteSubtitlesReady, this,
            [this](const QString &itemId, const QString &language, const QJsonArray &subtitles) {
                if (!remoteSearchIsCurrent(itemId, language))
                    return;
                m_remoteSubtitles.clear();
                for (const QJsonValue &value : subtitles) {
                    if (m_remoteSubtitles.size() >= MaxRemoteSubtitles)
                        break;
                    if (!value.isObject())
                        continue;
                    const QJsonObject object = value.toObject();
                    const auto text = [&object](QLatin1String key, int max) {
                        return object.value(key).toString().simplified().left(max);
                    };
                    const QString id = object.value(QStringLiteral("Id")).toString();
                    if (id.isEmpty() || id.size() > 128)
                        continue;
                    m_remoteSubtitles.append(QVariantMap{
                        {QStringLiteral("id"), id},
                        {QStringLiteral("name"), text(QLatin1String("Name"), 200)},
                        {QStringLiteral("provider"), text(QLatin1String("ProviderName"), 80)},
                        {QStringLiteral("format"), text(QLatin1String("Format"), 16)},
                        {QStringLiteral("author"), text(QLatin1String("Author"), 120)},
                        {QStringLiteral("downloads"),
                         object.value(QStringLiteral("DownloadCount")).toInt()},
                        {QStringLiteral("forced"), object.value(QStringLiteral("Forced")).toBool()},
                        {QStringLiteral("hearingImpaired"),
                         object.value(QStringLiteral("HearingImpaired")).toBool()}});
                }
                m_remoteSubtitleSearching = false;
                m_remoteSubtitleQuery.clear();
                m_remoteSubtitleError.clear();
                Q_EMIT remoteSubtitlesChanged();
            });
    connect(&m_client, &JellyfinClient::remoteSubtitlesFailed, this,
            [this](const QString &itemId, const QString &language, int httpStatus) {
                if (!remoteSearchIsCurrent(itemId, language))
                    return;
                m_remoteSubtitleSearching = false;
                m_remoteSubtitleQuery.clear();
                m_remoteSubtitleError = httpStatus == 0     ? QStringLiteral("network")
                                        : httpStatus == 404 ? QStringLiteral("noprovider")
                                                            : QStringLiteral("failed");
                m_remoteSubtitles.clear();
                Q_EMIT remoteSubtitlesChanged();
            });
    connect(&m_client, &JellyfinClient::remoteSubtitleDownloaded, this,
            [this](const QString &itemId, const QString &subtitleId) {
                // A reply for a download of an item played before is not this one's
                if (itemId != m_itemId || m_remoteSubtitleDownloadItem != itemId || m_localPlayback)
                    return;
                m_remoteSubtitleDownloadItem.clear();
                Q_EMIT remoteSubtitleDownloaded(subtitleId);
                // Before mpv told a position (still loading, or the resume seek pending) the item stands where it started
                const qint64 at = m_engine.knownPositionTicks().value_or(m_resumedFrom);
                const bool paused = m_session.state() == PlaybackSession::State::Paused;
                // The same item again, not a new queue: a series keeps its episodes, and a paused video stays paused
                if (startItem(itemId, at, m_groupItem))
                    m_reopenPaused = paused;
            });
    connect(&m_client, &JellyfinClient::remoteSubtitleDownloadFailed, this,
            [this](const QString &itemId, const QString &subtitleId, int httpStatus) {
                if (itemId != m_itemId || m_remoteSubtitleDownloadItem != itemId)
                    return;
                m_remoteSubtitleDownloadItem.clear();
                Q_EMIT remoteSubtitleDownloadFailed(subtitleId, httpStatus);
            });
    // The server took the download but no new file showed: playback goes on as it is, with no reopen
    connect(&m_client, &JellyfinClient::remoteSubtitleNotSaved, this,
            [this](const QString &itemId, const QString &subtitleId) {
                if (itemId != m_itemId || m_remoteSubtitleDownloadItem != itemId)
                    return;
                m_remoteSubtitleDownloadItem.clear();
                Q_EMIT remoteSubtitleNotSaved(subtitleId);
            });
    // A refreshed /Users/Me can change the policy the OSD entry reads
    connect(&m_client, &JellyfinClient::userProfileRefreshed, this, [this] { Q_EMIT changed(); });
    m_sleep.setSingleShot(true);
    connect(&m_sleep, &QTimer::timeout, this, [this] { stop(); });
    m_switchTimeout.setSingleShot(true);
    m_switchTimeout.setInterval(15'000);
    connect(&m_switchTimeout, &QTimer::timeout, this, [this] {
        // Paused by the user after the switched stream loaded: nothing tells whether it would play on
        if (m_switchSettleTicks >= 0 && m_session.state() == PlaybackSession::State::Paused) {
            settleSwitch();
            return;
        }
        // A seek still on its way gets the wait again once it lands (the engine settles a stuck one itself)
        if (m_switchSettleTicks >= 0 && m_engine.seeking()) {
            m_switchTimeout.start();
            return;
        }
        if (!m_switchingStream && m_switchSettleTicks < 0)
            return;
        // A stream that never loads, or never plays on, would leave the player opening or buffering forever
        m_switchTimedOut = true;
        failStreamSwitch();
        m_engine.stop();
    });
    m_seekReport.setSingleShot(true);
    m_seekReport.setInterval(250);
    connect(&m_seekReport, &QTimer::timeout, this, [this] { m_reporter.reportNow(); });
    m_clock.setInterval(30'000);
    connect(&m_clock, &QTimer::timeout, this, [this] {
        persistQueueCursor();
        Q_EMIT changed();
    });
    m_positionNotice.setSingleShot(true);
    m_positionNotice.setInterval(100);
    connect(&m_positionNotice, &QTimer::timeout, this, [this] {
        if (!std::exchange(m_positionNoticePending, false))
            return;
        Q_EMIT changed();
        m_positionNotice.start();
    });
    m_trackTimer.setInterval(2000);
    connect(&m_trackTimer, &QTimer::timeout, this, &PlayerController::refreshTracks);
    // The group did not bring its next item: the playback ends as it would alone
    m_groupAdvance.setSingleShot(true);
    m_groupAdvance.setInterval(10'000);
    connect(&m_groupAdvance, &QTimer::timeout, this, [this] {
        if (!std::exchange(m_awaitingGroup, false))
            return;
        // The player closes, so the group no longer waits for this session
        if (m_group)
            m_group->stopFollowing();
        clearCurrent();
        Q_EMIT finished();
        Q_EMIT changed();
    });
    loadMemory();
}

PlayerController::~PlayerController()
{
    m_session.removeObserver(m_observerId);
    onAccountLeaving();
    // A renderer's Stop still on its way leaves before its network manager goes: a quit stops the TV too, within a bounded wait
    if (m_cast->active()) {
        QEventLoop wait;
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(m_cast, &CastController::stopped, &wait, &QEventLoop::quit);
        connect(m_cast, &CastController::failed, &wait, &QEventLoop::quit);
        connect(&deadline, &QTimer::timeout, &wait, &QEventLoop::quit);
        deadline.start(2000);
        wait.exec(QEventLoop::ExcludeUserInputEvents);
    }
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
    if (m_casting) {
        m_engine.detach(item, false);
        disconnect(item, &QQuickItem::windowChanged, this, nullptr);
        m_mpvSettings.setWindow(nullptr);
        m_video = nullptr;
        return;
    }
    // This is also the window-close path: the renderer must go before its
    // handle's worker thread. Stopped is queued once by the state observer.
    m_stopping = true;
    m_client.cancelPlaybackInfo();
    m_loading = false;
    m_trackTimer.stop();
    persistQueueCursor();
    reportLocalPosition(false);
    m_queue.setCurrentPlaybackItemId({});
    m_engine.detach(item);
    disconnect(item, &QQuickItem::windowChanged, this, nullptr);
    m_mpvSettings.setWindow(nullptr);
    m_video = nullptr;
    clearCurrent();
    Q_EMIT changed();
}

void PlayerController::videoRendererChanged(MpvAbstractItem *item)
{
    if (item && item == m_video)
        m_engine.restoreVideo();
}

void PlayerController::onAccountLeaving()
{
    // A streamed source needs the token: nothing outlives the account
    cancelClipExport();
    dropClipMarks();
    // The end-file comes after the local flag is gone: the position is stored now
    reportLocalPosition(false);
    m_pendingLocal = {};
    if (m_localPlayback) {
        m_localPlayback = false;
        Q_EMIT localPlaybackChanged();
    }
    persistQueueCursor();
    m_stopping = true;
    closeCast(true);
    clearCastTarget();
    m_castFailed = false;
    m_awaitingGroup = false;
    m_groupAdvance.stop();
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
    dropPendingSubtitle();
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

bool PlayerController::playItemWithStreams(const QString &id, qint64 resumeTicks,
                                           int audioStreamIndex, int subtitleStreamIndex)
{
    if (audioStreamIndex < -1 || audioStreamIndex > 1024 || subtitleStreamIndex < -2
        || subtitleStreamIndex > 1024)
        return false;
    if (grouped())
        return playItem(id, resumeTicks);
    m_pendingStreams = PendingStreams{id, audioStreamIndex, subtitleStreamIndex};
    if (!playItem(id, resumeTicks)) {
        m_pendingStreams.reset();
        return false;
    }
    return true;
}

bool PlayerController::castTarget() const
{
    return !m_castTarget.isEmpty() && m_castTarget != QLatin1String("local");
}

bool PlayerController::castTo(const QString &udn)
{
    if (grouped())
        return false;
    if (udn.isEmpty() || udn == QLatin1String("local")) {
        // This Computer: a cast comes back at the renderer's position, a failed one from where the device got to
        if (m_castFailed && !m_itemId.isEmpty()) {
            m_castFailed = false;
            return startItem(m_itemId, m_resumeTicks);
        }
        const bool wasCasting = m_casting;
        const bool negotiating = !m_casting && m_loading && castTarget() && !m_itemId.isEmpty();
        const qint64 position = positionTicks();
        clearCastTarget();
        // The source on its way was asked for the renderer: this computer asks again
        if (negotiating)
            return startItem(m_itemId, m_resumeTicks);
        if (!wasCasting || m_itemId.isEmpty()) {
            Q_EMIT changed();
            return true;
        }
        closeCast(true);
        return startItem(m_itemId, std::max<qint64>(0, position));
    }
    // A downloaded file has no server stream a renderer could fetch
    if (!m_cast->hasTarget(udn) || m_localPlayback)
        return false;
    if (castTarget() && udn == m_castTarget && (m_casting || m_loading))
        return true;
    const qint64 position = positionTicks();
    // The streams chosen here go along: the renderer gets the same audio and the subtitle burnt in
    bool otherAudio = false;
    if (!m_casting && !m_itemId.isEmpty()) {
        m_requestedStreams.audioStreamIndex = m_selectedAudioIndex;
        // A subtitle that is no server stream (a file added here) cannot go to the device: no stale choice of an earlier item is kept for it
        m_requestedStreams.subtitleStreamIndex = m_selectedSubtitleIndex ? m_selectedSubtitleIndex
                                                 : (m_subtitlesOff || m_selectedSubtitleId != 0)
                                                     ? std::optional<int>(-1)
                                                     : m_requestedStreams.subtitleStreamIndex;
        const StreamSummary *track = pickStream(m_source.media.source, u"Audio");
        otherAudio = m_selectedAudioIndex && track && track->index != *m_selectedAudioIndex;
    }
    // A failed cast's position is where the device got to
    const qint64 start = m_castFailed ? m_resumeTicks : position;
    closeCast(true);
    m_castTarget = udn;
    m_castName = m_cast->nameFor(udn);
    // A renderer plays a direct file's default audio: another track needs the server to pick it
    m_castTranscodeOnly = otherAudio;
    m_castFailed = false;
    if (m_itemId.isEmpty()) {
        Q_EMIT changed();
        return true;
    }
    return startItem(m_itemId, std::max<qint64>(0, start));
}

bool PlayerController::playItemOn(const QString &udn, const QString &id, qint64 resumeTicks)
{
    if (!isValidJellyfinId(id) || resumeTicks < 0 || grouped())
        return false;
    if (udn.isEmpty() || udn == QLatin1String("local")) {
        // playItem keeps the queue's cursor where the cast got to, then startItem closes the cast
        clearCastTarget();
        return playItem(id, resumeTicks);
    }
    if (!m_cast->hasTarget(udn))
        return false;
    m_castTarget = udn;
    m_castName = m_cast->nameFor(udn);
    m_castTranscodeOnly = false;
    m_castFailed = false;
    if (playItem(id, resumeTicks))
        return true;
    clearCastTarget();
    return false;
}

void PlayerController::closeCast(bool stopRenderer)
{
    m_castSeek.stop();
    m_castTrackPending = false;
    if (!m_casting)
        return;
    // Still casting while the session closes: the Stopped carries the renderer's position and method
    if (m_session.state() != PlaybackSession::State::Idle) {
        m_session.stop();
        m_session.stopped();
    }
    if (stopRenderer)
        m_cast->stop();
    m_casting = false;
}

void PlayerController::clearCastTarget()
{
    m_castTarget.clear();
    m_castName.clear();
    m_castTranscodeOnly = false;
}

void PlayerController::castFailed(const QString &message)
{
    // The item stays with its error: Continue Here plays it on this computer from where the device got to, Close Player ends it
    if (m_casting) {
        m_resumeTicks = std::max<qint64>(0, m_castPositionMs) * 10'000;
        if (m_session.state() != PlaybackSession::State::Idle) {
            m_session.failed();
            m_session.stop();
            m_session.stopped();
        }
        m_casting = false;
    }
    m_loading = false;
    clearCastTarget();
    m_castFailed = true;
    m_errorText = message;
    Q_EMIT changed();
}

void PlayerController::stopCasting()
{
    stopLocally();
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
    m_castFailed = false;
    // A server item that replaces a local file: the local one stores where it stopped before its flag goes
    reportLocalPosition(false);
    m_pendingLocal = {};
    m_localSubtitles.clear();
    if (m_localPlayback) {
        m_localPlayback = false;
        Q_EMIT localPlaybackChanged();
    }
    const bool sameItem = id == m_itemId;
    if (!sameItem) {
        m_subtitlesOff = false;
        m_itemRuntimeTicks = 0;
        m_castTranscodeOnly = false;
    }
    // Another item starts at full quality; the same one again (a reopen, Repeat One) keeps a lowered bit rate
    dropFallback();
    m_canRetry = false;
    m_failedSwitchManual = false;
    m_switchTimedOut = false;
    // A reopen ends the wait for a switched stream to play on; a switch that reopens starts its own
    if (std::exchange(m_switchSettleTicks, -1) >= 0) {
        m_switchTimeout.stop();
        m_switchManual = false;
    }
    // A group plays the group's stream: never at a cap this session lowered on its own, nor at the user's pin
    if (!sameItem || grouped) {
        m_fallback.reset();
        m_preparedSource.reset();
        m_switchingStream = false;
        m_switchTimeout.stop();
        m_switchManual = false;
        // The streams a lowered request asked for are the old item's: another item picks its own
        m_requestedStreams = {};
        m_detailsStreams.reset();
        m_requestedCap = UnlimitedBitrate;
    }
    if (m_pendingStreams && m_pendingStreams->itemId == id) {
        if (m_pendingStreams->audio >= 0)
            m_requestedStreams.audioStreamIndex = m_pendingStreams->audio;
        if (m_pendingStreams->subtitle >= -1)
            m_requestedStreams.subtitleStreamIndex = m_pendingStreams->subtitle;
    } else if (m_pendingStreams) {
        m_pendingStreams.reset();
    }
    if (grouped)
        m_manualCap = 0;
    // Preserve the prior item's cursor before the new id replaces it.
    if (!m_itemId.isEmpty() && !sameItem)
        persistQueueCursor();
    // A new item on a cast closes the old item's server session while its method and position are still known, and stops the renderer, so a new item that fails cannot leave the old one playing there
    closeCast(true);
    // A group plays its items here, at its own pace
    if (grouped)
        clearCastTarget();
    // A different item cannot be opened until the old mpv end-file arrived.
    // The pending id survives that stop and is negotiated exactly once.
    m_client.cancelPlaybackInfo();
    cancelSeriesRequest();
    m_seriesPlaylistWanted = false;
    m_groupItem = grouped;
    // A loop does not run in a group: it ends here
    applyClipLoop();
    m_awaitingGroup = false;
    m_groupAdvance.stop();
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
    setSegments({});
    m_source = PlaybackSource{};
    // The same item again (a repeat) keeps the aspect and the rate the user chose; a group plays at 1.0
    m_keptRate.reset();
    if (sameItem && !grouped)
        m_keptRate = m_engine.playbackRate();
    else
        m_mpvSettings.setAspectOverride({});
    // A reopen of the same item (a quality, an audio or a subtitle choice, a retry) keeps what the viewer set for it; another item starts from the settings
    if (!sameItem) {
        resetForNextItem();
    }
    m_externalSubtitles.clear();
    m_subtitleOptions.clear();
    dropPendingSubtitle();
    // A download of this item still waiting for the server survives its reopen (a fallback step, a pick, Try Again): its answer must still find it
    const QString downloading = sameItem ? m_remoteSubtitleDownloadItem : QString();
    resetRemoteSubtitles();
    m_remoteSubtitleDownloadItem = downloading;
    m_reopenPaused = false;
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
    // Clips belong to the item that made them
    if (!sameItem) {
        cancelClipExport();
        dropClipMarks();
        m_localSourcePath.clear();
    }
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    m_audioFacts.clear();
    m_subtitleFacts.clear();
    m_selectedAudioIndex.reset();
    m_selectedSubtitleIndex.reset();
    m_selectedAudioId = 0;
    m_selectedSubtitleId = 0;
    m_secondarySubtitleId = 0;
    // A same-item reopen keeps the secondary it played and finds it again by its stream index
    if (!sameItem)
        m_secondaryStreamIndex.reset();
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

bool PlayerController::canManageSubtitles() const
{
    // A downloaded file has no server source to fetch a subtitle into, and a group member cannot reopen its item alone without leaving the group's timing
    return !m_localPlayback && !grouped() && m_client.canManageSubtitles();
}

bool PlayerController::searchRemoteSubtitles(const QString &language)
{
    // A new search replaces one in flight: the older reply no longer matches the query and is dropped
    if (m_itemId.isEmpty() || !canManageSubtitles())
        return false;
    m_remoteSubtitles.clear();
    m_remoteSubtitleSearching = true;
    m_remoteSubtitleQuery = m_itemId + QLatin1Char('/') + language;
    m_remoteSubtitleLanguage = language;
    m_remoteSubtitleError.clear();
    Q_EMIT remoteSubtitlesChanged();
    if (!m_client.searchRemoteSubtitles(m_itemId, language)) {
        m_remoteSubtitleSearching = false;
        m_remoteSubtitleQuery.clear();
        m_remoteSubtitleError = QStringLiteral("failed");
        Q_EMIT remoteSubtitlesChanged();
        return false;
    }
    return true;
}

bool PlayerController::downloadRemoteSubtitle(const QString &subtitleId)
{
    // One download at a time: each success reopens the item
    if (!m_remoteSubtitleDownloadItem.isEmpty() || m_itemId.isEmpty() || !canManageSubtitles()
        || !m_client.downloadRemoteSubtitle(m_itemId, subtitleId, m_remoteSubtitleLanguage))
        return false;
    m_remoteSubtitleDownloadItem = m_itemId;
    return true;
}

bool PlayerController::remoteSearchIsCurrent(const QString &itemId, const QString &language) const
{
    return m_remoteSubtitleSearching && itemId == m_itemId
           && m_remoteSubtitleQuery == itemId + QLatin1Char('/') + language;
}

void PlayerController::resetRemoteSubtitles()
{
    m_remoteSubtitles.clear();
    m_remoteSubtitleSearching = false;
    m_remoteSubtitleQuery.clear();
    m_remoteSubtitleLanguage.clear();
    m_remoteSubtitleDownloadItem.clear();
    m_remoteSubtitleError.clear();
    Q_EMIT remoteSubtitlesChanged();
}

void PlayerController::reportLocalPosition(bool ended)
{
    // While a local file waits for the previous item to end, the current id and state are still that item's
    if (!m_localPlayback || m_pendingLocal.pending || !m_fileLoaded || m_itemId.isEmpty())
        return;
    // Before mpv told a position the stored resume point stays
    const std::optional<qint64> known = m_engine.knownPositionTicks();
    if (!known)
        return;
    const qint64 ticks = *known;
    const qint64 duration = m_engine.durationTicks();
    // Watched at the end or past 90 %, as the server counts a played item; a watched item starts again from the beginning
    const bool played = ended || (duration > 0 && ticks >= duration / 10 * 9);
    Q_EMIT localPositionChanged(m_itemId, played ? 0 : ticks, played);
}

namespace {
// The limit and the extensions are the downloads' own (DownloadsController::MaxSubtitleTracks, subtitleExtension)
constexpr int MaxLocalSubtitles = 8;
constexpr int MaxAddedSubtitles = 8;

// The chapters a download stored, with the caps of the server's own: the same parser reads them
QList<ChapterSummary> localChapters(const QVariantList &entries)
{
    QJsonArray chapters;
    for (const QVariant &entry : entries) {
        const QVariantMap map = entry.toMap();
        chapters.append(
            QJsonObject{{QStringLiteral("Name"), map.value(QStringLiteral("name")).toString()},
                        {QStringLiteral("StartPositionTicks"),
                         map.value(QStringLiteral("startTicks")).toLongLong()}});
    }
    return parseChapters(chapters);
}

// Only regular files of the download's own `subs` folder, with a text subtitle extension
QList<PlayerController::LocalSubtitle> localSubtitles(const QVariantList &entries,
                                                      const QFileInfo &media)
{
    static const QRegularExpression language(
        QRegularExpression::anchoredPattern(QStringLiteral("[a-z]{2,3}")));
    static const QStringList extensions{QStringLiteral("srt"), QStringLiteral("ass"),
                                        QStringLiteral("ssa"), QStringLiteral("vtt")};
    // Resolved, so a `subs` folder that is a link elsewhere is not the download's own
    const QString folder = media.canonicalPath() + QStringLiteral("/subs");
    QList<PlayerController::LocalSubtitle> result;
    for (const QVariant &entry : entries) {
        if (result.size() == MaxLocalSubtitles)
            break;
        const QVariantMap map = entry.toMap();
        const QFileInfo file(map.value(QStringLiteral("path")).toString());
        if (!file.isAbsolute() || !file.isFile() || file.isSymLink()
            || QFileInfo(file.canonicalFilePath()).path() != folder
            || !extensions.contains(file.suffix()))
            continue;
        const QString lang = map.value(QStringLiteral("language")).toString();
        result.append({file.canonicalFilePath(),
                       map.value(QStringLiteral("title")).toString().simplified().left(120),
                       language.match(lang).hasMatch() ? lang : QString()});
    }
    return result;
}
} // namespace

bool PlayerController::playLocalFile(const QString &path, const QString &itemId,
                                     const QString &title, const QString &subtitle,
                                     qint64 resumeTicks, const QVariantMap &extras)
{
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.isFile() || info.isSymLink() || !isValidJellyfinId(itemId)
        || resumeTicks < 0 || grouped())
        return false;
    // A stored point past the cap the queue uses never makes the file unplayable
    resumeTicks = std::min(resumeTicks, 24LL * 3600 * TicksPerSecond);
    // The interrupted queue item keeps its real position: once loading, the cursor would fall back to the pending resume point
    if (!m_localPlayback)
        persistQueueCursor();
    // A local file that replaces another one stores where that one stopped
    reportLocalPosition(false);
    // Its clips are the file's own: the marks of the previous item do not carry over
    cancelClipExport();
    dropClipMarks();
    m_localSourcePath = path;
    m_pendingLocal.path = path;
    m_pendingLocal.itemId = itemId;
    m_pendingLocal.title = title.left(512);
    m_pendingLocal.subtitle = subtitle.left(512);
    m_pendingLocal.resumeTicks = resumeTicks;
    m_pendingLocal.chapters = localChapters(extras.value(QStringLiteral("chapters")).toList());
    m_pendingLocal.subtitles =
        localSubtitles(extras.value(QStringLiteral("subtitles")).toList(), info);
    m_pendingLocal.pending = true;
    m_loading = true;
    m_errorText.clear();
    if (!m_localPlayback) {
        m_localPlayback = true;
        Q_EMIT localPlaybackChanged();
    }
    if (m_session.state() == PlaybackSession::State::Idle)
        openLocal(); // the engine may attach with the player page, which opens it
    else
        m_engine.stop(); // the end of the previous item opens the local one
    return true;
}

bool PlayerController::openLocal()
{
    if (!m_pendingLocal.pending)
        return false;
    if (!m_engine.isInitialized() || m_session.state() != PlaybackSession::State::Idle)
        return false;
    const PendingLocal request = std::exchange(m_pendingLocal, {});
    m_client.cancelPlaybackInfo();
    cancelSeriesRequest();
    dropSeriesPlaylist();
    // A local file never falls back
    dropFallback();
    m_fallback.reset();
    m_preparedSource.reset();
    m_switchingStream = false;
    m_switchSettleTicks = -1;
    m_switchTimeout.stop();
    m_switchManual = false;
    m_manualCap = 0;
    m_requestedStreams = {};
    m_detailsStreams.reset();
    m_requestedCap = UnlimitedBitrate;
    m_canRetry = false;
    m_seriesPlaylistWanted = false;
    m_groupItem = false;
    m_awaitingGroup = false;
    m_groupAdvance.stop();
    m_mpvSettings.setSpeedHeld(false);
    m_queue.setCurrentPlaybackItemId({});
    m_playlist.clear();
    m_queueIndex = -1;
    if (!m_itemSeriesId.isEmpty()) {
        m_itemSeriesId.clear();
        Q_EMIT seriesChoicesChanged();
    }
    m_seriesApplied = false;
    m_rateDecided = false;
    m_subtitleOptions.clear();
    m_externalSubtitles.clear();
    dropPendingSubtitle();
    m_pendingSecondaryFile.clear();
    m_loadedSubtitles.clear();
    m_loadedSubtitleOrder.clear();
    m_subtitlesOff = false;
    resetRemoteSubtitles();
    m_reopenPaused = false;
    m_skipLabel.clear();
    m_skipEnd = 0;
    // Nothing of a server item played before stays: its source, rate, aspect and trickplay tiles
    m_source = PlaybackSource{};
    m_keptRate.reset();
    m_mpvSettings.setAspectOverride({});
    m_requestedFrame.reset();
    m_lastTile = {};
    m_cachedTileIndex = -1;
    m_pendingTileIndex = -1;
    clearPreview();
    m_selectedAudioId = 0;
    m_selectedSubtitleId = 0;
    m_secondarySubtitleId = 0;
    m_selectedAudioIndex.reset();
    m_selectedSubtitleIndex.reset();
    m_secondaryStreamIndex.reset();
    m_itemId = request.itemId;
    m_title = request.title;
    m_subtitle = request.subtitle;
    m_primaryTag.clear();
    m_resumeTicks = request.resumeTicks;
    m_resumedFrom = request.resumeTicks;
    m_loading = false;
    m_fileLoaded = false;
    m_stopping = false;
    m_errorText.clear();
    m_playlist.clear();
    m_queueIndex = -1;
    setSegments({});
    resetForNextItem();
    m_trickplay.reset();
    m_trickplayInfo = {};
    m_itemChapters = request.chapters;
    updateChapters();
    m_localSubtitles = request.subtitles;
    resetUpNext();
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    m_audioFacts.clear();
    m_subtitleFacts.clear();
    Q_EMIT tracksChanged();
    m_mpvSettings.apply();
    PlaybackSource source;
    source.itemId = request.itemId;
    source.mediaSourceId = QStringLiteral("local");
    source.playSessionId = QStringLiteral("local");
    source.method = PlaybackSource::Method::Local;
    source.url = QUrl::fromLocalFile(request.path);
    if (!m_engine.open(source, false)) {
        m_errorText = i18n("The downloaded file could not be opened.");
        Q_EMIT changed();
        return false;
    }
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
    if (m_pendingLocal.pending) {
        openLocal();
        return;
    }
    if (!m_loading || m_itemId.isEmpty() || m_session.state() != PlaybackSession::State::Idle)
        return;
    if (castTarget()) {
        if (!m_cast->hasTarget(m_castTarget)) {
            castFailed(i18n("The device is no longer available: search for it again from Cast."));
            return;
        }
        const QJsonObject profile =
            m_cast->deviceProfile(m_castTarget, castBurnIn(), m_castTranscodeOnly);
        if (!m_client.requestPlaybackInfo(m_itemId, m_resumeTicks, profile, UnlimitedBitrate,
                                          m_requestedStreams)) {
            m_loading = false;
            m_errorText = i18n("Unable to request playback from the server.");
            Q_EMIT changed();
        }
        return;
    }
    if (!m_engine.isInitialized())
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
    // A lowered stream the server already offered opens without a second request
    if (std::optional<PlaybackSource> prepared = std::exchange(m_preparedSource, std::nullopt);
        prepared && prepared->itemId == m_itemId) {
        onSource(*prepared);
        return;
    }
    // At a lowered cap a reopen (a subtitle download, Repeat One, Try Again) asks for the streams the lowered one had: a transcode carries one audio track only
    m_requestedCap = effectiveCap();
    if (m_requestedCap >= UnlimitedBitrate)
        m_requestedStreams = {};
    // A repeat or another reopen of the same item keeps the tracks the details page picked
    if (m_detailsStreams && m_detailsStreams->itemId == m_itemId) {
        if (m_detailsStreams->audio >= 0)
            m_requestedStreams.audioStreamIndex = m_detailsStreams->audio;
        if (m_detailsStreams->subtitle >= -1)
            m_requestedStreams.subtitleStreamIndex = m_detailsStreams->subtitle;
    }
    if (m_pendingStreams && m_pendingStreams->itemId == m_itemId) {
        if (m_pendingStreams->audio >= 0)
            m_requestedStreams.audioStreamIndex = m_pendingStreams->audio;
        if (m_pendingStreams->subtitle >= -1)
            m_requestedStreams.subtitleStreamIndex = m_pendingStreams->subtitle;
    }
    const QJsonObject profile = buildDeviceProfile(m_settings.deviceProfileOptions());
    if (!m_client.requestPlaybackInfo(m_itemId, m_resumeTicks, profile, m_requestedCap,
                                      m_requestedStreams)) {
        m_loading = false;
        m_errorText = i18n("Unable to request playback from the server.");
        Q_EMIT changed();
    }
}

bool PlayerController::fallbackApplies() const
{
    return m_manualCap == 0
           && m_settings.value(QStringLiteral("video"), QStringLiteral("adaptive_bitrate")).toBool()
           && !m_localPlayback && m_source.method != PlaybackSource::Method::Local && !m_groupItem
           && !grouped() && !m_itemId.isEmpty() && !m_casting && !castTarget();
}

bool PlayerController::fallbackEligible() const
{
    const PlaybackSession::State state = m_session.state();
    if (!fallbackApplies() || m_qualitySwitching || m_loading || m_stopping || m_engine.seeking()
        || (state != PlaybackSession::State::Playing && state != PlaybackSession::State::Buffering))
        return false;
    // Near the end a reopen would cost a play for little: the 90 % the played mark uses, and the Up Next card
    const qint64 duration = m_engine.durationTicks();
    if (duration > 0 && positionTicks() >= duration / 10 * 9)
        return false;
    return !m_upNextShown && !m_upNextCancelled;
}

namespace {
// A direct play sends the file as it is, whatever cap was asked: only a transcode plays at its target or at the cap
std::optional<qint64> sourceBitrate(const PlaybackSource &source, qint64 requestedCap)
{
    if (source.method == PlaybackSource::Method::Direct)
        return streamBitrate(0, source.media.source.bitrate, UnlimitedBitrate);
    return streamBitrate(source.media.targetBitrate, source.media.source.bitrate, requestedCap);
}
} // namespace

std::optional<qint64> PlayerController::currentStreamBitrate() const
{
    if (!fallbackApplies())
        return std::nullopt;
    return sourceBitrate(m_source, m_requestedCap);
}

JellyfinClient::StreamChoice PlayerController::currentStreams() const
{
    JellyfinClient::StreamChoice streams = m_requestedStreams;
    streams.mediaSourceId = m_source.mediaSourceId;
    // A direct play knows the server's indexes of the tracks that play; a transcode keeps what its request asked for, unless a server's external subtitle was loaded since
    if (directPlay()) {
        if (m_selectedAudioIndex)
            streams.audioStreamIndex = m_selectedAudioIndex;
        // A subtitle file added by hand plays here: the server must not burn one of its own in under it
        streams.subtitleStreamIndex =
            m_selectedSubtitleId == 0 || !m_selectedAddedSubtitle.isEmpty()
                ? std::optional<int>(-1)
                : m_selectedSubtitleIndex;
    } else if (m_selectedSubtitleIndex) {
        streams.subtitleStreamIndex = m_selectedSubtitleIndex;
    } else if (m_subtitlesOff || !m_selectedAddedSubtitle.isEmpty()) {
        // No track selected in a transcode may also be a burned-in subtitle: only the user's own choice means none
        streams.subtitleStreamIndex = -1;
    }
    return streams;
}

qint64 PlayerController::effectiveCap() const
{
    return m_manualCap > 0 ? m_manualCap : m_fallback.cap();
}

bool PlayerController::requestStreamChange(qint64 cap, bool manual)
{
    if (m_itemId.isEmpty() || m_localPlayback || m_groupItem || grouped() || m_loading || m_stopping
        || m_engine.seeking() || !m_client.isAuthenticated())
        return false;
    if (manual && (m_qualitySwitching || !qualityAvailable()))
        return false;
    const PlaybackSession::State state = m_session.state();
    if (!manual && state != PlaybackSession::State::Playing
        && state != PlaybackSession::State::Buffering)
        return false;
    const JellyfinClient::StreamChoice streams = currentStreams();
    const qint64 at = m_engine.knownPositionTicks().value_or(m_resumedFrom);
    const QJsonObject profile = buildDeviceProfile(m_settings.deviceProfileOptions());
    // The new request replaces one still on its way (the client keeps one): a fallback step the user's choice overtakes is cancelled, and nothing changes when the request is refused
    if (!m_client.requestPlaybackInfo(m_itemId, at, profile, cap, streams))
        return false;
    if (manual)
        m_fallback.cancelPending();
    m_fallbackRequest = {m_source.playSessionId, cap, streams, true, manual};
    if (manual) {
        m_qualitySwitching = true;
        Q_EMIT changed();
    }
    return true;
}

bool PlayerController::qualityAvailable() const
{
    return active() && !m_localPlayback && m_source.method != PlaybackSource::Method::Local
           && !m_groupItem && !grouped() && m_client.canTranscode();
}

QVariantList PlayerController::qualitySteps() const
{
    QVariantList steps;
    const auto label = [](qint64 cap) {
        const double mbps = double(cap) / 1'000'000.0;
        const int decimals = mbps == std::floor(mbps) ? 0 : 1;
        return i18nc("@item:inmenu maximum bit rate in Mbit/s", "%1 Mbps",
                     QString::number(mbps, 'f', decimals));
    };
    QString automatic = i18n("Automatic");
    if (m_fallback.cap() < UnlimitedBitrate)
        automatic = i18nc("@item:inmenu automatic quality lowered on a slow connection",
                          "Automatic (%1 Mbps)", label(m_fallback.cap()));
    steps.append(QVariantMap{{QStringLiteral("cap"), 0},
                             {QStringLiteral("text"), automatic},
                             {QStringLiteral("checked"), m_manualCap == 0}});
    // The file's own bit rate bounds the steps, not the transcode playing now: a lower pin must still offer the steps above it
    const qint64 source = m_source.media.source.bitrate;
    for (qint64 cap : FallbackLadder) {
        // A pin made on an earlier item stays listed, checked, even above this file's bit rate
        if (source > 0 && cap >= source && cap != m_manualCap)
            continue;
        steps.append(QVariantMap{{QStringLiteral("cap"), cap},
                                 {QStringLiteral("text"), label(cap)},
                                 {QStringLiteral("checked"), m_manualCap == cap}});
    }
    return steps;
}

void PlayerController::setQuality(qint64 cap)
{
    if (!qualityAvailable() || m_qualitySwitching || cap < 0 || cap > UnlimitedBitrate)
        return;
    if (cap == m_manualCap)
        return;
    // Automatic on a direct play that already runs unlimited: only the pin changes
    if (cap == 0 && m_fallback.cap() >= UnlimitedBitrate
        && m_source.method == PlaybackSource::Method::Direct) {
        m_manualCap = 0;
        Q_EMIT changed();
        return;
    }
    if (!requestStreamChange(cap == 0 ? UnlimitedBitrate : cap, true)) {
        m_qualitySwitching = false;
        Q_EMIT changed();
        Q_EMIT qualityChangeFailed();
    }
}

void PlayerController::failStreamSwitch()
{
    // A manual step that fails returns to the pin before it (or Automatic); the fallback keeps its own budget
    const bool manual = std::exchange(m_switchManual, false);
    if (manual)
        m_manualCap = m_manualCapBeforeSwitch;
    else
        m_fallback.exhaust();
    m_failedSwitchManual = manual;
    m_switchingStream = false;
    m_switchSettleTicks = -1;
    m_switchTimeout.stop();
    m_canRetry = true;
    m_errorText =
        manual
            ? i18n("The selected quality could not be opened. Try again at the previous quality.")
            : i18n("The lower quality stream could not be opened. Try again at the previous "
                   "quality.");
    Q_EMIT changed();
}

void PlayerController::settleSwitch()
{
    m_switchSettleTicks = -1;
    m_switchTimeout.stop();
    m_switchManual = false;
}

void PlayerController::setQualityBitrate(qint64 bitsPerSecond)
{
    // The file's own bit rate, not a transcode's target: a remote asking at least that wants no limit
    const qint64 file = m_source.media.source.bitrate;
    if (bitsPerSecond <= 0 || (file > 0 && bitsPerSecond >= file)) {
        setQuality(0);
        return;
    }
    qint64 chosen = 0;
    for (qint64 step : FallbackLadder) {
        if (step <= bitsPerSecond) {
            chosen = step;
            break;
        }
    }
    setQuality(chosen > 0 ? chosen : FallbackLadder.back());
}

void PlayerController::dropFallback()
{
    if (!std::exchange(m_fallbackRequest.active, false))
        return;
    m_fallbackRequest = {};
    m_fallback.cancelPending();
    // A manual pick whose answer will never come: the menu takes picks again (the callers emit `changed`)
    m_qualitySwitching = false;
}

void PlayerController::onStarving(qint64 cap)
{
    if (!fallbackEligible()) {
        m_fallback.cancelPending();
        return;
    }
    // Playback goes on untouched while the server is asked: a refusal never interrupts it
    if (!requestStreamChange(cap, false)) {
        m_fallback.exhaust();
        Q_EMIT qualityUnavailable();
    }
}

void PlayerController::acceptFallback(const PlaybackSource &source)
{
    if (!m_fallbackRequest.active)
        return;
    const FallbackRequest request = std::exchange(m_fallbackRequest, {});
    // Whatever answer this is, no pick of the user waits any longer
    if (std::exchange(m_qualitySwitching, false))
        Q_EMIT changed();
    // Only for the stream it was asked from, and only if it still may switch: for the fallback another item, a reopen, a pause, a seek, Up Next or the 90 % mark since then drops it, with the budget kept; a pick, a group joined or a stop on its way
    if (source.itemId != m_itemId || m_source.playSessionId != request.playSessionId
        || (!request.manual && !fallbackEligible())
        || (request.manual && (!qualityAvailable() || m_stopping))) {
        m_fallback.cancelPending();
        return;
    }
    const qint64 pin = request.cap >= UnlimitedBitrate ? 0 : request.cap;
    if (request.manual) {
        // The URLs differ by their play session: the same media source played directly is the same file
        const bool same = source.method == PlaybackSource::Method::Direct
                          && m_source.method == PlaybackSource::Method::Direct
                          && source.mediaSourceId == m_source.mediaSourceId;
        if (same) {
            // The server offered what already plays: the pin is enough
            m_manualCap = pin;
            Q_EMIT changed();
            return;
        }
    } else {
        const std::optional<qint64> now = currentStreamBitrate();
        const std::optional<qint64> next = sourceBitrate(source, request.cap);
        if (!now || !next || *next >= *now) {
            qCInfo(playerLog) << "the server offered no lower stream";
            m_fallback.exhaust();
            return;
        }
    }
    const qint64 at = m_engine.knownPositionTicks().value_or(m_resumedFrom);
    // The reopen cancels a series listing still on its way, which a slow link makes likely: the item's reply asks for it again
    const bool relist = m_seriesPlaylistWanted || !m_seriesListItem.isEmpty();
    // A manual pick may come while paused: the new stream opens paused too
    const bool paused = m_session.state() == PlaybackSession::State::Paused;
    // The same item again at the same position: the stop reports the old stream, the reopen the new one. That Stopped then Start costs one extra play count on the server per step (up to `BitrateFallback::MaxSteps`); reporting the new ids as Progress instead would need `DELETE /Videos/ActiveEncodings` to close the old job, which is unverified on 12.4
    if (!startItem(m_itemId, at, false))
        return;
    m_reopenPaused = paused;
    m_seriesPlaylistWanted = relist;
    // Committed once the reopen runs; its request is the prepared source, opened at Idle
    if (request.manual) {
        m_manualCapBeforeSwitch = m_manualCap;
        m_manualCap = pin;
        if (pin == 0)
            m_fallback.reset();
    } else {
        m_fallback.stepTaken(request.cap);
    }
    m_capBeforeStep = m_requestedCap;
    m_requestedCap = request.cap;
    m_requestedStreams = request.streams;
    m_retryTicks = at;
    m_preparedSource = source;
    m_switchManual = request.manual;
    m_switchingStream = true;
}

bool PlayerController::retryPlayback()
{
    if (!m_canRetry || m_itemId.isEmpty())
        return false;
    m_canRetry = false;
    m_switchTimeout.stop();
    m_switchTimedOut = false;
    m_switchManual = false;
    const qint64 previous = m_capBeforeStep;
    if (std::exchange(m_failedSwitchManual, false)) {
        // The restored pin opens as it was; Automatic gets back the fallback's step that the failed pick had cleared
        if (m_manualCap == 0 && m_fallback.cap() != previous) {
            m_fallback.reset();
            if (previous < UnlimitedBitrate)
                m_fallback.stepTaken(previous);
        }
    } else {
        // The bit rate before the step that failed, and no further step for this item
        m_fallback.reset();
        if (previous < UnlimitedBitrate)
            m_fallback.stepTaken(previous);
        m_fallback.exhaust();
    }
    return startItem(m_itemId, m_retryTicks, false);
}

void PlayerController::onSource(const PlaybackSource &source)
{
    if (!m_loading) {
        acceptFallback(source);
        return;
    }
    if (m_stopping || source.itemId != m_itemId)
        return;
    if (castTarget()) {
        startCast(source);
        return;
    }
    if (!m_engine.isInitialized() || m_session.state() != PlaybackSession::State::Idle)
        return;
    m_loading = false;
    m_source = source;
    m_subtitleOptions = source.externalSubtitles;
    // A new file drops the subtitle files mpv had added to the old one
    m_loadedSubtitles.clear();
    m_loadedSubtitleOrder.clear();
    m_externalSubtitles.clear();
    for (const ExternalSubtitle &entry : m_subtitleOptions)
        m_externalSubtitles.append(QVariantMap{
            {QStringLiteral("index"), entry.index},
            {QStringLiteral("name"),
             entry.label.isEmpty() ? i18n("External subtitle %1").arg(entry.index) : entry.label}});
    Q_EMIT tracksChanged();
    if (!m_engine.open(source, m_groupItem || std::exchange(m_reopenPaused, false))) {
        // A lowered stream that cannot even start offers Try Again, as one that fails while opening does
        if (m_switchingStream)
            failStreamSwitch();
        else
            m_errorText = i18n("The selected media source could not be opened.");
        Q_EMIT changed();
        return;
    }
    if (m_switchingStream)
        m_switchTimeout.start();
    refreshSegments();
    updateTrickplayLayout();
    Q_EMIT changed();
}

bool PlayerController::castBurnIn() const
{
    // A target that takes a text track gets the profile that leaves the choice to the server: text formats stay external for the track, image formats are encoded into the picture
    if (m_cast->takesTextTrack(m_castTarget))
        return false;
    // A DLNA renderer takes no track: the server burns the chosen subtitle in
    const std::optional<int> index = m_requestedStreams.subtitleStreamIndex;
    return index && *index >= 0;
}

std::optional<CastTextTrack> PlayerController::castTextTrack(qint64 startTicks) const
{
    if (m_castTarget.isEmpty() || !m_cast->takesTextTrack(m_castTarget))
        return std::nullopt;
    const std::optional<int> index = m_requestedStreams.subtitleStreamIndex;
    if (!index || *index < 0)
        return std::nullopt;
    for (const StreamSummary &stream : m_source.media.source.streams) {
        if (stream.type != QLatin1String("Subtitle") || stream.index != *index)
            continue;
        if (!isTextSubtitleCodec(stream.codec))
            return std::nullopt;
        CastTextTrack track;
        track.url = castSubtitleUrl(m_client.serverUrl(), m_source.itemId, m_source.mediaSourceId,
                                    *index, startTicks);
        if (track.url.isEmpty())
            return std::nullopt;
        track.language = stream.language;
        track.name = stream.title.isEmpty() ? stream.language : stream.title;
        return track;
    }
    return std::nullopt;
}

void PlayerController::startCast(const PlaybackSource &source)
{
    if (!m_cast->hasTarget(m_castTarget) || m_session.state() != PlaybackSession::State::Idle) {
        castFailed(i18n("The device is no longer available: search for it again from Cast."));
        return;
    }
    const QString name = m_cast->nameFor(m_castTarget);
    // Checked again here: the renderer gets nothing that would let it act as the user
    const QString query = source.url.query();
    if (carriesToken(source.url, m_client.accessToken().toUtf8())
        || query.contains(QLatin1String("api_key"), Qt::CaseInsensitive)
        || query.contains(QLatin1String("apikey"), Qt::CaseInsensitive)) {
        castFailed(i18n(
            "The server's stream link would give the device your sign-in, so it was not sent."));
        return;
    }
    const bool transcode = source.method == PlaybackSource::Method::Transcode;
    const QString mime = m_cast->mimeFor(m_castTarget, source.media.source.container, transcode);
    if (mime.isEmpty()) {
        castFailed(i18nc("@info %1 is a device name",
                         "%1 cannot play this kind of video: play it on this computer instead.",
                         name));
        return;
    }
    m_loading = false;
    m_source = source;
    m_castTrackPending = false;
    // A Google Cast device (the renderer that takes a text track) gets a transcoded frame of a size it shows whole; the seeks reload this URL too
    if (transcode && m_cast->takesTextTrack(m_castTarget)) {
        if (const StreamSummary *video = pickStream(m_source.media.source, u"Video"))
            m_source.url = castFrameSizeUrl(m_source.url, video->width, video->height);
    }
    m_subtitleOptions.clear();
    m_externalSubtitles.clear();
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    Q_EMIT tracksChanged();
    m_castDurationMs = m_itemRuntimeTicks > 0 ? m_itemRuntimeTicks / 10'000 : 0;
    m_castPositionMs = m_resumeTicks / 10'000;
    PlaybackSession::Identity identity{source.itemId.toStdString(),
                                       source.mediaSourceId.toStdString(),
                                       source.playSessionId.toStdString()};
    if (!m_session.open(identity, PlaybackSession::Route::Cast)) {
        castFailed(i18n("The playback session could not be opened."));
        return;
    }
    m_casting = true;
    // The item's own reply may come after Playback Info: the renderer then lists the media source's name
    const QString title = m_title.isEmpty() ? source.media.source.name : m_title;
    // The renderer counts a transcode's position from its start: the start is put in the URL, whatever the server wrote there
    CastLoad request;
    request.url = transcode ? withStartTicks(m_source.url, m_resumeTicks) : m_source.url;
    request.mime = mime;
    request.title = title;
    request.subtitle = m_subtitle;
    request.startMs = m_castPositionMs;
    request.durationMs = m_castDurationMs;
    request.serverBase = m_client.serverUrl();
    request.direct = !transcode;
    request.imageUrl = artworkUrl();
    if (const auto track = castTextTrack(transcode ? m_resumeTicks : 0))
        request.track = *track;
    if (!m_cast->load(m_castTarget, request)) {
        castFailed(i18nc("@info %1 is a device name",
                         "%1 could not play this video: check that it can reach the server.",
                         name));
        return;
    }
    m_castName = name;
    updateChapters();
    Q_EMIT changed();
}

void PlayerController::onCastPlaying()
{
    if (!m_casting)
        return;
    // The transitions report by themselves: Start on the first PLAYING, Progress on a resume
    switch (m_session.state()) {
    case PlaybackSession::State::Opening:
        if (m_session.opened())
            m_playedClock.start();
        break;
    case PlaybackSession::State::Paused:
        m_session.resume();
        if (std::exchange(m_castTrackPending, false))
            onCastTextTrackReady();
        break;
    case PlaybackSession::State::Buffering:
        m_session.bufferRecovered();
        break;
    default:
        break;
    }
    Q_EMIT changed();
}

void PlayerController::onCastPaused()
{
    if (!m_casting)
        return;
    // A renderer can come up paused: the session opens (Start) before it pauses
    if (m_session.state() == PlaybackSession::State::Opening && m_session.opened())
        m_playedClock.start();
    m_session.pause();
    Q_EMIT changed();
}

void PlayerController::onCastBuffering()
{
    if (!m_casting)
        return;
    m_session.buffering();
    Q_EMIT changed();
}

void PlayerController::onCastEnded()
{
    if (!m_casting)
        return;
    // The end reports the duration, as a local end within the end window does; still casting, so the Stopped and the repeat check read the renderer
    if (m_castDurationMs > 0)
        m_castPositionMs = m_castDurationMs;
    if (m_session.state() != PlaybackSession::State::Idle) {
        m_session.stop();
        m_session.stopped();
    }
    onEnd(QStringLiteral("eof"));
}

void PlayerController::onCastStopped()
{
    if (!m_casting)
        return;
    const QString name = m_castName;
    if (m_session.state() != PlaybackSession::State::Idle) {
        m_session.stop();
        m_session.stopped();
    }
    clearCastTarget();
    onEnd(QStringLiteral("stop"));
    Q_EMIT castStoppedElsewhere(name);
}

void PlayerController::onCastFailed(CastRenderer::Failure failure)
{
    if (!m_casting)
        return;
    const QString name = m_castName;
    // A direct play the renderer refused before it ever played, or left standing at its start, is asked once more as a transcode
    const bool neverPlayed =
        m_session.state() == PlaybackSession::State::Opening
        || (m_session.state() != PlaybackSession::State::Idle && m_castPositionMs < 2000);
    if (failure == CastRenderer::Failure::Refused && neverPlayed
        && m_source.method == PlaybackSource::Method::Direct && !m_castTranscodeOnly) {
        closeCast(false);
        m_castTranscodeOnly = true;
        startItem(m_itemId, m_resumeTicks);
        return;
    }
    switch (failure) {
    case CastRenderer::Failure::Refused:
        castFailed(i18nc("@info %1 is a device name",
                         "%1 could not play this video: check that it can reach the server.",
                         name));
        break;
    case CastRenderer::Failure::Untrusted:
        castFailed(i18nc("@info %1 is a device name",
                         "Kinefin could not verify that %1 is a Google Cast device, so nothing was "
                         "sent to it: play on this computer or choose another device.",
                         name));
        break;
    case CastRenderer::Failure::NotResponding:
        castFailed(i18nc("@info %1 is a device name",
                         "%1 stopped responding: check that it is turned on and connected to this "
                         "network.",
                         name));
        break;
    case CastRenderer::Failure::ServerRefused:
        castFailed(i18n("The server does not let other devices play its videos without a sign-in, "
                        "so this video cannot be cast."));
        break;
    }
}

void PlayerController::onCastPosition()
{
    if (!m_casting)
        return;
    if (const auto position = m_cast->positionMs())
        m_castPositionMs = *position;
    if (const auto duration = m_cast->durationMs(); duration && *duration > 0)
        m_castDurationMs = *duration;
    updateChapters();
    updateSkip();
    Q_EMIT changed();
}

// A transcode went out without its text track while the server was still extracting it: the stream is asked again from where the device is, with the track
void PlayerController::onCastTextTrackReady()
{
    if (!m_casting || m_source.method != PlaybackSource::Method::Transcode)
        return;
    if (m_session.state() == PlaybackSession::State::Paused) {
        m_castTrackPending = true;
        return;
    }
    // A seek on its way keeps its target: its reload carries the track too
    if (!m_castSeek.isActive())
        m_castSeekTarget = -1;
    m_castSeek.start();
}

void PlayerController::onCastVolume()
{
    if (!m_casting)
        return;
    Q_EMIT changed();
}

void PlayerController::onPlaybackItem(const QString &id, const QJsonObject &item)
{
    // A reply for an item that is no longer the current one must not name or lay out this one, nor a server reply a local file of the same item
    if (id != m_itemId || !active() || m_localPlayback)
        return;
    const double runtime = item.value(QStringLiteral("RunTimeTicks")).toDouble(-1);
    m_itemRuntimeTicks =
        runtime > 0 && runtime <= 24.0 * 3600 * TicksPerSecond ? qint64(runtime) : 0;
    // The renderer's own duration of a transcode is not trusted: the item's runtime replaces it once it is known
    if (m_casting && m_itemRuntimeTicks > 0) {
        m_castDurationMs = m_itemRuntimeTicks / 10'000;
        m_cast->setDurationMs(m_castDurationMs);
    }
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
    // No Up Next card while casting: the end advances by the autoplay rules
    const std::optional<qint64> position =
        m_casting ? std::optional<qint64>{} : m_engine.knownPositionTicks();
    const qint64 duration = durationTicks();
    std::optional<UpNextWindow> window;
    // A part that loops does not go on to the next item: the card would start it at the early advance
    if (!m_itemId.isEmpty() && !m_loading && position && pendingAdvance(false) >= 0
        && !m_clipLooping)
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

void PlayerController::onState(PlaybackSession::State from, PlaybackSession::State to)
{
    // A cache stall of a server stream feeds the fallback; leaving it, for any state, ends the stall
    if (to == PlaybackSession::State::Buffering && fallbackApplies())
        m_fallback.stalled();
    else if (from == PlaybackSession::State::Buffering)
        m_fallback.recovered();
    if (to == PlaybackSession::State::Playing)
        m_clock.start();
    if (to == PlaybackSession::State::Idle) {
        m_trackTimer.stop();
        m_clock.stop();
        if (m_loading)
            QTimer::singleShot(0, this, &PlayerController::requestPending);
        if (m_pendingLocal.pending)
            QTimer::singleShot(0, this, &PlayerController::openLocal);
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
    // The watchdog already reported the stream that never opened: the item stays where it is
    if (std::exchange(m_switchTimedOut, false)) {
        Q_EMIT changed();
        return;
    }
    // The group decides what follows its item: nothing moves on or plays again here, and when it moves on the player waits for its next item
    if (m_groupItem) {
        m_playedClock.invalidate();
        const bool movesOn = reason == QLatin1String("eof") && !m_stopping && m_group
                             && m_group->itemEnded(m_itemId);
        if (movesOn) {
            m_awaitingGroup = true;
            m_groupAdvance.start();
            Q_EMIT changed();
            return;
        }
        clearCurrent();
        Q_EMIT finished();
        Q_EMIT changed();
        return;
    }
    // A local file has no queue, no repeat of a server item and no reporting: it just ends
    if (m_localPlayback && !m_pendingLocal.pending) {
        m_sleep.stop();
        m_sleepAtEndOfItem = false;
        m_sleepAtEndOfQueue = false;
        // A file that never loaded keeps the resume point it had; one that failed midway keeps where it got to
        reportLocalPosition(reason == QLatin1String("eof"));
        m_queue.setCurrentPlaybackItemId({});
        clearCurrent();
        Q_EMIT finished();
        Q_EMIT changed();
        return;
    }
    // The switched stream did not open or play on: the item stays, with its error and Try Again at the bit rate before the step
    if ((m_switchingStream || m_switchSettleTicks >= 0) && reason == QLatin1String("error")
        && !m_stopping) {
        failStreamSwitch();
        Q_EMIT changed();
        return;
    }
    const bool ended = reason == QLatin1String("eof") && !m_stopping;
    // The sleep timer's end modes stop here: before the queue, the next episode or a repeat would start
    const bool sleepNow =
        ended && (m_sleepAtEndOfItem || (m_sleepAtEndOfQueue && pendingAdvance(false) < 0));
    if (sleepNow) {
        m_sleep.stop();
        m_sleepAtEndOfItem = false;
        m_sleepAtEndOfQueue = false;
    }
    // Cancel on the Up Next card ends this playback here, without the advance or the replay of a repeat; a repeat mode chosen after it that replays the item wins
    const bool cancelled = m_upNextCancelled && pendingAdvance(false) >= 0;
    const bool playedLongEnough =
        m_playedClock.isValid() && m_playedClock.elapsed() >= MinimumPlayedMs;
    m_playedClock.invalidate();
    if (ended && !cancelled && !sleepNow && advancesAtEnd() && next())
        return;
    // An item with nothing to advance to plays again when either repeat mode is on, after a real end: a stream that stops at once or far from its end does not loop
    const QString again = m_itemId;
    const bool nearEnd = m_casting
                             ? m_castDurationMs > 0 && m_castPositionMs >= m_castDurationMs - 5000
                             : m_engine.nearEnd();
    if (ended && !cancelled && !sleepNow && m_repeat != 0 && nearEnd && playedLongEnough
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
    m_switchTimeout.stop();
    m_switchTimedOut = false;
    m_switchManual = false;
    m_switchSettleTicks = -1;
    m_failedSwitchManual = false;
    m_localSubtitles.clear();
    m_subtitlesOff = false;
    m_pendingSubtitleSecondary = false;
    m_pendingSecondaryFile.clear();
    cancelClipExport();
    dropClipMarks();
    m_localSourcePath.clear();
    dropFallback();
    m_fallback.reset();
    m_preparedSource.reset();
    m_switchingStream = false;
    m_canRetry = false;
    m_requestedCap = UnlimitedBitrate;
    m_requestedStreams = {};
    m_detailsStreams.reset();
    m_manualCap = 0;
    m_qualitySwitching = false;
    dropSeriesPlaylist();
    m_casting = false;
    clearCastTarget();
    m_castFailed = false;
    m_castPositionMs = 0;
    m_castDurationMs = 0;
    m_itemRuntimeTicks = 0;
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
    setSegments({});
    resetForNextItem();
    m_source = PlaybackSource{};
    m_keptRate.reset();
    m_mpvSettings.setAspectOverride({});
    m_itemChapters.clear();
    updateChapters();
    resetUpNext();
    m_fileLoaded = false;
    m_selectedAudioId = 0;
    m_selectedSubtitleId = 0;
    m_secondarySubtitleId = 0;
    m_selectedAudioIndex.reset();
    m_selectedSubtitleIndex.reset();
    m_secondaryStreamIndex.reset();
    m_groupItem = false;
    m_awaitingGroup = false;
    m_groupAdvance.stop();
    m_mpvSettings.setSpeedHeld(false);
    m_pendingLocal = {};
    if (m_localPlayback) {
        m_localPlayback = false;
        Q_EMIT localPlaybackChanged();
    }
    if (!m_itemSeriesId.isEmpty()) {
        m_itemSeriesId.clear();
        Q_EMIT seriesChoicesChanged();
    }
}

void PlayerController::pause()
{
    if (grouped())
        m_group->requestPause();
    else if (m_casting) {
        m_cast->pause();
    } else {
        m_engine.pause();
        reportLocalPosition(false);
    }
}
void PlayerController::resume()
{
    if (grouped())
        m_group->requestUnpause();
    else if (m_casting)
        m_cast->resume();
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
void PlayerController::applyRate(double rate)
{
    if (m_groupItem && std::isfinite(rate))
        m_engine.setRate(std::clamp(rate, 0.9, 1.1));
}
void PlayerController::releaseGroupItem()
{
    m_groupItem = false;
    m_mpvSettings.setSpeedHeld(false);
    // Waiting for the group's next item, there is nothing left to play
    if (std::exchange(m_awaitingGroup, false)) {
        m_groupAdvance.stop();
        clearCurrent();
        Q_EMIT finished();
        Q_EMIT changed();
    }
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
    // Closing the player leaves the group playing on without this session
    if (grouped())
        m_group->stopFollowing();
    stopLocally();
}
void PlayerController::stopLocally()
{
    persistQueueCursor();
    m_pendingStreams.reset();
    m_stopping = true;
    m_client.cancelPlaybackInfo();
    m_loading = false;
    m_sleep.stop();
    m_sleepAtEndOfItem = false;
    m_sleepAtEndOfQueue = false;
    if (m_casting) {
        closeCast(true);
        m_queue.setCurrentPlaybackItemId({});
        clearCurrent();
        Q_EMIT finished();
    } else if (m_session.state() == PlaybackSession::State::Idle) {
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
        return m_group->requestNext();
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
    if (grouped()) {
        if (!loading() && positionTicks() > 3 * TicksPerSecond) {
            seek(0);
            return true;
        }
        return m_group->requestPrevious();
    }
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
    // Loading, and waiting for a group's next item, count as playing for the desktop (the screen stays on)
    if (m_loading || m_awaitingGroup)
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
    if (m_engine.canSeek() && active() && !loading() && ticks >= 0)
        m_group->requestSeek(
            m_engine.durationTicks() > 0 ? std::min(ticks, m_engine.durationTicks()) : ticks);
}
void PlayerController::seekLocally(qint64 ticks)
{
    if (m_casting) {
        if (!m_cast->canSeek() || m_loading || ticks < 0)
            return;
        const qint64 target = durationTicks() > 0 ? std::min(ticks, durationTicks()) : ticks;
        keepIntrosAt(target);
        // A transcode seeks as the server's own DLNA control does: the same stream asked from another start, under a new play session id (see withStartTicks); a run of seeks reloads it once, at the last target
        bool moved = true;
        if (m_source.method == PlaybackSource::Method::Transcode) {
            m_castSeekTarget = target;
            m_castSeek.start();
        } else {
            moved = m_cast->seek(target / 10'000);
        }
        if (moved) {
            m_castPositionMs = target / 10'000;
            Q_EMIT seeked(target);
            m_seekReport.start();
            Q_EMIT changed();
        }
        return;
    }
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
    // A group plays at its own pace, and a renderer has no speed of ours
    if (grouped() || m_casting || !std::isfinite(value) || value < MinimumRate
        || value > MaximumRate)
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
    if (level < 0 || level > 100)
        return;
    if (m_casting) {
        if (m_cast->setVolume(level)) {
            Q_EMIT changed();
            m_reporter.reportNow();
        }
        return;
    }
    if (!m_engine.isInitialized())
        return;
    m_volume = level;
    m_engine.setMpvProperty(QStringLiteral("volume"), level);
    Q_EMIT changed();
    m_reporter.reportNow();
}
void PlayerController::toggleMute()
{
    if (m_casting) {
        if (m_cast->setMuted(!m_cast->muted())) {
            Q_EMIT changed();
            m_reporter.reportNow();
        }
        return;
    }
    if (!m_engine.isInitialized())
        return;
    m_muted = !m_muted;
    m_engine.setMpvProperty(QStringLiteral("mute"), m_muted);
    Q_EMIT changed();
    m_reporter.reportNow();
}
void PlayerController::setShuffle(bool enabled)
{
    if (grouped()) {
        m_group->requestShuffle(enabled);
        return;
    }
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

bool PlayerController::nightMode() const
{
    return m_settings.value(QStringLiteral("audio"), QStringLiteral("night_mode")).toBool();
}

void PlayerController::setNightMode(bool enabled)
{
    // The settings observer rebuilds the chain; this only reports that decoding replaced passthrough
    const bool decodes = enabled && m_mpvSettings.passthroughConfigured();
    if (!m_settings.setValue(QStringLiteral("audio"), QStringLiteral("night_mode"), enabled))
        return;
    if (decodes)
        Q_EMIT nightModePassthroughDisabled();
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
    if (mode < 0 || mode > 2)
        return;
    if (grouped()) {
        m_group->requestRepeatMode(mode);
        return;
    }
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
    m_secondarySubtitleId = 0;
    m_addedSubtitleTracks.clear();
    QString selectedFile;
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
        // mpv's selection order within the type: 0 is `sid`, 1 `secondary-sid`
        const bool secondary = type == QLatin1String("sub")
                               && map.value(QStringLiteral("main-selection")).toInt() == 1;
        const bool text = type == QLatin1String("sub")
                          && textSubtitleCodec(map.value(QStringLiteral("codec")).toString());
        if (const std::optional<int> index = serverStreamIndex(map, direct);
            selected && index && !secondary) {
            if (type == QLatin1String("audio"))
                m_selectedAudioIndex = index;
            else
                m_selectedSubtitleIndex = index;
        }
        if (selected) {
            if (type == QLatin1String("audio"))
                m_selectedAudioId = id;
            else if (secondary)
                m_secondarySubtitleId = id;
            else
                m_selectedSubtitleId = id;
        }
        if (type == QLatin1String("sub") && map.value(QStringLiteral("external")).toBool()) {
            const QString file = map.value(QStringLiteral("external-filename")).toString();
            const bool added =
                std::any_of(m_addedSubtitles.cbegin(), m_addedSubtitles.cend(),
                            [&file](const LocalSubtitle &entry) { return entry.path == file; });
            if (added) {
                m_addedSubtitleTracks.insert(id, file);
                if (selected && !secondary)
                    selectedFile = file;
            }
        }
        const QVariantMap result{{QStringLiteral("id"), id},
                                 {QStringLiteral("name"), label},
                                 {QStringLiteral("selected"), selected && !secondary},
                                 {QStringLiteral("secondary"), secondary},
                                 {QStringLiteral("text"), text}};
        (type == QLatin1String("audio") ? m_audioFacts : m_subtitleFacts)
            .append({id, trackLanguage(map.value(QStringLiteral("lang")).toString()),
                     map.value(QStringLiteral("forced")).toBool(), text});
        if (type == QLatin1String("audio"))
            audio.append(result);
        else
            subs.append(result);
    }
    // A file added for the second slot that mpv now lists becomes the secondary one, as the user asked
    if (!m_pendingSecondaryFile.isEmpty()) {
        for (const QVariant &entry : tracks) {
            const QVariantMap map = entry.toMap();
            if (map.value(QStringLiteral("type")).toString() == QLatin1String("sub")
                && map.value(QStringLiteral("external-filename")).toString()
                       == m_pendingSecondaryFile) {
                m_pendingSecondaryFile.clear();
                setSecondarySubtitle(map.value(QStringLiteral("id")).toInt());
                break;
            }
        }
    }
    if (audio != m_audioTracks || subs != m_subtitleTracks) {
        m_audioTracks = audio;
        m_subtitleTracks = subs;
        Q_EMIT tracksChanged();
    }
    // Only a list that holds the added files tells which one is selected: while a reopened file is still loading, the one that was selected stays the one to select again
    if (m_trackRestored && !m_addedSubtitleTracks.isEmpty())
        m_selectedAddedSubtitle = selectedFile;
    if (!m_trackRestored && !tracks.isEmpty()) {
        m_trackRestored = true;
        restoreTracks();
    }
    // An empty track list is a list that has not arrived yet: the pick waits, as the restored one does
    if (!tracks.isEmpty())
        applyPendingStreams();
}

void PlayerController::applyPendingStreams()
{
    if (!m_pendingStreams || m_pendingStreams->itemId != m_itemId)
        return;
    const PendingStreams pending = *m_pendingStreams;
    m_pendingStreams.reset();
    if (pending.audio >= 0)
        selectAudioStreamIndex(pending.audio);
    if (pending.subtitle >= -1)
        selectSubtitleStreamIndex(pending.subtitle);
    m_detailsStreams = pending;
}
void PlayerController::selectAudioTrack(int id)
{
    if (id < 1 || id > 1024 || !active())
        return;
    m_detailsStreams.reset();
    m_engine.setMpvProperty(QStringLiteral("aid"), id);
    m_trackChosen = true;
    rememberTracks(id, m_selectedSubtitleId, m_secondarySubtitleId);
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
    m_detailsStreams.reset();
    // A choice made now wins over a download for the same slot still on its way
    if (!m_pendingSubtitleSecondary)
        dropPendingSubtitle();
    // mpv refuses a track selected in both slots: the second one goes first
    if (id != 0 && id == m_secondarySubtitleId)
        selectSecondarySubtitleTrack(0);
    m_engine.setMpvProperty(QStringLiteral("sid"),
                            id == 0 ? QVariant(QStringLiteral("no")) : QVariant(id));
    m_selectedSubtitleId = id;
    m_subtitlesOff = id == 0;
    m_trackChosen = true;
    m_selectedSubtitleIndex =
        id == 0 ? std::nullopt : streamIndexOfTrack(QStringLiteral("sub"), id);
    // A subtitle file added by hand is for this item only, and its mpv id means nothing to the next session or the next episode
    m_selectedAddedSubtitle = m_addedSubtitleTracks.value(id);
    if (!m_addedSubtitleTracks.contains(id))
        rememberTracks(m_selectedAudioId, id, m_secondarySubtitleId);
    if (keepsSeriesTracks() && !m_addedSubtitleTracks.contains(id)) {
        const auto choice = choiceOf(m_subtitleFacts, id);
        if (id == 0 || choice)
            seriesStore().setSubtitle(m_itemSeriesId, id == 0, choice);
    }
    QTimer::singleShot(0, this, &PlayerController::refreshTracks);
    m_reporter.reportNow();
}
QList<TrackFact> PlayerController::secondarySubtitleFacts() const
{
    QList<TrackFact> candidates;
    for (const TrackFact &track : m_subtitleFacts) {
        if (track.text && track.id != m_selectedSubtitleId)
            candidates.append(track);
    }
    return candidates;
}
bool PlayerController::setSecondarySubtitle(int id)
{
    if (id < 0 || id > 1024 || !active())
        return false;
    if (id != 0) {
        const QList<TrackFact> candidates = secondarySubtitleFacts();
        if (!std::any_of(candidates.cbegin(), candidates.cend(),
                         [id](const TrackFact &track) { return track.id == id; }))
            return false;
    }
    m_engine.setMpvProperty(QStringLiteral("secondary-sid"),
                            id == 0 ? QVariant(QStringLiteral("no")) : QVariant(id));
    m_secondarySubtitleId = id;
    m_secondaryStreamIndex = id == 0 ? std::nullopt : streamIndexOfTrack(QStringLiteral("sub"), id);
    QTimer::singleShot(0, this, &PlayerController::refreshTracks);
    return true;
}
void PlayerController::selectSecondarySubtitleTrack(int id)
{
    if (m_pendingSubtitleSecondary)
        dropPendingSubtitle();
    if (!setSecondarySubtitle(id))
        return;
    m_trackChosen = true;
    if (m_addedSubtitleTracks.contains(id))
        return;
    rememberTracks(m_selectedAudioId, m_selectedSubtitleId, id);
    if (keepsSeriesTracks()) {
        const auto choice = id == 0 ? std::nullopt : choiceOf(secondarySubtitleFacts(), id);
        // Off removes the series' choice; a track the series cannot describe leaves the store as it was
        if (id == 0 || choice)
            seriesStore().setSecondarySubtitle(m_itemSeriesId, choice);
    }
}
bool PlayerController::directPlay() const
{
    // A transcode carries the one track the server chose: mpv's stream index says nothing about the source's
    return active() && m_engine.isInitialized() && !m_source.itemId.isEmpty()
           && m_source.itemId == m_itemId && m_source.method == PlaybackSource::Method::Direct;
}
std::optional<int> PlayerController::streamIndexOfTrack(const QString &type, int id) const
{
    if (!active() || !m_engine.isInitialized())
        return std::nullopt;
    const bool direct = directPlay();
    const QVariantList tracks = m_engine.mpvProperty(QStringLiteral("track-list")).toList();
    for (const QVariant &entry : tracks) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("type")).toString() == type
            && map.value(QStringLiteral("id")).toInt() == id)
            return serverStreamIndex(map, direct);
    }
    return std::nullopt;
}
std::optional<int> PlayerController::serverStreamIndex(const QVariantMap &track, bool direct) const
{
    // An external file has its own numbering in mpv: only a server's subtitle this item downloaded maps back
    if (track.value(QStringLiteral("external")).toBool()) {
        const auto found = m_loadedSubtitles.constFind(
            track.value(QStringLiteral("external-filename")).toString());
        return found != m_loadedSubtitles.cend() ? std::optional<int>(*found) : std::nullopt;
    }
    // mpv's `ff-index` is the server's only in a direct play: a transcode has its own numbering
    const QVariant index = track.value(QStringLiteral("ff-index"));
    if (!direct || !index.isValid() || index.toInt() < 0)
        return std::nullopt;
    return index.toInt();
}
int PlayerController::trackOfStreamIndex(const QString &type, int index) const
{
    if (!active() || !m_engine.isInitialized())
        return 0;
    const bool direct = directPlay();
    const QVariantList tracks = m_engine.mpvProperty(QStringLiteral("track-list")).toList();
    for (const QVariant &entry : tracks) {
        const QVariantMap map = entry.toMap();
        const int id = map.value(QStringLiteral("id")).toInt();
        if (map.value(QStringLiteral("type")).toString() == type && id > 0 && id <= 1024
            && serverStreamIndex(map, direct) == index)
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
    // Subtitles off needs no track, only a stream whose subtitle indexes are the server's: a direct play, or a server's external subtitle loaded on a transcode
    if (index == -1) {
        if (!directPlay() && !m_selectedSubtitleIndex)
            return false;
        selectSubtitleTrack(0);
        return true;
    }
    const int id = trackOfStreamIndex(QStringLiteral("sub"), index);
    // A server's external subtitle not loaded yet is downloaded, and selected once it is
    if (id == 0)
        return downloadSubtitle(index);
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
    m_sleepAtEndOfItem = false;
    m_sleepAtEndOfQueue = false;
    if (minutes == 0)
        m_sleep.stop();
    else
        m_sleep.start(minutes * 60 * 1000);
    Q_EMIT changed();
}
void PlayerController::setSleepAtEndOfItem(bool enabled)
{
    if (enabled == m_sleepAtEndOfItem)
        return;
    m_sleepAtEndOfItem = enabled;
    if (enabled) {
        m_sleep.stop();
        m_sleepAtEndOfQueue = false;
    }
    Q_EMIT changed();
}
void PlayerController::setSleepAtEndOfQueue(bool enabled)
{
    if (enabled == m_sleepAtEndOfQueue)
        return;
    m_sleepAtEndOfQueue = enabled;
    if (enabled) {
        m_sleep.stop();
        m_sleepAtEndOfItem = false;
    }
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
    // The path is within the profile's private data directory, writable even inside Flatpak. A portal export can be added independently
    m_screenshotPath = file;
    Q_EMIT changed();
}

qint64 PlayerController::clipLengthTicks() const
{
    return m_clipStart >= 0 && m_clipEnd > m_clipStart ? m_clipEnd - m_clipStart : 0;
}

bool PlayerController::clipFromCache() const
{
    return !(m_localPlayback && !m_localSourcePath.isEmpty()) && !directPlay();
}

bool PlayerController::clipCached() const
{
    if (m_casting || !m_engine.isInitialized())
        return false;
    const double start = double(m_clipStart) / TicksPerSecond;
    const double end = double(m_clipEnd) / TicksPerSecond;
    const QVariantList ranges = m_engine.mpvProperty(QStringLiteral("demuxer-cache-state"))
                                    .toMap()
                                    .value(QStringLiteral("seekable-ranges"))
                                    .toList();
    return std::any_of(ranges.cbegin(), ranges.cend(), [start, end](const QVariant &value) {
        const QVariantMap range = value.toMap();
        return range.value(QStringLiteral("start")).toDouble() <= start
               && range.value(QStringLiteral("end")).toDouble() >= end;
    });
}

QString PlayerController::clipCommonReason() const
{
    const bool local = m_localPlayback && !m_localSourcePath.isEmpty();
    if (!local) {
        const StreamSummary *video = pickStream(m_source.media.source, u"Video");
        bool hdr = video
                   && (video->videoRange == QLatin1String("HDR")
                       || video->videoRangeType.startsWith(QLatin1String("HDR"))
                       || video->videoRangeType.startsWith(QLatin1String("DOVI")));
        // A stream's clip is the picture mpv decodes: a transcode the server tone mapped is SDR whatever the source was
        const QString gamma =
            clipFromCache() ? m_engine.mpvProperty(QStringLiteral("video-params/gamma")).toString()
                            : QString();
        if (!gamma.isEmpty())
            hdr = gamma == QLatin1String("pq") || gamma == QLatin1String("hlg");
        if (hdr)
            return i18n("Clips cannot be saved from HDR video");
        if (clipFromCache() && !clipCached())
            return i18n("The start of the clip is no longer buffered");
    }
    return {};
}

bool PlayerController::clipReady(bool gif, QString *reason) const
{
    const auto refuse = [reason](const QString &text) {
        if (reason)
            *reason = text;
        return false;
    };
    if (clipLengthTicks() < TicksPerSecond)
        return refuse(i18n("Set a start and an end"));
    const QString common = clipCommonReason();
    if (!common.isEmpty())
        return refuse(common);
    if (clipLengthTicks() > (gif ? 15 : 60) * TicksPerSecond)
        return refuse(gif ? i18n("A GIF lasts at most 15 seconds")
                          : i18n("A video clip lasts at most 60 seconds"));
    const bool encoder = gif ? ClipExporter::hasEncoder(QStringLiteral("gif"))
                             : (ClipExporter::hasEncoder(QStringLiteral("libx264"))
                                || (ClipExporter::hasEncoder(QStringLiteral("libvpx-vp9"))
                                    && ClipExporter::hasEncoder(QStringLiteral("libopus"))));
    if (!encoder)
        return refuse(i18n("No usable video encoder"));
    return true;
}

QString PlayerController::clipLine() const
{
    if (!active())
        return {};
    if (m_clipStart < 0 && m_clipEnd < 0)
        return i18n("Set a start and an end");
    if (m_clipStart < 0)
        return i18n("End at %1; set a start", timeText(m_clipEnd));
    if (m_clipEnd < 0)
        return i18n("Start at %1; set an end", timeText(m_clipStart));
    if (m_clipEnd <= m_clipStart)
        return i18n("The end of the clip is before its start");
    return i18n("%1 to %2 (%3 s)", timeText(m_clipStart), timeText(m_clipEnd),
                clipLengthTicks() / TicksPerSecond);
}

QString PlayerController::clipReason() const
{
    if (clipLengthTicks() < TicksPerSecond)
        return {};
    const QString common = clipCommonReason();
    if (!common.isEmpty())
        return common;
    if (!ClipExporter::hasEncoder(QStringLiteral("libx264"))
        && !(ClipExporter::hasEncoder(QStringLiteral("libvpx-vp9"))
             && ClipExporter::hasEncoder(QStringLiteral("libopus"))))
        return i18n("No usable video encoder");
    return {};
}

QString PlayerController::clipLengthReason() const
{
    if (clipLengthTicks() <= 0)
        return {};
    if (clipLengthTicks() > 60 * TicksPerSecond)
        return i18n("A video clip lasts at most 60 seconds");
    if (clipLengthTicks() > 15 * TicksPerSecond)
        return i18n("A GIF lasts at most 15 seconds");
    return {};
}

void PlayerController::markClipStart()
{
    if (!active())
        return;
    m_clipStart = positionTicks();
    applyClipLoop();
    Q_EMIT clipMarkChanged(i18n("Clip start at %1", timeText(m_clipStart)));
    Q_EMIT changed();
}

void PlayerController::markClipEnd()
{
    if (!active())
        return;
    m_clipEnd = positionTicks();
    applyClipLoop();
    Q_EMIT clipMarkChanged(i18n("Clip end at %1", timeText(m_clipEnd)));
    Q_EMIT changed();
}

void PlayerController::clearClip()
{
    if (m_clipStart < 0 && m_clipEnd < 0)
        return;
    dropClipMarks();
    Q_EMIT changed();
}

void PlayerController::dropClipMarks()
{
    m_clipStart = -1;
    m_clipEnd = -1;
    applyClipLoop();
}

void PlayerController::applyClipLoop()
{
    // A mark that makes the range empty, a group and a renderer end the loop; the loop is the marks' while it runs
    if (m_clipLooping && !canLoopClip())
        m_clipLooping = false;
    const bool looping = m_clipLooping;
    if (!looping && !m_loopSet)
        return;
    m_loopSet = looping;
    m_engine.setMpvProperty(QStringLiteral("ab-loop-a"),
                            looping ? QVariant(double(m_clipStart) / TicksPerSecond)
                                    : QVariant(QStringLiteral("no")));
    m_engine.setMpvProperty(QStringLiteral("ab-loop-b"),
                            looping ? QVariant(double(m_clipEnd) / TicksPerSecond)
                                    : QVariant(QStringLiteral("no")));
}

void PlayerController::setClipLooping(bool looping)
{
    if (looping && !canLoopClip()) {
        // The key has nothing to loop: it says what is missing
        if (!m_clipLooping)
            Q_EMIT clipMarkChanged(i18n("Mark a start and an end to loop the part"));
        return;
    }
    if (looping == m_clipLooping)
        return;
    m_clipLooping = looping;
    applyClipLoop();
    Q_EMIT clipMarkChanged(looping ? i18n("Looping the marked part") : i18n("Loop off"));
    Q_EMIT changed();
}

void PlayerController::refreshEnhancements()
{
    QVariantList files;
    QStringList active;
    bool deband = false;
    if (m_engine.isInitialized() && this->active() && !m_profileDataDir.isEmpty()) {
        constexpr int MaxShaders = 32;
        constexpr qint64 MaxShaderSize = 256 * 1024;
        deband = m_engine.mpvProperty(QStringLiteral("deband")).toBool();
        QStringList used;
        for (const QVariant &entry : m_engine.mpvProperty(QStringLiteral("glsl-shaders")).toList())
            used.append(entry.toString());
        const QDir directory(m_profileDataDir + QStringLiteral("/shaders"));
        const QFileInfoList entries =
            directory.entryInfoList({QStringLiteral("*.glsl"), QStringLiteral("*.hook")},
                                    QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &info : entries) {
            if (files.size() >= MaxShaders)
                break;
            const QString file = info.fileName();
            // A name with a control character would not survive mpv's option parser or a menu
            // The separators of mpv's list option (`:` for paths, `,` for the rest) would split the name when it is removed again
            if (info.size() > MaxShaderSize || std::ranges::any_of(file, [](QChar c) {
                    return c.unicode() < 0x20 || c == QLatin1Char(':') || c == QLatin1Char(',');
                }))
                continue;
            files.append(QVariantMap{{QStringLiteral("file"), file},
                                     {QStringLiteral("name"), info.completeBaseName()}});
            if (used.contains(info.absoluteFilePath()))
                active.append(file);
        }
    }
    if (deband == m_deband && files == m_shaders && active == m_activeShaders)
        return;
    m_deband = deband;
    m_shaders = files;
    m_activeShaders = active;
    Q_EMIT enhancementsChanged();
}

void PlayerController::keepEnhancementBase()
{
    if (m_enhancementsTouched)
        return;
    m_enhancementsTouched = true;
    m_debandBase = m_engine.mpvProperty(QStringLiteral("deband")).toBool();
    m_shadersBase = m_engine.mpvProperty(QStringLiteral("glsl-shaders"));
}

void PlayerController::resetForNextItem()
{
    resetEnhancements();
    resetDelays();
    for (const LocalSubtitle &added : std::as_const(m_addedSubtitles))
        QFile::remove(added.path);
    m_addedSubtitles.clear();
    m_selectedAddedSubtitle.clear();
    m_addedSubtitleTracks.clear();
}

void PlayerController::resetDelays()
{
    if (m_audioDelay == 0 && m_subtitleDelay == 0)
        return;
    m_audioDelay = 0;
    m_subtitleDelay = 0;
    m_mpvSettings.setAudioOffset(0);
    m_mpvSettings.setSubtitleOffset(0);
    Q_EMIT changed();
}

void PlayerController::resetEnhancements()
{
    if (!m_enhancementsTouched)
        return;
    m_enhancementsTouched = false;
    if (m_engine.isInitialized()) {
        m_engine.setMpvProperty(QStringLiteral("deband"), m_debandBase);
        m_engine.setMpvProperty(QStringLiteral("glsl-shaders"), m_shadersBase);
    }
    m_deband = false;
    m_shaders.clear();
    m_activeShaders.clear();
    Q_EMIT enhancementsChanged();
}

void PlayerController::setDeband(bool on)
{
    if (!active() || m_casting)
        return;
    keepEnhancementBase();
    m_engine.setMpvProperty(QStringLiteral("deband"), on);
    refreshEnhancements();
}

void PlayerController::setShader(const QString &file, bool on)
{
    if (!active() || m_casting)
        return;
    refreshEnhancements();
    // Only a file the menu lists: the name never reaches mpv as a path of its own making
    const bool known = std::ranges::any_of(m_shaders, [&file](const QVariant &entry) {
        return entry.toMap().value(QStringLiteral("file")).toString() == file;
    });
    if (!known)
        return;
    // Already as asked: another append would add a copy
    if (m_activeShaders.contains(file) == on)
        return;
    keepEnhancementBase();
    // A shader mpv cannot compile is not reported: libmpv's log messages do not reach the app, as mpvqt does not forward them
    const QString path = QDir(m_profileDataDir + QStringLiteral("/shaders")).absoluteFilePath(file);
    m_engine.mpvCommand({QStringLiteral("change-list"), QStringLiteral("glsl-shaders"),
                         on ? QStringLiteral("append") : QStringLiteral("remove"), path});
    refreshEnhancements();
}

// mpv pauses by itself after a step, but the session only knows the pauses it made: pausing first keeps the state, the reports and the buttons in step with it
void PlayerController::frameStep()
{
    if (!active() || m_casting || grouped())
        return;
    pause();
    m_engine.mpvCommand({QStringLiteral("frame-step")});
}

void PlayerController::frameBackStep()
{
    if (!active() || m_casting || grouped())
        return;
    pause();
    m_engine.mpvCommand({QStringLiteral("frame-back-step")});
}

QString PlayerController::suggestedClipName(bool gif) const
{
    const QString title = m_title.isEmpty() ? m_subtitle : m_title;
    const QString stem = ExportNames::component(title, QStringLiteral("Kinefin"));
    const qint64 start = m_clipStart >= 0 ? m_clipStart : positionTicks();
    const qint64 seconds = start / TicksPerSecond;
    const QString time = QStringLiteral("%1-%2-%3")
                             .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
                             .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
                             .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    const QString extension = gif ? QStringLiteral(".gif")
                              : ClipExporter::hasEncoder(QStringLiteral("libx264"))
                                  ? QStringLiteral(".mp4")
                                  : QStringLiteral(".webm");
    return stem + QLatin1Char(' ') + time + extension;
}

QUrl PlayerController::suggestedClipUrl(bool gif) const
{
    QString folder = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (folder.isEmpty())
        folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (folder.isEmpty())
        folder = QDir::homePath();
    return QUrl::fromLocalFile(folder + QLatin1Char('/') + suggestedClipName(gif));
}

bool PlayerController::saveClip(bool gif, const QUrl &destination)
{
    if (m_clipExporter.running() || !destination.isLocalFile() || m_profileDataDir.isEmpty()
        || !clipReady(gif, nullptr))
        return false;
    const QString target = destination.toLocalFile();
    const QFileInfo targetInfo(target);
    if (!targetInfo.isAbsolute() || targetInfo.isDir())
        return false;
    const QString directory = m_profileDataDir + QStringLiteral("/clips");
    if (!ensurePrivateDirectory(directory))
        return false;
    const double seconds = double(clipLengthTicks()) / TicksPerSecond;
    const double rate = gif ? 2.0 * 1024 * 1024 : (8.0 + 0.16) * 1000 * 1000 / 8.0;
    const bool fromCache = clipFromCache();
    // The dump of a streamed clip holds at most what mpv caches
    const qint64 dumpBound = fromCache ? m_engine.mpvProperty(QStringLiteral("demuxer-cache-state"))
                                             .toMap()
                                             .value(QStringLiteral("total-bytes"))
                                             .toLongLong()
                                       : 0;
    const QStorageInfo storage(directory);
    if (storage.isValid() && storage.bytesAvailable() < qint64(2.0 * rate * seconds) + dumpBound) {
        QFile::remove(target);
        Q_EMIT clipSaveFailed(true);
        return false;
    }
    ClipRequest request;
    request.format = gif ? ClipRequest::Format::Gif : ClipRequest::Format::Video;
    const bool local = m_localPlayback && !m_localSourcePath.isEmpty();
    request.source = local ? QUrl::fromLocalFile(m_localSourcePath) : m_source.url;
    if (!local && !fromCache)
        request.authorization = m_client.streamAuthorization();
    request.startTicks = m_clipStart;
    request.endTicks = m_clipEnd;
    request.audioId = m_selectedAudioId;
    request.subtitleId = m_selectedSubtitleId;
    request.secondarySubtitleId = m_secondarySubtitleId;
    if (local) {
        for (const LocalSubtitle &file : std::as_const(m_localSubtitles))
            request.subtitleFiles.append(file.path);
    }
    // The subtitle files mpv added come after the download's own: the server's external ones and the ones added by hand, in the order of mpv's tracks
    request.subtitleFiles.append(m_loadedSubtitleOrder);
    request.subtitleStyle = m_mpvSettings.subtitleProperties();
    request.tlsVerify = m_mpvSettings.tlsVerify();
    request.tlsCaFile = m_mpvSettings.tlsCaFile();
    const QString extension = gif ? QStringLiteral(".gif")
                              : ClipExporter::hasEncoder(QStringLiteral("libx264"))
                                  ? QStringLiteral(".mp4")
                                  : QStringLiteral(".webm");
    const QString stem =
        directory + QStringLiteral("/clip-")
        + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))
        + QLatin1Char('-') + QString::number(++m_clipSeq);
    const QString staging = stem + extension;
    if (fromCache) {
        const QString dump = stem + QStringLiteral("-cache.ts");
        const std::optional<double> dumpStart = dumpClipCache(dump);
        if (!dumpStart) {
            QFile::remove(dump);
            QFile::remove(target);
            Q_EMIT clipSaveFailed(false);
            return false;
        }
        m_clipDump = dump;
        // The dump starts at 0 where the cache's keyframe before the start mark was
        const qint64 offset = std::llround(*dumpStart * TicksPerSecond);
        request.source = QUrl::fromLocalFile(dump);
        request.startTicks = m_clipStart - offset;
        request.endTicks = m_clipEnd - offset;
        request.decodeFromFileStart = true;
        const DumpTrackIds ids = dumpTrackIds(
            m_engine.mpvProperty(QStringLiteral("track-list")).toList(), m_selectedAudioId,
            m_selectedSubtitleId, m_secondarySubtitleId, request.subtitleFiles);
        request.audioId = ids.audio;
        request.subtitleId = ids.subtitle;
        request.secondarySubtitleId = ids.secondarySubtitle;
        // The subtitle files keep the item's times, the dump starts at A′
        for (auto &[name, value] : request.subtitleStyle) {
            if (name == QLatin1String("sub-delay") || name == QLatin1String("secondary-sub-delay"))
                value = value.toDouble() - *dumpStart;
        }
    }
    m_clipDestination = destination;
    m_clipProgress = 0;
    m_clipGif = gif;
    if (!m_clipExporter.start(request, staging)) {
        // A refusal that reported itself has cleaned up already; the others leave the dump and the granted file
        if (!m_clipDestination.isEmpty()) {
            cancelClipExport();
            Q_EMIT clipSaveFailed(false);
        }
        return false;
    }
    Q_EMIT clipStateChanged();
    return true;
}

void PlayerController::cancelClip()
{
    cancelClipExport();
}

std::optional<double> PlayerController::dumpClipCache(const QString &path)
{
    if (!clipCached())
        return std::nullopt;
    const double start = double(m_clipStart) / TicksPerSecond;
    const double end = double(m_clipEnd) / TicksPerSecond;
    // The A-B marks only tell the two commands the range, which read them whatever the loop count: with the count at 0 the player never loops, also when it is before B
    const QVariant loopCount = m_engine.mpvProperty(QStringLiteral("ab-loop-count"));
    m_engine.setMpvProperty(QStringLiteral("ab-loop-count"), 0);
    m_engine.setMpvProperty(QStringLiteral("ab-loop-a"), start);
    m_engine.setMpvProperty(QStringLiteral("ab-loop-b"), end);
    const bool aligned = m_engine.mpvCommand({QStringLiteral("ab-loop-align-cache")});
    bool startKnown = false;
    bool endKnown = false;
    const double alignedStart =
        m_engine.mpvProperty(QStringLiteral("ab-loop-a")).toDouble(&startKnown);
    const double alignedEnd = m_engine.mpvProperty(QStringLiteral("ab-loop-b")).toDouble(&endKnown);
    const bool dumped = aligned && startKnown && endKnown
                        && m_engine.mpvCommand({QStringLiteral("ab-loop-dump-cache"), path});
    m_engine.setMpvProperty(QStringLiteral("ab-loop-a"), QStringLiteral("no"));
    m_engine.setMpvProperty(QStringLiteral("ab-loop-b"), QStringLiteral("no"));
    if (loopCount.isValid())
        m_engine.setMpvProperty(QStringLiteral("ab-loop-count"), loopCount);
    // A loop that ran over the marks goes on
    m_loopSet = false;
    applyClipLoop();
    // A start after the mark would cut the clip's first frames, an end well before the other mark its last ones: what they need left the cache. An end a frame short is the alignment's own, when B is a keyframe
    if (!dumped || alignedStart > start + 0.001 || alignedEnd < end - 0.5
        || QFileInfo(path).size() <= 0)
        return std::nullopt;
    return alignedStart;
}

void PlayerController::dropClipDump()
{
    if (m_clipDump.isEmpty())
        return;
    QFile::remove(m_clipDump);
    m_clipDump.clear();
}

void PlayerController::cancelClipExport()
{
    const bool wasRunning = m_clipExporter.running();
    m_clipExporter.cancel();
    dropClipDump();
    if (m_clipDestination.isLocalFile())
        QFile::remove(m_clipDestination.toLocalFile());
    m_clipDestination.clear();
    m_clipProgress = 0;
    m_clipGif = false;
    if (wasRunning)
        Q_EMIT clipStateChanged();
}

void PlayerController::onClipFinished(const QString &stagingPath)
{
    const QString target =
        m_clipDestination.isLocalFile() ? m_clipDestination.toLocalFile() : QString();
    m_clipDestination.clear();
    bool ok = false;
    if (!target.isEmpty()) {
        if (QFileInfo::exists(target))
            QFile::remove(target);
        ok = QFile::copy(stagingPath, target);
    }
    QFile::remove(stagingPath);
    dropClipDump();
    m_clipProgress = 0;
    const bool gif = m_clipGif;
    m_clipGif = false;
    Q_EMIT clipStateChanged();
    if (!ok) {
        if (!target.isEmpty())
            QFile::remove(target);
        Q_EMIT clipSaveFailed(false);
        return;
    }
    Q_EMIT clipSaved(QFileInfo(target).fileName(), gif);
}

void PlayerController::onClipFailed(ClipExporter::Error error)
{
    dropClipDump();
    if (m_clipDestination.isLocalFile())
        QFile::remove(m_clipDestination.toLocalFile());
    m_clipDestination.clear();
    m_clipProgress = 0;
    m_clipGif = false;
    Q_EMIT clipStateChanged();
    Q_EMIT clipSaveFailed(error == ClipExporter::Error::NoSpace);
}

void PlayerController::setSegments(const QVariantList &segments)
{
    m_segments = segments;
    QVariantList bands;
    for (const QVariant &value : segments) {
        const QVariantMap row = value.toMap();
        bands.append(QVariantMap{{QStringLiteral("type"), row.value(QStringLiteral("type"))},
                                 {QStringLiteral("startTicks"), row.value(QStringLiteral("start"))},
                                 {QStringLiteral("endTicks"), row.value(QStringLiteral("end"))}});
    }
    std::ranges::sort(bands, [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("startTicks")).toLongLong()
               < b.toMap().value(QStringLiteral("startTicks")).toLongLong();
    });
    if (bands == m_segmentBands)
        return;
    m_segmentBands = bands;
    Q_EMIT segmentsChanged();
}

void PlayerController::refreshSegments()
{
    setSegments({});
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
        label = type == QLatin1String("Intro")   ? i18n("Skip Intro")
                : type == QLatin1String("Recap") ? i18n("Skip Recap")
                : type == QLatin1String("Outro") ? i18n("Skip Outro")
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
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                       {QStringLiteral("tracks"), m_memory},
                                                       {QStringLiteral("queue"), m_queueMemory}})
                                 .toJson(QJsonDocument::Compact);
    if (bytes.size() > 256 * 1024)
        return;
    writePrivateFile(m_memoryFile, bytes);
}
void PlayerController::rememberTracks(int audioId, int subtitleId, int secondaryId)
{
    // Offline there is no signed-in owner to key the memory by
    if (m_itemId.isEmpty() || m_memoryFile.isEmpty() || m_client.serverId().isEmpty()
        || m_client.userId().isEmpty())
        return;
    QJsonObject entry{{QStringLiteral("audio"), audioId},
                      {QStringLiteral("subtitle"), subtitleId},
                      {QStringLiteral("at"), QDateTime::currentMSecsSinceEpoch()}};
    // Absent for none: a track's mpv id means nothing once the file is replaced
    if (secondaryId > 0)
        entry.insert(QStringLiteral("subtitle2"), secondaryId);
    const QString key = memoryKey();
    m_memory.insert(key, entry);
    evictOldestChoices(m_memory, key);
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
    // A same-item reopen knows the server's stream itself; otherwise the item's or the series' choice
    if (m_rememberSubtitles) {
        if (m_secondaryStreamIndex)
            restoreSecondarySubtitle();
        else if (tracks.secondary > 0)
            setSecondarySubtitle(tracks.secondary);
    }
    // The subtitle file added by hand that was selected before the reopen is not the item's remembered choice: it goes first
    for (auto added = m_addedSubtitleTracks.cbegin(); added != m_addedSubtitleTracks.cend();
         ++added) {
        if (!m_selectedAddedSubtitle.isEmpty() && added.value() == m_selectedAddedSubtitle) {
            m_engine.setMpvProperty(QStringLiteral("sid"), added.key());
            m_trackChosen = true;
            break;
        }
    }
}

void PlayerController::restoreSecondarySubtitle()
{
    if (!m_secondaryStreamIndex)
        return;
    if (const int id = trackOfStreamIndex(QStringLiteral("sub"), *m_secondaryStreamIndex); id > 0) {
        // The new file lists the same embedded track: it takes the second slot again
        if (setSecondarySubtitle(id))
            return;
    }
    // A server's external subtitle: the new stream offers it, so the file is downloaded again and selected once listed
    const bool offered = std::any_of(
        m_subtitleOptions.cbegin(), m_subtitleOptions.cend(),
        [this](const ExternalSubtitle &entry) { return entry.index == *m_secondaryStreamIndex; });
    if (offered) {
        // One download at a time: while the primary's is pending, its completion asks for this one
        if (downloadSubtitle(*m_secondaryStreamIndex, true) || !m_pendingSubtitle.isEmpty())
            return;
    }
    qCInfo(playerLog) << "the reopened stream has no second subtitle";
    m_secondaryStreamIndex.reset();
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
    return requestSubtitle(index, false);
}

bool PlayerController::downloadSecondarySubtitle(int index)
{
    return requestSubtitle(index, true);
}

bool PlayerController::requestSubtitle(int index, bool secondary)
{
    if (!active())
        return false;
    // A file loaded already is chosen, not fetched again
    if (const int id = trackOfStreamIndex(QStringLiteral("sub"), index); id != 0) {
        if (secondary)
            selectSecondarySubtitleTrack(id);
        else
            selectSubtitleTrack(id);
        return true;
    }
    // The newer choice for the same slot replaces one still on its way; the other slot's is kept, and the user hears why nothing happens
    if (!m_pendingSubtitle.isEmpty()) {
        if (m_pendingSubtitleSecondary != secondary) {
            Q_EMIT subtitleBusy();
            return false;
        }
        dropPendingSubtitle();
    }
    return downloadSubtitle(index, secondary);
}

void PlayerController::dropPendingSubtitle()
{
    if (m_pendingSubtitle.isEmpty())
        return;
    m_client.cancelExternalSubtitle();
    m_pendingSubtitle.clear();
    m_pendingSubtitleSecondary = false;
}

bool PlayerController::canUploadAddedSubtitles() const
{
    return active() && !m_localPlayback && !m_casting && m_client.isAdministrator();
}

bool PlayerController::addSubtitleFile(const QUrl &file)
{
    constexpr qint64 MaxSubtitleBytes = 2LL * 1024 * 1024;
    const auto refuse = [this](const QString &reason) {
        Q_EMIT subtitleFileRefused(reason);
        return false;
    };
    if (!active() || !m_engine.isInitialized())
        return refuse(i18n("Nothing is playing to add a subtitle to"));
    if (m_casting)
        return refuse(i18n("A subtitle cannot be added while casting"));
    // mpv has no file to add it to before the video has started
    if (!m_fileLoaded || m_stopping)
        return refuse(i18n("Wait until the video has started to add a subtitle"));
    if (!file.isLocalFile())
        return refuse(i18n("Only a subtitle file on this computer can be added"));
    const QFileInfo info(file.toLocalFile());
    // The Flatpak sees only what the portal hands it: a dropped file of the host may not exist here
    if (!info.exists())
        return refuse(i18n("The file cannot be opened from here. Choose it with Add Subtitle File "
                           "in the Subtitles menu"));
    const QString suffix = info.suffix().toLower();
    if (!info.isFile()
        || !QStringList{QStringLiteral("srt"), QStringLiteral("ass"), QStringLiteral("ssa"),
                        QStringLiteral("vtt")}
                .contains(suffix))
        return refuse(i18n("The file is not an srt, ass, ssa, or vtt subtitle"));
    if (m_addedSubtitles.size() >= MaxAddedSubtitles)
        return refuse(i18n("No more subtitle files can be added to this item"));
    // The read is bounded whatever the file has become since the check
    QFile source(info.absoluteFilePath());
    const QByteArray bytes = source.open(QIODevice::ReadOnly) && info.isReadable()
                                 ? source.read(MaxSubtitleBytes + 1)
                                 : QByteArray();
    if (bytes.isEmpty() || bytes.size() > MaxSubtitleBytes)
        return refuse(i18n("The subtitle must be a readable file of at most 2 MiB"));
    if (!m_subtitleDir.isValid())
        return refuse(
            i18n("The subtitle could not be added. Check the free disk space and try again"));
    // A copy of its own: the original may go away or change while the item plays, and a reopen adds it again from here
    const QString copy = m_subtitleDir.path() + QStringLiteral("/added-")
                         + QString::number(++m_subtitleFileSeq) + QLatin1Char('.') + suffix;
    QSaveFile out(copy);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
        return refuse(
            i18n("The subtitle could not be added. Check the free disk space and try again"));
    const QString title = info.completeBaseName();
    if (!m_engine.mpvCommand({QStringLiteral("sub-add"), copy, QStringLiteral("select"), title})) {
        QFile::remove(copy);
        return refuse(i18n("The file could not be read as a subtitle"));
    }
    // A choice made now wins over a download that is still on its way, and over the item's own remembered one
    m_detailsStreams.reset();
    if (!m_pendingSubtitleSecondary)
        dropPendingSubtitle();
    m_trackChosen = true;
    m_addedSubtitles.append({copy, title, QString()});
    m_loadedSubtitleOrder.append(copy);
    m_selectedAddedSubtitle = copy;
    m_subtitlesOff = false;
    QTimer::singleShot(250, this, [this] {
        refreshTracks();
        m_reporter.reportNow();
    });
    Q_EMIT subtitleFileAdded(title, QUrl::fromLocalFile(copy));
    return true;
}

bool PlayerController::downloadSubtitle(int index, bool secondary)
{
    if (!active() || !m_subtitleDir.isValid() || !m_pendingSubtitle.isEmpty())
        return false;
    for (const ExternalSubtitle &subtitle : m_subtitleOptions) {
        if (subtitle.index != index)
            continue;
        if (m_client.downloadExternalSubtitle(subtitle.url, subtitle.extracted)) {
            m_pendingSubtitle = subtitle.url;
            m_pendingSubtitleSecondary = secondary;
            if (secondary)
                m_secondaryStreamIndex = index;
            if (subtitle.extracted)
                Q_EMIT subtitleExtracting();
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
    const auto position = m_loading   ? std::optional<qint64>{}
                          : m_casting ? std::optional<qint64>(positionTicks())
                                      : m_engine.knownPositionTicks();
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
