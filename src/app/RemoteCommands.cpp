// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RemoteCommands.h"

#include "api/JellyfinSocketClient.h"
#include "api/PlaybackReport.h"

#include <QHash>

#include <algorithm>

namespace Kinefin {
namespace {
// The longest position a remote seek or start may name: a day, as everywhere else in the player
constexpr qint64 MaxTicks = 24LL * 3600 * 10'000'000;

const QHash<QString, QString> &playstateKeycodes()
{
    static const QHash<QString, QString> keycodes{
        {QStringLiteral("PlayPause"), QStringLiteral("playpause")},
        {QStringLiteral("Pause"), QStringLiteral("pause")},
        {QStringLiteral("Unpause"), QStringLiteral("play")},
        {QStringLiteral("Stop"), QStringLiteral("stop")},
        {QStringLiteral("NextTrack"), QStringLiteral("next")},
        {QStringLiteral("PreviousTrack"), QStringLiteral("previous")},
        {QStringLiteral("FastForward"), QStringLiteral("fastforward")},
        {QStringLiteral("Rewind"), QStringLiteral("rewind")}};
    return keycodes;
}

const QHash<QString, QString> &generalKeycodes()
{
    static const QHash<QString, QString> keycodes{
        {QStringLiteral("MoveUp"), QStringLiteral("up")},
        {QStringLiteral("MoveDown"), QStringLiteral("down")},
        {QStringLiteral("MoveLeft"), QStringLiteral("left")},
        {QStringLiteral("MoveRight"), QStringLiteral("right")},
        {QStringLiteral("Select"), QStringLiteral("select")},
        {QStringLiteral("Back"), QStringLiteral("back")},
        {QStringLiteral("GoHome"), QStringLiteral("home")},
        {QStringLiteral("VolumeUp"), QStringLiteral("volumeup")},
        {QStringLiteral("VolumeDown"), QStringLiteral("volumedown")},
        {QStringLiteral("ToggleMute"), QStringLiteral("togglemute")},
        {QStringLiteral("ToggleFullscreen"), QStringLiteral("togglefullscreen")}};
    return keycodes;
}

// An integer argument in [low, high], as QString::toInt reads it
bool intArgument(const QVariantMap &arguments, const QString &key, int low, int high, int *out)
{
    bool ok = false;
    const int value = arguments.value(key).toString().toInt(&ok);
    if (!ok || value < low || value > high)
        return false;
    *out = value;
    return true;
}
} // namespace

RemoteCommands::RemoteCommands(QObject *parent) : QObject(parent) {}

void RemoteCommands::attach(JellyfinSocketClient *socket)
{
    connect(socket, &JellyfinSocketClient::playstateCommand, this,
            &RemoteCommands::handlePlaystate);
    connect(socket, &JellyfinSocketClient::generalCommand, this,
            &RemoteCommands::handleGeneralCommand);
    connect(socket, &JellyfinSocketClient::playRequested, this, &RemoteCommands::handlePlay);
}

void RemoteCommands::handlePlaystate(const QString &command, qint64 seekPositionTicks)
{
    if (command == QLatin1String("Seek")) {
        if (seekPositionTicks >= 0 && seekPositionTicks <= MaxTicks)
            Q_EMIT seekRequested(seekPositionTicks);
        return;
    }
    const QString keycode = playstateKeycodes().value(command);
    if (!keycode.isEmpty())
        Q_EMIT keycodeRequested(keycode);
}

void RemoteCommands::handleGeneralCommand(const QString &name, const QVariantMap &arguments)
{
    if (!supportedGeneralCommands().contains(name))
        return;
    if (const QString keycode = generalKeycodes().value(name); !keycode.isEmpty()) {
        Q_EMIT keycodeRequested(keycode);
        return;
    }
    int value = 0;
    if (name == QLatin1String("Mute") || name == QLatin1String("Unmute")) {
        Q_EMIT muteRequested(name == QLatin1String("Mute"));
    } else if (name == QLatin1String("SetVolume")) {
        if (intArgument(arguments, QStringLiteral("Volume"), 0, 100, &value))
            Q_EMIT volumeRequested(value);
    } else if (name == QLatin1String("SetAudioStreamIndex")) {
        if (intArgument(arguments, QStringLiteral("Index"), 0, 1024, &value))
            Q_EMIT audioStreamRequested(value);
    } else if (name == QLatin1String("SetSubtitleStreamIndex")) {
        if (intArgument(arguments, QStringLiteral("Index"), -1, 1024, &value))
            Q_EMIT subtitleStreamRequested(value);
    } else if (name == QLatin1String("SetRepeatMode")) {
        static const QStringList modes{QStringLiteral("RepeatNone"), QStringLiteral("RepeatAll"),
                                       QStringLiteral("RepeatOne")};
        const int mode =
            int(modes.indexOf(arguments.value(QStringLiteral("RepeatMode")).toString()));
        if (mode >= 0)
            Q_EMIT repeatModeRequested(mode);
    } else if (name == QLatin1String("SetShuffleQueue")) {
        const QString mode = arguments.value(QStringLiteral("ShuffleMode")).toString();
        if (mode == QLatin1String("Shuffle") || mode == QLatin1String("Sorted"))
            Q_EMIT shuffleRequested(mode == QLatin1String("Shuffle"));
    }
}

void RemoteCommands::handlePlay(const RemotePlayRequest &request)
{
    if (request.itemIds.isEmpty())
        return;
    if (request.command == QLatin1String("PlayNow")) {
        const qint64 ticks =
            request.startPositionTicks >= 0 && request.startPositionTicks <= MaxTicks
                ? request.startPositionTicks
                : 0;
        Q_EMIT playNowRequested(request.itemIds, std::max(0, request.startIndex), ticks);
    } else if (request.command == QLatin1String("PlayNext")
               || request.command == QLatin1String("PlayLast")) {
        Q_EMIT queueRequested(request.itemIds, request.command == QLatin1String("PlayNext"));
    }
}
} // namespace Kinefin
