// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The download index: the private atomic file, round trips, records dropped alone, later and damaged files kept aside, the caps and the eviction order
#include "downloads/DownloadStore.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <sys/stat.h>

#include <utility>

using Kinefin::DownloadRecord;
using Kinefin::DownloadStore;

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
    result.type = QStringLiteral("Movie");
    result.container = QStringLiteral("mkv");
    result.etag = QStringLiteral("\"etag-%1\"").arg(index);
    result.primaryTag = QStringLiteral("tag%1").arg(index);
    result.state = state;
    result.size = 4096;
    result.bytesWritten = state == QLatin1String("completed") ? 4096 : 0;
    result.addedAt = index;
    result.updatedAt = index;
    return result;
}

QJsonObject fileObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeObject(const QString &path, const QJsonObject &object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0;
}
} // namespace

class TestDownloadStore : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void roundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("downloads/index.json"));

        DownloadStore store;
        store.setStoragePath(path);
        QVERIFY(store.upsert(record(1, QStringLiteral("completed"))));
        QCOMPARE(store.records().size(), 1);

        struct stat info{};
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &info), 0);
        QCOMPARE(int(info.st_mode & 0777), 0600);

        DownloadStore reloaded;
        reloaded.setStoragePath(path);
        QCOMPARE(reloaded.records().size(), 1);
        const std::optional<DownloadRecord> found = reloaded.find(kServer, kUser, hex(1));
        QVERIFY(found);
        QCOMPARE(found->name, QStringLiteral("Item 1"));
        QCOMPARE(found->state, QStringLiteral("completed"));
        QCOMPARE(found->size, 4096);
        QCOMPARE(found->bytesWritten, 4096);
        QCOMPARE(found->etag, QStringLiteral("\"etag-1\""));
        QCOMPARE(found->primaryTag, QStringLiteral("tag1"));
    }

    void replacesTheSameItemWhateverTheCase()
    {
        DownloadStore store;
        DownloadRecord first = record(1);
        first.itemId = hex(1).toUpper();
        QVERIFY(store.upsert(first));
        DownloadRecord second = record(1);
        second.name = QStringLiteral("Renamed");
        second.state = QStringLiteral("paused");
        QVERIFY(store.upsert(second));
        QCOMPARE(store.records().size(), 1);
        const std::optional<DownloadRecord> found = store.find(kServer, kUser, hex(1));
        QVERIFY(found);
        QCOMPARE(found->name, QStringLiteral("Renamed"));
        QCOMPARE(found->state, QStringLiteral("paused"));
        QCOMPARE(found->itemId, hex(1));
    }

    void dropsOnlyTheInvalidRecord()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        DownloadStore store;
        store.setStoragePath(path);
        QVERIFY(store.upsert(record(1)));
        store.flush();

        QJsonObject root = fileObject(path);
        QVERIFY(!root.isEmpty());
        QJsonArray records = root.value(QStringLiteral("records")).toArray();
        QCOMPARE(records.size(), 1);
        QJsonObject invalid = records.at(0).toObject();
        invalid.insert(QStringLiteral("itemId"), hex(2));
        invalid.insert(QStringLiteral("state"), QStringLiteral("unknown"));
        records.append(invalid);
        root.insert(QStringLiteral("records"), records);
        QVERIFY(writeObject(path, root));

        DownloadStore reloaded;
        reloaded.setStoragePath(path);
        QCOMPARE(reloaded.records().size(), 1);
        QCOMPARE(reloaded.records().constFirst().itemId, hex(1));
    }

    void keepsLaterVersionAside()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        DownloadStore store;
        store.setStoragePath(path);
        QVERIFY(store.upsert(record(1)));
        store.flush();

        QJsonObject root = fileObject(path);
        root.insert(QStringLiteral("version"), DownloadStore::FileVersion + 1);
        QVERIFY(writeObject(path, root));

        DownloadStore reloaded;
        reloaded.setStoragePath(path);
        QVERIFY(reloaded.records().isEmpty());
        QVERIFY(!QFile::exists(path));
        QVERIFY(QFile::exists(path + QStringLiteral(".broken")));
    }

    void keepsDamagedFileAside()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QByteArray(DownloadStore::MaxFileBytes + 1, 'x')) > 0);
        file.close();

        DownloadStore store;
        store.setStoragePath(path);
        QVERIFY(store.records().isEmpty());
        QVERIFY(QFile::exists(path + QStringLiteral(".broken")));
    }

    void capsTheFileAndTheRecords()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        DownloadStore store;
        store.setStoragePath(path);
        int accepted = 0;
        for (int index = 0; index < DownloadStore::MaxRecords + 20; ++index) {
            DownloadRecord entry = record(index);
            entry.name = QString(200, QLatin1Char('n'));
            if (store.upsert(entry, false))
                ++accepted;
        }
        store.flush();
        QCOMPARE(store.records().size(), accepted);
        QVERIFY(accepted < DownloadStore::MaxRecords + 20);
        QVERIFY(QFileInfo(path).size() <= DownloadStore::MaxFileBytes);

        DownloadStore reloaded;
        reloaded.setStoragePath(path);
        QCOMPARE(reloaded.records().size(), accepted);
    }

    void evictsCompletedBeforeQueued()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        DownloadStore store;
        store.setStoragePath(path);
        DownloadRecord completed = record(0, QStringLiteral("completed"));
        // Larger than a queued entry, so freeing it always makes room for the next one
        completed.name = QString(DownloadRecord::MaxName, QLatin1Char('c'));
        QVERIFY(store.upsert(completed, false));
        int queued = 0;
        for (int index = 1; index <= DownloadStore::MaxRecords; ++index) {
            DownloadRecord entry = record(index);
            entry.name = QString(200, QLatin1Char('q'));
            if (store.upsert(entry, false))
                ++queued;
        }
        QVERIFY(!store.find(kServer, kUser, hex(0)));
        QCOMPARE(store.records().size(), queued);
        store.flush();
        QVERIFY(QFileInfo(path).size() <= DownloadStore::MaxFileBytes);
    }

    void refusesAnUpdateThatDoesNotFit()
    {
        DownloadStore store;
        QList<DownloadRecord> shorts;
        for (int index = 1; index <= 3; ++index) {
            DownloadRecord entry = record(index);
            entry.name = QStringLiteral("x");
            QVERIFY(store.upsert(entry, false));
            shorts.append(entry);
        }
        // The byte cap is reached well before the record cap, and a refused insert leaves less room than one short record
        int index = 4;
        while (store.upsert(record(index), false))
            ++index;
        const int filled = store.records().size();
        // A backslash doubles in JSON, so growing a short name overflows even the little room left
        const QString longName(DownloadRecord::MaxName, QLatin1Char('\\'));
        QVERIFY(filled > 10);
        QVERIFY(filled < DownloadStore::MaxRecords);

        bool refused = false;
        for (const DownloadRecord &entry : std::as_const(shorts)) {
            DownloadRecord grown = entry;
            grown.name = longName;
            if (store.upsert(grown, false))
                continue;
            refused = true;
            const std::optional<DownloadRecord> kept = store.find(kServer, kUser, entry.itemId);
            QVERIFY(kept);
            QCOMPARE(kept->name, QStringLiteral("x"));
            break;
        }
        QVERIFY(refused);
        QCOMPARE(store.records().size(), filled);
    }

    void removeAndFlush()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        DownloadStore store;
        store.setStoragePath(path);
        QVERIFY(store.upsert(record(1), false));
        QVERIFY(store.remove(kServer, kUser, hex(1), true));
        QVERIFY(store.records().isEmpty());

        DownloadStore reloaded;
        reloaded.setStoragePath(path);
        QVERIFY(reloaded.records().isEmpty());
    }

    void reportsAFailedWrite()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile blocker(dir.filePath(QStringLiteral("blocked")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("x");
        blocker.close();

        DownloadStore store;
        store.setStoragePath(dir.filePath(QStringLiteral("blocked/index.json")));
        QSignalSpy failed(&store, &DownloadStore::saveFailed);
        QVERIFY(store.upsert(record(1)));
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(store.records().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestDownloadStore)
#include "test_download_store.moc"
