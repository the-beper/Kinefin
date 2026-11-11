// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>

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
private:
    void applyAudio();
    void applyVideo();
    void applySubtitles();
    void applyOverrides();
    ClientSettings &m_settings;
    PlaybackEngine &m_engine;
    double m_audioOffset = 0;
    double m_subtitleOffset = 0;
};
} // namespace Kinefin
