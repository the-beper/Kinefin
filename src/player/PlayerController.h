// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QTemporaryDir>
#include <QUrl>
#include "player/PlaybackEngine.h"
#include "player/PlaybackReporter.h"
#include "player/PlayerSettings.h"
#include "player/Trickplay.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <optional>

class MpvAbstractItem;
class MpvController;

namespace Kinefin {
class JellyfinClient;
class PlayQueue;
class ClientSettings;

// `01:24:10` with hours, `05:07` without; a negative count shows as zero
QString playbackTimeText(qint64 ticks, bool withHours);

// Single playback owner. Negotiation and server reporting stay on the authenticated
// client's event loop; mpv's worker thread is accessed only via MpvAbstractItem.
// Never persist or expose stream URLs or authentication headers to QML.
class PlayerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool paused READ paused NOTIFY changed)
    Q_PROPERTY(bool buffering READ buffering NOTIFY changed)
    Q_PROPERTY(QString itemId READ itemId NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString subtitle READ subtitle NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
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
  public:
    PlayerController(JellyfinClient &client, PlayQueue &queue, ClientSettings &settings,
                     const QString &profileId, QObject *parent = nullptr);
    ~PlayerController() override;

    bool active() const { return m_loading || m_session.state() != PlaybackSession::State::Idle; }
    bool loading() const
    {
        return m_loading || m_session.state() == PlaybackSession::State::Opening;
    }
    bool paused() const { return m_session.state() == PlaybackSession::State::Paused; }
    bool buffering() const { return m_session.state() == PlaybackSession::State::Buffering; }
    QString itemId() const { return m_itemId; }
    // The series name for an episode, else the item's name: empty until the server answered
    QString title() const { return m_title; }
    // The episode label and name of an episode (`S2:E6 — Weightless`), empty for anything else
    QString subtitle() const { return m_subtitle; }
    QString errorText() const { return m_errorText; }
    qint64 positionTicks() const { return m_engine.positionTicks(); }
    qint64 durationTicks() const { return m_engine.durationTicks(); }
    bool canSeek() const { return m_engine.canSeek(); }
    double rate() const { return m_engine.playbackRate(); }
    int volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    double audioDelay() const { return m_audioDelay; }
    double subtitleDelay() const { return m_subtitleDelay; }
    bool shuffle() const { return m_shuffle; }
    int repeatMode() const { return m_repeat; }
    // The saved queue only: the episodes of a series played alone are not in it
    int currentIndex() const { return m_playlistEphemeral ? -1 : m_queueIndex; }
    int queueCount() const { return m_playlistEphemeral ? 0 : m_playlist.size(); }
    // Whether Next and Previous have somewhere to go, in the saved queue or the episodes of the series
    bool hasNext() const { return nextIndex(1, false) >= 0; }
    bool hasPrevious() const { return nextIndex(-1, false) >= 0; }
    QVariantList audioTracks() const { return m_audioTracks; }
    QVariantList subtitleTracks() const { return m_subtitleTracks; }
    QVariantList externalSubtitles() const { return m_externalSubtitles; }
    QString previewImage() const { return m_previewImage; }
    QString screenshotPath() const { return m_screenshotPath; }
    QString skipLabel() const { return m_skipLabel; }
    int sleepRemainingMinutes() const;

    // Rendering item owns the libmpv handle. The call is made on its ready signal,
    // and detach precedes destruction (including account switch and window close).
    bool attachVideo(MpvAbstractItem *item, MpvController *controller);
    void detachVideo(MpvAbstractItem *item);

    Q_INVOKABLE bool playItem(const QString &id, qint64 resumeTicks = 0);
    Q_INVOKABLE bool playQueue(int index);
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
    Q_INVOKABLE void adjustAudioDelay(double seconds);
    Q_INVOKABLE void adjustSubtitleDelay(double seconds);
    Q_INVOKABLE void setSleepTimer(int minutes); // 0 disables
    Q_INVOKABLE void screenshot();
    Q_INVOKABLE void skipSegment();
    Q_INVOKABLE bool downloadSubtitle(int index);
    Q_INVOKABLE void previewAt(qint64 ticks);
    Q_INVOKABLE void clearPreview();
    Q_INVOKABLE void refreshTracks();
    // Elapsed or remaining time of the video: hours show when its duration is at least one hour
    Q_INVOKABLE QString timeText(qint64 ticks) const;
    // Used before a session changes, never by QML to re-authenticate.
    void onAccountLeaving();
    // The signed-in user's EnableNextEpisodeAutoPlay: whether the end of an episode played alone starts the next one
    void setNextEpisodeAutoPlay(bool enabled) { m_nextEpisodeAutoPlay = enabled; }

  Q_SIGNALS:
    void changed();
    void tracksChanged();
    void previewChanged();
    void finished();

  private:
    // The listing channel of the series' episodes (see the channel table in the architecture doc)
    static constexpr int EpisodesChannel = 4096;

    bool startItem(const QString &id, qint64 resumeTicks);
    void requestPending();
    void onSource(const PlaybackSource &source);
    void onPlaybackItem(const QString &id, const QJsonObject &item);
    void onSeriesEpisodes(const QJsonArray &items, int total, int channel);
    // Turns the episodes read so far into the series playlist, or leaves the episode alone when they do not hold it
    void finishSeriesPlaylist();
    void updateTrickplayLayout();
    void cancelSeriesRequest();
    void dropSeriesPlaylist();
    void onEnd(const QString &reason);
    void onState(PlaybackSession::State from, PlaybackSession::State to);
    // `pick` false answers whether a step exists without drawing the random index of shuffle
    int nextIndex(int direction, bool pick = true) const;
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
    QTimer m_trackTimer;
    QJsonObject m_memory;
    QJsonObject m_queueMemory;
    QTemporaryDir m_subtitleDir;
    PlaybackSource m_source;
    QList<ExternalSubtitle> m_subtitleOptions;
    std::optional<TrickplayLayout> m_trickplay;
    QJsonObject m_trickplayInfo;
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
    bool m_stopping = false;
    bool m_trackRestored = false;
    int m_selectedAudioId = 0;
    int m_selectedSubtitleId = 0;
    std::optional<int> m_selectedAudioIndex;
    std::optional<int> m_selectedSubtitleIndex;
};
} // namespace Kinefin
