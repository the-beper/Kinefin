// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// What another Jellyfin session's commands become: keycodes of the remote-control socket and the shell's own requests, and what is dropped
#include "RemoteCommands.h"
#include "api/JellyfinSocketClient.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::RemoteCommands;

namespace {
QString item(int number)
{
    return QStringLiteral("a1%1").arg(number, 30, 16, QLatin1Char('0'));
}
} // namespace

class TestRemoteCommands : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void playstateCommandsBecomeKeycodes_data()
    {
        QTest::addColumn<QString>("command");
        QTest::addColumn<QString>("keycode");
        QTest::newRow("PlayPause") << QStringLiteral("PlayPause") << QStringLiteral("playpause");
        QTest::newRow("Pause") << QStringLiteral("Pause") << QStringLiteral("pause");
        QTest::newRow("Unpause") << QStringLiteral("Unpause") << QStringLiteral("play");
        QTest::newRow("Stop") << QStringLiteral("Stop") << QStringLiteral("stop");
        QTest::newRow("NextTrack") << QStringLiteral("NextTrack") << QStringLiteral("next");
        QTest::newRow("PreviousTrack")
            << QStringLiteral("PreviousTrack") << QStringLiteral("previous");
        QTest::newRow("FastForward")
            << QStringLiteral("FastForward") << QStringLiteral("fastforward");
        QTest::newRow("Rewind") << QStringLiteral("Rewind") << QStringLiteral("rewind");
    }
    void playstateCommandsBecomeKeycodes()
    {
        QFETCH(QString, command);
        QFETCH(QString, keycode);
        RemoteCommands remote;
        QSignalSpy keycodes(&remote, &RemoteCommands::keycodeRequested);
        remote.handlePlaystate(command, -1);
        QCOMPARE(keycodes.size(), 1);
        QCOMPARE(keycodes.constFirst().at(0).toString(), keycode);
    }

    void aSeekNeedsAPositionWithinADay()
    {
        RemoteCommands remote;
        QSignalSpy seeks(&remote, &RemoteCommands::seekRequested);
        QSignalSpy keycodes(&remote, &RemoteCommands::keycodeRequested);
        remote.handlePlaystate(QStringLiteral("Seek"), 1234);
        remote.handlePlaystate(QStringLiteral("Seek"), -1);
        remote.handlePlaystate(QStringLiteral("Seek"), 24LL * 3600 * 10'000'000 + 1);
        remote.handlePlaystate(QStringLiteral("Unknown"), 0);
        QCOMPARE(seeks.size(), 1);
        QCOMPARE(seeks.constFirst().at(0).toLongLong(), 1234);
        QCOMPARE(keycodes.size(), 0);
    }

    void generalCommandsBecomeKeycodes_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("keycode");
        QTest::newRow("MoveUp") << QStringLiteral("MoveUp") << QStringLiteral("up");
        QTest::newRow("MoveDown") << QStringLiteral("MoveDown") << QStringLiteral("down");
        QTest::newRow("MoveLeft") << QStringLiteral("MoveLeft") << QStringLiteral("left");
        QTest::newRow("MoveRight") << QStringLiteral("MoveRight") << QStringLiteral("right");
        QTest::newRow("Select") << QStringLiteral("Select") << QStringLiteral("select");
        QTest::newRow("Back") << QStringLiteral("Back") << QStringLiteral("back");
        QTest::newRow("GoHome") << QStringLiteral("GoHome") << QStringLiteral("home");
        QTest::newRow("VolumeUp") << QStringLiteral("VolumeUp") << QStringLiteral("volumeup");
        QTest::newRow("VolumeDown") << QStringLiteral("VolumeDown") << QStringLiteral("volumedown");
        QTest::newRow("ToggleMute") << QStringLiteral("ToggleMute") << QStringLiteral("togglemute");
        QTest::newRow("ToggleFullscreen")
            << QStringLiteral("ToggleFullscreen") << QStringLiteral("togglefullscreen");
    }
    void generalCommandsBecomeKeycodes()
    {
        QFETCH(QString, name);
        QFETCH(QString, keycode);
        RemoteCommands remote;
        QSignalSpy keycodes(&remote, &RemoteCommands::keycodeRequested);
        remote.handleGeneralCommand(name, {});
        QCOMPARE(keycodes.size(), 1);
        QCOMPARE(keycodes.constFirst().at(0).toString(), keycode);
    }

    void generalCommandsWithArgumentsAreBounded()
    {
        RemoteCommands remote;
        QSignalSpy volumes(&remote, &RemoteCommands::volumeRequested);
        QSignalSpy mutes(&remote, &RemoteCommands::muteRequested);
        QSignalSpy audio(&remote, &RemoteCommands::audioStreamRequested);
        QSignalSpy subtitles(&remote, &RemoteCommands::subtitleStreamRequested);
        QSignalSpy repeats(&remote, &RemoteCommands::repeatModeRequested);
        QSignalSpy shuffles(&remote, &RemoteCommands::shuffleRequested);
        const auto arg = [](const char *key, const QString &value) {
            return QVariantMap{{QLatin1String(key), value}};
        };
        remote.handleGeneralCommand(QStringLiteral("SetVolume"),
                                    arg("Volume", QStringLiteral("35")));
        remote.handleGeneralCommand(QStringLiteral("SetVolume"),
                                    arg("Volume", QStringLiteral("101")));
        remote.handleGeneralCommand(QStringLiteral("SetVolume"),
                                    arg("Volume", QStringLiteral("x")));
        remote.handleGeneralCommand(QStringLiteral("SetVolume"), {});
        QCOMPARE(volumes.size(), 1);
        QCOMPARE(volumes.constFirst().at(0).toInt(), 35);
        remote.handleGeneralCommand(QStringLiteral("Mute"), {});
        remote.handleGeneralCommand(QStringLiteral("Unmute"), {});
        QCOMPARE(mutes.size(), 2);
        QCOMPARE(mutes.at(0).at(0).toBool(), true);
        QCOMPARE(mutes.at(1).at(0).toBool(), false);
        remote.handleGeneralCommand(QStringLiteral("SetAudioStreamIndex"),
                                    arg("Index", QStringLiteral("2")));
        remote.handleGeneralCommand(QStringLiteral("SetAudioStreamIndex"),
                                    arg("Index", QStringLiteral("-1")));
        QCOMPARE(audio.size(), 1);
        QCOMPARE(audio.constFirst().at(0).toInt(), 2);
        remote.handleGeneralCommand(QStringLiteral("SetSubtitleStreamIndex"),
                                    arg("Index", QStringLiteral("-1")));
        remote.handleGeneralCommand(QStringLiteral("SetSubtitleStreamIndex"),
                                    arg("Index", QStringLiteral("-2")));
        QCOMPARE(subtitles.size(), 1);
        QCOMPARE(subtitles.constFirst().at(0).toInt(), -1);
        remote.handleGeneralCommand(QStringLiteral("SetRepeatMode"),
                                    arg("RepeatMode", QStringLiteral("RepeatAll")));
        remote.handleGeneralCommand(QStringLiteral("SetRepeatMode"),
                                    arg("RepeatMode", QStringLiteral("RepeatOne")));
        remote.handleGeneralCommand(QStringLiteral("SetRepeatMode"),
                                    arg("RepeatMode", QStringLiteral("Sometimes")));
        QCOMPARE(repeats.size(), 2);
        QCOMPARE(repeats.at(0).at(0).toInt(), 1);
        QCOMPARE(repeats.at(1).at(0).toInt(), 2);
        remote.handleGeneralCommand(QStringLiteral("SetShuffleQueue"),
                                    arg("ShuffleMode", QStringLiteral("Shuffle")));
        remote.handleGeneralCommand(QStringLiteral("SetShuffleQueue"),
                                    arg("ShuffleMode", QStringLiteral("Sorted")));
        remote.handleGeneralCommand(QStringLiteral("SetShuffleQueue"),
                                    arg("ShuffleMode", QStringLiteral("Random")));
        QCOMPARE(shuffles.size(), 2);
        QCOMPARE(shuffles.at(0).at(0).toBool(), true);
        QCOMPARE(shuffles.at(1).at(0).toBool(), false);
    }

    void unsupportedCommandsAreDropped()
    {
        RemoteCommands remote;
        QSignalSpy keycodes(&remote, &RemoteCommands::keycodeRequested);
        QSignalSpy plays(&remote, &RemoteCommands::playNowRequested);
        // A message on the screen is never shown, nor anything the app does not announce
        for (const char *name : {"DisplayMessage", "SendString", "GoToSettings", "TakeScreenshot"})
            remote.handleGeneralCommand(QLatin1String(name),
                                        {{QStringLiteral("Text"), QStringLiteral("hello")}});
        QCOMPARE(keycodes.size(), 0);
        QCOMPARE(plays.size(), 0);
    }

    void playCommandsSplitIntoNowAndQueue()
    {
        RemoteCommands remote;
        QSignalSpy now(&remote, &RemoteCommands::playNowRequested);
        QSignalSpy queued(&remote, &RemoteCommands::queueRequested);
        const QStringList ids{item(1), item(2), item(3)};
        remote.handlePlay({QStringLiteral("PlayNow"), ids, 900, 2});
        QCOMPARE(now.size(), 1);
        QCOMPARE(now.constFirst().at(0).toStringList(), ids);
        QCOMPARE(now.constFirst().at(1).toInt(), 2);
        QCOMPARE(now.constFirst().at(2).toLongLong(), 900);
        // No start named: the first item, from its beginning
        remote.handlePlay({QStringLiteral("PlayNow"), ids, -1, -1});
        QCOMPARE(now.size(), 2);
        QCOMPARE(now.at(1).at(1).toInt(), 0);
        QCOMPARE(now.at(1).at(2).toLongLong(), 0);
        remote.handlePlay({QStringLiteral("PlayNext"), ids, -1, -1});
        remote.handlePlay({QStringLiteral("PlayLast"), ids, -1, -1});
        QCOMPARE(queued.size(), 2);
        QCOMPARE(queued.at(0).at(1).toBool(), true);
        QCOMPARE(queued.at(1).at(1).toBool(), false);
        remote.handlePlay({QStringLiteral("PlayInstantMix"), ids, -1, -1});
        remote.handlePlay({QStringLiteral("PlayNow"), {}, -1, -1});
        QCOMPARE(now.size(), 2);
        QCOMPARE(queued.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestRemoteCommands)
#include "test_remote_commands.moc"
