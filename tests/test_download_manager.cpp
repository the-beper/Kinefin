// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The download queue: parallel starts, persistence, pause and resume, cancel/remove (a removal outranks a gate change and drops the whole item folder), record updates, offline, the metered rule, automatic retries, the container fallback and the restart of a record left fetching
#include "FakeJellyfin.h"
#include "downloads/DownloadManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Kinefin::DownloadManager;
using Kinefin::DownloadRecord;
using Kinefin::DownloadSource;
using Kinefin::DownloadStore;

namespace {
const QString kServer = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
const QString kUser = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
const QString kRule = QStringLiteral("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
const QByteArray kAuth = QByteArrayLiteral(
    "MediaBrowser Client=\"Kinefin\", Token=\"f2f4b7c1ee8a4d0bb2a5f9d1f9e0e2ab\"");

QString hex(int index)
{
    return QStringLiteral("%1").arg(index, 32, 16, QLatin1Char('0'));
}

DownloadRecord queued(int index, const QString &ruleId = {})
{
    DownloadRecord result;
    result.serverId = kServer;
    result.userId = kUser;
    result.itemId = hex(index);
    result.name = QStringLiteral("Item %1").arg(index);
    result.type = QStringLiteral("Movie");
    result.container = QStringLiteral("mkv");
    result.etag = QStringLiteral("\"etag-1\"");
    result.size = 64 * 1024;
    result.addedAt = index;
    result.updatedAt = index;
    result.ruleId = ruleId;
    return result;
}

DownloadSource sourceFor(const FakeJellyfin &server, const DownloadRecord &record)
{
    DownloadSource source;
    source.url = server.url(QStringLiteral("/Items/%1/Download").arg(record.itemId));
    source.authorization = kAuth;
    return source;
}

QByteArray contents(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QString itemDirectory(const QTemporaryDir &dir, int index)
{
    return dir.filePath(kServer + QLatin1Char('/') + kUser + QLatin1Char('/') + hex(index));
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
} // namespace

class TestDownloadManager : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void initTestCase() { qRegisterMetaType<Kinefin::DownloadRecord>(); }

    void downloadsAndPersists()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        QVERIFY(manager.enqueue(queued(1)));
        QVERIFY(manager.enqueue(queued(2)));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 2, 10000);

        for (int index = 1; index <= 2; ++index) {
            const std::optional<DownloadRecord> found =
                manager.store()->find(kServer, kUser, hex(index));
            QVERIFY(found);
            QCOMPARE(found->state, QStringLiteral("completed"));
            QCOMPARE(found->bytesWritten, 64 * 1024);
            const QString path = manager.localPath(*found);
            QVERIFY(!path.isEmpty());
            QCOMPARE(contents(path), server.downloadBodyFor(hex(index).toLatin1()));
        }
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("index.json"))));

        DownloadStore reloaded;
        reloaded.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        QCOMPARE(reloaded.records().size(), 2);
        QCOMPARE(reloaded.records().constFirst().state, QStringLiteral("completed"));
    }

    void honoursTheParallelLimit()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setParallelDownloads(1);
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        QVERIFY(manager.enqueue(queued(1)));
        QVERIFY(manager.enqueue(queued(2)));
        QVERIFY(manager.enqueue(queued(3)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        QTest::qWait(300);
        QCOMPARE(server.downloadRequests, 1);

        server.releaseDownloads();
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 2, 5000);
        QTest::qWait(300);
        QCOMPARE(server.downloadRequests, 2);
        server.releaseDownloads();
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 3, 5000);
        server.releaseDownloads();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 3, 10000);
    }

    void pausesAndResumes()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        QVERIFY(manager.enqueue(queued(1)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        QVERIFY(manager.pause(kServer, kUser, hex(1)));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.store()->find(kServer, kUser, hex(1))->wanted, 5000);
        QCOMPARE(manager.store()->find(kServer, kUser, hex(1))->state, QStringLiteral("paused"));

        server.holdDownload = false;
        QVERIFY(manager.retry(kServer, kUser, hex(1)));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        const std::optional<DownloadRecord> found = manager.store()->find(kServer, kUser, hex(1));
        QVERIFY(found);
        QCOMPARE(found->state, QStringLiteral("completed"));
        QCOMPARE(contents(manager.localPath(*found)), server.downloadBodyFor(hex(1).toLatin1()));
    }

    void removeDeletesTheFilesAndTheRecord()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QVERIFY(manager.enqueue(queued(1)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        QVERIFY(manager.remove(kServer, kUser, hex(1)));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.store()->find(kServer, kUser, hex(1)), 5000);
        const QString relative = kServer + QLatin1Char('/') + kUser + QLatin1Char('/');
        QVERIFY(!QFile::exists(dir.filePath(relative + hex(1) + QStringLiteral("/media.mkv"))));
        QVERIFY(!QFile::exists(dir.filePath(relative + hex(1) + QStringLiteral("/media.part"))));
    }

    void removeDropsTheWholeItemFolder()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        QVERIFY(writeFile(outside.filePath(QStringLiteral("keep.txt")), "keep"));
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        QVERIFY(manager.enqueue(queued(1)));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);

        const QString item = itemDirectory(dir, 1);
        QVERIFY(QDir().mkpath(item + QStringLiteral("/subs")));
        QVERIFY(writeFile(item + QStringLiteral("/poster.jpg"), "poster"));
        QVERIFY(writeFile(item + QStringLiteral("/info.json"), "{}"));
        QVERIFY(writeFile(item + QStringLiteral("/subs/0.srt"), "sub"));
        // A file left by an earlier container, and a link out of the folder that must not be followed
        QVERIFY(writeFile(item + QStringLiteral("/media.avi"), "old"));
        QVERIFY(QFile::link(outside.path(), item + QStringLiteral("/escape")));

        QVERIFY(manager.remove(kServer, kUser, hex(1)));
        QVERIFY(!manager.store()->find(kServer, kUser, hex(1)));
        QVERIFY(!QFileInfo::exists(item));
        QVERIFY(QFile::exists(outside.filePath(QStringLiteral("keep.txt"))));
        // The emptied owner folders go too
        QVERIFY(!QFileInfo::exists(dir.filePath(kServer)));
    }

    void removeKeepsTheFolderOfAnotherItem()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setOnline(false);
        QVERIFY(manager.enqueue(queued(1)));
        QVERIFY(manager.enqueue(queued(2)));
        QVERIFY(QDir().mkpath(itemDirectory(dir, 1)));
        QVERIFY(QDir().mkpath(itemDirectory(dir, 2)));
        QVERIFY(writeFile(itemDirectory(dir, 1) + QStringLiteral("/poster.jpg"), "p"));
        QVERIFY(writeFile(itemDirectory(dir, 2) + QStringLiteral("/poster.jpg"), "p"));
        QVERIFY(manager.remove(kServer, kUser, hex(1)));
        QVERIFY(!QFileInfo::exists(itemDirectory(dir, 1)));
        QVERIFY(QFile::exists(itemDirectory(dir, 2) + QStringLiteral("/poster.jpg")));
    }

    void aRemovalSurvivesAGateChange_data()
    {
        QTest::addColumn<bool>("byMeter");
        QTest::newRow("offline") << false;
        QTest::newRow("metered") << true;
    }

    // The gate closing after remove() asks to stop the same transfer; the weaker reason must not turn the removal into a requeue
    void aRemovalSurvivesAGateChange()
    {
        QFETCH(bool, byMeter);
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setMetered(false);
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy removed(&manager, &DownloadManager::downloadRemoved);
        QVERIFY(manager.enqueue(queued(1, kRule)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        QVERIFY(manager.remove(kServer, kUser, hex(1)));
        if (byMeter)
            manager.setMetered(true);
        else
            manager.setOnline(false);
        QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 5000);
        QVERIFY(!manager.store()->find(kServer, kUser, hex(1)));
        QVERIFY(!QFileInfo::exists(itemDirectory(dir, 1)));

        // Opening the gate again must not bring the record back
        if (byMeter)
            manager.setMetered(false);
        else
            manager.setOnline(true);
        QVERIFY(!manager.store()->find(kServer, kUser, hex(1)));
        QCOMPARE(server.downloadRequests, 1);
    }

    void aRemovalSurvivesAPause()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy removed(&manager, &DownloadManager::downloadRemoved);
        QVERIFY(manager.enqueue(queued(1)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        QVERIFY(manager.remove(kServer, kUser, hex(1)));
        manager.pause(kServer, kUser, hex(1));
        QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 5000);
        QVERIFY(!manager.store()->find(kServer, kUser, hex(1)));
    }

    void completesAPendingRemovalWhenTheProfileChanges()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString first = dir.filePath(QStringLiteral("first/index.json"));
        const QString second = dir.filePath(QStringLiteral("second/index.json"));
        DownloadManager manager;
        manager.setStoragePath(first);
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QVERIFY(manager.enqueue(queued(1)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        QVERIFY(manager.remove(kServer, kUser, hex(1)));
        manager.setStoragePath(second);

        DownloadStore old;
        old.setStoragePath(first);
        QVERIFY(old.records().isEmpty());
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("first/") + kServer)));
        QVERIFY(manager.store()->records().isEmpty());
    }

    void updateRecordKeepsTheTransferStateOfARunningDownload()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QVERIFY(manager.enqueue(queued(1)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        const std::optional<DownloadRecord> running = manager.store()->find(kServer, kUser, hex(1));
        QVERIFY(running);
        QCOMPARE(running->state, QStringLiteral("fetching"));

        DownloadRecord edited = *running;
        edited.positionTicks = 123;
        edited.played = true;
        edited.state = QStringLiteral("exported");
        edited.size = 1;
        QSignalSpy changed(&manager, &DownloadManager::recordChanged);
        QVERIFY(manager.updateRecord(edited));
        QCOMPARE(changed.count(), 1);
        const std::optional<DownloadRecord> after = manager.store()->find(kServer, kUser, hex(1));
        QVERIFY(after);
        QCOMPARE(after->positionTicks, 123);
        QVERIFY(after->played);
        QCOMPARE(after->state, QStringLiteral("fetching"));
        QCOMPARE(after->size, 64 * 1024);
        QCOMPARE(changed.constFirst().at(0).value<DownloadRecord>().positionTicks, 123);

        QVERIFY(!manager.updateRecord(queued(9)));
    }

    void updateRecordStoresACompletedRecord()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        DownloadManager manager;
        manager.setStoragePath(path);
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        QVERIFY(manager.enqueue(queued(1)));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);

        DownloadRecord edited = *manager.store()->find(kServer, kUser, hex(1));
        edited.state = QStringLiteral("exported");
        edited.positionTicks = 5;
        QSignalSpy changed(&manager, &DownloadManager::recordChanged);
        QVERIFY(manager.updateRecord(edited));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.constFirst().at(0).value<DownloadRecord>().state,
                 QStringLiteral("exported"));

        DownloadStore reloaded;
        reloaded.setStoragePath(path);
        const std::optional<DownloadRecord> found = reloaded.find(kServer, kUser, hex(1));
        QVERIFY(found);
        QCOMPARE(found->state, QStringLiteral("exported"));
        QCOMPARE(found->positionTicks, 5);
    }

    void offlineHoldsTheQueue()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        manager.setOnline(false);
        QVERIFY(manager.enqueue(queued(1)));
        QTest::qWait(300);
        QCOMPARE(server.downloadRequests, 0);
        manager.setOnline(true);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
    }

    void meteredHoldsOnlyAutomaticDownloads()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        manager.setMetered(true);
        QVERIFY(manager.enqueue(queued(1, kRule)));
        QTest::qWait(300);
        QCOMPARE(server.downloadRequests, 0);
        QCOMPARE(manager.store()->find(kServer, kUser, hex(1))->state, QStringLiteral("waiting"));

        QVERIFY(manager.enqueue(queued(2)));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        QCOMPARE(manager.store()->find(kServer, kUser, hex(2))->state, QStringLiteral("completed"));
        QCOMPARE(server.downloadRequests, 1);

        manager.setMetered(false);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 2, 10000);
        QCOMPARE(manager.store()->find(kServer, kUser, hex(1))->state, QStringLiteral("completed"));
    }

    void requeuesARecordLeftFetching()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        {
            DownloadStore store;
            store.setStoragePath(path);
            DownloadRecord stale = queued(1);
            stale.state = QStringLiteral("fetching");
            QVERIFY(store.upsert(stale));
        }
        DownloadManager manager;
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        manager.setStoragePath(path);
        const std::optional<DownloadRecord> started = manager.store()->find(kServer, kUser, hex(1));
        QVERIFY(started);
        QVERIFY(started->state != QLatin1String("fetching"));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        QCOMPARE(manager.store()->find(kServer, kUser, hex(1))->state, QStringLiteral("completed"));
    }

    void pausesAnErroredRecord()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadStatus = 500;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QVERIFY(manager.enqueue(queued(1)));
        QTRY_COMPARE_WITH_TIMEOUT(manager.store()->find(kServer, kUser, hex(1))->state,
                                  QStringLiteral("error"), 5000);
        QVERIFY(manager.pause(kServer, kUser, hex(1)));
        const std::optional<DownloadRecord> paused = manager.store()->find(kServer, kUser, hex(1));
        QVERIFY(paused);
        QCOMPARE(paused->state, QStringLiteral("paused"));
        QVERIFY(paused->error.isEmpty());

        server.downloadStatus = 0;
        QVERIFY(manager.retry(kServer, kUser, hex(1)));
        QTRY_COMPARE_WITH_TIMEOUT(manager.store()->find(kServer, kUser, hex(1))->state,
                                  QStringLiteral("completed"), 10000);
    }

    void retriesAutomaticDownloads()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadStatus = 503;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setMetered(false);
        manager.setRetryDelays({300});
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        QVERIFY(manager.enqueue(queued(1, kRule)));
        QTRY_COMPARE_WITH_TIMEOUT(manager.store()->find(kServer, kUser, hex(1))->state,
                                  QStringLiteral("error"), 5000);
        QCOMPARE(server.downloadRequests, 1);

        server.downloadStatus = 0;
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        QVERIFY(server.downloadRequests >= 2);
        QCOMPARE(manager.store()->find(kServer, kUser, hex(1))->state, QStringLiteral("completed"));
    }

    void meteredStopsARunningAutomaticDownload()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setMetered(false);
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        QVERIFY(manager.enqueue(queued(1, kRule)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        manager.setMetered(true);
        QTRY_COMPARE_WITH_TIMEOUT(manager.store()->find(kServer, kUser, hex(1))->state,
                                  QStringLiteral("waiting"), 5000);
        QTest::qWait(200);
        QCOMPARE(server.downloadRequests, 1);

        server.holdDownload = false;
        manager.setMetered(false);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        QCOMPARE(manager.store()->find(kServer, kUser, hex(1))->state, QStringLiteral("completed"));
    }

    void offlineWhileRunningResumes()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        QVERIFY(manager.enqueue(queued(1)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        // The gate opens again before the cancelled transfer reports its failure
        manager.setOnline(false);
        manager.setOnline(true);
        server.holdDownload = false;
        server.releaseDownloads();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        QCOMPARE(manager.store()->find(kServer, kUser, hex(1))->state, QStringLiteral("completed"));
    }

    void keepsTheRecordEnqueuedAfterARemove()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        QVERIFY(manager.enqueue(queued(1)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        QVERIFY(manager.remove(kServer, kUser, hex(1)));
        QVERIFY(manager.enqueue(queued(1)));
        server.holdDownload = false;
        server.releaseDownloads();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        const std::optional<DownloadRecord> found = manager.store()->find(kServer, kUser, hex(1));
        QVERIFY(found);
        QCOMPARE(found->state, QStringLiteral("completed"));
    }

    void normalisesTheContainer()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        manager.setSourceResolver([&server](const DownloadRecord &record) {
            return std::optional<DownloadSource>(sourceFor(server, record));
        });
        QSignalSpy completed(&manager, &DownloadManager::downloadCompleted);
        DownloadRecord listed = queued(1);
        listed.container = QStringLiteral("MKV,webm");
        QVERIFY(manager.enqueue(listed));
        QCOMPARE(manager.store()->find(kServer, kUser, hex(1))->container, QStringLiteral("mkv"));

        DownloadRecord none = queued(2);
        none.container.clear();
        QVERIFY(manager.enqueue(none));
        QCOMPARE(manager.store()->find(kServer, kUser, hex(2))->container, QStringLiteral("bin"));

        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 2, 10000);
        const QString relative = kServer + QLatin1Char('/') + kUser + QLatin1Char('/');
        QVERIFY(QFile::exists(dir.filePath(relative + hex(1) + QStringLiteral("/media.mkv"))));
        QVERIFY(QFile::exists(dir.filePath(relative + hex(2) + QStringLiteral("/media.bin"))));
    }

    void holdsWithoutAResolver()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadManager manager;
        manager.setStoragePath(dir.filePath(QStringLiteral("index.json")));
        QVERIFY(manager.enqueue(queued(1)));
        QCOMPARE(manager.store()->find(kServer, kUser, hex(1))->state, QStringLiteral("waiting"));
    }
};

QTEST_GUILESS_MAIN(TestDownloadManager)
#include "test_download_manager.moc"
