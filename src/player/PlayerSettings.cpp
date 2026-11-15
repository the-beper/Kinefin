// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/PlayerSettings.h"

#include "player/PlaybackEngine.h"
#include "settings/ClientSettings.h"

#include <QGuiApplication>
#include <QRegularExpression>
#include <QScreen>
#include <QVariant>
#include <QWindow>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Kinefin {
namespace {
QString mpvBool(bool enabled)
{
    return enabled ? QStringLiteral("yes") : QStringLiteral("no");
}
} // namespace

PlayerSettings::PlayerSettings(ClientSettings &settings, PlaybackEngine &engine, QObject *parent)
    : QObject(parent), m_settings(settings), m_engine(engine)
{
    connect(&settings, &ClientSettings::valueChanged, this,
            [this](const QString &section, const QString &) {
                if (section == QLatin1String("audio") || section == QLatin1String("video")
                    || section == QLatin1String("subtitles") || section == QLatin1String("other"))
                    apply();
            });
}

void PlayerSettings::setWindow(QWindow *window)
{
    disconnect(m_windowConnection);
    m_window = window;
    if (window)
        m_windowConnection =
            connect(window, &QWindow::screenChanged, this, [this](QScreen *screen) {
                followScreen(screen);
                followRate();
            });
    followScreen(window ? window->screen() : nullptr);
    followRate();
}

void PlayerSettings::followScreen(QScreen *screen)
{
    disconnect(m_screenConnection);
    m_screen = screen;
    if (screen)
        m_screenConnection =
            connect(screen, &QScreen::refreshRateChanged, this, [this] { followRate(); });
}

// Only what the rate decides: a full apply would also reset what the user changed while playing, such as the speed
void PlayerSettings::followRate()
{
    if (!m_engine.isInitialized())
        return;
    applyDisplayRate();
    applyOverrides({QStringLiteral("display-fps-override"), QStringLiteral("audio-delay")});
}

qreal PlayerSettings::displayRate() const
{
    const QScreen *screen = m_screen ? m_screen.data() : QGuiApplication::primaryScreen();
    return screen ? screen->refreshRate() : 0;
}

void PlayerSettings::apply()
{
    if (!m_engine.isInitialized())
        return;
    applyAudio();
    applyVideo();
    applySubtitles();
    applyOverrides(); // intentionally last; same precedence as the web client
}

void PlayerSettings::applyAudio()
{
    const auto v = [this](const char *key) {
        return m_settings.value(QStringLiteral("audio"), QString::fromLatin1(key));
    };
    const QString type = v("devicetype").toString();
    m_engine.setMpvProperty(QStringLiteral("audio-exclusive"), mpvBool(v("exclusive").toBool()));
    QString device = v("device").toString();
    if (device.isEmpty())
        device = QStringLiteral("auto");
    if (device != QLatin1String("auto")) {
        const QVariantList devices =
            m_engine.mpvProperty(QStringLiteral("audio-device-list")).toList();
        if (!devices.isEmpty()) {
            const bool available =
                std::any_of(devices.cbegin(), devices.cend(), [&device](const QVariant &entry) {
                    return entry.toMap().value(QStringLiteral("name")).toString() == device;
                });
            if (!available)
                device = QStringLiteral("auto");
        }
    }
    m_engine.setMpvProperty(QStringLiteral("audio-device"), device);
    const bool normalize = v("normalize").toBool();
    m_engine.setMpvProperty(QStringLiteral("audio-normalize-downmix"), mpvBool(normalize));
    m_engine.setMpvProperty(QStringLiteral("audio-swresample-o"),
                            QStringLiteral("surround_mix_level=1"));
    QStringList codecs;
    if (type != QLatin1String("basic")) {
        for (const char *codec : {"ac3", "dts", "eac3", "truehd"}) {
            if ((type == QLatin1String("hdmi") || QString::fromLatin1(codec) == QLatin1String("ac3")
                 || QString::fromLatin1(codec) == QLatin1String("dts"))
                && v(QByteArray("passthrough.").append(codec).constData()).toBool()) {
                codecs.append(QString::fromLatin1(codec));
            }
        }
        if (type == QLatin1String("hdmi") && v("passthrough.dts-hd").toBool()) {
            codecs.removeAll(QStringLiteral("dts"));
            codecs.append(QStringLiteral("dts-hd"));
        }
    }
    m_engine.setMpvProperty(QStringLiteral("audio-spdif"), codecs.join(QLatin1Char(',')));
    m_engine.setMpvProperty(QStringLiteral("audio-channels"), type == QLatin1String("spdif")
                                                                  ? QStringLiteral("2.0")
                                                                  : v("channels").toString());
    if (type == QLatin1String("spdif") && v("passthrough.ac3").toBool())
        m_engine.mpvCommand(
            {QStringLiteral("af"), QStringLiteral("add"), QStringLiteral("@ac3:lavcac3enc")});
    else
        m_engine.mpvCommand(
            {QStringLiteral("af"), QStringLiteral("remove"), QStringLiteral("@ac3")});
}

void PlayerSettings::applyVideo()
{
    const auto v = [this](const char *key) {
        return m_settings.value(QStringLiteral("video"), QString::fromLatin1(key));
    };
    const QString hw = v("hardwareDecoding").toString();
    m_engine.setMpvProperty(QStringLiteral("hwdec"),
                            hw == QLatin1String("disabled") ? QStringLiteral("no")
                            : hw == QLatin1String("copy")   ? QStringLiteral("auto-copy")
                                                            : QStringLiteral("auto"));
    m_engine.setMpvProperty(QStringLiteral("deinterlace"), mpvBool(v("deinterlace").toBool()));
    const QString forcedVo = v("debug.force_vo").toString().trimmed();
    if (!forcedVo.isEmpty())
        m_engine.setMpvProperty(QStringLiteral("vo"), forcedVo);
    m_engine.setMpvProperty(QStringLiteral("video-sync"), v("sync_mode").toString());
    m_engine.setMpvProperty(QStringLiteral("demuxer-max-bytes"),
                            QString(QString::number(v("cache").toInt()) + QStringLiteral("MiB")));
    m_engine.setMpvProperty(QStringLiteral("speed"), v("default_playback_speed").toDouble());
    // Reset all modifiers before applying a new aspect; custom means do not override mpv.
    const QString aspect = v("aspect").toString();
    if (aspect != QLatin1String("custom")) {
        m_engine.setMpvProperty(QStringLiteral("panscan"), 0.0);
        m_engine.setMpvProperty(QStringLiteral("video-aspect-override"), QStringLiteral("no"));
        m_engine.setMpvProperty(QStringLiteral("keepaspect"), QStringLiteral("yes"));
        m_engine.setMpvProperty(QStringLiteral("video-unscaled"), QStringLiteral("no"));
        if (aspect == QLatin1String("zoom"))
            m_engine.setMpvProperty(QStringLiteral("panscan"), 1.0);
        else if (aspect == QLatin1String("force_4_3"))
            m_engine.setMpvProperty(QStringLiteral("video-aspect-override"), QStringLiteral("4:3"));
        else if (aspect == QLatin1String("force_16_9"))
            m_engine.setMpvProperty(QStringLiteral("video-aspect-override"),
                                    QStringLiteral("16:9"));
        else if (aspect == QLatin1String("force_16_9_if_4_3")) {
            const QVariantMap decoded =
                m_engine.mpvProperty(QStringLiteral("video-dec-params")).toMap();
            if (std::abs(decoded.value(QStringLiteral("aspect")).toDouble() - 4.0 / 3) <= 0.1)
                m_engine.setMpvProperty(QStringLiteral("video-aspect-override"),
                                        QStringLiteral("16:9"));
        } else if (aspect == QLatin1String("stretch"))
            m_engine.setMpvProperty(QStringLiteral("keepaspect"), QStringLiteral("no"));
        else if (aspect == QLatin1String("noscaling"))
            m_engine.setMpvProperty(QStringLiteral("video-unscaled"), QStringLiteral("yes"));
    }
    applyDisplayRate();
}

void PlayerSettings::applyDisplayRate()
{
    const double refresh = displayRate();
    // 0 lets mpv measure the rate itself, and clears the value of a previous screen
    m_engine.setMpvProperty(QStringLiteral("display-fps-override"),
                            refresh > 20.0 && refresh < 500.0 ? refresh : 0.0);
    const char *bucket = "normal";
    for (int fps : {24, 25, 50}) {
        if (std::abs(refresh - fps) <= 0.5) {
            bucket = fps == 24 ? "24hz" : fps == 25 ? "25hz" : "50hz";
            break;
        }
    }
    const QByteArray key = QByteArray("audio_delay.").append(bucket);
    const int delay = m_settings.value(QStringLiteral("video"), QString::fromLatin1(key)).toInt();
    m_engine.setMpvProperty(QStringLiteral("audio-delay"), delay / 1000.0 + m_audioOffset);
}

void PlayerSettings::applySubtitles()
{
    const auto v = [this](const char *key) {
        return m_settings.value(QStringLiteral("subtitles"), QString::fromLatin1(key));
    };
    m_engine.setMpvProperty(QStringLiteral("sub-ass-style-overrides"),
                            QString(QStringLiteral("ScaledBorderAndShadow=")
                                    + mpvBool(v("ass_scale_border_and_shadow").toBool())));
    m_engine.setMpvProperty(QStringLiteral("sub-ass-override"), v("ass_style_override").toString());
    const int size = v("size").toInt();
    m_engine.setMpvProperty(QStringLiteral("sub-scale"), size > 0 ? size / 32.0 : 1.0);
    for (const auto &pair : {std::pair{"font", "sub-font"},
                             {"color", "sub-color"},
                             {"border_color", "sub-border-color"}}) {
        const QString value = v(pair.first).toString();
        if (!value.isEmpty())
            m_engine.setMpvProperty(QString::fromLatin1(pair.second), value);
    }
    const int border = v("border_size").toInt();
    if (border >= 0)
        m_engine.setMpvProperty(QStringLiteral("sub-border-size"), border);
    const QString bg = v("background_color").toString();
    const QString alpha = v("background_transparency").toString();
    if (QRegularExpression(QStringLiteral("^#[0-9a-fA-F]{6}$")).match(bg).hasMatch()
        && QRegularExpression(QStringLiteral("^[0-9a-fA-F]{2}$")).match(alpha).hasMatch())
        m_engine.setMpvProperty(QStringLiteral("sub-back-color"),
                                QString(QStringLiteral("#") + alpha + bg.mid(1)));
    const QStringList placement = v("placement").toString().split(QLatin1Char(','));
    const QString align = placement.value(0).trimmed().toLower();
    if (align == QLatin1String("left") || align == QLatin1String("center")
        || align == QLatin1String("right"))
        m_engine.setMpvProperty(QStringLiteral("sub-align-x"), align);
    const QString vertical = placement.value(1, align).trimmed().toLower();
    if (vertical == QLatin1String("top") || vertical == QLatin1String("bottom"))
        m_engine.setMpvProperty(QStringLiteral("sub-pos"),
                                vertical == QLatin1String("top") ? 10 : 100);
    m_engine.setMpvProperty(QStringLiteral("sub-delay"), m_subtitleOffset);
}

void PlayerSettings::applyOverrides(const QStringList &only)
{
    const QString conf =
        m_settings.value(QStringLiteral("other"), QStringLiteral("other_conf")).toString();
    static const QRegularExpression keyPattern(QStringLiteral("^[a-z][a-z0-9-]{0,63}$"));
    for (const QString &line : conf.split(QLatin1Char('\n'))) {
        const QString entry = line.trimmed();
        if (entry.isEmpty() || entry.startsWith(QLatin1Char('#')))
            continue;
        const qsizetype pos = entry.indexOf(QLatin1Char('='));
        if (pos <= 0)
            continue;
        const QString name = entry.left(pos).trimmed();
        if (!only.isEmpty() && !only.contains(name))
            continue;
        if (keyPattern.match(name).hasMatch() && !name.startsWith(QStringLiteral("http-"))
            && name != QLatin1String("scripts") && name != QLatin1String("script")
            && name != QLatin1String("input-conf") && name != QLatin1String("include"))
            m_engine.setMpvProperty(name, entry.mid(pos + 1).trimmed());
    }
}

void PlayerSettings::setAudioOffset(double seconds)
{
    m_audioOffset = qBound(-10.0, seconds, 10.0);
    // The offset the user set while playing wins over mpv.conf, and nothing else is reset
    applyDisplayRate();
}
void PlayerSettings::setSubtitleOffset(double seconds)
{
    m_subtitleOffset = qBound(-10.0, seconds, 10.0);
    m_engine.setMpvProperty(QStringLiteral("sub-delay"), m_subtitleOffset);
}
} // namespace Kinefin
