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
    // Whether the group's queue has an entry after or before the playing one (Repeat All wraps), and the requests to go there; false when there is none
    [[nodiscard]] virtual bool hasNext() const = 0;
    [[nodiscard]] virtual bool hasPrevious() const = 0;
    virtual bool requestNext() = 0;
    virtual bool requestPrevious() = 0;
    // The group's item `itemId` played to its end: true when the group moves on to another entry, which the player then waits for
    virtual bool itemEnded(const QString &itemId) = 0;
    // The player was closed: the group plays on without this session, which stays a member
    virtual void stopFollowing() = 0;
    // The group's shuffle and repeat (0 off, 1 all, 2 one) and the requests to change them
    [[nodiscard]] virtual bool shuffle() const = 0;
    [[nodiscard]] virtual int repeatMode() const = 0;
    virtual void requestShuffle(bool shuffle) = 0;
    virtual void requestRepeatMode(int mode) = 0;
};

} // namespace Kinefin
