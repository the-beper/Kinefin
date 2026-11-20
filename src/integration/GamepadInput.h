// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

struct SDL_Gamepad;

namespace Kinefin {
class GamepadMapper;

// SDL3 gamepads of the running app: hints, the GUI-thread pump, hotplug, pads opened and closed, SDL messages forwarded to the profile log; the mapper turns their events into the shared keycodes
class GamepadInput final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int connectedCount READ connectedCount NOTIFY connectedChanged)
    // The first pad's printed face labels, south, east, west and north: A, B, X, Y or Cross, Circle, Square, Triangle
    Q_PROPERTY(QStringList faceLabels READ faceLabels NOTIFY connectedChanged)
  public:
    explicit GamepadInput(QObject *parent = nullptr);
    ~GamepadInput() override;

    // Initialises SDL for gamepads only; false, with one warning, when SDL refuses. Safe to call twice
    bool start();
    void stop();
    [[nodiscard]] bool running() const { return m_running; }
    void setRepeatEnabled(bool enabled);

    int connectedCount() const { return int(m_pads.size()); }
    QStringList faceLabels() const;

  Q_SIGNALS:
    void keycodeRequested(const QString &keycode, bool autoRepeat);
    void connectedChanged();

  private:
    void pump();
    void followApplicationState();
    void openPad(quint32 id);
    void closePad(quint32 id);
    void applyInterval();

    GamepadMapper *m_mapper = nullptr;
    QTimer m_timer;
    QElapsedTimer m_sinceTick;
    QHash<quint32, SDL_Gamepad *> m_pads;
    bool m_running = false;
    // SDL was initialised once: the destructor quits it then, and only then
    bool m_initialised = false;
    bool m_active = true;
};
} // namespace Kinefin
