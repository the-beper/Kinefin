// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The SDL side on a virtual pad: hotplug, a button becoming a keycode, the printed labels, the signal handlers SDL must leave alone, and SDL initialised only while the input runs
#include "integration/GamepadInput.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <SDL3/SDL.h>

#include <csignal>

using Kinefin::GamepadInput;

namespace {
// A 15-button, 6-axis gamepad with SDL's own mapping; the virtual joystick API needs no real device
SDL_JoystickID attachPad()
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.nbuttons = 15;
    desc.button_mask =
        (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST)
        | (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH)
        | (1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START)
        | (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)
        | (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) | (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN)
        | (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) | (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    desc.naxes = 6;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY)
                     | (1u << SDL_GAMEPAD_AXIS_RIGHTX) | (1u << SDL_GAMEPAD_AXIS_RIGHTY)
                     | (1u << SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
                     | (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    desc.name = "Kinefin Test Pad";
    return SDL_AttachVirtualJoystick(&desc);
}
} // namespace

class TestGamepadInput : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void signalHandlersStayDefault()
    {
        struct sigaction termBefore{};
        struct sigaction interruptBefore{};
        sigaction(SIGTERM, nullptr, &termBefore);
        sigaction(SIGINT, nullptr, &interruptBefore);
        GamepadInput input;
        QVERIFY(input.start());
        struct sigaction term{};
        struct sigaction interrupt{};
        sigaction(SIGTERM, nullptr, &term);
        sigaction(SIGINT, nullptr, &interrupt);
        QVERIFY(term.sa_handler == termBefore.sa_handler);
        QVERIFY(interrupt.sa_handler == interruptBefore.sa_handler);
        // SDL leaves whatever handler the process had
        QVERIFY(term.sa_handler != SIG_IGN);
    }

    void aVirtualPadConnectsAndDisconnects()
    {
        GamepadInput input;
        QVERIFY(input.start());
        QCOMPARE(input.connectedCount(), 0);
        QSignalSpy connected(&input, &GamepadInput::connectedChanged);
        const SDL_JoystickID id = attachPad();
        QVERIFY(id > 0);
        QTRY_COMPARE(input.connectedCount(), 1);
        QVERIFY(connected.count() >= 1);
        SDL_DetachVirtualJoystick(id);
        QTRY_COMPARE(input.connectedCount(), 0);
    }

    void aButtonOfTheVirtualPadBecomesAKeycode()
    {
        GamepadInput input;
        QVERIFY(input.start());
        const SDL_JoystickID id = attachPad();
        QVERIFY(id > 0);
        QTRY_COMPARE(input.connectedCount(), 1);
        QSignalSpy keys(&input, &GamepadInput::keycodeRequested);
        SDL_Joystick *joystick = SDL_GetJoystickFromID(id);
        QVERIFY(joystick);
        QVERIFY(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
        QTRY_VERIFY(!keys.isEmpty());
        QCOMPARE(keys.constFirst().constFirst().toString(), QStringLiteral("select"));
        QCOMPARE(keys.constFirst().at(1).toBool(), false);
        SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
        SDL_DetachVirtualJoystick(id);
    }

    void theLabelsFollowThePad()
    {
        GamepadInput input;
        QVERIFY(input.start());
        QVERIFY(input.faceLabels().isEmpty());
        const SDL_JoystickID id = attachPad();
        QVERIFY(id > 0);
        QTRY_VERIFY(!input.faceLabels().isEmpty());
        QCOMPARE(input.faceLabels().size(), 4);
        for (const QString &label : input.faceLabels())
            QVERIFY(!label.isEmpty());
        SDL_DetachVirtualJoystick(id);
    }

    void sdlIsInitialisedOnlyWhileRunning()
    {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        GamepadInput input;
        QVERIFY(!input.running());
        QCOMPARE(SDL_WasInit(SDL_INIT_GAMEPAD), 0);
        QVERIFY(input.start());
        QCOMPARE(SDL_WasInit(SDL_INIT_GAMEPAD), SDL_INIT_GAMEPAD);
        input.stop();
        QCOMPARE(SDL_WasInit(SDL_INIT_GAMEPAD), 0);
    }
};

QTEST_MAIN(TestGamepadInput)
#include "test_gamepad_input.moc"
