// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QScreen;
class QWindow;

namespace Kinefin {
class PlaybackEngine;
class ClientSettings;

// Applies the profile's validated settings to the same mpv instance that renders
// video. Settings changes are reflected live; the user-owned other_conf wins last.
class PlayerSettings final : public QObject {
    Q_OBJECT
  public:
    PlayerSettings(ClientSettings &settings, PlaybackEngine &engine, QObject *parent = nullptr);
    void apply();
    void setAudioOffset(double seconds);
    void setSubtitleOffset(double seconds);
    // The window the video is shown in: `display-fps-override` and the `audio_delay.*` bucket follow its screen's rate, also when it moves to another screen or the rate changes. Without one the primary screen's rate is used
    void setWindow(QWindow *window);
    qreal displayRate() const;
    // The aspect mode in force: the choice made for this playback, else the `video.aspect` setting (`custom` when it is, which leaves mpv alone; an invalid one reads as `normal`)
    QString aspectMode() const;
    // One of the setting's values except `custom` applies at once and survives every later `apply()`; an empty or invalid one returns to the setting
    void setAspectOverride(const QString &mode);
    bool hasAspectOverride() const { return !m_aspectOverride.isEmpty(); }
    static bool isAspectMode(const QString &mode);
    // Re-evaluates the aspect once a file is loaded, for the mode that needs the decoded picture; the other modes and `custom` are left as they are
    void applyLoadedAspect();

  private:
    void applyAudio();
    void applyVideo();
    // `reset` also clears the modifiers when the setting is `custom`, which otherwise leaves mpv alone
    void applyAspect(bool reset = false);
    // `applyAspect` and then the aspect entries of mpv.conf
    void reapplyAspect(bool reset);
    void applySubtitles();
    // Every entry of mpv.conf, or only those named in `only`
    void applyOverrides(const QStringList &only = {});
    void applyDisplayRate();
    void followScreen(QScreen *screen);
    void followRate();
    ClientSettings &m_settings;
    PlaybackEngine &m_engine;
    double m_audioOffset = 0;
    double m_subtitleOffset = 0;
    QString m_aspectOverride;
    QPointer<QWindow> m_window;
    QPointer<QScreen> m_screen;
    QMetaObject::Connection m_windowConnection;
    QMetaObject::Connection m_screenConnection;
};
} // namespace Kinefin
