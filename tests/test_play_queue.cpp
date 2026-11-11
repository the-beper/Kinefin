// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The play queue against the fake server: what each kind of item expands to, the order and the cap, edits, failures and the loss of the session
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/PlayQueue.h"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <sys/stat.h>

using Kinefin::PlayQueue;

namespace {
const QString kMovie = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");
const QString kSeason = QStringLiteral("11aa22bb33cc44dd55ee66ff778899aa");
const QString kSeries = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
const QString kBoxSet = QStringLiteral("bb11cc22dd33ee44ff5566778899aabb");

QJsonObject firstItem(const QString &fixtureName)
{
    return QJsonDocument::fromJson(fixture(fixtureName))
        .object()
        .value(QStringLiteral("Items"))
        .toArray()
        .first()
        .toObject();
}

QString episodeId(int number)
{
    return QStringLiteral("%1").arg(number, 32, 16, QLatin1Char('e'));
}

QByteArray envelope(const QJsonArray &items)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                     {QStringLiteral("TotalRecordCount"), int(items.size())}})
        .toJson(QJsonDocument::Compact);
}

struct Fixture {
    Fixture()
    {
        put(kMovie, QStringLiteral("movie_item"));
        put(kSeason, QStringLiteral("season_item"));
        put(kSeries, QStringLiteral("series_item"));
        put(kBoxSet, QStringLiteral("boxset_item"));
        ok = server.listen() && login(client, server);
        path = ownerPath(client.serverId(), client.userId());
        queue.setStoragePath(legacyPath);
        queue.setSaveDelay(std::chrono::milliseconds(20));
    }

    void put(const QString &id, const QString &fixtureName)
    {
        server.itemsById.insert(
            id, QJsonDocument(firstItem(fixtureName)).toJson(QJsonDocument::Compact));
    }
    QString name(int row) const
    {
        return queue.data(queue.index(row), PlayQueue::NameRole).toString();
    }
    QString id(int row) const
    {
        return queue.data(queue.index(row), PlayQueue::ItemIdRole).toString();
    }
    // Adds and waits for the outcome: true when rows joined
    bool addAndWait(const QString &itemId, PlayQueue::Placement placement)
    {
        QSignalSpy added(&queue, &PlayQueue::added);
        QSignalSpy failed(&queue, &PlayQueue::addFailed);
        if (!queue.add(itemId, placement))
            return false;
        return QTest::qWaitFor([&] { return added.size() + failed.size() > 0; }, 10000)
               && added.size() == 1;
    }

    // The file the queue of an owner is kept in
    QString ownerPath(const QString &serverId, const QString &userId) const
    {
        return dir.filePath(
            QStringLiteral("profile/queues/%1_%2.json").arg(serverId.toLower(), userId.toLower()));
    }
    // The queue becomes the signed-in user's: what it keeps is saved under this owner
    void own() { queue.restoreFor(client.serverId(), client.userId()); }
    QByteArray stored() const
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    void write(const QByteArray &bytes) const { writeAt(path, bytes); }
    void writeAt(const QString &target, const QByteArray &bytes) const
    {
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile file(target);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }
    static QByteArray readAt(const QString &target)
    {
        QFile file(target);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    QByteArray fileFor(const QStringList &ids, const QString &serverId, const QString &userId,
                       int version = 1) const
    {
        QJsonArray array;
        for (const QString &id : ids)
            array.append(id);
        return QJsonDocument(QJsonObject{{QStringLiteral("version"), version},
                                         {QStringLiteral("serverId"), serverId},
                                         {QStringLiteral("userId"), userId},
                                         {QStringLiteral("items"), array}})
            .toJson(QJsonDocument::Compact);
    }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    QTemporaryDir dir;
    // The queue of earlier versions, which the storage path names; `path` is the signed-in user's own file
    QString legacyPath = dir.filePath(QStringLiteral("profile/queue.json"));
    QString path;
    PlayQueue queue{&client};
    QAbstractItemModelTester tester{&queue};
    bool ok = false;
};
} // namespace

class TestPlayQueue : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void aMovieJoinsAtTheEndAndPlayNextGoesToTheTop()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QCOMPARE(f.queue.count(), 1);
        QCOMPARE(f.name(0), QStringLiteral("Arcturus (2012)"));
        f.server.generatedEpisodeTotal = 3;
        f.server.itemsById.insert(
            episodeId(1),
            QJsonDocument(QJsonObject{{QStringLiteral("Id"), episodeId(1)},
                                      {QStringLiteral("Name"), QStringLiteral("Second")},
                                      {QStringLiteral("Type"), QStringLiteral("Episode")}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(f.addAndWait(episodeId(1), PlayQueue::Next));
        QCOMPARE(f.queue.count(), 2);
        QCOMPARE(f.name(0), QStringLiteral("Second"));
    }

    void addingAQueuedItemMovesItToTheNewPlace()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.itemsById.insert(
            episodeId(2),
            QJsonDocument(QJsonObject{{QStringLiteral("Id"), episodeId(2)},
                                      {QStringLiteral("Name"), QStringLiteral("Other")},
                                      {QStringLiteral("Type"), QStringLiteral("Movie")}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QVERIFY(f.addAndWait(episodeId(2), PlayQueue::Last));
        QCOMPARE(f.id(0), kMovie);
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QCOMPARE(f.queue.count(), 2);
        QCOMPARE(f.id(1), kMovie);
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Next));
        QCOMPARE(f.id(0), kMovie);
        QCOMPARE(f.queue.count(), 2);
    }

    void aSeasonStartsAtItsFirstUnplayedEpisode()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 10;
        f.server.generatedEpisodesPlayed = 3;
        QVERIFY(f.addAndWait(kSeason, PlayQueue::Last));
        QCOMPARE(f.queue.count(), 7);
        QCOMPARE(f.name(0), QStringLiteral("Episode 4"));
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("Episodes")).constFirst());
        QCOMPARE(wire.queryItemValue(QStringLiteral("seasonId")), kSeason);
    }

    void aSeasonThatIsPlayedAddsAllOfIt()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 10;
        f.server.generatedEpisodesPlayed = 10;
        QVERIFY(f.addAndWait(kSeason, PlayQueue::Last));
        QCOMPARE(f.queue.count(), 10);
        QCOMPARE(f.name(0), QStringLiteral("Episode 1"));
    }

    void aSeriesStartsAtNextUpAcrossPages()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 250;
        f.server.listingRawBody.insert(
            QStringLiteral("NextUp"),
            envelope(
                QJsonArray{QJsonObject{{QStringLiteral("Id"), episodeId(120)},
                                       {QStringLiteral("Type"), QStringLiteral("Episode")},
                                       {QStringLiteral("Name"), QStringLiteral("Episode 120")}}}));
        QVERIFY(f.addAndWait(kSeries, PlayQueue::Last));
        QCOMPARE(f.queue.count(), 131);
        QCOMPARE(f.name(0), QStringLiteral("Episode 120"));
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Episodes")).size(), 3);
        const QUrlQuery next = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("NextUp")).constFirst());
        QCOMPARE(next.queryItemValue(QStringLiteral("seriesId")), kSeries);
        QCOMPARE(next.queryItemValue(QStringLiteral("limit")), QStringLiteral("1"));
    }

    void aSeriesWithoutNextUpStartsAtTheTop()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 5;
        f.server.listingRawBody.insert(QStringLiteral("NextUp"), envelope({}));
        QVERIFY(f.addAndWait(kSeries, PlayQueue::Last));
        QCOMPARE(f.queue.count(), 5);
        QCOMPARE(f.name(0), QStringLiteral("Episode 1"));
    }

    void aCollectionAddsItsPlayableChildren()
    {
        Fixture f;
        QVERIFY(f.ok);
        const auto child = [](const QString &id, const QString &type, const QString &name,
                              const QString &location = QString()) {
            QJsonObject object{{QStringLiteral("Id"), id},
                               {QStringLiteral("Type"), type},
                               {QStringLiteral("Name"), name}};
            if (!location.isEmpty())
                object.insert(QStringLiteral("LocationType"), location);
            return object;
        };
        f.server.listingRawBody.insert(
            QStringLiteral("Items"),
            envelope(QJsonArray{
                child(episodeId(1), QStringLiteral("Movie"), QStringLiteral("A")),
                child(episodeId(2), QStringLiteral("Series"), QStringLiteral("Not playable")),
                child(episodeId(3), QStringLiteral("Episode"), QStringLiteral("Virtual"),
                      QStringLiteral("Virtual")),
                child(episodeId(4), QStringLiteral("Video"), QStringLiteral("B"))}));
        QVERIFY(f.addAndWait(kBoxSet, PlayQueue::Last));
        QCOMPARE(f.queue.count(), 2);
        QCOMPARE(f.name(0), QStringLiteral("A"));
        QCOMPARE(f.name(1), QStringLiteral("B"));
    }

    void anUnsupportedTypeIsRefused()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.itemsById.insert(
            episodeId(9),
            QJsonDocument(QJsonObject{{QStringLiteral("Id"), episodeId(9)},
                                      {QStringLiteral("Type"), QStringLiteral("Person")},
                                      {QStringLiteral("Name"), QStringLiteral("Someone")}})
                .toJson(QJsonDocument::Compact));
        QSignalSpy nothing(&f.queue, &PlayQueue::nothingToAdd);
        QSignalSpy failed(&f.queue, &PlayQueue::addFailed);
        QVERIFY(f.queue.add(episodeId(9), PlayQueue::Last));
        QTRY_COMPARE(nothing.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(!f.queue.busy());
    }

    void anInvalidIdSendsNothingAndABusyQueueRefuses()
    {
        Fixture f;
        QVERIFY(f.ok);
        const int before = f.server.listingRequests;
        QVERIFY(!f.queue.add(QStringLiteral("bad id"), PlayQueue::Last));
        QVERIFY(!f.queue.add(QString(), PlayQueue::Last));
        QCOMPARE(f.server.listingRequests, before);
        f.server.holdListings = true;
        QVERIFY(f.queue.add(kMovie, PlayQueue::Last));
        QVERIFY(f.queue.busy());
        QVERIFY(!f.queue.add(kMovie, PlayQueue::Last));
        QSignalSpy added(&f.queue, &PlayQueue::added);
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTRY_COMPARE(added.size(), 1);
        QVERIFY(!f.queue.busy());
    }

    void aFailedExpansionAddsNothing()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        f.server.generatedEpisodeTotal = 250;
        f.server.listingStatus.insert(QStringLiteral("Episodes"), 500);
        QSignalSpy failed(&f.queue, &PlayQueue::addFailed);
        QVERIFY(f.queue.add(kSeason, PlayQueue::Last));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(f.queue.count(), 1);
        QVERIFY(!f.queue.busy());
    }

    void theCapTruncatesAnExpansion()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 700;
        QSignalSpy added(&f.queue, &PlayQueue::added);
        QVERIFY(f.queue.add(kSeason, PlayQueue::Last));
        QTRY_COMPARE(added.size(), 1);
        QCOMPARE(f.queue.count(), PlayQueue::MaxEntries);
        QCOMPARE(added.constFirst().at(0).toInt(), int(PlayQueue::MaxEntries));
        QVERIFY(added.constFirst().at(1).toBool());
    }

    void undoRestoresTheLastReorderOrRemovalOnlyOnce()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 5;
        QVERIFY(f.addAndWait(kSeason, PlayQueue::Last));
        const QStringList original = f.queue.itemIds();

        QVERIFY(f.queue.move(0, 3));
        QVERIFY(f.queue.canUndo());
        QVERIFY(!f.queue.move(0, 8)); // An invalid edit does not replace the snapshot.
        QVERIFY(f.queue.undo());
        QCOMPARE(f.queue.itemIds(), original);
        QVERIFY(!f.queue.canUndo());
        QVERIFY(!f.queue.undo()); // One level, without redo.

        QVERIFY(f.queue.remove(2));
        QCOMPARE(f.queue.count(), 4);
        QVERIFY(f.queue.canUndo());
        QVERIFY(!f.queue.remove(20));
        QVERIFY(f.queue.undo());
        QCOMPARE(f.queue.itemIds(), original);
        QCOMPARE(f.name(2), QStringLiteral("Episode 3"));
        QVERIFY(!f.queue.canUndo());
    }

    void undoRevertsAdditionsWithoutFetchingAgain()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        const int requests = f.server.listingRequests;
        QVERIFY(f.queue.canUndo());
        QVERIFY(f.queue.undo());
        QCOMPARE(f.queue.count(), 0);
        QCOMPARE(f.server.listingRequests, requests);
        QVERIFY(!f.queue.canUndo());

        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        f.server.generatedEpisodeTotal = 3;
        QVERIFY(f.addAndWait(kSeason, PlayQueue::Next));
        QCOMPARE(f.queue.count(), 4);
        QVERIFY(f.queue.undo());
        QCOMPARE(f.queue.itemIds(), QStringList{kMovie});
    }

    void undoRestoresClearAndPersistsTheRevertedQueue()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.own();
        f.server.generatedEpisodeTotal = 5;
        QVERIFY(f.addAndWait(kSeason, PlayQueue::Last));
        const QStringList original = f.queue.itemIds();
        QTRY_VERIFY(!f.stored().isEmpty());
        const int requests = f.server.listingRequests;

        f.queue.clear();
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(f.queue.canUndo());
        QTRY_VERIFY(!QFileInfo::exists(f.path));
        QVERIFY(f.queue.undo());
        QCOMPARE(f.queue.itemIds(), original);
        QCOMPARE(f.server.listingRequests, requests);
        QTRY_VERIFY(!f.stored().isEmpty());
        QCOMPARE(QJsonDocument::fromJson(f.stored())
                     .object()
                     .value(QStringLiteral("items"))
                     .toArray()
                     .size(),
                 int(original.size()));
        QVERIFY(!f.queue.canUndo());
    }

    void undoIsUnavailableWhileExpandingAndDoesNotSurviveAnOwnerChange()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.own();
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        f.server.generatedEpisodeTotal = 3;
        f.server.holdListings = true;
        QVERIFY(f.queue.add(kSeason, PlayQueue::Next));
        QVERIFY(f.queue.busy());
        QVERIFY(!f.queue.undo()); // The pending addition must not follow a rollback.
        QCOMPARE(f.queue.itemIds(), QStringList{kMovie});
        QSignalSpy added(&f.queue, &PlayQueue::added);
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTRY_COMPARE(added.size(), 1);
        QCOMPARE(f.queue.count(), 4);
        QVERIFY(f.queue.undo());
        QCOMPARE(f.queue.itemIds(), QStringList{kMovie});

        f.queue.clear();
        QVERIFY(f.queue.canUndo()); // Clear from nonempty is itself undoable.
        f.queue.restoreFor(f.client.serverId(), f.client.userId());
        QVERIFY(!f.queue.canUndo());
        QVERIFY(!f.queue.undo());
        QTRY_COMPARE(f.queue.count(), 0);

        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QVERIFY(f.queue.canUndo());
        f.client.disconnectFromServer();
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(!f.queue.canUndo());
        QVERIFY(!f.queue.undo());
    }

    void movingRemovingAndClearing()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 5;
        f.server.generatedEpisodesPlayed = 5;
        QVERIFY(f.addAndWait(kSeason, PlayQueue::Last));
        QCOMPARE(f.queue.count(), 5);
        QVERIFY(f.queue.move(0, 3));
        QCOMPARE(f.name(3), QStringLiteral("Episode 1"));
        QCOMPARE(f.name(0), QStringLiteral("Episode 2"));
        QVERIFY(f.queue.move(4, 0));
        QCOMPARE(f.name(0), QStringLiteral("Episode 5"));
        QVERIFY(!f.queue.move(-1, 2));
        QVERIFY(!f.queue.move(0, 5));
        QVERIFY(f.queue.remove(2));
        QVERIFY(!f.queue.remove(4));
        QVERIFY(!f.queue.remove(-1));
        QCOMPARE(f.queue.count(), 4);
        QSignalSpy counts(&f.queue, &PlayQueue::countChanged);
        f.queue.clear();
        QCOMPARE(f.queue.count(), 0);
        QCOMPARE(counts.size(), 1);
        f.queue.clear();
        QCOMPARE(counts.size(), 1);
    }

    void aLostSessionEmptiesTheQueueAndAbortsTheExpansion()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        f.server.generatedEpisodeTotal = 10;
        f.server.holdListings = true;
        QVERIFY(f.queue.add(kSeason, PlayQueue::Last));
        QVERIFY(f.queue.busy());
        QSignalSpy added(&f.queue, &PlayQueue::added);
        QSignalSpy failed(&f.queue, &PlayQueue::addFailed);
        f.client.disconnectFromServer();
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(!f.queue.busy());
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTest::qWait(200);
        QCOMPARE(added.size(), 0);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(f.queue.count(), 0);
    }

    void aQueueIsSavedAndRestoredInStoredOrder()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.own();
        for (int i = 1; i <= 3; ++i)
            f.server.itemsById.insert(
                episodeId(i),
                QJsonDocument(
                    QJsonObject{{QStringLiteral("Id"), episodeId(i)},
                                {QStringLiteral("Name"), QStringLiteral("Movie %1").arg(i)},
                                {QStringLiteral("Type"), QStringLiteral("Movie")}})
                    .toJson(QJsonDocument::Compact));
        QVERIFY(f.addAndWait(episodeId(2), PlayQueue::Last));
        QVERIFY(f.addAndWait(episodeId(3), PlayQueue::Last));
        QVERIFY(f.addAndWait(episodeId(1), PlayQueue::Last));
        QTRY_VERIFY(f.stored().contains(episodeId(1).toUtf8())
                    && f.stored().contains(episodeId(3).toUtf8()));
        // A second queue on the same file reads it back, though the server answers in the opposite order
        f.server.reverseIdsAnswer = true;
        PlayQueue again(&f.client);
        QAbstractItemModelTester tester(&again);
        again.setStoragePath(f.legacyPath);
        again.restoreFor(f.client.serverId(), f.client.userId());
        QVERIFY(again.restoring());
        QVERIFY(again.busy());
        QTRY_VERIFY(!again.restoring());
        QCOMPARE(again.count(), 3);
        QCOMPARE(again.data(again.index(0), PlayQueue::NameRole).toString(),
                 QStringLiteral("Movie 2"));
        QCOMPARE(again.data(again.index(2), PlayQueue::NameRole).toString(),
                 QStringLiteral("Movie 1"));
    }

    void whatTheServerNoLongerHasIsDroppedAndTheFileRewritten()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.write(f.fileFor({kMovie, episodeId(99)}, f.client.serverId(), f.client.userId()));
        f.queue.restoreFor(f.client.serverId(), f.client.userId());
        QTRY_VERIFY(!f.queue.restoring());
        QCOMPARE(f.queue.count(), 1);
        QCOMPARE(f.id(0), kMovie);
        QTRY_VERIFY(!f.stored().contains(episodeId(99).toUtf8()));
        QVERIFY(f.stored().contains(kMovie.toUtf8()));
    }

    void aFailedRestoreKeepsTheFileAndRetries()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QByteArray bytes = f.fileFor({kMovie}, f.client.serverId(), f.client.userId());
        f.write(bytes);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        f.queue.restoreFor(f.client.serverId(), f.client.userId());
        QTRY_VERIFY(f.queue.restoreFailed());
        QVERIFY(!f.queue.restoring());
        QVERIFY(!f.queue.busy());
        QCOMPARE(f.stored(), bytes);
        f.server.listingStatus.clear();
        f.queue.retryRestore();
        QVERIFY(f.queue.restoring());
        QTRY_VERIFY(!f.queue.restoring());
        QVERIFY(!f.queue.restoreFailed());
        QCOMPARE(f.queue.count(), 1);
    }

    void eachUserKeepsOwnQueueFile()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString other = QStringLiteral("0123456789abcdef0123456789abcdef");
        f.server.itemsById.insert(
            episodeId(1),
            QJsonDocument(QJsonObject{{QStringLiteral("Id"), episodeId(1)},
                                      {QStringLiteral("Name"), QStringLiteral("Other")},
                                      {QStringLiteral("Type"), QStringLiteral("Movie")}})
                .toJson(QJsonDocument::Compact));
        f.own();
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QTRY_VERIFY(f.stored().contains(kMovie.toUtf8()));
        const QByteArray mine = f.stored();
        // Another user signs in: no rows, and the first file stays as it is
        f.queue.restoreFor(f.client.serverId(), other);
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(f.addAndWait(episodeId(1), PlayQueue::Last));
        const QString theirs = f.ownerPath(f.client.serverId(), other);
        QTRY_VERIFY(f.readAt(theirs).contains(episodeId(1).toUtf8()));
        QVERIFY(!f.readAt(theirs).contains(kMovie.toUtf8()));
        QCOMPARE(f.stored(), mine);
        // The first user is back: their own queue, not the other's
        f.queue.restoreFor(f.client.serverId(), f.client.userId());
        QTRY_VERIFY(!f.queue.restoring());
        QCOMPARE(f.queue.count(), 1);
        QCOMPARE(f.id(0), kMovie);
        // The folder holds nothing but the two files, and is private
        QCOMPARE(QDir(QFileInfo(f.path).absolutePath()).entryList(QDir::Files).size(), 2);
        const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                            | QFileDevice::ReadOther | QFileDevice::WriteOther
                            | QFileDevice::ExeOther;
        QVERIFY(!(QFileInfo(QFileInfo(f.path).absolutePath()).permissions() & others));
    }

    void legacyQueueMigratesToOwner()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString other = QStringLiteral("0123456789abcdef0123456789abcdef");
        const QByteArray legacy = f.fileFor({kMovie}, f.client.serverId(), f.client.userId());
        f.writeAt(f.legacyPath, legacy);
        const int before = f.server.listingRequests;
        // Another owner's restore leaves the old file alone
        f.queue.restoreFor(f.client.serverId(), other);
        QCOMPARE(f.queue.count(), 0);
        QCOMPARE(f.server.listingRequests, before);
        QCOMPARE(f.readAt(f.legacyPath), legacy);
        QVERIFY(!QFileInfo::exists(f.ownerPath(f.client.serverId(), other)));
        // The owner's restore moves it
        f.own();
        QVERIFY(f.queue.restoring());
        QVERIFY(!QFileInfo::exists(f.legacyPath));
        QCOMPARE(f.stored(), legacy);
        QTRY_VERIFY(!f.queue.restoring());
        QCOMPARE(f.queue.count(), 1);
        QCOMPARE(f.id(0), kMovie);
    }

    void aLegacyQueueStaysWhenTheOwnerFileCannotBeWritten()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QByteArray legacy = f.fileFor({kMovie}, f.client.serverId(), f.client.userId());
        f.writeAt(f.legacyPath, legacy);
        // A regular file where the `queues` folder should be
        QFile blocker(f.dir.filePath(QStringLiteral("profile/queues")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        f.own();
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(!f.queue.restoring());
        QCOMPARE(f.readAt(f.legacyPath), legacy);
    }

    void anExistingOwnerFileWinsOverTheLegacyOne()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QByteArray legacy = f.fileFor({kSeason}, f.client.serverId(), f.client.userId());
        f.writeAt(f.legacyPath, legacy);
        f.write(f.fileFor({kMovie}, f.client.serverId(), f.client.userId()));
        f.own();
        QTRY_VERIFY(!f.queue.restoring());
        QCOMPARE(f.queue.count(), 1);
        QCOMPARE(f.id(0), kMovie);
        QCOMPARE(f.readAt(f.legacyPath), legacy);
    }

    void aFileThatIsNotWhatItsNameSaysIsRemoved()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.write(f.fileFor({kMovie}, f.client.serverId(),
                          QStringLiteral("0123456789abcdef0123456789abcdef")));
        const int before = f.server.listingRequests;
        f.own();
        QCOMPARE(f.queue.count(), 0);
        QCOMPARE(f.server.listingRequests, before);
        QVERIFY(!QFileInfo::exists(f.path));
    }

    void anOwnerWithAnInvalidIdKeepsNothing()
    {
        Fixture f;
        QVERIFY(f.ok);
        // A valid owner's file that a hostile id must not reach
        f.write(f.fileFor({kMovie}, f.client.serverId(), f.client.userId()));
        const QByteArray mine = f.stored();
        const QStringList hostile = {QStringLiteral("../x"), QStringLiteral("a/b"),
                                     QStringLiteral(".."), QStringLiteral("a_b")};
        for (const QString &id : hostile) {
            f.queue.restoreFor(f.client.serverId(), id);
            QVERIFY(f.addAndWait(kSeason, PlayQueue::Last));
            QTest::qWait(50);
            f.queue.forgetStored(f.client.serverId(), id);
            f.queue.forgetStoredUser(id, f.client.userId());
            f.queue.restoreFor(id, f.client.userId());
            QVERIFY(f.addAndWait(kSeason, PlayQueue::Last));
            QTest::qWait(50);
        }
        // No user at all is no owner either
        f.queue.restoreFor(f.client.serverId(), QString());
        QVERIFY(f.addAndWait(kSeason, PlayQueue::Last));
        QTest::qWait(50);
        // Nothing was written but the valid owner's file, and nothing outside the queues folder
        QCOMPARE(f.stored(), mine);
        QCOMPARE(QDir(QFileInfo(f.path).absolutePath()).entryList(QDir::Files).size(), 1);
        QVERIFY(!QFileInfo::exists(f.legacyPath));
        QVERIFY(!QFileInfo::exists(f.dir.filePath(QStringLiteral("x.json"))));
        QVERIFY(!QFileInfo::exists(f.dir.filePath(QStringLiteral("profile/x.json"))));
    }

    void aStoredQueueIsAskedFromTheServerInBatchesOfAHundred()
    {
        Fixture f;
        QVERIFY(f.ok);
        QStringList ids;
        for (int i = 1; i <= 250; ++i)
            ids.append(QStringLiteral("%1").arg(i, 32, 16, QLatin1Char('0')));
        f.write(f.fileFor(ids, f.client.serverId(), f.client.userId()));
        f.queue.restoreFor(f.client.serverId(), f.client.userId());
        QTRY_VERIFY(!f.queue.restoring());
        const QList<QByteArray> routes = f.server.listingRoutes.value(QStringLiteral("Items"));
        QCOMPARE(routes.size(), 3);
        for (const QByteArray &route : routes)
            QVERIFY(FakeJellyfin::capturedListingQuery(route)
                        .queryItemValue(QStringLiteral("ids"))
                        .split(QLatin1Char(','))
                        .size()
                    <= 100);
        // Nothing of it is known to the server: the queue is empty and the file goes
        QCOMPARE(f.queue.count(), 0);
        QTRY_VERIFY(f.stored().isEmpty());
    }

    void unusableFilesAreRemovedWithoutAnyRequest_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const QString server = QStringLiteral("0123456789abcdef0123456789abcdef");
        const auto file = [&](const QJsonObject &object) {
            return QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        QJsonArray tooMany;
        for (int i = 1; i <= 501; ++i)
            tooMany.append(QStringLiteral("%1").arg(i, 32, 16, QLatin1Char('0')));
        const QJsonObject base{{QStringLiteral("version"), 1},
                               {QStringLiteral("serverId"), server},
                               {QStringLiteral("userId"), server},
                               {QStringLiteral("items"), QJsonArray{kMovie}}};
        const auto with = [&](const QString &key, const QJsonValue &value) {
            QJsonObject object = base;
            object.insert(key, value);
            return file(object);
        };
        QTest::newRow("not JSON") << QByteArrayLiteral("not json at all");
        QTest::newRow("an array") << QByteArrayLiteral("[1,2]");
        QTest::newRow("version a string") << with(QStringLiteral("version"), QStringLiteral("1"));
        QTest::newRow("version zero") << with(QStringLiteral("version"), 0);
        QTest::newRow("a bad id") << with(QStringLiteral("items"),
                                          QJsonArray{QStringLiteral("../x")});
        QTest::newRow("an id that is a number") << with(QStringLiteral("items"), QJsonArray{5});
        QTest::newRow("501 ids") << with(QStringLiteral("items"), tooMany);
        QTest::newRow("a duplicate")
            << with(QStringLiteral("items"), QJsonArray{kMovie, kMovie.toUpper()});
        QTest::newRow("a duplicate without dashes")
            << with(QStringLiteral("items"),
                    QJsonArray{QStringLiteral("0123456789abcdef-0123456789abcdef"),
                               QStringLiteral("0123456789abcdef0123456789abcdef")});
        QTest::newRow("no owner") << with(QStringLiteral("serverId"), QString());
        QTest::newRow("a bad owner") << with(QStringLiteral("userId"), QStringLiteral("a b"));
        QTest::newRow("items missing") << with(QStringLiteral("items"), QJsonValue());
        QTest::newRow("over 64 KiB") << (file(base) + QByteArray(PlayQueue::MaxFileBytes, ' '));
    }

    void unusableFilesAreRemovedWithoutAnyRequest()
    {
        QFETCH(QByteArray, bytes);
        Fixture f;
        QVERIFY(f.ok);
        f.write(bytes);
        const int before = f.server.listingRequests;
        f.queue.restoreFor(f.client.serverId(), f.client.userId());
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(!f.queue.restoring());
        QCOMPARE(f.server.listingRequests, before);
        QVERIFY(!QFileInfo::exists(f.path));
    }

    void aNewerVersionIsMovedAside()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QByteArray bytes = f.fileFor({kMovie}, f.client.serverId(), f.client.userId(), 2);
        f.write(bytes);
        f.queue.restoreFor(f.client.serverId(), f.client.userId());
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(!QFileInfo::exists(f.path));
        QFile aside(f.path + QStringLiteral(".broken"));
        QVERIFY(aside.open(QIODevice::ReadOnly));
        QCOMPARE(aside.readAll(), bytes);
    }

    void theFileHoldsIdsOnlyAndIsPrivate()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.own();
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QTRY_VERIFY(!f.stored().isEmpty());
        const QByteArray bytes = f.stored();
        for (const char *forbidden : {"Arcturus", "http", "Token", "token", "Name", "imageUrl"})
            QVERIFY2(!bytes.contains(forbidden), forbidden);
        QVERIFY(bytes.contains(kMovie.toUtf8()));
        const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                            | QFileDevice::ReadOther | QFileDevice::WriteOther
                            | QFileDevice::ExeOther;
        const QFileDevice::Permissions file = QFileInfo(f.path).permissions();
        QVERIFY(file.testFlag(QFileDevice::ReadOwner) && file.testFlag(QFileDevice::WriteOwner));
        QVERIFY(!(file & others));
        QVERIFY(!file.testFlag(QFileDevice::ExeOwner));
        const QFileDevice::Permissions folder =
            QFileInfo(QFileInfo(f.path).absolutePath()).permissions();
        QVERIFY(!(folder & others));
    }

    void aPendingSaveIsFlushedWhenTheQueueIsDestroyed()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto queue = std::make_unique<PlayQueue>(&f.client);
        queue->setStoragePath(f.legacyPath);
        queue->setSaveDelay(std::chrono::minutes(10));
        queue->restoreFor(f.client.serverId(), f.client.userId());
        QSignalSpy added(queue.get(), &PlayQueue::added);
        QVERIFY(queue->add(kMovie, PlayQueue::Last));
        QTRY_COMPARE(added.size(), 1);
        QVERIFY(!QFileInfo::exists(f.path));
        queue.reset();
        QVERIFY(f.stored().contains(kMovie.toUtf8()));
    }

    void anEmptiedQueueDeletesItsFile()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.own();
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QTRY_VERIFY(QFileInfo::exists(f.path));
        f.queue.remove(0);
        QTRY_VERIFY(!QFileInfo::exists(f.path));
    }

    void aLostSessionSavesWhatWasPendingAndKeepsTheFile()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.queue.setSaveDelay(std::chrono::minutes(10));
        f.own();
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QVERIFY(!QFileInfo::exists(f.path));
        const QString serverId = f.client.serverId();
        f.client.disconnectFromServer();
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(f.stored().contains(kMovie.toUtf8()));
        // Signing out forgets it
        f.queue.forgetStored(serverId);
        QVERIFY(!QFileInfo::exists(f.path));
    }

    void forgettingOneServerLeavesAnothersFile()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.own();
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QTRY_VERIFY(QFileInfo::exists(f.path));
        f.queue.forgetStored(QStringLiteral("00000000000000000000000000000001"));
        QVERIFY(QFileInfo::exists(f.path));
        QCOMPARE(f.queue.count(), 1);
        f.queue.forgetStored(f.client.serverId());
        QVERIFY(!QFileInfo::exists(f.path));
        QCOMPARE(f.queue.count(), 0);
    }

    void anUnwritablePathReportsOncePerStreak()
    {
        Fixture f;
        QVERIFY(f.ok);
        // A regular file where the folder should be
        QFile blocker(f.dir.filePath(QStringLiteral("blocker")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        f.queue.setStoragePath(f.dir.filePath(QStringLiteral("blocker/queue.json")));
        f.own();
        QSignalSpy failures(&f.queue, &PlayQueue::saveFailed);
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QTRY_COMPARE(failures.size(), 1);
        f.queue.remove(0);
        QTest::qWait(150);
        QCOMPARE(failures.size(), 1);
        QCOMPARE(f.queue.count(), 0);
    }

    void anExpansionThatReadsOnlySkippedEntriesEnds()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 5;
        f.server.listingRawBody.insert(
            QStringLiteral("Items"),
            QJsonDocument(QJsonObject{{QStringLiteral("Items"),
                                       QJsonArray{QJsonObject{
                                           {QStringLiteral("Id"), episodeId(1)},
                                           {QStringLiteral("Type"), QStringLiteral("Folder")}}}},
                                      {QStringLiteral("TotalRecordCount"), 2147483647}})
                .toJson(QJsonDocument::Compact));
        QSignalSpy nothing(&f.queue, &PlayQueue::nothingToAdd);
        QVERIFY(f.queue.add(kBoxSet, PlayQueue::Last));
        QTRY_COMPARE(nothing.size(), 1);
        QVERIFY(!f.queue.busy());
        QVERIFY(f.server.listingRoutes.value(QStringLiteral("Items")).size()
                <= PlayQueue::MaxPages + 1);
    }

    void aFailedNextUpAddsNothing()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 5;
        f.server.listingStatus.insert(QStringLiteral("NextUp"), 500);
        QSignalSpy failed(&f.queue, &PlayQueue::addFailed);
        QVERIFY(f.queue.add(kSeries, PlayQueue::Last));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(f.queue.count(), 0);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Episodes")).size(), 0);
    }

    void aSeriesAsksForItsEpisodesWithoutASeason()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedEpisodeTotal = 3;
        f.server.listingRawBody.insert(QStringLiteral("NextUp"), envelope({}));
        QVERIFY(f.addAndWait(kSeries, PlayQueue::Last));
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("Episodes")).constFirst());
        QVERIFY(wire.queryItemValue(QStringLiteral("seasonId")).isEmpty());
        QVERIFY(
            f.server.lastListingRoute.value(QStringLiteral("Episodes")).contains(kSeries.toUtf8()));
    }

    void aPlainAddIsNotTruncatedAndSignalsTheEdit()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy added(&f.queue, &PlayQueue::added);
        QSignalSpy edited(&f.queue, &PlayQueue::edited);
        QVERIFY(f.queue.add(kMovie, PlayQueue::Last));
        QTRY_COMPARE(added.size(), 1);
        QVERIFY(!added.constFirst().at(1).toBool());
        QCOMPARE(edited.size(), 1);
    }

    void aLostSessionIsNoEdit()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QSignalSpy edited(&f.queue, &PlayQueue::edited);
        f.client.disconnectFromServer();
        QCOMPARE(f.queue.count(), 0);
        QCOMPARE(edited.size(), 0);
    }

    void clearingWhileAnExpansionRunsEndsItQuietly()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        f.server.generatedEpisodeTotal = 10;
        f.server.holdListings = true;
        QVERIFY(f.queue.add(kSeason, PlayQueue::Last));
        QSignalSpy added(&f.queue, &PlayQueue::added);
        QSignalSpy failed(&f.queue, &PlayQueue::addFailed);
        f.queue.clear();
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(!f.queue.busy());
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTest::qWait(200);
        QCOMPARE(f.queue.count(), 0);
        QCOMPARE(added.size(), 0);
        QCOMPARE(failed.size(), 0);
    }

    void addingAQueuedItemAtTheCapKeepsIt()
    {
        Fixture f;
        QVERIFY(f.ok);
        // A full queue: the 700 episodes of a season cut at 500
        f.server.generatedEpisodeTotal = 700;
        QVERIFY(f.addAndWait(kSeason, PlayQueue::Last));
        QCOMPARE(f.queue.count(), int(PlayQueue::MaxEntries));
        const QString queuedLate = f.id(400);
        f.server.itemsById.insert(
            queuedLate.toLower(),
            QJsonDocument(QJsonObject{{QStringLiteral("Id"), queuedLate},
                                      {QStringLiteral("Name"), QStringLiteral("Late")},
                                      {QStringLiteral("Type"), QStringLiteral("Movie")}})
                .toJson(QJsonDocument::Compact));
        QSignalSpy added(&f.queue, &PlayQueue::added);
        QVERIFY(f.queue.add(queuedLate, PlayQueue::Next));
        QTRY_COMPARE(added.size(), 1);
        // It moved to the top and the list did not grow
        QCOMPARE(f.queue.count(), int(PlayQueue::MaxEntries));
        QCOMPARE(f.id(0), queuedLate);
        QVERIFY(!added.constFirst().at(1).toBool());
        // A new item finds no room
        QSignalSpy again(&f.queue, &PlayQueue::added);
        QVERIFY(f.queue.add(kMovie, PlayQueue::Last));
        QTRY_COMPARE(again.size(), 1);
        QCOMPARE(again.constFirst().at(0).toInt(), 0);
        QVERIFY(again.constFirst().at(1).toBool());
        QCOMPARE(f.queue.count(), int(PlayQueue::MaxEntries));
    }

    void aLinkOrAPipeIsNeverRead()
    {
        Fixture f;
        QVERIFY(f.ok);
        const int before = f.server.listingRequests;
        // A link to a valid queue file
        const QString target = f.dir.filePath(QStringLiteral("target.json"));
        QFile real(target);
        QVERIFY(real.open(QIODevice::WriteOnly));
        real.write(f.fileFor({kMovie}, f.client.serverId(), f.client.userId()));
        real.close();
        QDir().mkpath(QFileInfo(f.path).absolutePath());
        QVERIFY(QFile::link(target, f.path));
        f.queue.restoreFor(f.client.serverId(), f.client.userId());
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(!QFileInfo(f.path).isSymLink());
        QVERIFY(QFileInfo::exists(target));
        // A pipe would block a read
        QVERIFY(::mkfifo(QFile::encodeName(f.path).constData(), 0600) == 0);
        f.queue.restoreFor(f.client.serverId(), f.client.userId());
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(!QFileInfo::exists(f.path));
        QCOMPARE(f.server.listingRequests, before);
    }

    void signingOutOfOneUserLeavesAnotherUsersQueue()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString other = QStringLiteral("0123456789abcdef0123456789abcdef");
        const QString theirs = f.ownerPath(f.client.serverId(), other);
        f.writeAt(theirs, f.fileFor({kMovie}, f.client.serverId(), other));
        f.queue.forgetStored(f.client.serverId(), f.client.userId());
        QVERIFY(QFileInfo::exists(theirs));
        // Removing the server takes every user's queue of it
        f.queue.forgetStored(f.client.serverId());
        QVERIFY(!QFileInfo::exists(theirs));
    }

    void forgetStoredUserRemovesOneOwnersFile()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString other = QStringLiteral("0123456789abcdef0123456789abcdef");
        const QString foreign = QStringLiteral("00000000000000000000000000000001");
        const QString theirs = f.ownerPath(f.client.serverId(), other);
        const QString elsewhere = f.ownerPath(foreign, f.client.userId());
        f.write(f.fileFor({kMovie}, f.client.serverId(), f.client.userId()));
        f.writeAt(theirs, f.fileFor({kMovie}, f.client.serverId(), other));
        f.writeAt(theirs + QStringLiteral(".broken"), QByteArrayLiteral("x"));
        f.writeAt(elsewhere, f.fileFor({kMovie}, foreign, f.client.userId()));
        f.writeAt(f.legacyPath, f.fileFor({kMovie}, f.client.serverId(), other));
        // The current user is in memory too
        f.own();
        QTRY_VERIFY(!f.queue.restoring());
        QCOMPARE(f.queue.count(), 1);
        f.queue.forgetStoredUser(f.client.serverId(), f.client.userId());
        QVERIFY(!QFileInfo::exists(f.path));
        QCOMPARE(f.queue.count(), 0);
        QVERIFY(QFileInfo::exists(theirs));
        QVERIFY(QFileInfo::exists(elsewhere));
        // An empty user is not the whole server
        f.queue.forgetStoredUser(f.client.serverId(), QString());
        QVERIFY(QFileInfo::exists(theirs));
        // The other user's file, the one set aside and the old file of that user go
        f.queue.forgetStoredUser(f.client.serverId(), other);
        QVERIFY(!QFileInfo::exists(theirs));
        QVERIFY(!QFileInfo::exists(theirs + QStringLiteral(".broken")));
        QVERIFY(!QFileInfo::exists(f.legacyPath));
        QVERIFY(QFileInfo::exists(elsewhere));
    }

    void forgettingAServerRemovesEveryQueueFileOfIt()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString foreign = QStringLiteral("00000000000000000000000000000001");
        const QString elsewhere = f.ownerPath(foreign, f.client.userId());
        const QString second =
            f.ownerPath(f.client.serverId(), QStringLiteral("abcdefabcdefabcdefabcdefabcdefab"));
        f.write(f.fileFor({kMovie}, f.client.serverId(), f.client.userId()));
        f.writeAt(second, QByteArrayLiteral("not even json"));
        f.writeAt(second + QStringLiteral(".broken"), QByteArrayLiteral("x"));
        f.writeAt(elsewhere, f.fileFor({kMovie}, foreign, f.client.userId()));
        f.writeAt(f.legacyPath, f.fileFor({kMovie}, f.client.serverId(), f.client.userId()));
        f.queue.forgetStored(f.client.serverId());
        QVERIFY(!QFileInfo::exists(f.path));
        QVERIFY(!QFileInfo::exists(second));
        QVERIFY(!QFileInfo::exists(second + QStringLiteral(".broken")));
        QVERIFY(!QFileInfo::exists(f.legacyPath));
        QVERIFY(QFileInfo::exists(elsewhere));
    }

    void removingAnotherServerKeepsThePendingEdits()
    {
        Fixture f;
        QVERIFY(f.ok);
        auto queue = std::make_unique<PlayQueue>(&f.client);
        queue->setStoragePath(f.legacyPath);
        queue->setSaveDelay(std::chrono::minutes(10));
        queue->restoreFor(f.client.serverId(), f.client.userId());
        QSignalSpy added(queue.get(), &PlayQueue::added);
        QVERIFY(queue->add(kMovie, PlayQueue::Last));
        QTRY_COMPARE(added.size(), 1);
        queue->forgetStored(QStringLiteral("00000000000000000000000000000001"));
        QCOMPARE(queue->count(), 1);
        queue.reset();
        QVERIFY(f.stored().contains(kMovie.toUtf8()));
    }

    void anAddAfterAFailedRestoreIsRefusedAndTheFileStays()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QByteArray bytes = f.fileFor({kMovie}, f.client.serverId(), f.client.userId());
        f.write(bytes);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        f.queue.restoreFor(f.client.serverId(), f.client.userId());
        QTRY_VERIFY(f.queue.restoreFailed());
        QSignalSpy failed(&f.queue, &PlayQueue::addFailed);
        QVERIFY(!f.queue.add(kMovie, PlayQueue::Last));
        QCOMPARE(failed.size(), 1);
        QTest::qWait(100);
        QCOMPARE(f.stored(), bytes);
    }

    void aNewSignInFlushesWhatTheLastOwnerEdited()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.queue.setSaveDelay(std::chrono::minutes(10));
        f.own();
        QVERIFY(f.addAndWait(kMovie, PlayQueue::Last));
        QVERIFY(!QFileInfo::exists(f.path));
        // Another user signs in: the first one's queue is written under the first one's name before the file is read
        f.queue.restoreFor(f.client.serverId(), QStringLiteral("0123456789abcdef0123456789abcdef"));
        QVERIFY(f.stored().contains(f.client.userId().toUtf8()));
        QVERIFY(f.stored().contains(kMovie.toUtf8()));
        // The rows were the first user's: the second starts with none
        QCOMPARE(f.queue.count(), 0);
    }
};

QTEST_MAIN(TestPlayQueue)
#include "test_play_queue.moc"
