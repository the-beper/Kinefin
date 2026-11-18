// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The watched-state outbox: one event per item, atomic round trip, a damaged file kept aside
#include "downloads/WatchedOutbox.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Kinefin::WatchedEvent;
using Kinefin::WatchedOutbox;

namespace {
const QString kServer = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
const QString kUser = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

QString hex(int index)
{
    return QStringLiteral("%1").arg(index, 32, 16, QLatin1Char('0'));
}

WatchedEvent watchedEvent(int index, const QString &eventId = {})
{
    WatchedEvent result;
    result.serverId = kServer;
    result.userId = kUser;
    result.itemId = hex(index);
    result.eventId = eventId.isEmpty() ? QStringLiteral("event-%1").arg(index) : eventId;
    result.createdAt = index;
    return result;
}
} // namespace

class TestWatchedOutbox : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void roundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("outbox.json"));
        WatchedOutbox outbox;
        outbox.setStoragePath(path);
        QVERIFY(outbox.append(watchedEvent(1)));
        QVERIFY(outbox.append(watchedEvent(2)));
        QCOMPARE(outbox.events().size(), 2);
        QVERIFY(QFile::exists(path));

        WatchedOutbox reloaded;
        reloaded.setStoragePath(path);
        QCOMPARE(reloaded.events().size(), 2);
        const std::optional<WatchedEvent> found = reloaded.find(kServer, kUser, hex(1));
        QVERIFY(found);
        QCOMPARE(found->eventId, QStringLiteral("event-1"));
        QVERIFY(reloaded.remove(kServer, kUser, hex(1)));
        QCOMPARE(reloaded.events().size(), 1);
        QVERIFY(!reloaded.find(kServer, kUser, hex(1)));
    }

    void replacesTheEventOfTheSameItem()
    {
        WatchedOutbox outbox;
        QVERIFY(outbox.append(watchedEvent(1)));
        QVERIFY(outbox.append(watchedEvent(1, QStringLiteral("newer"))));
        QCOMPARE(outbox.events().size(), 1);
        QCOMPARE(outbox.events().constFirst().eventId, QStringLiteral("newer"));
    }

    void keepsADamagedFileAside()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("outbox.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("not json") > 0);
        file.close();
        WatchedOutbox outbox;
        outbox.setStoragePath(path);
        QVERIFY(outbox.events().isEmpty());
        QVERIFY(QFile::exists(path + QStringLiteral(".broken")));
    }
};

QTEST_GUILESS_MAIN(TestWatchedOutbox)
#include "test_watched_outbox.moc"
