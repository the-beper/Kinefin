// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStringList>
#include <QtGlobal>

namespace Kinefin {

// What the player asks of a watching-together group while it routes the user's playback actions there (SyncPlayController implements it): every action that would move the shared playback becomes a request, and the group's commands come back through the player's C++-only apply methods, so no path (QML, MPRIS, remote control) can move the local playback alone
class GroupPlayback {
  public:
    virtual ~GroupPlayback() = default;
    // Whether the player's pause, resume, seek and play actions go to the group now
    [[nodiscard]] virtual bool routesPlayback() const = 0;
    virtual void requestPause() = 0;
    virtual void requestUnpause() = 0;
    virtual void requestSeek(qint64 ticks) = 0;
    // A new group queue of `itemIds`, starting at `index` from `startTicks`; false when the group cannot take it
    virtual bool requestQueue(const QStringList &itemIds, int index, qint64 startTicks) = 0;
};

} // namespace Kinefin
