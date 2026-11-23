// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings/MpvConf.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <utility>

class QScreen;
class QWindow;

namespace Kinefin {
class PlaybackEngine;
class ClientSettings;

// Applies the profile's validated settings to the same mpv instance that renders video, live: the profile's mpv.conf first, the managed settings over it, and the user-owned other_conf last
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
    // While held, `apply()` leaves the speed alone: a watching-together group plays at 1.0 whatever the settings say
    void setSpeedHeld(bool held) { m_speedHeld = held; }
    // A passthrough codec is selected for the device type: night mode decodes it instead
    [[nodiscard]] bool passthroughConfigured() const;
    // The subtitle properties of the settings, in the order they are set: the clip exporter shows the same
    // font, colours, size and placement as the player
    [[nodiscard]] QList<std::pair<QString, QVariant>> subtitleProperties() const;
    // The transport options of mpv for a second instance (the clip exporter): tls-verify and the detected bundle
    [[nodiscard]] bool tlsVerify() const;
    [[nodiscard]] QString tlsCaFile() const;

    // The CA bundle paths tried by `autodetectCertBundle`, in order
    static QStringList certBundleCandidates();
    // The first of `candidates` that is a readable file, or empty
    static QString detectCertBundle(const QStringList &candidates);

  private:
    // `tls-verify` follows `certificateErrorsIgnored` (mpv itself does not verify by default), `tls-ca-file` follows `autodetectCertBundle`; mpv.conf entries are applied after them
    void applyTls();
    void applyAudio();
    // `audio-spdif` and the audio filter chain: the night filter before the AC-3 re-encoder. mpv.conf's `af` and `audio-spdif` lines are re-applied after it
    void applyAudioChain();
    // The passthrough codecs of the device type, in mpv's order
    [[nodiscard]] QStringList passthroughCodecs() const;
    void applyVideo();
    // `reset` also clears the modifiers when the setting is `custom`, which otherwise leaves mpv alone
    void applyAspect(bool reset = false);
    // `applyAspect` and then the aspect entries of mpv.conf
    void reapplyAspect(bool reset);
    void applySubtitles();
    // One entry: a plain value is set as a property, a list operation (`glsl-shaders-append`) goes through change-list
    void applyEntry(const MpvConfEntry &entry);
    // The valid entries of the profile's mpv.conf, before any managed setting; a problem is logged
    void applyConfFile();
    // Every entry of the Manual MPV Configuration setting, or only those named in `only`
    void applyOverrides(const QStringList &only = {});
    void applyDisplayRate();
    void followScreen(QScreen *screen);
    void followRate();
    ClientSettings &m_settings;
    PlaybackEngine &m_engine;
    double m_audioOffset = 0;
    double m_subtitleOffset = 0;
    QString m_aspectOverride;
    bool m_speedHeld = false;
    QPointer<QWindow> m_window;
    QPointer<QScreen> m_screen;
    QMetaObject::Connection m_windowConnection;
    QMetaObject::Connection m_screenConnection;
};
} // namespace Kinefin
