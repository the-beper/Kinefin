// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The download queue: parallel starts, persistence, pause and resume, cancel/remove, offline, the metered rule, automatic retries, the container fallback and the restart of a record left fetching
#include "FakeJellyfin.h"
#include "downloads/DownloadManager.h"

#include <QFile>
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
