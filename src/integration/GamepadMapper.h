// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <optional>

namespace Kinefin {
// The fixed button table from SDL3 gamepad events to the shared keycodes: sticks and triggers become directions and presses, a held direction repeats, and nothing acts while no Kinefin window has the focus
class GamepadMapper final : public QObject {
    Q_OBJECT
  public:
    struct Timing {
        int repeatDelayMs = 400;
        int repeatIntervalMs = 100;
    };

    explicit GamepadMapper(QObject *parent = nullptr);

    // Tests shorten the constants, as BitrateFallback takes its timing
    void setTiming(const Timing &timing);
    void setRepeatEnabled(bool enabled);
    void setActive(bool active);
    // SDL3 gamepad button and axis ids, as SDL_GamepadButton and SDL_GamepadAxis
    void button(quint32 device, int sdlButton, bool down);
    void axis(quint32 device, int sdlAxis, qint16 value);
    void deviceRemoved(quint32 device);
    // Forgets every held control, as when SDL stops: a release that never comes must not leave a key held
    void reset();
    // Moves the mapper's clock and emits the repeats that are due
    void advance(qint64 milliseconds);

  Q_SIGNALS:
    void keycodeRequested(const QString &keycode, bool autoRepeat);

  private:
    enum class Control {
        South,
        East,
        West,
        North,
        Start,
        Back,
        LeftShoulder,
        RightShoulder,
        LeftTrigger,
        RightTrigger,
        DpadUp,
        DpadDown,
        DpadLeft,
        DpadRight,
        LeftStickUp,
        LeftStickDown,
        LeftStickLeft,
        LeftStickRight,
        RightStickUp,
        RightStickDown,
        RightStickLeft,
        RightStickRight,
    };
    struct Held {
        QString keycode;
        bool repeats = false;
        qint64 nextRepeat = 0;
    };
    struct Stick {
        qint16 x = 0;
        qint16 y = 0;
        // The direction held now; left and right of the right stick hold none
        std::optional<Control> direction;
    };

    static QString keycodeFor(Control control);
    static bool repeatsFor(Control control);
    void press(quint32 device, Control control);
    void release(quint32 device, Control control);
    void updateStick(quint32 device, bool right, int axis, qint16 value);

    bool m_active = true;
    bool m_repeatEnabled = true;
    Timing m_timing;
    qint64 m_now = 0;
    QHash<quint64, Held> m_held;
    QHash<quint64, Stick> m_leftSticks;
    QHash<quint64, Stick> m_rightSticks;
    QHash<quint64, bool> m_triggers;
};
} // namespace Kinefin
