// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QTimer>
#include <optional>

namespace Kinefin {
class PlayerController;

// Keeps the screen on while the player plays (opening, loading and buffering included), through org.freedesktop.ScreenSaver: one Inhibit when playback starts, the UnInhibit when it pauses or stops, and on destruction. A desktop without the service is warned about once
class ScreenSaverInhibitor final : public QObject {
    Q_OBJECT
  public:
    ScreenSaverInhibitor(PlayerController &player, const QDBusConnection &bus,
                         QObject *parent = nullptr);
    ~ScreenSaverInhibitor() override;

    // For the tests: whether a cookie is held
    bool inhibiting() const { return m_cookie.has_value(); }

  private:
    void update();
    void release();

    // The player's state is read a turn of the event loop after it changed: an episode that ends and starts the next one passes through Stopped on the way, which must not release and take the inhibition again
    QTimer m_settle;

    PlayerController &m_player;
    QDBusConnection m_bus;
    std::optional<uint> m_cookie;
    bool m_pending = false;
    bool m_warned = false;
    // The last try failed during this run of playback: the next try waits for the next one
    bool m_failed = false;
};
} // namespace Kinefin
