// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The rows of the Downloads page: the active queue in order, completed items after it, the progress and the counts the footer shows
#include "downloads/DownloadManager.h"
#include "models/DownloadsModel.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Kinefin::DownloadManager;
using Kinefin::DownloadRecord;
using Kinefin::DownloadsModel;

namespace {
const QString kServer = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
const QString kUser = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

QString hex(int index)
{
    return QStringLiteral("%1").arg(index, 32, 16, QLatin1Char('0'));
}

DownloadRecord record(int index, const QString &state = QStringLiteral("queued"))
{
    DownloadRecord result;
    result.serverId = kServer;
    result.userId = kUser;
    result.itemId = hex(index);
    result.name = QStringLiteral("Item %1").arg(index);
    result.type = index % 2 == 0 ? QStringLiteral("Episode") : QStringLiteral("Movie");
    result.seriesName = index % 2 == 0 ? QStringLiteral("Series") : QString();
    result.seasonNumber = index % 2 == 0 ? 2 : -1;
    result.episodeNumber = index % 2 == 0 ? index : -1;
    result.container = QStringLiteral("mkv");
    result.primaryTag = QStringLiteral("tag%1").arg(index);
    result.size = 4096;
    result.bytesWritten = state == QLatin1String("completed") ? 4096 : 0;
    result.state = state;
    result.addedAt = index;
    result.updatedAt = index;
    return result;
}
} // namespace

class TestDownloadsModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void mirrorsTheQueue()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        DownloadsModel model(&manager);
        QSignalSpy counts(&model, &DownloadsModel::countsChanged);
        // The page's grids rebuild only the cards that change: rows come, go and move, the model never resets
        QSignalSpy resets(&model, &QAbstractItemModel::modelAboutToBeReset);

        QVERIFY(manager.enqueue(record(1)));
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.activeCount(), 1);
        QCOMPARE(model.completedCount(), 0);
        QCOMPARE(model.data(model.index(0), DownloadsModel::NameRole).toString(),
                 QStringLiteral("Item 1"));
        QCOMPARE(model.data(model.index(0), DownloadsModel::StateRole).toString(),
                 QStringLiteral("waiting"));
        QVERIFY(counts.count() >= 1);

        DownloadRecord done = record(2, QStringLiteral("completed"));
        done.runtimeTicks = 36'000'000'000;
        done.played = true;
        done.addedAt = 1'700'000'000'000;
        QVERIFY(manager.store()->upsert(done, false));
        model.refresh();
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.activeCount(), 1);
        QCOMPARE(model.completedCount(), 1);
        QCOMPARE(model.storedBytes(), 4096);
        // Active rows first, completed ones after them
        QCOMPARE(model.data(model.index(0), DownloadsModel::SectionRole).toString(),
                 QStringLiteral("active"));
        QCOMPARE(model.data(model.index(1), DownloadsModel::SectionRole).toString(),
                 QStringLiteral("completed"));
        QCOMPARE(model.data(model.index(1), DownloadsModel::ItemIdRole).toString(), hex(2));
        QCOMPARE(model.data(model.index(1), DownloadsModel::TypeRole).toString(),
                 QStringLiteral("Episode"));
        QCOMPARE(model.data(model.index(1), DownloadsModel::SeriesNameRole).toString(),
                 QStringLiteral("Series"));
        QCOMPARE(model.data(model.index(1), DownloadsModel::SeasonNumberRole).toInt(), 2);
        QCOMPARE(model.data(model.index(1), DownloadsModel::EpisodeNumberRole).toInt(), 2);
        QCOMPARE(model.data(model.index(1), DownloadsModel::ProgressRole).toDouble(), 1.0);
        QCOMPARE(model.data(model.index(1), DownloadsModel::PrimaryTagRole).toString(),
                 QStringLiteral("tag2"));
        // The running time, whether it was watched and when it was queued, for the card of the page
        QCOMPARE(model.data(model.index(1), DownloadsModel::RuntimeTicksRole).toLongLong(),
                 36'000'000'000);
        QVERIFY(model.data(model.index(1), DownloadsModel::PlayedRole).toBool());
        QCOMPARE(model.data(model.index(1), DownloadsModel::AddedAtRole).toLongLong(),
                 1'700'000'000'000);
        QVERIFY(!model.data(model.index(0), DownloadsModel::PlayedRole).toBool());
        // Neither a waiting nor a completed row has a speed
        QCOMPARE(model.data(model.index(0), DownloadsModel::BytesPerSecondRole).toLongLong(), 0);
        QCOMPARE(model.data(model.index(1), DownloadsModel::BytesPerSecondRole).toLongLong(), 0);
        // The waiting row completes and moves to the completed section, and the model still never reset
        DownloadRecord finished = record(1, QStringLiteral("completed"));
        QVERIFY(manager.store()->upsert(finished, false));
        model.refresh();
        QCOMPARE(model.activeCount(), 0);
        QCOMPARE(model.completedCount(), 2);
        QCOMPARE(resets.count(), 0);
    }

    void reportsProgress()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        DownloadsModel model(&manager);
        DownloadRecord downloading = record(1);
        downloading.state = QStringLiteral("fetching");
        downloading.bytesWritten = 1024;
        QVERIFY(manager.store()->upsert(downloading, false));
        model.refresh();
        QCOMPARE(model.data(model.index(0), DownloadsModel::ProgressRole).toDouble(), 0.25);
        QCOMPARE(model.data(model.index(0), DownloadsModel::BytesWrittenRole).toLongLong(), 1024);
        QCOMPARE(model.data(model.index(0), DownloadsModel::SizeRole).toLongLong(), 4096);
        // No transfer runs for it, so there is no speed to show
        QCOMPARE(model.data(model.index(0), DownloadsModel::BytesPerSecondRole).toLongLong(), 0);
    }
};

QTEST_GUILESS_MAIN(TestDownloadsModel)
#include "test_downloads_model.moc"
