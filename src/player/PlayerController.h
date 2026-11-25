// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QTemporaryDir>
#include <QUrl>
#include "api/JellyfinClient.h"
#include "cast/CastController.h"
#include "models/ItemDetails.h"
#include "player/BitrateFallback.h"
#include "player/ClipExporter.h"
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
#include <algorithm>
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

// The tracks to select once a file's tracks are known: the item's own remembered ids (`audio`, `subtitle`, 0 for off; `secondary`, 0 for none), then the series' choices, each kind only while the user remembers it. 0 leaves mpv's pick
struct RememberedTracks {
    int audio = 0;
    int subtitle = 0;
    bool subtitleOff = false;
    // The second subtitle (mpv's `secondary-sid`): a text track other than the primary
    int secondary = 0;
};
RememberedTracks rememberedTracks(const QJsonObject &item, const SeriesChoice &series,
                                  const QList<TrackFact> &audio, const QList<TrackFact> &subtitles,
                                  bool rememberAudio, bool rememberSubtitles);

// A subtitle codec mpv renders as text: only these can take the second slot
bool textSubtitleCodec(const QString &codec);

// The track ids a cache dump gives the player's choices: the dump holds only the streams mpv had selected, numbered per kind in their order, and the exporter adds `subtitleFiles` after them. `tracks` is mpv's `track-list`; 0 is none, also for a choice the dump cannot carry
struct DumpTrackIds {
    int audio = 0;
    int subtitle = 0;
    int secondarySubtitle = 0;
};
DumpTrackIds dumpTrackIds(const QVariantList &tracks, int audio, int subtitle,
                          int secondarySubtitle, const QStringList &subtitleFiles);

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
    // The DLNA renderers the Cast picker lists, and the one that plays
    Q_PROPERTY(Kinefin::CastController *cast READ castController CONSTANT)
    Q_PROPERTY(bool casting READ casting NOTIFY changed)
    // The renderer chosen for this playback (its UDN), empty when it plays here
    Q_PROPERTY(QString castTargetId READ castTargetId NOTIFY changed)
    Q_PROPERTY(QString castTargetName READ castTargetName NOTIFY changed)
    Q_PROPERTY(bool castCanPause READ castCanPause NOTIFY changed)
    Q_PROPERTY(bool castHasVolume READ castHasVolume NOTIFY changed)
    // A cast that failed left its item here: Continue Here plays it on this computer from where the device stopped
    Q_PROPERTY(bool canContinueHere READ canContinueHere NOTIFY changed)
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
    Q_PROPERTY(bool sleepAtEndOfItem READ sleepAtEndOfItem NOTIFY changed)
    Q_PROPERTY(bool sleepAtEndOfQueue READ sleepAtEndOfQueue NOTIFY changed)
    Q_PROPERTY(QString aspectMode READ aspectMode NOTIFY changed)
    // The night-mode filter of the audio chain, remembered per profile: the OSD's check and the Settings switch are one setting
    Q_PROPERTY(bool nightMode READ nightMode NOTIFY changed)
    // Clip and GIF marks of the playing item, -1 when unset: the line the Clip menu states and the two Save entries
    Q_PROPERTY(qint64 clipStartTicks READ clipStartTicks NOTIFY changed)
    Q_PROPERTY(qint64 clipEndTicks READ clipEndTicks NOTIFY changed)
    // Whether the marked part plays over and over (mpv's A-B loop), and whether it can: both marks set with the end after the start, no renderer, no group (a loop seeks, and every member would)
    Q_PROPERTY(bool clipLooping READ clipLooping NOTIFY changed)
    Q_PROPERTY(bool canLoopClip READ canLoopClip NOTIFY changed)
    Q_PROPERTY(QString clipLine READ clipLine NOTIFY changed)
    // Why neither Save entry can be used with the current range, empty when they can (route, HDR, length, encoder)
    Q_PROPERTY(QString clipReason READ clipReason NOTIFY changed)
    // The length limit the marked range exceeds, empty when it fits (the GIF limit under 60 s, the video one above)
    Q_PROPERTY(QString clipLengthReason READ clipLengthReason NOTIFY changed)
    Q_PROPERTY(bool clipVideoEnabled READ clipVideoEnabled NOTIFY changed)
    Q_PROPERTY(bool clipGifEnabled READ clipGifEnabled NOTIFY changed)
    // The Quality submenu: Automatic (0) or a pinned bit rate, disabled for local files, groups and without the transcode policy
    Q_PROPERTY(qint64 manualQualityCap READ manualQualityCap NOTIFY changed)
    Q_PROPERTY(bool qualityAvailable READ qualityAvailable NOTIFY changed)
    Q_PROPERTY(bool qualitySwitching READ qualitySwitching NOTIFY changed)
    Q_PROPERTY(QVariantList qualitySteps READ qualitySteps NOTIFY changed)
    Q_PROPERTY(bool clipSaving READ clipSaving NOTIFY clipStateChanged)
    Q_PROPERTY(bool clipSavingGif READ clipSavingGif NOTIFY clipStateChanged)
    Q_PROPERTY(double clipProgress READ clipProgress NOTIFY clipStateChanged)
    // The chapters of the playing item, from the server: `name` (plain text, "Chapter N" when the server gives none), `startTicks` and `time` (as timeText shows it)
    Q_PROPERTY(QVariantList chapters READ chapters NOTIFY chaptersChanged)
    // The Intro, Recap and Outro segments of the playing item for the seek bar: `type`, `startTicks` and `endTicks`, in the order they start; empty while they are not known
    Q_PROPERTY(QVariantList segmentBands READ segmentBands NOTIFY segmentsChanged)
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
    CastController *castController() const { return m_cast; }
    bool casting() const { return m_casting; }
    QString castTargetId() const { return castTarget() ? m_castTarget : QString(); }
    QString castTargetName() const { return m_castName; }
    bool castCanPause() const { return m_casting && m_cast->canPause(); }
    bool castHasVolume() const { return m_casting && m_cast->hasVolume(); }
    bool canContinueHere() const { return m_castFailed && !m_itemId.isEmpty(); }
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
    qint64 positionTicks() const
    {
        return m_casting ? m_castPositionMs * 10'000 : m_engine.positionTicks();
    }
    qint64 durationTicks() const
    {
        return m_casting ? m_castDurationMs * 10'000 : m_engine.durationTicks();
    }
    bool canSeek() const { return m_casting ? m_cast->canSeek() : m_engine.canSeek(); }
    double rate() const { return m_casting ? 1.0 : m_engine.playbackRate(); }
    int volume() const { return m_casting ? std::max(0, m_cast->volume()) : m_volume; }
    bool muted() const { return m_casting ? m_cast->muted() : m_muted; }
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
    bool nightMode() const;
    qint64 clipStartTicks() const { return m_clipStart; }
    qint64 clipEndTicks() const { return m_clipEnd; }
    bool clipLooping() const { return m_clipLooping; }
    bool canLoopClip() const
    {
        return m_clipStart >= 0 && m_clipEnd > m_clipStart && active() && !m_casting && !grouped();
    }
    QString clipLine() const;
    QString clipReason() const;
    QString clipLengthReason() const;
    bool clipVideoEnabled() const { return !m_clipExporter.running() && clipReady(false, nullptr); }
    bool clipGifEnabled() const { return !m_clipExporter.running() && clipReady(true, nullptr); }
    bool clipSaving() const { return m_clipExporter.running(); }
    bool clipSavingGif() const { return m_clipGif; }
    qint64 manualQualityCap() const { return m_manualCap; }
    bool qualityAvailable() const;
    bool qualitySwitching() const { return m_qualitySwitching; }
    QVariantList qualitySteps() const;
    double clipProgress() const { return m_clipProgress; }
    QVariantList chapters() const { return m_chapterList; }
    QVariantList segmentBands() const { return m_segmentBands; }
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
    bool sleepAtEndOfItem() const { return m_sleepAtEndOfItem; }
    bool sleepAtEndOfQueue() const { return m_sleepAtEndOfQueue; }
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
    // As playItem, with the audio and subtitle stream indexes chosen on the details page before playback: -1 leaves the choice to mpv or the server, a subtitle -1 turns subtitles off. The indexes are the server's (a direct play) and also go in the PlaybackInfo request (a transcode)
    Q_INVOKABLE bool playItemWithStreams(const QString &id, qint64 resumeTicks,
                                         int audioStreamIndex, int subtitleStreamIndex);
    // Hands the current playback to a renderer at the current position; `local` brings a cast back here at the renderer's position, or a failed cast's item from where it stopped
    Q_INVOKABLE bool castTo(const QString &udn);
    // Starts `id` on a renderer, or here for `local` (a cast stops first)
    Q_INVOKABLE bool playItemOn(const QString &udn, const QString &id, qint64 resumeTicks = 0);
    Q_INVOKABLE void stopCasting();
    // A subtitle file of a download, checked to be one of its own
    struct LocalSubtitle {
        QString path;
        QString title;
        QString language;
    };
    // Plays a completed download from its local path; title and subtitle come from the download record, `extras` its `chapters` and `subtitles` (DownloadsController::playableDownload). No request is made and nothing is reported
    Q_INVOKABLE bool playLocalFile(const QString &path, const QString &itemId, const QString &title,
                                   const QString &subtitle = QString(), qint64 resumeTicks = 0,
                                   const QVariantMap &extras = {});
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
    // The second subtitle shown with the primary (mpv's `secondary-sid`); 0 turns it off. Only a text track the primary does not use takes the slot
    Q_INVOKABLE void selectSecondarySubtitleTrack(int id); // 0 = none
    // Selects the track whose server `MediaStream.Index` (mpv's `ff-index`) is `index`, in a direct play only; false, with nothing changed, when nothing plays, the server transcodes, the index is out of range (0 to 1024) or no track of that type has it
    Q_INVOKABLE bool selectAudioStreamIndex(int index);
    // As selectAudioStreamIndex for a subtitle track; -1 selects none
    Q_INVOKABLE bool selectSubtitleStreamIndex(int index);
    Q_INVOKABLE void adjustAudioDelay(double seconds);
    Q_INVOKABLE void adjustSubtitleDelay(double seconds);
    Q_INVOKABLE void setSleepTimer(int minutes); // 0 disables every mode
    // Stop when the item playing now ends, before the queue or the next episode starts
    Q_INVOKABLE void setSleepAtEndOfItem(bool enabled);
    // Stop when the queue reaches its end (before a repeat wraps); a lone item ends like End of Current Item
    Q_INVOKABLE void setSleepAtEndOfQueue(bool enabled);
    Q_INVOKABLE void screenshot();
    // The clip marks at the current position; both are cleared by another item and by a stop
    Q_INVOKABLE void markClipStart();
    Q_INVOKABLE void markClipEnd();
    Q_INVOKABLE void clearClip();
    // Plays the marked part again and again, or stops doing so; a mark that is moved moves the loop, and clearing the clip, another item and a stop end it
    Q_INVOKABLE void setClipLooping(bool looping);
    // One frame forward or back, which pauses (mpv's frame-step and frame-back-step); not while casting or in a group
    Q_INVOKABLE void frameStep();
    Q_INVOKABLE void frameBackStep();
    // The Video Enhancement menu: debanding is mpv's own flag, and the shaders are the files of the profile's `shaders` folder (`.glsl` and `.hook`, readable files with links followed, at most 32 of at most 256 KiB), each `{file, name}`; `activeShaders` holds the files that mpv's `glsl-shaders` has. The two are apart so that switching a shader does not make a new list of entries. `refreshEnhancements()` reads them again (when the menu opens); a change applies at once, to this playback only: the next item starts from the settings again
    Q_PROPERTY(bool deband READ deband NOTIFY enhancementsChanged)
    Q_PROPERTY(QVariantList shaders READ shaders NOTIFY enhancementsChanged)
    Q_PROPERTY(QStringList activeShaders READ activeShaders NOTIFY enhancementsChanged)
    bool deband() const { return m_deband; }
    QVariantList shaders() const { return m_shaders; }
    QStringList activeShaders() const { return m_activeShaders; }
    Q_INVOKABLE void refreshEnhancements();
    Q_INVOKABLE void setDeband(bool on);
    Q_INVOKABLE void setShader(const QString &file, bool on);
    // The file name the Save dialog proposes: the title and the start time, with the extension of the format
    Q_INVOKABLE QString suggestedClipName(bool gif) const;
    // The same name in the user's Videos folder, the starting point of the Save dialog
    Q_INVOKABLE QUrl suggestedClipUrl(bool gif) const;
    // Encodes the marked range into the file the portal granted; false, with no notification, when the marks or the route refuse it
    Q_INVOKABLE bool saveClip(bool gif, const QUrl &destination);
    Q_INVOKABLE void cancelClip();
    // Pins the playback's maximum bit rate (0 is Automatic) and asks the server for that stream at the current position
    Q_INVOKABLE void setQuality(qint64 cap);
    // A remote Bitrate: 0, or at least the file's own bit rate, is Automatic; else the highest ladder step not above it (the lowest below the ladder)
    Q_INVOKABLE void setQualityBitrate(qint64 bitsPerSecond);
    Q_INVOKABLE void skipSegment();
    Q_INVOKABLE bool downloadSubtitle(int index);
    // Shows a subtitle file of this computer for the item that plays (a drop on the player, or the Subtitles menu's Add Subtitle File): an srt, ass, ssa or vtt file of at most 2 MiB, copied to the player's own folder and added to mpv and selected under its file name. For this item only; the answer comes as subtitleFileAdded or subtitleFileRefused, which says why in words
    Q_INVOKABLE bool addSubtitleFile(const QUrl &file);
    // Whether the subtitle file could be kept on the server too: the item is the server's and the user an administrator, the same as for Edit Subtitles
    Q_INVOKABLE bool canUploadAddedSubtitles() const;
    // A server external text subtitle not loaded yet, downloaded and shown as the second one
    Q_INVOKABLE bool downloadSecondarySubtitle(int index);
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
    // The stream switch watchdog, for the tests
    void setSwitchTimeout(int milliseconds) { m_switchTimeout.setInterval(milliseconds); }
    // One of the `video.aspect` values except `custom`: applied at once to this playback only, the setting is not written
    Q_INVOKABLE void setAspectMode(const QString &mode);
    // Turns the night-mode filter on or off, through the profile's setting; the chain is rebuilt at once
    Q_INVOKABLE void setNightMode(bool enabled);
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
    // The server answered the download but listed no new subtitle file in time (it could not save it)
    void remoteSubtitleNotSaved(const QString &subtitleId);
    // The stream kept starving and plays on at a lower bit rate (bit/s, a step of the fallback ladder)
    void qualityLowered(qint64 bitsPerSecond);
    // A requested quality the server refused, or a stream that did not open: playback goes on
    void qualityChangeFailed();
    // The stream kept starving and the request for a lower one failed or was refused: playback goes on as it was
    void qualityUnavailable();
    // Night mode was turned on while a passthrough codec is selected: the codec is decoded instead, so the chain can act
    void nightModePassthroughDisabled();
    // A seek was asked of mpv, to this position
    void seeked(qint64 ticks);
    void tracksChanged();
    void chaptersChanged();
    void segmentsChanged();
    void enhancementsChanged();
    void currentChapterChanged();
    void upNextChanged();
    void seriesChoicesChanged();
    void previewChanged();
    void finished();
    // The renderer stopped on its own or another controller took it: the player closes, the shell says where it stopped
    void castStoppedElsewhere(const QString &deviceName);
    // A subtitle stream of the file is on its way: the server extracts it first, which can take minutes the first time
    void subtitleExtracting();
    // A subtitle file was added to the item that plays: its title (the file name without the suffix) and the copy the player keeps, which an upload can read
    void subtitleFileAdded(const QString &title, const QUrl &copy);
    void subtitleFileRefused(const QString &reason);
    // A subtitle for the other slot is still on its way: one download runs at a time
    void subtitleBusy();
    // A mark was set: the shell shows where
    void clipMarkChanged(const QString &text);
    // A clip or GIF reached the granted file (`fileName` only, `gif` for the wording), or did not: noSpace names the disk message
    void clipSaved(const QString &fileName, bool gif);
    void clipSaveFailed(bool noSpace);
    // The export started, progressed (`clipProgress`) or ended
    void clipStateChanged();

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
    // The server's stream index of mpv's track `id` of `type`: an embedded track of a direct play, or a server's external subtitle this item loaded
    std::optional<int> streamIndexOfTrack(const QString &type, int id) const;
    std::optional<int> serverStreamIndex(const QVariantMap &track, bool direct) const;
    void requestPending();
    // The fallback: the stream keeps starving, so the server is asked for the same item at `cap`
    void onStarving(qint64 cap);
    // A PlaybackInfo reply that came while the item plays: the lowered stream, accepted when it is lower
    void acceptFallback(const PlaybackSource &source);
    bool fallbackApplies() const;
    bool fallbackEligible() const;
    // The cap the next request asks: the manual pin when set, else the fallback's
    qint64 effectiveCap() const;
    // Asks PlaybackInfo at `cap`: the fallback's step or the user's choice, which ignores the fallback's gates and may go up
    bool requestStreamChange(qint64 cap, bool manual);
    // The new stream did not open: the item stays with Try Again at the cap before the switch
    void failStreamSwitch();
    // The switched stream plays on from its resume point: the watchdog stops
    void settleSwitch();
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
    bool castTarget() const;
    void startCast(const PlaybackSource &source);
    // Whether the chosen subtitle must be burnt in for the cast target, and the WebVTT track of it for a target that fetches one
    bool castBurnIn() const;
    std::optional<CastTextTrack> castTextTrack(qint64 startTicks) const;
    void onCastPlaying();
    void onCastPaused();
    void onCastBuffering();
    void onCastEnded();
    void onCastStopped();
    void onCastFailed(CastRenderer::Failure failure);
    // Closes the cast's server session (Stopped at the renderer's position) and, with `stopRenderer`, stops the renderer too; the target stays
    void closeCast(bool stopRenderer);
    void clearCastTarget();
    void castFailed(const QString &message);
    void onCastPosition();
    void onCastVolume();
    void onCastTextTrackReady();
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
    // The tracks the second subtitle slot may take: text subtitles other than the primary
    QList<TrackFact> secondarySubtitleFacts() const;
    // The playing item can give a clip or GIF: SDR, and a local file, a direct play or a stream whose cache still holds the marks
    bool clipReady(bool gif, QString *reason) const;
    // What refuses a clip whatever its format (HDR, marks gone from the cache), empty when nothing does
    QString clipCommonReason() const;
    qint64 clipLengthTicks() const;
    // A streamed route (transcode or server remux): the clip comes from mpv's cache, not from a file the exporter could read
    bool clipFromCache() const;
    // One of mpv's cached ranges holds both marks
    bool clipCached() const;
    // Forgets the marks and ends the loop that ran over them
    void dropClipMarks();
    // Puts mpv's A-B loop where the marks and `m_clipLooping` say, and ends the loop when the marks no longer allow one
    void applyClipLoop();
    // Puts mpv's debanding and shaders back to what they were before the menu changed them
    void resetEnhancements();
    // The audio and subtitle delays go back to none: they are for the item they were set on
    void resetDelays();
    // What is for the item that plays and not for the next: the enhancements, the delays and the subtitle files added by hand
    void resetForNextItem();
    // Remembers what mpv has before the menu's first change
    void keepEnhancementBase();
    // Writes the cached range of the marks to `path` and returns where it starts (at or before the start mark), nothing on failure
    std::optional<double> dumpClipCache(const QString &path);
    void dropClipDump();
    void onClipFinished(const QString &stagingPath);
    void onClipFailed(ClipExporter::Error error);
    void cancelClipExport();
    // mpv's `secondary-sid`, without the memory a choice writes: 0 turns it off
    bool setSecondarySubtitle(int id);
    // After a same-item reopen, the second subtitle comes back by its stream index or is downloaded again
    void restoreSecondarySubtitle();
    // One server subtitle download, into the primary or the second slot
    bool downloadSubtitle(int index, bool secondary);
    // The user's request for a server subtitle: the loaded file when there is one, else a download that replaces the same slot's pending one
    bool requestSubtitle(int index, bool secondary);
    // Forgets the download on its way and stops it
    void dropPendingSubtitle();
    // The series' speed and aspect, once the file is loaded and the series known; a restore, never a choice
    void applySeriesPlayback();
    // Track memory and queue positions of an owner (`server` or `server/user`) go
    void pruneMemory(const QString &owner);
    // The Intro segments around `ticks` are never skipped automatically: the user went there. Before the segments are known the position is kept for when they come
    void keepIntrosAt(qint64 ticks);
    void refreshSegments();
    // The segments the skip button, the automatic skip and the Up Next card read, and the bands the seek bar draws from them
    void setSegments(const QVariantList &segments);
    void updateSkip();
    QString memoryKey() const;
    void restoreTracks();
    void rememberTracks(int audioId, int subtitleId, int secondaryId);
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
    CastController *m_cast = nullptr;
    bool m_casting = false;
    // The renderer refused the direct play of this item: it is asked once more as a transcode
    bool m_castTranscodeOnly = false;
    bool m_castFailed = false;
    // A run of seeks on a transcode reloads it once, at the last target; a negative target is where the device is when the reload goes out
    QTimer m_castSeek;
    qint64 m_castSeekTarget = 0;
    // The text track of a paused transcode cast became ready: the reload that brings it in waits for the resume, since a LOAD plays at once
    bool m_castTrackPending = false;
    QString m_castTarget;
    QString m_castName;
    qint64 m_castPositionMs = 0;
    qint64 m_castDurationMs = 0;
    qint64 m_itemRuntimeTicks = 0;
    PlayerSettings m_mpvSettings;
    PlaybackSession::ObserverId m_observerId = 0;
    QPointer<MpvAbstractItem> m_video;
    QTimer m_sleep;
    bool m_sleepAtEndOfItem = false;
    bool m_sleepAtEndOfQueue = false;
    QTimer m_clock;
    // One report after a run of seeks, however many there were
    QTimer m_seekReport;
    QTimer m_trackTimer;
    // The position moves with every video frame: the shell is told at most this often, the last change of a burst after it
    QTimer m_positionNotice;
    bool m_positionNoticePending = false;
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
    // Distinguishes two subtitle files saved within the same millisecond
    int m_subtitleFileSeq = 0;
    // Clip and GIF export: the marks of the playing item, the file the portal granted and the progress
    ClipExporter m_clipExporter;
    qint64 m_clipStart = -1;
    qint64 m_clipEnd = -1;
    bool m_deband = false;
    QVariantList m_shaders;
    QStringList m_activeShaders;
    // What mpv had before the menu changed anything in this playback, put back when the next one starts
    bool m_enhancementsTouched = false;
    bool m_debandBase = false;
    QVariant m_shadersBase;
    bool m_clipLooping = false;
    // Whether mpv's A and B hold the loop of the marks, so that a loop that is off leaves them alone
    bool m_loopSet = false;
    QUrl m_clipDestination;
    double m_clipProgress = 0;
    bool m_clipGif = false;
    int m_clipSeq = 0;
    // The cache dump a streamed clip encodes from, removed when the export ends
    QString m_clipDump;
    // The file of a local playback, the source of its clip
    QString m_localSourcePath;
    // The pending download goes to the second slot, not to the primary
    bool m_pendingSubtitleSecondary = false;
    // A subtitle file added for the second slot, waiting for mpv to list it
    QString m_pendingSecondaryFile;
    // The server's stream index of each external subtitle file loaded into mpv for this stream, by file name, in the order it was added (mpv's track ids follow that order)
    QHash<QString, int> m_loadedSubtitles;
    // Every subtitle file mpv was given for this stream (the server's and the ones added by hand), in the order they were added: a clip export lists them the same way
    QStringList m_loadedSubtitleOrder;
    // The user turned subtitles off for this item; a same-item reopen keeps it
    bool m_subtitlesOff = false;
    int m_cachedTileIndex = -1;
    int m_pendingTileIndex = -1;
    QVariantList m_audioTracks;
    QVariantList m_subtitleTracks;
    QVariantList m_externalSubtitles;
    QVariantList m_segments;
    QVariantList m_segmentBands;
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
    // The language the results on show were searched in, kept after they arrive: a download names it to tell its own saved file from the ones the item already had
    QString m_remoteSubtitleLanguage;
    // The bit rate fallback: the rule, the request in flight, the lowered source waiting for the reopen and what the last request asked for
    BitrateFallback m_fallback;
    struct FallbackRequest {
        QString playSessionId;
        qint64 cap = 0;
        JellyfinClient::StreamChoice streams;
        bool active = false;
        bool manual = false;
    } m_fallbackRequest;
    std::optional<PlaybackSource> m_preparedSource;
    // The file opening is the lowered stream: its load completes the step, its failure offers Try Again
    bool m_switchingStream = false;
    qint64 m_requestedCap = UnlimitedBitrate;
    JellyfinClient::StreamChoice m_requestedStreams;
    // The choice made on the details page, applied once the file's tracks are known (and sent with the PlaybackInfo request)
    struct PendingStreams {
        QString itemId;
        int audio = -1;
        int subtitle = -2;
    };
    std::optional<PendingStreams> m_pendingStreams;
    // What the details page picked for the current item: a reopen at the unlimited cap asks for it again
    std::optional<PendingStreams> m_detailsStreams;
    void applyPendingStreams();
    // The cap before the step that failed, and where it failed: what Try Again opens again
    qint64 m_capBeforeStep = UnlimitedBitrate;
    // The user's pinned maximum bit rate (0 Automatic), the pin a manual switch replaces and the switch it waits for
    qint64 m_manualCap = 0;
    qint64 m_manualCapBeforeSwitch = 0;
    bool m_qualitySwitching = false;
    qint64 m_retryTicks = 0;
    bool m_canRetry = false;
    // The item of the subtitle download in flight, empty when none runs
    QString m_remoteSubtitleDownloadItem;
    QString m_remoteSubtitleError;
    // The reopen after a subtitle download keeps a paused video paused
    bool m_reopenPaused = false;
    // A stream switch that never loads, or never plays on from its resume seek: the watchdog offers Try Again at the previous cap
    QTimer m_switchTimeout;
    bool m_switchTimedOut = false;
    bool m_switchManual = false;
    // After the switched stream loaded: the position it must play on from, -1 when no switch waits for it
    qint64 m_switchSettleTicks = -1;
    // The failure Try Again answers was a manual switch: the fallback keeps its budget
    bool m_failedSwitchManual = false;
    struct PendingLocal {
        QString path;
        QString itemId;
        QString title;
        QString subtitle;
        qint64 resumeTicks = 0;
        QList<ChapterSummary> chapters;
        QList<LocalSubtitle> subtitles;
        bool pending = false;
    } m_pendingLocal;
    // The subtitle files of the download that plays, added to mpv once it loaded
    QList<LocalSubtitle> m_localSubtitles;
    // The subtitle files the viewer added by hand to this item, added to mpv again when the item reopens
    QList<LocalSubtitle> m_addedSubtitles;
    // The added file that is the selected subtitle, and the mpv ids of the added files in the track list as it was last read
    QString m_selectedAddedSubtitle;
    QHash<int, QString> m_addedSubtitleTracks;
    bool m_stopping = false;
    bool m_trackRestored = false;
    int m_selectedAudioId = 0;
    int m_selectedSubtitleId = 0;
    int m_secondarySubtitleId = 0;
    std::optional<int> m_selectedAudioIndex;
    std::optional<int> m_selectedSubtitleIndex;
    // The server stream index of the second subtitle, kept to select it again after a same-item reopen
    std::optional<int> m_secondaryStreamIndex;
};
} // namespace Kinefin
