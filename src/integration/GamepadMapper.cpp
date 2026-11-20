// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/GamepadMapper.h"

#include <QList>

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cmath>

namespace Kinefin {
namespace {
constexpr int kPressThreshold = 16000;
constexpr int kReleaseThreshold = 8000;

quint64 heldKey(quint32 device, int control)
{
    return (quint64(device) << 8) | quint64(control & 0xff);
}
} // namespace

GamepadMapper::GamepadMapper(QObject *parent) : QObject(parent) {}

void GamepadMapper::setTiming(const Timing &timing)
{
    m_timing = timing;
}

void GamepadMapper::setRepeatEnabled(bool enabled)
{
    m_repeatEnabled = enabled;
}

void GamepadMapper::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (!active)
        reset();
}

void GamepadMapper::reset()
{
    m_held.clear();
    m_leftSticks.clear();
    m_rightSticks.clear();
    m_triggers.clear();
}

QString GamepadMapper::keycodeFor(Control control)
{
    switch (control) {
    case Control::South:
        return QStringLiteral("select");
    case Control::East:
        return QStringLiteral("back");
    case Control::West:
        return QStringLiteral("menu");
    case Control::North:
        return QStringLiteral("playpause");
    case Control::Start:
        return QStringLiteral("mainmenu");
    case Control::Back:
        return QStringLiteral("expandplayer");
    case Control::LeftShoulder:
        return QStringLiteral("previoustab");
    case Control::RightShoulder:
        return QStringLiteral("nexttab");
    case Control::LeftTrigger:
        return QStringLiteral("previouschapter");
    case Control::RightTrigger:
        return QStringLiteral("nextchapter");
    case Control::DpadUp:
    case Control::LeftStickUp:
        return QStringLiteral("up");
    case Control::DpadDown:
    case Control::LeftStickDown:
        return QStringLiteral("down");
    case Control::DpadLeft:
    case Control::LeftStickLeft:
        return QStringLiteral("left");
    case Control::DpadRight:
    case Control::LeftStickRight:
        return QStringLiteral("right");
    case Control::RightStickUp:
        return QStringLiteral("volumeup");
    case Control::RightStickDown:
        return QStringLiteral("volumedown");
    case Control::RightStickLeft:
    case Control::RightStickRight:
        return {};
    }
    return {};
}

bool GamepadMapper::repeatsFor(Control control)
{
    return control == Control::DpadUp || control == Control::DpadDown
           || control == Control::DpadLeft || control == Control::DpadRight
           || control == Control::LeftStickUp || control == Control::LeftStickDown
           || control == Control::LeftStickLeft || control == Control::LeftStickRight
           || control == Control::RightStickUp || control == Control::RightStickDown;
}

void GamepadMapper::press(quint32 device, Control control)
{
    if (!m_active)
        return;
    const quint64 key = heldKey(device, int(control));
    if (m_held.contains(key))
        return;
    const QString keycode = keycodeFor(control);
    if (keycode.isEmpty())
        return;
    Held held;
    held.keycode = keycode;
    held.repeats = m_repeatEnabled && repeatsFor(control);
    if (held.repeats)
        held.nextRepeat = m_now + m_timing.repeatDelayMs;
    m_held.insert(key, held);
    Q_EMIT keycodeRequested(keycode, false);
}

void GamepadMapper::release(quint32 device, Control control)
{
    m_held.remove(heldKey(device, int(control)));
}

void GamepadMapper::button(quint32 device, int sdlButton, bool down)
{
    Control control;
    switch (sdlButton) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        control = Control::South;
        break;
    case SDL_GAMEPAD_BUTTON_EAST:
        control = Control::East;
        break;
    case SDL_GAMEPAD_BUTTON_WEST:
        control = Control::West;
        break;
    case SDL_GAMEPAD_BUTTON_NORTH:
        control = Control::North;
        break;
    case SDL_GAMEPAD_BUTTON_START:
        control = Control::Start;
        break;
    case SDL_GAMEPAD_BUTTON_BACK:
        control = Control::Back;
        break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        control = Control::LeftShoulder;
        break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        control = Control::RightShoulder;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        control = Control::DpadUp;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        control = Control::DpadDown;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        control = Control::DpadLeft;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        control = Control::DpadRight;
        break;
    default:
        return;
    }
    if (down)
        press(device, control);
    else
        release(device, control);
}

void GamepadMapper::axis(quint32 device, int sdlAxis, qint16 value)
{
    if (sdlAxis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || sdlAxis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
        const quint64 key = heldKey(device, sdlAxis);
        const bool wasDown = m_triggers.value(key);
        if (!wasDown && value >= kPressThreshold) {
            m_triggers.insert(key, true);
            press(device, sdlAxis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? Control::LeftTrigger
                                                                   : Control::RightTrigger);
        } else if (wasDown && value < kReleaseThreshold) {
            m_triggers.insert(key, false);
            release(device, sdlAxis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? Control::LeftTrigger
                                                                     : Control::RightTrigger);
        }
        return;
    }
    switch (sdlAxis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
        updateStick(device, false, 0, value);
        break;
    case SDL_GAMEPAD_AXIS_LEFTY:
        updateStick(device, false, 1, value);
        break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
        updateStick(device, true, 0, value);
        break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
        updateStick(device, true, 1, value);
        break;
    default:
        break;
    }
}

void GamepadMapper::updateStick(quint32 device, bool right, int axisIndex, qint16 value)
{
    QHash<quint64, Stick> &sticks = right ? m_rightSticks : m_leftSticks;
    Stick &stick = sticks[device];
    (axisIndex == 0 ? stick.x : stick.y) = value;
    if (!m_active)
        return;
    const auto directionFor = [right](bool vertical, qint16 amount) {
        if (vertical)
            return amount > 0 ? (right ? Control::RightStickDown : Control::LeftStickDown)
                              : (right ? Control::RightStickUp : Control::LeftStickUp);
        return amount > 0 ? (right ? Control::RightStickRight : Control::LeftStickRight)
                          : (right ? Control::RightStickLeft : Control::LeftStickLeft);
    };
    const auto amountFor = [&stick](Control control) {
        switch (control) {
        case Control::LeftStickUp:
        case Control::RightStickUp:
            return qint16(-stick.y);
        case Control::LeftStickDown:
        case Control::RightStickDown:
            return stick.y;
        case Control::LeftStickLeft:
        case Control::RightStickLeft:
            return qint16(-stick.x);
        case Control::LeftStickRight:
        case Control::RightStickRight:
            return stick.x;
        default:
            return qint16(0);
        }
    };
    // A direction already held releases below the rest band, before another can be pressed at the higher threshold; the right stick's horizontal directions were never pressed, and releasing them is a no-op
    if (stick.direction && std::abs(int(amountFor(*stick.direction))) < kReleaseThreshold) {
        release(device, *stick.direction);
        stick.direction.reset();
    }
    if (stick.direction)
        return;
    const bool vertical = std::abs(int(stick.y)) > std::abs(int(stick.x));
    const qint16 amount = vertical ? stick.y : stick.x;
    if (std::abs(int(amount)) < kPressThreshold)
        return;
    const Control direction = directionFor(vertical, amount);
    stick.direction = direction;
    // The right stick moves the volume only: its horizontal directions hold the stick without a key
    if (right && (direction == Control::RightStickLeft || direction == Control::RightStickRight))
        return;
    press(device, direction);
}

void GamepadMapper::deviceRemoved(quint32 device)
{
    for (auto it = m_held.begin(); it != m_held.end();) {
        if (quint32(it.key() >> 8) == device)
            it = m_held.erase(it);
        else
            ++it;
    }
    m_leftSticks.remove(device);
    m_rightSticks.remove(device);
    for (auto it = m_triggers.begin(); it != m_triggers.end();) {
        if (quint32(it.key() >> 8) == device)
            it = m_triggers.erase(it);
        else
            ++it;
    }
}

void GamepadMapper::advance(qint64 milliseconds)
{
    if (milliseconds <= 0)
        return;
    m_now += milliseconds;
    if (!m_active || !m_repeatEnabled)
        return;
    // One repeat per held control and tick, however long the tick was: a stalled event loop or a suspend must not replay the missed ones in a burst
    QList<quint64> due;
    for (auto it = m_held.begin(); it != m_held.end(); ++it) {
        if (!it->repeats || it->nextRepeat > m_now)
            continue;
        due.append(it.key());
        it->nextRepeat = m_now + std::max(1, m_timing.repeatIntervalMs);
    }
    // Emitted after the walk, each only while still held: a receiver may stop the input or remove a pad, which drops held controls
    for (const quint64 key : std::as_const(due)) {
        const auto held = m_held.constFind(key);
        if (held != m_held.constEnd())
            Q_EMIT keycodeRequested(held->keycode, true);
    }
}
} // namespace Kinefin
