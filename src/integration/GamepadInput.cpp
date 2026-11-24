// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/GamepadInput.h"

#include "integration/GamepadMapper.h"
#include "integration/Mpris.h"

#include <KLocalizedString>

#include <QDebug>
#include <QGuiApplication>

// Only the SDL 3.2 API: the KDE runtime ships 3.2.30 while the build image has 3.4, so a call that exists only since 3.4 builds on the image and fails the Flatpak build
#include <SDL3/SDL.h>

namespace Kinefin {
namespace {
// One tick while a window is in front, a slower hotplug check without one, the slowest with no pad
constexpr int kActiveIntervalMs = 16;
constexpr int kBackgroundIntervalMs = 250;
constexpr int kIdleIntervalMs = 1000;

// An app with no window of its own (the offscreen tests) has nothing to protect: input is taken
bool windowHasFocus()
{
    return QGuiApplication::applicationState() != Qt::ApplicationInactive
           || QGuiApplication::allWindows().isEmpty();
}

QString faceLabel(SDL_GamepadButtonLabel label)
{
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
        return i18nc("@item:intable game controller button label", "Cross");
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
        return i18nc("@item:intable game controller button label", "Circle");
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
        return i18nc("@item:intable game controller button label", "Square");
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
        return i18nc("@item:intable game controller button label", "Triangle");
    case SDL_GAMEPAD_BUTTON_LABEL_B:
        return QStringLiteral("B");
    case SDL_GAMEPAD_BUTTON_LABEL_X:
        return QStringLiteral("X");
    case SDL_GAMEPAD_BUTTON_LABEL_Y:
        return QStringLiteral("Y");
    default:
        return QStringLiteral("A");
    }
}

void logMessage(void *, int, SDL_LogPriority priority, const char *message)
{
    // Device names and driver text are untrusted: the socket's cleaning, then the log
    const QString clean = mprisText(QString::fromUtf8(message)).left(512);
    if (clean.isEmpty())
        return;
    if (priority >= SDL_LOG_PRIORITY_WARN)
        qWarning("Gamepad: %s", qPrintable(clean));
    else
        qInfo("Gamepad: %s", qPrintable(clean));
}
} // namespace

GamepadInput::GamepadInput(QObject *parent)
    : QObject(parent), m_mapper(new GamepadMapper(this)), m_timer(this)
{
    connect(m_mapper, &GamepadMapper::keycodeRequested, this, &GamepadInput::keycodeRequested);
    connect(&m_timer, &QTimer::timeout, this, &GamepadInput::pump);
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            &GamepadInput::followApplicationState);
}

GamepadInput::~GamepadInput()
{
    stop();
    if (m_initialised)
        SDL_Quit();
}

bool GamepadInput::start()
{
    if (m_running)
        return true;
    // Kinefin keeps the default signal handling; with no SDL window the background hint keeps events flowing, and HIDAPI stays off (the sandbox has no /dev/hidraw)
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetLogOutputFunction(logMessage, nullptr);
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        qWarning("Gamepad input is off: %s", SDL_GetError());
        return false;
    }
    m_running = true;
    m_initialised = true;
    m_active = windowHasFocus();
    m_mapper->setActive(m_active);
    m_sinceTick.start();
    m_timer.start(kIdleIntervalMs);
    return true;
}

void GamepadInput::stop()
{
    if (!m_running)
        return;
    m_running = false;
    m_timer.stop();
    for (SDL_Gamepad *pad : std::as_const(m_pads))
        SDL_CloseGamepad(pad);
    m_pads.clear();
    m_mapper->reset();
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    Q_EMIT connectedChanged();
}

void GamepadInput::setRepeatEnabled(bool enabled)
{
    m_mapper->setRepeatEnabled(enabled);
}

QStringList GamepadInput::faceLabels() const
{
    if (m_pads.isEmpty())
        return {};
    SDL_Gamepad *pad = m_pads.constBegin().value();
    return {faceLabel(SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_SOUTH)),
            faceLabel(SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_EAST)),
            faceLabel(SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_WEST)),
            faceLabel(SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_NORTH))};
}

void GamepadInput::followApplicationState()
{
    const bool active = windowHasFocus();
    if (m_active == active)
        return;
    m_active = active;
    m_mapper->setActive(active);
    applyInterval();
}

void GamepadInput::applyInterval()
{
    m_timer.setInterval(m_pads.isEmpty() ? kIdleIntervalMs
                        : m_active       ? kActiveIntervalMs
                                         : kBackgroundIntervalMs);
}

void GamepadInput::pump()
{
    if (!m_running)
        return;
    // Repeats keep their own clock, at the tick rate of the moment
    m_mapper->advance(m_sinceTick.restart());
    // A key handled above, or one below, may have turned the input off: SDL is gone then
    if (!m_running)
        return;
    SDL_UpdateJoysticks();
    SDL_Event event;
    while (m_running && SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            openPad(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            closePad(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            m_mapper->button(event.gbutton.which, event.gbutton.button,
                             event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            m_mapper->axis(event.gaxis.which, event.gaxis.axis, event.gaxis.value);
            break;
        default:
            break;
        }
    }
    if (m_running)
        applyInterval();
}

void GamepadInput::openPad(quint32 id)
{
    if (m_pads.contains(id))
        return;
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (!pad) {
        qWarning("Gamepad could not be opened: %s", SDL_GetError());
        return;
    }
    m_pads.insert(id, pad);
    const char *name = SDL_GetGamepadName(pad);
    qInfo("Gamepad connected: %s",
          qPrintable(mprisText(QString::fromUtf8(name ? name : "")).left(64)));
    applyInterval();
    Q_EMIT connectedChanged();
}

void GamepadInput::closePad(quint32 id)
{
    SDL_Gamepad *pad = m_pads.take(id);
    if (!pad)
        return;
    const char *name = SDL_GetGamepadName(pad);
    qInfo("Gamepad disconnected: %s",
          qPrintable(mprisText(QString::fromUtf8(name ? name : "")).left(64)));
    m_mapper->deviceRemoved(id);
    SDL_CloseGamepad(pad);
    applyInterval();
    Q_EMIT connectedChanged();
}
} // namespace Kinefin
