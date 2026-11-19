// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The download index: the private atomic file, round trips, records dropped alone, later and damaged files kept aside, the caps (a full queue refuses a new record and keeps the completed ones) and the debounced write
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
        // Realistic records leave the record cap as the one that binds
        QCOMPARE(accepted, DownloadStore::MaxRecords);
        QCOMPARE(store.records().size(), accepted);
        QVERIFY(QFileInfo(path).size() <= DownloadStore::MaxFileBytes);

        DownloadStore reloaded;
        reloaded.setStoragePath(path);
        QCOMPARE(reloaded.records().size(), accepted);
    }

    void refusesANewRecordAtTheCapAndKeepsTheCompletedOnes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        DownloadStore store;
        store.setStoragePath(path);
        for (int index = 0; index < DownloadStore::MaxRecords; ++index) {
            const QString state =
                index % 2 == 0 ? QStringLiteral("completed") : QStringLiteral("exported");
            QVERIFY(store.upsert(record(index, state), false));
        }
        QCOMPARE(store.records().size(), DownloadStore::MaxRecords);

        QVERIFY(!store.upsert(record(DownloadStore::MaxRecords), false));
        QCOMPARE(store.records().size(), DownloadStore::MaxRecords);
        QVERIFY(!store.find(kServer, kUser, hex(DownloadStore::MaxRecords)));
        for (int index = 0; index < DownloadStore::MaxRecords; ++index)
            QVERIFY(store.find(kServer, kUser, hex(index)));

        // An existing record is still updated, and a removal makes room again
        DownloadRecord renamed = record(0, QStringLiteral("completed"));
        renamed.name = QStringLiteral("Renamed");
        QVERIFY(store.upsert(renamed, false));
        QCOMPARE(store.find(kServer, kUser, hex(0))->name, QStringLiteral("Renamed"));
        QVERIFY(store.remove(kServer, kUser, hex(1), false));
        QVERIFY(store.upsert(record(DownloadStore::MaxRecords), false));
        QCOMPARE(store.records().size(), DownloadStore::MaxRecords);
        store.flush();

        DownloadStore reloaded;
        reloaded.setStoragePath(path);
        QCOMPARE(reloaded.records().size(), DownloadStore::MaxRecords);
        QVERIFY(reloaded.find(kServer, kUser, hex(0)));
        QVERIFY(!reloaded.find(kServer, kUser, hex(1)));
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
        // A backslash doubles in JSON: names, tag and ETag at their caps make the largest valid record, so the byte cap is reached before the record cap
        const auto heavy = [](DownloadRecord entry) {
            entry.name = QString(DownloadRecord::MaxName, QLatin1Char('\\'));
            entry.seriesName = QString(DownloadRecord::MaxName, QLatin1Char('\\'));
            entry.etag = QString(DownloadRecord::MaxEtag, QLatin1Char('\\'));
            entry.primaryTag = QString(DownloadRecord::MaxTag, QLatin1Char('\\'));
            return entry;
        };
        int index = 4;
        while (store.upsert(heavy(record(index)), false))
            ++index;
        const int filled = store.records().size();
        QVERIFY(filled > 10);
        QVERIFY(filled < DownloadStore::MaxRecords);

        bool refused = false;
        for (const DownloadRecord &entry : std::as_const(shorts)) {
            DownloadRecord grown = heavy(entry);
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

    // Progress arrives faster than the debounce interval: the write must still happen while it keeps coming
    void savesWhileUpdatesKeepArriving()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        DownloadStore store;
        store.setStoragePath(path);
        int step = 0;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                     DownloadRecord entry = record(1, QStringLiteral("fetching"));
                                     entry.bytesWritten = ++step;
                                     if (!store.upsert(entry, false))
                                         return false;
                                     const QJsonValue records =
                                         fileObject(path).value(QStringLiteral("records"));
                                     const QJsonObject first = records.toArray().at(0).toObject();
                                     return first.value(QStringLiteral("bytesWritten")).toInt() > 0;
                                 })(),
                                 5000);
    }

    void dropsARecordWithAnOutOfRangeNumber_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<double>("value");
        QTest::newRow("position 1e300") << QStringLiteral("positionTicks") << 1e300;
        QTest::newRow("position negative") << QStringLiteral("positionTicks") << -1.0;
        QTest::newRow("size 1e300") << QStringLiteral("size") << 1e300;
        QTest::newRow("bytes written 1e300") << QStringLiteral("bytesWritten") << 1e300;
        QTest::newRow("added at 1e300") << QStringLiteral("addedAt") << 1e300;
    }

    void dropsARecordWithAnOutOfRangeNumber()
    {
        QFETCH(QString, field);
        QFETCH(double, value);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.json"));
        DownloadStore store;
        store.setStoragePath(path);
        QVERIFY(store.upsert(record(1)));
        QVERIFY(store.upsert(record(2)));
        store.flush();

        QJsonObject root = fileObject(path);
        QJsonArray records = root.value(QStringLiteral("records")).toArray();
        QCOMPARE(records.size(), 2);
        QJsonObject damaged = records.at(0).toObject();
        damaged.insert(field, value);
        records.replace(0, damaged);
        root.insert(QStringLiteral("records"), records);
        QVERIFY(writeObject(path, root));

        DownloadStore reloaded;
        reloaded.setStoragePath(path);
        QCOMPARE(reloaded.records().size(), 1);
        QCOMPARE(reloaded.records().constFirst().itemId, hex(2));
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
