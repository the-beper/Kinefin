// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QFile>
#include <QJsonDocument>
#include <QtTest>

#include "api/PlaybackReport.h"

using Kinefin::PlaybackReport;
using Kinefin::playbackReportBody;
using Kinefin::PlaybackSource;

namespace {
QJsonObject golden(const QString &name)
{
    QFile file(QStringLiteral(KINEFIN_SOURCE_DIR "/tests/fixtures/") + name
               + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

PlaybackReport report(PlaybackReport::Kind kind)
{
    PlaybackReport value;
    value.kind = kind;
    value.itemId = QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
    value.mediaSourceId = QStringLiteral("c1b2a3d4e5f6071829304a5b6c7d8e9f");
    value.playSessionId = QStringLiteral("b6c7d8e9f0a1b2c3");
    return value;
}
} // namespace

class TestPlaybackReport : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void startMatchesGolden()
    {
        PlaybackReport value = report(PlaybackReport::Kind::Start);
        value.positionTicks = 600000000;
        const auto body = playbackReportBody(value);
        QVERIFY(body);
        QVERIFY(!golden(QStringLiteral("playback_start_request")).isEmpty());
        QCOMPARE(*body, golden(QStringLiteral("playback_start_request")));
    }

    void progressPausedTranscode()
    {
        PlaybackReport value = report(PlaybackReport::Kind::Progress);
        value.method = PlaybackSource::Method::Transcode;
        value.isPaused = true;
        value.positionTicks = 123456789;
        const auto body = playbackReportBody(value);
        QVERIFY(body);
        QCOMPARE(*body, golden(QStringLiteral("playback_progress_request")));
        // The same report, playing and direct, differs only in those two fields
        value.method = PlaybackSource::Method::Direct;
        value.isPaused = false;
        QCOMPARE(playbackReportBody(value)->value(QStringLiteral("PlayMethod")).toString(),
                 QStringLiteral("DirectPlay"));
        QCOMPARE(playbackReportBody(value)->value(QStringLiteral("IsPaused")).toBool(), false);
    }

    void stoppedAlwaysCarriesPosition()
    {
        PlaybackReport value = report(PlaybackReport::Kind::Stopped);
        value.positionTicks = 100540000000;
        const auto body = playbackReportBody(value);
        QVERIFY(body);
        QCOMPARE(*body, golden(QStringLiteral("playback_stopped_request")));
        // Position zero is still a position: only a missing key would mark the item as played
        value.positionTicks = 0;
        value.failed = true;
        const auto failed = playbackReportBody(value);
        QVERIFY(failed->contains(QStringLiteral("PositionTicks")));
        QCOMPARE(failed->value(QStringLiteral("PositionTicks")).toVariant().toLongLong(), 0);
        QCOMPARE(failed->value(QStringLiteral("Failed")).toBool(), true);
    }

    void omitsFieldsTheSliceCannotFill()
    {
        for (const auto kind : {PlaybackReport::Kind::Start, PlaybackReport::Kind::Progress,
                                PlaybackReport::Kind::Stopped}) {
            const auto body = playbackReportBody(report(kind));
            QVERIFY(body);
            for (const QString &key :
                 {QStringLiteral("Item"), QStringLiteral("SessionId"),
                  QStringLiteral("AudioStreamIndex"), QStringLiteral("SubtitleStreamIndex"),
                  QStringLiteral("IsMuted"), QStringLiteral("VolumeLevel"),
                  QStringLiteral("RepeatMode"), QStringLiteral("PlaybackOrder"),
                  QStringLiteral("NowPlayingQueue"), QStringLiteral("LiveStreamId")}) {
                QVERIFY2(!body->contains(key), qPrintable(key));
            }
        }
        // Stopped has no pause, seek or method fields
        const auto stopped = playbackReportBody(report(PlaybackReport::Kind::Stopped));
        for (const QString &key :
             {QStringLiteral("IsPaused"), QStringLiteral("CanSeek"), QStringLiteral("PlayMethod")})
            QVERIFY2(!stopped->contains(key), qPrintable(key));
    }

    void rejectsInvalidReports()
    {
        for (const auto kind : {PlaybackReport::Kind::Start, PlaybackReport::Kind::Progress,
                                PlaybackReport::Kind::Stopped}) {
            PlaybackReport value = report(kind);
            value.positionTicks = -1;
            QVERIFY(!playbackReportBody(value));
            value = report(kind);
            value.itemId = QStringLiteral("a/b");
            QVERIFY(!playbackReportBody(value));
            value = report(kind);
            value.mediaSourceId.clear();
            QVERIFY(!playbackReportBody(value));
            value = report(kind);
            value.playSessionId = QStringLiteral("x y");
            QVERIFY(!playbackReportBody(value));
        }
    }

    void capabilitiesMatchGolden()
    {
        QVERIFY(!golden(QStringLiteral("client_capabilities_request")).isEmpty());
        QCOMPARE(Kinefin::clientCapabilitiesBody(),
                 golden(QStringLiteral("client_capabilities_request")));
        QVERIFY(!Kinefin::clientCapabilitiesBody().contains(QStringLiteral("DeviceProfile")));
    }
};
QTEST_APPLESS_MAIN(TestPlaybackReport)
#include "test_playback_report.moc"
