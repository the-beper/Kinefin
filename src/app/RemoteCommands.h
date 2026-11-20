// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>

namespace Kinefin {
class JellyfinSocketClient;
struct RemotePlayRequest;

// The commands another Jellyfin session sends to this one over the WebSocket (the socket validates the frames), as the shell's actions: navigation and player keys under the names of the remote-control socket, the rest as signals of their own. Commands outside supportedGeneralCommands() and the Playstate table, and arguments out of range, are dropped
class RemoteCommands : public QObject {
    Q_OBJECT

  public:
    explicit RemoteCommands(QObject *parent = nullptr);

    // Takes the validated commands of `socket` from now on
    void attach(JellyfinSocketClient *socket);

    void handlePlaystate(const QString &command, qint64 seekPositionTicks);
    void handleGeneralCommand(const QString &name, const QVariantMap &arguments);
    void handlePlay(const RemotePlayRequest &request);

  Q_SIGNALS:
    // A keycode of the remote-control socket: "up", "down", "left", "right", "select", "back", "home", "playpause", "play", "pause", "stop", "next", "previous", "fastforward", "rewind", "volumeup", "volumedown", "togglemute", "togglefullscreen"
    void keycodeRequested(const QString &keycode);
    void seekRequested(qint64 ticks);
    // 0 to 100
    void volumeRequested(int level);
    void muteRequested(bool muted);
    // The server's stream index of the media source; -1 turns subtitles off
    void audioStreamRequested(int index);
    void subtitleStreamRequested(int index);
    // The maximum bit rate another session asked for, as it sent it; the player maps it to Automatic or the nearest step
    void qualityRequested(qint64 bitsPerSecond);
    // 0 off, 1 all, 2 the current item
    void repeatModeRequested(int mode);
    void shuffleRequested(bool shuffled);
    // PlayNow: the items become the queue and play from `startIndex` (an index into `itemIds`, 0 when the command named none) at `startTicks`
    void playNowRequested(const QStringList &itemIds, int startIndex, qint64 startTicks);
    // PlayNext and PlayLast: the items join the queue after the current one or at its end
    void queueRequested(const QStringList &itemIds, bool next);
};
} // namespace Kinefin
