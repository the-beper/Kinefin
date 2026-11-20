// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The fixed gamepad table on its own: buttons and axes in, keycodes out, with the repeat and activity rules
#include "integration/GamepadMapper.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <SDL3/SDL_gamepad.h>

using Kinefin::GamepadMapper;

class TestGamepadMapper : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void eachControlMapsToItsKeycode()
    {
        const std::pair<int, const char *> buttons[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, "select"},
            {SDL_GAMEPAD_BUTTON_EAST, "back"},
            {SDL_GAMEPAD_BUTTON_WEST, "menu"},
            {SDL_GAMEPAD_BUTTON_NORTH, "playpause"},
            {SDL_GAMEPAD_BUTTON_START, "mainmenu"},
            {SDL_GAMEPAD_BUTTON_BACK, "expandplayer"},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "previoustab"},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "nexttab"},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, "up"},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "down"},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "left"},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "right"}};
        for (const auto &[button, keycode] : buttons) {
            GamepadMapper mapper;
            QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
            mapper.button(1, button, true);
            QCOMPARE(keys.count(), 1);
            QCOMPARE(keys.constFirst().constFirst().toString(), QString::fromLatin1(keycode));
            QCOMPARE(keys.constFirst().at(1).toBool(), false);
            mapper.button(1, button, false);
        }
        const std::pair<int, const char *> axes[] = {
            {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, "previouschapter"},
            {SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, "nextchapter"}};
        for (const auto &[axis, keycode] : axes) {
            GamepadMapper mapper;
            QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
            mapper.axis(1, axis, 16000);
            QCOMPARE(keys.count(), 1);
            QCOMPARE(keys.constFirst().constFirst().toString(), QString::fromLatin1(keycode));
            mapper.axis(1, axis, 0);
        }
        // The right stick moves the volume only; a new direction needs the old one to fall to rest
        GamepadMapper mapper;
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTY, -16000);
        QCOMPARE(keys.count(), 1);
        QCOMPARE(keys.constFirst().constFirst().toString(), QStringLiteral("volumeup"));
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTY, 0);
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTY, 16000);
        QCOMPARE(keys.count(), 2);
        QCOMPARE(keys.last().constFirst().toString(), QStringLiteral("volumedown"));
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTX, 16000);
        QCOMPARE(keys.count(), 2);
    }

    void sticksPressAtTheThresholdAndReleaseBelowTheRestBand()
    {
        GamepadMapper mapper;
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFTX, 16000);
        QCOMPARE(keys.count(), 1);
        QCOMPARE(keys.constFirst().constFirst().toString(), QStringLiteral("right"));
        // Above the rest band the direction stays held
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFTX, 9000);
        QCOMPARE(keys.count(), 1);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFTX, 8000);
        QCOMPARE(keys.count(), 1);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFTX, 7999);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFTX, 16000);
        QCOMPARE(keys.count(), 2);
        QCOMPARE(keys.last().constFirst().toString(), QStringLiteral("right"));
    }

    void onlyTheDominantAxisCounts()
    {
        GamepadMapper mapper;
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFTX, 9000);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFTY, 16000);
        QCOMPARE(keys.count(), 1);
        QCOMPARE(keys.constFirst().constFirst().toString(), QStringLiteral("down"));
        // Left of the right stick holds the stick without a key
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTX, -16000);
        QCOMPARE(keys.count(), 1);
    }

    void triggersPressAndRelease()
    {
        GamepadMapper mapper;
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 15999);
        QCOMPARE(keys.count(), 0);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 16000);
        QCOMPARE(keys.count(), 1);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 9000);
        QCOMPARE(keys.count(), 1);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 7999);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 16000);
        QCOMPARE(keys.count(), 2);
    }

    void directionsRepeatAfterTheDelay()
    {
        GamepadMapper mapper;
        mapper.setTiming({400, 100});
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        QCOMPARE(keys.count(), 1);
        QCOMPARE(keys.constFirst().at(1).toBool(), false);
        mapper.advance(399);
        QCOMPARE(keys.count(), 1);
        mapper.advance(1);
        QCOMPARE(keys.count(), 2);
        QCOMPARE(keys.last().at(1).toBool(), true);
        mapper.advance(100);
        QCOMPARE(keys.count(), 3);
        mapper.advance(1000);
        QCOMPARE(keys.last().constFirst().toString(), QStringLiteral("up"));
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
        const int held = keys.count();
        mapper.advance(1000);
        QCOMPARE(keys.count(), held);
    }

    void noRepeatWithInputRepeatOff()
    {
        GamepadMapper mapper;
        mapper.setTiming({400, 100});
        mapper.setRepeatEnabled(false);
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        mapper.advance(2000);
        QCOMPARE(keys.count(), 1);
    }

    void buttonsNeverRepeat()
    {
        GamepadMapper mapper;
        mapper.setTiming({400, 100});
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.button(1, SDL_GAMEPAD_BUTTON_SOUTH, true);
        mapper.advance(2000);
        QCOMPARE(keys.count(), 1);
    }

    void repeatsCarryTheAutoRepeatFlag()
    {
        GamepadMapper mapper;
        mapper.setTiming({100, 100});
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTY, -20000);
        mapper.advance(100);
        QCOMPARE(keys.count(), 2);
        QCOMPARE(keys.at(0).at(1).toBool(), false);
        QCOMPARE(keys.at(1).at(1).toBool(), true);
        QCOMPARE(keys.at(1).at(0).toString(), QStringLiteral("volumeup"));
    }

    void aRemovedPadReleasesItsControls()
    {
        GamepadMapper mapper;
        mapper.setTiming({100, 100});
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        mapper.button(2, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        QCOMPARE(keys.count(), 2);
        mapper.deviceRemoved(1);
        mapper.advance(100);
        QCOMPARE(keys.count(), 3);
        QCOMPARE(keys.last().constFirst().toString(), QStringLiteral("up"));
        for (int tick = 0; tick < 10; ++tick)
            mapper.advance(100);
        QCOMPARE(keys.count(), 13);
        mapper.deviceRemoved(2);
        const int held = keys.count();
        mapper.advance(1000);
        QCOMPARE(keys.count(), held);
    }

    void theRightStickReleasesTheVolume()
    {
        GamepadMapper mapper;
        mapper.setTiming({100, 100});
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTY, -20000);
        mapper.advance(100);
        QCOMPARE(keys.count(), 2);
        // Back at rest: no more repeats, and the next push is a new press
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTY, 0);
        mapper.advance(1000);
        QCOMPARE(keys.count(), 2);
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTY, -20000);
        QCOMPARE(keys.count(), 3);
        QCOMPARE(keys.last().constFirst().toString(), QStringLiteral("volumeup"));
        QCOMPARE(keys.last().at(1).toBool(), false);
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTY, 0);
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTY, 20000);
        QCOMPARE(keys.count(), 4);
        QCOMPARE(keys.last().constFirst().toString(), QStringLiteral("volumedown"));
        mapper.axis(1, SDL_GAMEPAD_AXIS_RIGHTY, 0);
        mapper.advance(1000);
        QCOMPARE(keys.count(), 4);
    }

    void aLongTickRepeatsOnce()
    {
        GamepadMapper mapper;
        mapper.setTiming({100, 100});
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        // A stalled loop or a suspend: the missed repeats are not replayed
        mapper.advance(60'000);
        QCOMPARE(keys.count(), 2);
        mapper.advance(99);
        QCOMPARE(keys.count(), 2);
        mapper.advance(1);
        QCOMPARE(keys.count(), 3);
    }

    void aResetForgetsTheHeldControls()
    {
        GamepadMapper mapper;
        mapper.setTiming({100, 100});
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 20000);
        QCOMPARE(keys.count(), 2);
        mapper.reset();
        mapper.advance(1000);
        QCOMPARE(keys.count(), 2);
        // The release never came: a new press acts at once
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 20000);
        QCOMPARE(keys.count(), 4);
    }

    void aReceiverMayDropTheControlsDuringARepeat()
    {
        GamepadMapper mapper;
        mapper.setTiming({100, 100});
        int received = 0;
        connect(&mapper, &GamepadMapper::keycodeRequested, &mapper,
                [&mapper, &received](const QString &, bool autoRepeat) {
                    ++received;
                    if (autoRepeat)
                        mapper.reset();
                });
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        mapper.button(2, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        QCOMPARE(received, 2);
        // Both repeats were due, but the first receiver dropped the held controls: the second never comes
        mapper.advance(100);
        QCOMPARE(received, 3);
        mapper.advance(1000);
        QCOMPARE(received, 3);
    }

    void nothingActsWhileInactive()
    {
        GamepadMapper mapper;
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.setActive(false);
        mapper.button(1, SDL_GAMEPAD_BUTTON_SOUTH, true);
        mapper.axis(1, SDL_GAMEPAD_AXIS_LEFTX, 20000);
        mapper.advance(1000);
        QCOMPARE(keys.count(), 0);
        mapper.setActive(true);
        mapper.button(1, SDL_GAMEPAD_BUTTON_SOUTH, true);
        QCOMPARE(keys.count(), 1);
    }

    void aHeldControlNeedsANewPressAfterActivation()
    {
        GamepadMapper mapper;
        mapper.setTiming({100, 100});
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        mapper.setActive(false);
        mapper.setActive(true);
        mapper.advance(1000);
        QCOMPARE(keys.count(), 1);
        // The release of the old press is ignored, then a new press acts
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        QCOMPARE(keys.count(), 2);
    }

    void twoPadsKeepTheirOwnState()
    {
        GamepadMapper mapper;
        mapper.setTiming({100, 100});
        QSignalSpy keys(&mapper, &GamepadMapper::keycodeRequested);
        mapper.button(1, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
        mapper.button(2, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
        QCOMPARE(keys.count(), 2);
        mapper.advance(100);
        QCOMPARE(keys.count(), 4);
        for (const QList<QVariant> &key : std::as_const(keys))
            QCOMPARE(key.constFirst().toString(), QStringLiteral("right"));
    }
};

QTEST_GUILESS_MAIN(TestGamepadMapper)
#include "test_gamepad_mapper.moc"
