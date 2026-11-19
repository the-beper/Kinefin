// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QTemporaryDir>
#include <QUrl>
#include "api/JellyfinClient.h"
#include "models/ItemDetails.h"
#include "player/BitrateFallback.h"
#include "player/GroupPlayback.h"
#include "player/PlaybackEngine.h"
#include "player/PlaybackReporter.h"
#include "player/PlayerSettings.h"
#include "player/SeriesPreferences.h"
#include "player/Trickplay.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

class MpvAbstractItem;
class MpvController;

namespace Kinefin {
class JellyfinClient;
class PlayQueue;
class ClientSettings;

// `01:24:10` with hours, `05:07` without; a negative count shows as zero
QString playbackTimeText(qint64 ticks, bool withHours);
// The chapters the player offers: sorted by start, one per start, none at or past a known duration; fewer than two count as none
QList<ChapterSummary> playableChapters(QList<ChapterSummary> chapters, qint64 durationTicks);
// The chapter playing at `ticks` among sorted chapters, -1 before the first
int chapterIndexAt(const QList<ChapterSummary> &chapters, qint64 ticks);
// Where a chapter step from `positionTicks` lands, -1 for nowhere: forward the next chapter's start; back the current chapter's start when more than 3 s into it, else the previous chapter's, else the start of the video
qint64 chapterStepTarget(const QList<ChapterSummary> &chapters, qint64 positionTicks,
                         int direction);

// The tracks to select once a file's tracks are known: the item's own remembered ids (`audio`, `subtitle`, 0 for off), then the series' choices, each kind only while the user remembers it. 0 leaves mpv's pick
struct RememberedTracks {
    int audio = 0;
    int subtitle = 0;
    bool subtitleOff = false;
};
RememberedTracks rememberedTracks(const QJsonObject &item, const SeriesChoice &series,
                                  const QList<TrackFact> &audio, const QList<TrackFact> &subtitles,
                                  bool rememberAudio, bool rememberSubtitles);

// When the Up Next card shows and when the next item starts, in ticks of the item
struct UpNextWindow {
    qint64 showAt = 0;
    qint64 advanceAt = 0;
};
// With an Outro segment that ends within 30 s of the end, the card shows at its start and the next item starts 10 s later (at the end at the latest); without one it shows 30 s before the end and the end itself starts the next item. None for an item under 10 minutes or of unknown duration
std::optional<UpNextWindow> upNextWindow(qint64 durationTicks, const QVariantList &segments);

// Single playback owner. Negotiation and server reporting stay on the authenticated
// client's event loop; mpv's worker thread is accessed only via MpvAbstractItem.
// Never persist or expose stream URLs or authentication headers to QML.
class PlayerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    // A file played from the downloads folder: no server session, no reporting, no server-only chrome
    Q_PROPERTY(bool localPlayback READ localPlayback NOTIFY localPlaybackChanged)
    // OpenSubtitles through the server plugin: hidden without the policy, one search at a time
    Q_PROPERTY(bool canManageSubtitles READ canManageSubtitles NOTIFY changed)
    Q_PROPERTY(QVariantList remoteSubtitles READ remoteSubtitles NOTIFY remoteSubtitlesChanged)
    Q_PROPERTY(
        bool remoteSubtitleSearching READ remoteSubtitleSearching NOTIFY remoteSubtitlesChanged)
    Q_PROPERTY(QString remoteSubtitleError READ remoteSubtitleError NOTIFY remoteSubtitlesChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY changed)
    Q_PROPERTY(bool buffering READ buffering NOTIFY changed)
    Q_PROPERTY(QString itemId READ itemId NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString subtitle READ subtitle NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    // The error can be retried in place: a lowered stream that did not open
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY changed)
    Q_PROPERTY(qint64 positionTicks READ positionTicks NOTIFY changed)
    Q_PROPERTY(qint64 durationTicks READ durationTicks NOTIFY changed)
    Q_PROPERTY(bool canSeek READ canSeek NOTIFY changed)
    Q_PROPERTY(double rate READ rate NOTIFY changed)
    Q_PROPERTY(int volume READ volume NOTIFY changed)
    Q_PROPERTY(bool muted READ muted NOTIFY changed)
    Q_PROPERTY(double audioDelay READ audioDelay NOTIFY changed)
    Q_PROPERTY(double subtitleDelay READ subtitleDelay NOTIFY changed)
    Q_PROPERTY(bool shuffle READ shuffle NOTIFY changed)
    Q_PROPERTY(int repeatMode READ repeatMode NOTIFY changed)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY changed)
    Q_PROPERTY(int queueCount READ queueCount NOTIFY changed)
    Q_PROPERTY(bool hasNext READ hasNext NOTIFY changed)
    Q_PROPERTY(bool hasPrevious READ hasPrevious NOTIFY changed)
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList subtitleTracks READ subtitleTracks NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList externalSubtitles READ externalSubtitles NOTIFY tracksChanged)
    Q_PROPERTY(QString previewImage READ previewImage NOTIFY previewChanged)
    Q_PROPERTY(QString screenshotPath READ screenshotPath NOTIFY changed)
    Q_PROPERTY(QString skipLabel READ skipLabel NOTIFY changed)
    Q_PROPERTY(int sleepRemainingMinutes READ sleepRemainingMinutes NOTIFY changed)
    Q_PROPERTY(QString aspectMode READ aspectMode NOTIFY changed)
    // The chapters of the playing item, from the server: `name` (plain text, "Chapter N" when the server gives none), `startTicks` and `time` (as timeText shows it)
    Q_PROPERTY(QVariantList chapters READ chapters NOTIFY chaptersChanged)
    // Notified only when the chapter changes, not with every position
    Q_PROPERTY(int currentChapter READ currentChapter NOTIFY currentChapterChanged)
    // The Up Next card: up near the end of an item whose end starts another one (never under Repeat One or for a lone item) until Cancel; `upNext` holds that item's `title`, `subtitle` (plain text) and `imageUrl`
    Q_PROPERTY(bool upNextShown READ upNextShown NOTIFY upNextChanged)
    Q_PROPERTY(QVariantMap upNext READ upNext NOTIFY upNextChanged)
    // Whole seconds of media time, at the current rate, until the next item starts
    Q_PROPERTY(int upNextSeconds READ upNextSeconds NOTIFY upNextChanged)
    // The episode playing belongs to a series with remembered choices (tracks, speed, aspect)
    Q_PROPERTY(bool hasSeriesChoices READ hasSeriesChoices NOTIFY seriesChoicesChanged)
  public:
    // What the desktop sees (MPRIS, the screensaver): opening, loading and buffering count as playing, stopping and a failure as stopped
    enum class Status { Stopped, Playing, Paused };
    static constexpr double MinimumRate = 0.5;
    static constexpr double MaximumRate = 4.0;

    PlayerController(JellyfinClient &client, PlayQueue &queue, ClientSettings &settings,
                     const QString &profileId, QObject *parent = nullptr);
    ~PlayerController() override;

    bool active() const
    {
        return m_loading || m_awaitingGroup || m_session.state() != PlaybackSession::State::Idle;
    }
    bool loading() const
    {
        return m_loading || m_awaitingGroup || m_session.state() == PlaybackSession::State::Opening;
    }
    bool localPlayback() const { return m_localPlayback; }
    bool canManageSubtitles() const;
    QVariantList remoteSubtitles() const { return m_remoteSubtitles; }
    bool remoteSubtitleSearching() const { return m_remoteSubtitleSearching; }
    QString remoteSubtitleError() const { return m_remoteSubtitleError; }
    bool paused() const { return m_session.state() == PlaybackSession::State::Paused; }
    bool buffering() const { return m_session.state() == PlaybackSession::State::Buffering; }
    QString itemId() const { return m_itemId; }
    // The series name for an episode, else the item's name: empty until the server answered
    QString title() const { return m_title; }
    // The episode label and name of an episode (`S2:E6 — Weightless`), empty for anything else
    QString subtitle() const { return m_subtitle; }
    QString errorText() const { return m_errorText; }
    bool canRetry() const { return m_canRetry; }
    qint64 positionTicks() const { return m_engine.positionTicks(); }
    qint64 durationTicks() const { return m_engine.durationTicks(); }
    bool canSeek() const { return m_engine.canSeek(); }
    double rate() const { return m_engine.playbackRate(); }
    int volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    double audioDelay() const { return m_audioDelay; }
    double subtitleDelay() const { return m_subtitleDelay; }
    // A group's own while one takes the player's actions
    bool shuffle() const { return grouped() ? m_group->shuffle() : m_shuffle; }
    int repeatMode() const { return grouped() ? m_group->repeatMode() : m_repeat; }
    // The saved queue only: the episodes of a series played alone are not in it
    int currentIndex() const { return m_playlistEphemeral ? -1 : m_queueIndex; }
    int queueCount() const { return m_playlistEphemeral ? 0 : m_playlist.size(); }
    // Whether Next and Previous have somewhere to go, in the saved queue or the episodes of the series
    bool hasNext() const { return grouped() ? m_group->hasNext() : nextIndex(1, false) >= 0; }
    bool hasPrevious() const
    {
        return grouped() ? m_group->hasPrevious() : nextIndex(-1, false) >= 0;
    }
    QVariantList audioTracks() const { return m_audioTracks; }
    QVariantList subtitleTracks() const { return m_subtitleTracks; }
    QVariantList externalSubtitles() const { return m_externalSubtitles; }
    QString previewImage() const { return m_previewImage; }
    QString screenshotPath() const { return m_screenshotPath; }
    QString skipLabel() const { return m_skipLabel; }
    // The aspect mode of this playback: the `video.aspect` setting until a choice is made, and again for the next item
    QString aspectMode() const { return m_mpvSettings.aspectMode(); }
    QVariantList chapters() const { return m_chapterList; }
    // The chapter playing now, -1 before the first or without chapters
    int currentChapter() const { return m_currentChapter; }
    bool upNextShown() const { return m_upNextShown; }
    QVariantMap upNext() const { return m_upNext; }
    int upNextSeconds() const { return m_upNextSeconds; }
    bool hasSeriesChoices() const
    {
        return !m_itemSeriesId.isEmpty() && m_seriesPreferences.contains(m_itemSeriesId);
    }
    int sleepRemainingMinutes() const;
    Status status() const;
    // The token-free URL of the current item's primary image (512 px), empty until the server named its tag
    QUrl artworkUrl() const;

    // Rendering item owns the libmpv handle. The call is made on its ready signal,
    // and detach precedes destruction (including account switch and window close).
    bool attachVideo(MpvAbstractItem *item, MpvController *controller);
    void detachVideo(MpvAbstractItem *item);
    // The attached item got a new render context (it moved to another window), after which libmpv drops the picture: selects the video track again, nothing else changes
    void videoRendererChanged(MpvAbstractItem *item);

    Q_INVOKABLE bool playItem(const QString &id, qint64 resumeTicks = 0);
    // Plays a completed download from its local path; title and subtitle come from the download record. No request is made and nothing is reported
    Q_INVOKABLE bool playLocalFile(const QString &path, const QString &itemId, const QString &title,
                                   const QString &subtitle = QString(), qint64 resumeTicks = 0);
    // Plays the queue from a row, at `startTicks` (0 to 24 h; else refused)
    Q_INVOKABLE bool playQueue(int index, qint64 startTicks = 0);
    Q_INVOKABLE bool resumeQueue();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE bool next();
    Q_INVOKABLE bool previous();
    Q_INVOKABLE void seek(qint64 ticks);
    Q_INVOKABLE void seekRelative(int seconds);
    Q_INVOKABLE void setRate(double rate);
    Q_INVOKABLE void setVolume(int level);
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void setShuffle(bool shuffle);
    Q_INVOKABLE void setRepeatMode(int mode); // 0 off, 1 all, 2 one
    Q_INVOKABLE void selectAudioTrack(int id);
    Q_INVOKABLE void selectSubtitleTrack(int id); // 0 = none
    // Selects the track whose server `MediaStream.Index` (mpv's `ff-index`) is `index`, in a direct play only; false, with nothing changed, when nothing plays, the server transcodes, the index is out of range (0 to 1024) or no track of that type has it
    Q_INVOKABLE bool selectAudioStreamIndex(int index);
    // As selectAudioStreamIndex for a subtitle track; -1 selects none
    Q_INVOKABLE bool selectSubtitleStreamIndex(int index);
    Q_INVOKABLE void adjustAudioDelay(double seconds);
    Q_INVOKABLE void adjustSubtitleDelay(double seconds);
    Q_INVOKABLE void setSleepTimer(int minutes); // 0 disables
    Q_INVOKABLE void screenshot();
    Q_INVOKABLE void skipSegment();
    Q_INVOKABLE bool downloadSubtitle(int index);
    Q_INVOKABLE void previewAt(qint64 ticks);
    Q_INVOKABLE void clearPreview();
    Q_INVOKABLE void refreshTracks();
    // Search the server's subtitle provider for the current item and language (ISO 639-2)
    Q_INVOKABLE bool searchRemoteSubtitles(const QString &language);
    // Download one result; on success the item reopens at the current position so the new track is in the source, keeping the queue and the series list
    Q_INVOKABLE bool downloadRemoteSubtitle(const QString &subtitleId);
    // Opens the item again where the lowered stream failed, at the bit rate before that step; false when there is nothing to retry
    Q_INVOKABLE bool retryPlayback();
    // The timing of the starvation rule, for the tests
    void setFallbackTiming(const BitrateFallback::Timing &timing) { m_fallback.setTiming(timing); }
    // One of the `video.aspect` values except `custom`: applied at once to this playback only, the setting is not written
    Q_INVOKABLE void setAspectMode(const QString &mode);
    // The chapter at a position of the timeline, -1 before the first or without chapters
    Q_INVOKABLE int chapterAt(qint64 ticks) const { return chapterIndexAt(m_chapters, ticks); }
    Q_INVOKABLE void jumpToChapter(int index);
    // 1 for the next chapter, -1 for the previous one (see chapterStepTarget)
    Q_INVOKABLE void stepChapter(int direction);
    // Play Now on the Up Next card: what it offers starts at once, as Next does
    Q_INVOKABLE void playUpNext();
    // Cancel on the Up Next card: the end of this item finishes the playback instead of starting the next one
    Q_INVOKABLE void cancelUpNext();
    // The next episodes of this series start from the settings again; this one keeps what it plays with
    Q_INVOKABLE void forgetSeriesChoices();
    // What the Playback Info overlay shows, from the negotiated source and the facts the engine cached from mpv (no network, no mpv call); a key is present only when its value is known. Never holds the URL or the token
    // - `playMethod`: DirectPlay or Transcode
    // - `streamType`, `protocol`: File, HLS or the container extension; http or https
    // - `videoWidth`, `videoHeight`, `droppedFrames`, `decoderDroppedFrames`, `hwdec`: mpv's dwidth, dheight, frame-drop-count, decoder-frame-drop-count and hwdec-current
    // - `targetVideoCodec`, `targetAudioCodec`, `transcodeReasons` (a list of the server's enum names): of a transcode
    // - `container`, `sizeBytes`, `bitrate` of the media source
    // - `videoCodec`, `videoProfile`, `videoLevel`, `videoBitrate`, `videoFps`, `videoBitDepth`, `videoPixelFormat`, `videoRange`, `colorSpace`, `colorPrimaries`, `colorTransfer`
    // - `audioCodec`, `audioChannels`, `audioBitrate`, `audioSampleRate` of the playing audio track
    Q_INVOKABLE QVariantMap playbackInfo() const;
    // Elapsed or remaining time of the video: hours show when its duration is at least one hour
    Q_INVOKABLE QString timeText(qint64 ticks) const;
    // Used before a session changes, never by QML to re-authenticate.
    void onAccountLeaving();
    // A watching-together group that takes the user's pause, resume, seek and play actions while it routes them, with speed, shuffle, repeat, Next and Previous refused meanwhile; null for none. Not owned
    void setGroupPlayback(GroupPlayback *group) { m_group = group; }
    // The group's commands, C++ only so that QML, MPRIS and the remote control can only reach the routed actions. openGroupItem opens an item of the group paused at `startTicks` (0 to 24 h), at speed 1.0, without the series' episodes and leaving the saved queue's position as it was; false for an invalid id or position
    bool openGroupItem(const QString &id, qint64 startTicks);
    void applyPause();
    void applyUnpause();
    void applySeek(qint64 ticks);
    // Stops whatever plays
    void applyStop();
    // A speed that brings the playback back in step with the group, 0.9 to 1.1
    void applyRate(double rate);
    // The group's state changed: the properties that read it (Next, Previous, shuffle, repeat) are read again
    void notifyGroupChanged() { Q_EMIT changed(); }
    // The group was left: what plays is the user's own again (its end may repeat, the speed setting applies)
    void releaseGroupItem();
    // A seek was asked of mpv and has not landed yet
    [[nodiscard]] bool seeking() const { return m_engine.seeking(); }
    // The group's item ended and the player waits for the group's next one
    [[nodiscard]] bool awaitingGroup() const { return m_awaitingGroup; }
    // The group's item `id` is loaded: what the group's pause, unpause and seek act on
    [[nodiscard]] bool groupItemLoaded(const QString &id) const;
    // Loaded, its position known and no seek running: the moment a group hears Ready
    [[nodiscard]] bool groupItemSettled(const QString &id) const;
    // The signed-in user's EnableNextEpisodeAutoPlay: whether the end of an episode played alone starts the next one
    void setNextEpisodeAutoPlay(bool enabled)
    {
        m_nextEpisodeAutoPlay = enabled;
        updateUpNext();
    }
    // The signed-in user's RememberAudioSelections and RememberSubtitleSelections: whether the item's and the series' track choices are applied
    void setRememberSelections(bool audio, bool subtitles)
    {
        m_rememberAudio = audio;
        m_rememberSubtitles = subtitles;
    }
    // A user's (or every user of a server's) track memory, queue position and series choices go
    void forgetStoredUser(const QString &serverId, const QString &userId);
    void forgetStoredServer(const QString &serverId);

  Q_SIGNALS:
    void changed();
    void localPlaybackChanged();
    // A local file's position, and whether it reached its end; the shell stores it on the download
    void localPositionChanged(const QString &itemId, qint64 ticks, bool played);
    void remoteSubtitlesChanged();
    void remoteSubtitleDownloaded(const QString &subtitleId);
    void remoteSubtitleDownloadFailed(const QString &subtitleId, int httpStatus);
    // The stream kept starving and plays on at a lower bit rate (bit/s, a step of the fallback ladder)
    void qualityLowered(qint64 bitsPerSecond);
    // The stream kept starving and the request for a lower one failed or was refused: playback goes on as it was
    void qualityUnavailable();
    // A seek was asked of mpv, to this position
    void seeked(qint64 ticks);
    void tracksChanged();
    void chaptersChanged();
    void currentChapterChanged();
    void upNextChanged();
    void seriesChoicesChanged();
    void previewChanged();
    void finished();

  private:
    // The listing channel of the series' episodes (see the channel table in the architecture doc)
    static constexpr int EpisodesChannel = 4096;
    // A provider may answer with thousands of results; the dialog lists the first ones
    static constexpr int MaxRemoteSubtitles = 100;
    bool remoteSearchIsCurrent(const QString &itemId, const QString &language) const;
    void resetRemoteSubtitles();
    // Emits localPositionChanged for the local file playing, if it loaded; `ended` is a real end of file
    void reportLocalPosition(bool ended);

    // `grouped` starts an item of a watching-together group (see openGroupItem)
    bool startItem(const QString &id, qint64 resumeTicks, bool grouped = false);
    // Whether the user's playback actions go to a group now
    bool grouped() const { return m_group && m_group->routesPlayback(); }
    void seekLocally(qint64 ticks);
    void stopLocally();
    // Something plays and the server sends the source as it is, so mpv's `ff-index` is the source's stream index
    bool directPlay() const;
    // mpv's track id of the `audio` or `sub` track with this `ff-index`, 0 when there is none or it is not a direct play
    int trackOfStreamIndex(const QString &type, int index) const;
    // The server's stream index of mpv's track `id` of `type`: direct play and embedded tracks only
    std::optional<int> streamIndexOfTrack(const QString &type, int id) const;
    void requestPending();
    // The fallback: the stream keeps starving, so the server is asked for the same item at `cap`
    void onStarving(qint64 cap);
    // A PlaybackInfo reply that came while the item plays: the lowered stream, accepted when it is lower
    void acceptFallback(const PlaybackSource &source);
    bool fallbackApplies() const;
    bool fallbackEligible() const;
    std::optional<qint64> currentStreamBitrate() const;
    JellyfinClient::StreamChoice currentStreams() const;
    void dropFallback();
    bool openLocal();
    void onSource(const PlaybackSource &source);
    void onPlaybackItem(const QString &id, const QJsonObject &item);
    void onSeriesEpisodes(const QJsonArray &items, int total, int channel);
    // Turns the episodes read so far into the series playlist, or leaves the episode alone when they do not hold it
    void finishSeriesPlaylist();
    void updateTrickplayLayout();
    // Offers the item's chapters that fall within the duration known now
    void updateChapters();
    void updateCurrentChapter();
    void cancelSeriesRequest();
    void dropSeriesPlaylist();
    void onEnd(const QString &reason);
    void onState(PlaybackSession::State from, PlaybackSession::State to);
    // `pick` false answers whether a step exists without drawing the random index of shuffle
    int nextIndex(int direction, bool pick = true) const;
    // Whether the end of this item moves on in the playlist: the saved queue always does, the episodes of a series with the user's autoplay preference or an explicit repeat mode
    bool advancesAtEnd() const
    {
        return !m_playlistEphemeral || m_nextEpisodeAutoPlay || m_repeat != 0;
    }
    // The playlist index of the other item the end would start, -1 when it replays this one (Repeat One, a lone item under Repeat All) or stops
    int pendingAdvance(bool pick) const;
    // The Up Next card follows the position, the duration, the segments, the rate and the playlist
    void updateUpNext();
    void resetUpNext();
    // The title, subtitle and picture of an item of the playlist, from the saved queue or the series listing
    QVariantMap upNextInfo(const QString &id) const;
    // The series store, for the signed-in user
    SeriesPreferences &seriesStore();
    // A track choice is the series' only for an episode played directly: a transcode carries the server's one audio track
    bool keepsSeriesTracks() const
    {
        return !m_itemSeriesId.isEmpty() && m_source.method == PlaybackSource::Method::Direct;
    }
    // The series' speed and aspect, once the file is loaded and the series known; a restore, never a choice
    void applySeriesPlayback();
    // Track memory and queue positions of an owner (`server` or `server/user`) go
    void pruneMemory(const QString &owner);
    // The Intro segments around `ticks` are never skipped automatically: the user went there. Before the segments are known the position is kept for when they come
    void keepIntrosAt(qint64 ticks);
    void refreshSegments();
    void updateSkip();
    QString memoryKey() const;
    void restoreTracks();
    void rememberTracks(int audioId, int subtitleId);
    void loadMemory();
    void saveMemory();
    void clearCurrent();
    void updatePlaylist();
    void persistQueueCursor();
    void displayTileFrame(const QImage &tile, const TrickplayFrame &frame);

    JellyfinClient &m_client;
    PlayQueue &m_queue;
    GroupPlayback *m_group = nullptr;
    // The item playing (or loading) is the group's: opened paused at speed 1.0, its end never moves on alone
    bool m_groupItem = false;
    // The group's item ended and the group moves on: the player waits for the next one (bounded by m_groupAdvance)
    bool m_awaitingGroup = false;
    QTimer m_groupAdvance;
    ClientSettings &m_settings;
    QString m_memoryFile;
    PlaybackSession m_session;
    PlaybackEngine m_engine;
    PlaybackReporter m_reporter;
    PlayerSettings m_mpvSettings;
    PlaybackSession::ObserverId m_observerId = 0;
    QPointer<MpvAbstractItem> m_video;
    QTimer m_sleep;
    QTimer m_clock;
    // One report after a run of seeks, however many there were
    QTimer m_seekReport;
    QTimer m_trackTimer;
    // Wall-clock time since the current file loaded
    QElapsedTimer m_playedClock;
    // The playback rate a restart of the same item keeps, applied after the settings
    std::optional<double> m_keptRate;
    QJsonObject m_memory;
    QJsonObject m_queueMemory;
    QTemporaryDir m_subtitleDir;
    PlaybackSource m_source;
    QList<ExternalSubtitle> m_subtitleOptions;
    std::optional<TrickplayLayout> m_trickplay;
    QJsonObject m_trickplayInfo;
    // The chapters as the server sent them, and those the player offers
    QList<ChapterSummary> m_itemChapters;
    QList<ChapterSummary> m_chapters;
    QVariantList m_chapterList;
    int m_currentChapter = -1;
    // What the Up Next card offers, drawn once under shuffle: Next and the end start it while the card is up
    QString m_upNextId;
    QVariantMap m_upNext;
    int m_upNextSeconds = 0;
    bool m_upNextShown = false;
    // Cancel on the card, or a jump past its early advance: this playback ends without moving on
    bool m_upNextCancelled = false;
    // The card's title, subtitle and picture of each episode of the series listing (by lower-case id), and of the pages read so far
    QHash<QString, QVariantMap> m_seriesInfo;
    QHash<QString, QVariantMap> m_pendingSeriesInfo;
    // The starts of the Intro segments skipped, or entered by a seek, in this playback; the seeks made before the segments came; where the playback resumed (no Intro before it is skipped)
    QList<qint64> m_handledIntros;
    QList<qint64> m_earlySeeks;
    qint64 m_resumedFrom = 0;
    SeriesPreferences m_seriesPreferences;
    // The series of the episode playing, empty for anything else
    QString m_itemSeriesId;
    // The audio and subtitle tracks of the file in mpv's order, with their language and forced flag
    QList<TrackFact> m_audioFacts;
    QList<TrackFact> m_subtitleFacts;
    bool m_rememberAudio = true;
    bool m_rememberSubtitles = true;
    // The speed of this playback is decided (a repeat kept it, or the user chose one); a track was chosen since the file loaded; the file is loaded; the series' speed and aspect were applied
    bool m_rateDecided = false;
    bool m_trackChosen = false;
    bool m_fileLoaded = false;
    bool m_seriesApplied = false;
    std::optional<TrickplayFrame> m_requestedFrame;
    QImage m_lastTile;
    QString m_previewImage;
    QString m_screenshotPath;
    QString m_profileDataDir;
    QUrl m_pendingSubtitle;
    int m_cachedTileIndex = -1;
    int m_pendingTileIndex = -1;
    QVariantList m_audioTracks;
    QVariantList m_subtitleTracks;
    QVariantList m_externalSubtitles;
    QVariantList m_segments;
    QStringList m_playlist;
    QString m_itemId;
    QString m_title;
    QString m_subtitle;
    QString m_primaryTag;
    // The item whose series' episodes are being listed, set while that request is in flight
    QString m_seriesListItem;
    QString m_seriesId;
    // The playable episodes of the pages read so far, and where the playing one sits among them (-1: not seen yet)
    QStringList m_seriesEpisodes;
    int m_seriesEpisodeIndex = -1;
    int m_seriesNextStart = 0;
    QString m_errorText;
    QString m_skipLabel;
    qint64 m_skipEnd = 0;
    qint64 m_resumeTicks = 0;
    int m_queueIndex = -1;
    int m_repeat = 0;
    int m_volume = 100;
    bool m_muted = false;
    double m_audioDelay = 0;
    double m_subtitleDelay = 0;
    bool m_shuffle = false;
    // m_playlist holds the episodes of the series of an episode played alone, not the saved queue: it never reaches the queue file or the server's reports
    bool m_playlistEphemeral = false;
    // playItem started an item whose reply may still ask for its series' episodes
    bool m_seriesPlaylistWanted = false;
    bool m_nextEpisodeAutoPlay = true;
    bool m_loading = false;
    bool m_localPlayback = false;
    QVariantList m_remoteSubtitles;
    bool m_remoteSubtitleSearching = false;
    // "<itemId>/<language>" of the search in flight: a reply for another one is dropped
    QString m_remoteSubtitleQuery;
    // The bit rate fallback: the rule, the request in flight, the lowered source waiting for the reopen and what the last request asked for
    BitrateFallback m_fallback;
    struct FallbackRequest {
        QString playSessionId;
        qint64 cap = 0;
        JellyfinClient::StreamChoice streams;
        bool active = false;
    } m_fallbackRequest;
    std::optional<PlaybackSource> m_preparedSource;
    // The file opening is the lowered stream: its load completes the step, its failure offers Try Again
    bool m_switchingStream = false;
    qint64 m_requestedCap = UnlimitedBitrate;
    JellyfinClient::StreamChoice m_requestedStreams;
    // The cap before the step that failed, and where it failed: what Try Again opens again
    qint64 m_capBeforeStep = UnlimitedBitrate;
    qint64 m_retryTicks = 0;
    bool m_canRetry = false;
    // The item of the subtitle download in flight, empty when none runs
    QString m_remoteSubtitleDownloadItem;
    QString m_remoteSubtitleError;
    // The reopen after a subtitle download keeps a paused video paused
    bool m_reopenPaused = false;
    struct PendingLocal {
        QString path;
        QString itemId;
        QString title;
        QString subtitle;
        qint64 resumeTicks = 0;
        bool pending = false;
    } m_pendingLocal;
    bool m_stopping = false;
    bool m_trackRestored = false;
    int m_selectedAudioId = 0;
    int m_selectedSubtitleId = 0;
    std::optional<int> m_selectedAudioIndex;
    std::optional<int> m_selectedSubtitleIndex;
};
} // namespace Kinefin
