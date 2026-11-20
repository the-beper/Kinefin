// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QString>
#include <QTimer>

namespace Kinefin {
class PlayerController;

// Keeps the computer from suspending while a cast plays, from the renderer's load (its opening and buffering included), through the Inhibit portal with the suspend flag only: the screen may still turn off, since the video is on another screen. The portal's request is closed when the cast pauses or ends, and on destruction. A desktop without the portal is warned about once
class SuspendInhibitor final : public QObject {
    Q_OBJECT
  public:
    SuspendInhibitor(PlayerController &player, const QDBusConnection &bus,
                     QObject *parent = nullptr);
    ~SuspendInhibitor() override;

    // For the tests: whether the portal granted an inhibition that is still held
    bool inhibiting() const { return !m_handle.isEmpty(); }

  private:
    void update();
    void release();

    // Read a turn of the event loop after the player changed: an episode that ends and starts the next one on the same device passes through Stopped
    QTimer m_settle;

    PlayerController &m_player;
    QDBusConnection m_bus;
    // The portal's request object: closing it ends the inhibition
    QString m_handle;
    bool m_pending = false;
    bool m_warned = false;
    // The last try failed during this cast: the next try waits for the next one
    bool m_failed = false;
};
} // namespace Kinefin
