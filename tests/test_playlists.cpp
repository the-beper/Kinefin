// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The list of playlists and the entries of one against the fake server: what is asked, the order, the caps, the channels and the end of a session, then the edits (moves, removals, renames, deletes): what is sent, in which order, and how a failure ends
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/PlaylistEntriesModel.h"
#include "models/PlaylistsController.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <algorithm>

using Kinefin::ItemRowModel;
using Kinefin::PlaylistEntriesModel;
using Kinefin::PlaylistsController;

namespace {
QString movieId(int number)
{
    return QStringLiteral("d1%1").arg(number, 30, 16, QLatin1Char('0'));
}

// The ids of `ids` in the order of `positions`
QStringList ordered(const QStringList &ids, std::initializer_list<int> positions)
{
    QStringList result;
    for (const int position : positions)
        result.append(ids.at(position));
    return result;
}

int requestsOf(const FakeJellyfin &server, const char *method)
{
    return int(std::count_if(server.playlistRequests.cbegin(), server.playlistRequests.cend(),
                             [method](const auto &request) { return request.method == method; }));
}

// The entries reads the fake saw
int readsOf(const FakeJellyfin &server)
{
    return requestsOf(server, "GET");
}

int writesOf(const FakeJellyfin &server)
{
    return int(server.playlistRequests.size()) - readsOf(server);
}

// The requests that are not reads, in arrival order
QList<FakeJellyfin::PlaylistRequest> writeRequests(const FakeJellyfin &server)
{
    QList<FakeJellyfin::PlaylistRequest> writes;
    for (const auto &request : server.playlistRequests) {
        if (request.method != "GET")
            writes.append(request);
    }
    return writes;
}

struct Fixture {
    Fixture() { ok = server.listen() && login(client, server); }
    // `count` movies named "M<n>" in the fake's catalogue; their ids
    QStringList movies(int count)
    {
        QStringList ids;
        for (int i = 0; i < count; ++i) {
            const QString id = movieId(i + 1);
            server.itemsById.insert(
                id, QJsonDocument(
                        QJsonObject{{QStringLiteral("Id"), id},
                                    {QStringLiteral("Name"), QStringLiteral("M%1").arg(i + 1)},
                                    {QStringLiteral("Type"), QStringLiteral("Movie")}})
                        .toJson(QJsonDocument::Compact));
            ids.append(id);
        }
        return ids;
    }
    QString rowName(const QAbstractItemModel &model, int row, int role) const
    {
        return model.data(model.index(row, 0), role).toString();
    }
    // The playlist list reads the fake has answered
    int listings() const { return int(server.listingRoutes.value(QStringLiteral("Items")).size()); }
    QStringList rowIds(const QAbstractItemModel &model) const
    {
        QStringList ids;
        for (int row = 0; row < model.rowCount(); ++row)
            ids.append(rowName(model, row, PlaylistEntriesModel::ItemIdRole));
        return ids;
    }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    bool ok = false;
};
} // namespace

class TestPlaylists : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void theListAsksForVideoPlaylistsByName()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.seedPlaylist(QStringLiteral("Weekend"), {});
        f.server.seedPlaylist(QStringLiteral("anime"), {});
        f.server.seedPlaylist(QStringLiteral("Christmas"), {});
        PlaylistsController playlists(&f.client);
        QAbstractItemModelTester tester(playlists.list());
        QCOMPARE(playlists.status(), PlaylistsController::Idle);
        playlists.reload();
        QCOMPARE(playlists.status(), PlaylistsController::Loading);
        QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
        const QByteArray route = f.server.lastListingRoute.value(QStringLiteral("Items"));
        for (const QByteArray &part :
             {QByteArray("includeItemTypes=Playlist"), QByteArray("mediaTypes=Video"),
              QByteArray("recursive=true"), QByteArray("sortBy=SortName"), QByteArray("limit=100")})
            QVERIFY2(route.contains(part), route.constData());
        QCOMPARE(playlists.list()->count(), 3);
        QCOMPARE(playlists.total(), 3);
        QCOMPARE(f.rowName(*playlists.list(), 0, ItemRowModel::NameRole), QStringLiteral("anime"));
        QCOMPARE(f.rowName(*playlists.list(), 2, ItemRowModel::NameRole),
                 QStringLiteral("Weekend"));
        const QString id = f.rowName(*playlists.list(), 1, ItemRowModel::ItemIdRole);
        QCOMPARE(playlists.nameOf(id.toUpper()), QStringLiteral("Christmas"));
        QVERIFY(playlists.nameOf(movieId(9)).isEmpty());
        // The same answer again keeps the rows as they are; a changed one replaces them
        QSignalSpy resets(playlists.list(), &QAbstractItemModel::modelReset);
        playlists.reload();
        QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
        QCOMPARE(resets.size(), 0);
        f.server.seedPlaylist(QStringLiteral("Zebra"), {});
        playlists.reload();
        QTRY_COMPARE(playlists.list()->count(), 4);
        QCOMPARE(resets.size(), 1);
    }

    void aFailedReadKeepsTheRowsAndASessionEndClearsThem()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.seedPlaylist(QStringLiteral("Weekend"), {});
        PlaylistsController playlists(&f.client);
        playlists.reload();
        QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        playlists.reload();
        QTRY_COMPARE(playlists.status(), PlaylistsController::Failed);
        QCOMPARE(playlists.list()->count(), 1);
        f.client.disconnectFromServer();
        QTRY_COMPARE(playlists.list()->count(), 0);
        QCOMPARE(playlists.status(), PlaylistsController::Idle);
    }

    void closingReleasesTheChannelAndDropsTheAnswer()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.seedPlaylist(QStringLiteral("Weekend"), {});
        PlaylistsController playlists(&f.client);
        f.server.holdListings = true;
        playlists.reload();
        QTRY_COMPARE(f.server.listingRequests, 1);
        playlists.close();
        QCOMPARE(playlists.status(), PlaylistsController::Idle);
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTest::qWait(200);
        QCOMPARE(playlists.status(), PlaylistsController::Idle);
        QCOMPARE(playlists.list()->count(), 0);
    }

    void entriesComeInTheirOrderWithTheirEntryIds()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        // The copies of one item share its entry id, as on the 12.4 server
        const QString playlist =
            f.server.seedPlaylist(QStringLiteral("Evening"), {ids.at(2), ids.at(0), ids.at(2)});
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QCOMPARE(entries.status(), PlaylistEntriesModel::Loading);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(entries.count(), 3);
        QCOMPARE(entries.total(), 3);
        QCOMPARE(f.rowName(entries, 0, PlaylistEntriesModel::NameRole), QStringLiteral("M3"));
        QCOMPARE(f.rowName(entries, 1, PlaylistEntriesModel::NameRole), QStringLiteral("M1"));
        QCOMPARE(f.rowName(entries, 2, PlaylistEntriesModel::ItemIdRole), ids.at(2));
        QCOMPARE(f.rowName(entries, 0, PlaylistEntriesModel::EntryIdRole),
                 f.server.entryIdOf(playlist, ids.at(2)));
        QVERIFY(!f.rowName(entries, 1, PlaylistEntriesModel::EntryIdRole).isEmpty());
        // A new read keeps the rows on screen until it ends, and the same entries again keep them as they are
        QSignalSpy resets(&entries, &QAbstractItemModel::modelReset);
        entries.load(playlist);
        QCOMPARE(entries.count(), 3);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(resets.size(), 0);
        // Another playlist's rows go at once
        const QString other = f.server.seedPlaylist(QStringLiteral("Other"), {ids.at(1)});
        entries.load(other);
        QCOMPARE(entries.count(), 0);
        QTRY_COMPARE(entries.count(), 1);
    }

    void entriesAreReadPageByPageUpToTheCap()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(PlaylistEntriesModel::MaxEntries + 30);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Long"), ids);
        PlaylistEntriesModel entries(&f.client);
        entries.load(playlist);
        QTRY_COMPARE_WITH_TIMEOUT(entries.status(), PlaylistEntriesModel::Ready, 15000);
        QCOMPARE(entries.count(), PlaylistEntriesModel::MaxEntries);
        QCOMPARE(entries.total(), PlaylistEntriesModel::MaxEntries + 30);
        QCOMPARE(f.rowName(entries, PlaylistEntriesModel::MaxEntries - 1,
                           PlaylistEntriesModel::ItemIdRole),
                 ids.at(PlaylistEntriesModel::MaxEntries - 1));
        const qsizetype reads =
            std::count_if(f.server.playlistRequests.cbegin(), f.server.playlistRequests.cend(),
                          [](const auto &request) { return request.method == "GET"; });
        QCOMPARE(reads, PlaylistEntriesModel::MaxEntries / PlaylistEntriesModel::PageSize);
    }

    void aFailedReadKeepsTheRowsOfThePlaylist()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(2);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        PlaylistEntriesModel entries(&f.client);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        // The playlist went away on the server: its read fails, what was shown stays
        f.server.playlists.clear();
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Failed);
        QCOMPARE(entries.count(), 2);
        // The end of the session takes everything
        f.client.disconnectFromServer();
        QTRY_COMPARE(entries.count(), 0);
        QVERIFY(entries.playlistId().isEmpty());
        QCOMPARE(entries.status(), PlaylistEntriesModel::Idle);
    }

    void anInvalidPlaylistIdIsNeverAsked()
    {
        Fixture f;
        QVERIFY(f.ok);
        PlaylistEntriesModel entries(&f.client);
        entries.load(QStringLiteral("../Users"));
        QCOMPARE(entries.status(), PlaylistEntriesModel::Idle);
        QTest::qWait(100);
        QVERIFY(f.server.playlistRequests.isEmpty());
    }

    void aMoveIsAppliedAtOnceAndConfirmedByOneRead()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(4);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(entries.editError(), PlaylistEntriesModel::NoEditError);
        QVERIFY(!entries.writing());
        QSignalSpy resets(&entries, &QAbstractItemModel::modelReset);
        QSignalSpy moves(&entries, &QAbstractItemModel::rowsMoved);
        QSignalSpy edited(&entries, &PlaylistEntriesModel::edited);
        QSignalSpy writingChanges(&entries, &PlaylistEntriesModel::writingChanged);
        const QString entry = f.server.entryIdOf(playlist, ids.at(0));
        QVERIFY(entries.canMove(0));
        QVERIFY(entries.move(0, 2));
        QCOMPARE(f.rowIds(entries), ordered(ids, {1, 2, 0, 3}));
        QCOMPARE(moves.size(), 1);
        QCOMPARE(edited.size(), 1);
        QVERIFY(entries.writing());
        QTRY_VERIFY(!entries.writing());
        QCOMPARE(writingChanges.size(), 2);
        QTRY_COMPARE(readsOf(f.server), 2);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QTest::qWait(100);
        QCOMPARE(readsOf(f.server), 2);
        const auto writes = writeRequests(f.server);
        QCOMPARE(writes.size(), 1);
        QCOMPARE(writes.constFirst().method, QByteArrayLiteral("POST"));
        const QByteArray suffix("/Playlists/" + playlist.toUtf8() + "/Items/" + entry.toUtf8()
                                + "/Move/2");
        QVERIFY2(writes.constFirst().route.endsWith(suffix), writes.constFirst().route.constData());
        QCOMPARE(f.server.playlistItemIds(playlist), ordered(ids, {1, 2, 0, 3}));
        QCOMPARE(f.rowIds(entries), f.server.playlistItemIds(playlist));
        QCOMPARE(resets.size(), 0);
        QCOMPARE(entries.editError(), PlaylistEntriesModel::NoEditError);
        QCOMPARE(entries.total(), 4);
    }

    void editsAreSentInOrderOneAtATime()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(5);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QSignalSpy resets(&entries, &QAbstractItemModel::modelReset);
        const QString removed = f.server.entryIdOf(playlist, ids.at(2));
        f.server.holdPlaylistWrites = true;
        QVERIFY(entries.move(0, 4));
        QVERIFY(entries.move(3, 1));
        QVERIFY(entries.remove(2));
        QVERIFY(entries.move(0, 2));
        const QStringList expected = ordered(ids, {4, 3, 1, 0});
        QCOMPARE(f.rowIds(entries), expected);
        QVERIFY(entries.writing());
        QTRY_VERIFY(f.server.heldPlaylistCount() == 1);
        QTest::qWait(100);
        QCOMPARE(writesOf(f.server), 1);
        for (int step = 1; step < 4; ++step) {
            f.server.releasePlaylistWrites();
            QTRY_COMPARE(writesOf(f.server), step + 1);
            QTRY_VERIFY(f.server.heldPlaylistCount() == 1);
            QVERIFY(entries.writing());
        }
        f.server.holdPlaylistWrites = false;
        f.server.releasePlaylistWrites();
        QTRY_VERIFY(!entries.writing());
        QCOMPARE(f.rowIds(entries), expected);
        QCOMPARE(f.server.playlistItemIds(playlist), expected);
        const auto writes = writeRequests(f.server);
        QCOMPARE(writes.size(), 4);
        QCOMPARE(writes.at(0).method, QByteArrayLiteral("POST"));
        QVERIFY2(writes.at(0).route.endsWith("/Move/4"), writes.at(0).route.constData());
        QCOMPARE(writes.at(1).method, QByteArrayLiteral("POST"));
        QVERIFY2(writes.at(1).route.endsWith("/Move/1"), writes.at(1).route.constData());
        QCOMPARE(writes.at(2).method, QByteArrayLiteral("DELETE"));
        QVERIFY2(writes.at(2).route.contains(QByteArray("entryIds=" + removed.toUtf8())),
                 writes.at(2).route.constData());
        QCOMPARE(writes.at(3).method, QByteArrayLiteral("POST"));
        QVERIFY2(writes.at(3).route.endsWith("/Move/2"), writes.at(3).route.constData());
        QTRY_COMPARE(readsOf(f.server), 2);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(entries.editError(), PlaylistEntriesModel::NoEditError);
    }

    void theWaitingEditsAreCapped()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        f.server.holdPlaylistWrites = true;
        // One edit on its way plus MaxWaitingWrites waiting; the first two rows swap back and forth
        const int accepted = 1 + PlaylistEntriesModel::MaxWaitingWrites;
        for (int i = 0; i < accepted; ++i) {
            const bool swapped = i % 2 == 0 ? entries.move(0, 1) : entries.move(1, 0);
            QVERIFY2(swapped, qPrintable(QString::number(i)));
        }
        QVERIFY(!entries.canMove(0));
        QVERIFY(!entries.canRemove(0));
        QVERIFY(!entries.move(0, 1));
        QVERIFY(!entries.remove(0));
        QCOMPARE(f.rowIds(entries), ordered(ids, {1, 0, 2}));
        QTRY_VERIFY(f.server.heldPlaylistCount() == 1);
        QTest::qWait(100);
        QCOMPARE(writesOf(f.server), 1);
        f.server.holdPlaylistWrites = false;
        f.server.releasePlaylistWrites();
        QTRY_VERIFY_WITH_TIMEOUT(!entries.writing(), 10000);
        QCOMPARE(writesOf(f.server), accepted);
        QCOMPARE(f.server.playlistItemIds(playlist), ordered(ids, {1, 0, 2}));
        QVERIFY(entries.canMove(0));
        QVERIFY(entries.canRemove(0));
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
    }

    void aFailedWriteDropsTheWaitingEditsAndReadsTheServerOrderAgain()
    {
        const QList<std::pair<int, PlaylistEntriesModel::EditError>> failures{
            {500, PlaylistEntriesModel::EditFailed}, {403, PlaylistEntriesModel::EditForbidden}};
        for (const auto &failure : failures) {
            Fixture f;
            QVERIFY(f.ok);
            const QStringList ids = f.movies(4);
            const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
            PlaylistEntriesModel entries(&f.client);
            QAbstractItemModelTester tester(&entries);
            entries.load(playlist);
            QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
            QSignalSpy errors(&entries, &PlaylistEntriesModel::editErrorChanged);
            f.server.playlistWriteStatus = failure.first;
            QVERIFY(entries.move(0, 3));
            QVERIFY(entries.move(0, 1));
            QVERIFY(entries.remove(2));
            QCOMPARE(entries.count(), 3);
            QTRY_COMPARE(entries.editError(), failure.second);
            QVERIFY(!entries.writing());
            QTRY_COMPARE(f.rowIds(entries), ids);
            QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
            QCOMPARE(entries.total(), 4);
            QTest::qWait(100);
            QCOMPARE(writesOf(f.server), 1);
            QCOMPARE(f.server.playlistItemIds(playlist), ids);
            QCOMPARE(errors.size(), 1);
            // The next accepted edit clears the error
            f.server.playlistWriteStatus = 0;
            QVERIFY(entries.canMove(0));
            QVERIFY(entries.move(0, 1));
            QCOMPARE(entries.editError(), PlaylistEntriesModel::NoEditError);
            QCOMPARE(errors.size(), 2);
            QTRY_VERIFY(!entries.writing());
            QCOMPARE(f.server.playlistItemIds(playlist), ordered(ids, {1, 0, 2, 3}));
            QCOMPARE(entries.editError(), PlaylistEntriesModel::NoEditError);
        }
    }

    void removingOneCopyRemovesEveryCopy()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        const QString playlist = f.server.seedPlaylist(
            QStringLiteral("Evening"), {ids.at(2), ids.at(0), ids.at(2), ids.at(1)});
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(entries.count(), 4);
        QVERIFY(!entries.canMove(0));
        QVERIFY(!entries.canMove(2));
        QVERIFY(entries.canMove(1));
        QVERIFY(entries.canMove(3));
        QVERIFY(!entries.move(0, 1));
        QVERIFY(!entries.move(2, 3));
        QCOMPARE(writesOf(f.server), 0);
        QVERIFY(entries.canRemove(0));
        QVERIFY(entries.canRemove(2));
        QSignalSpy resets(&entries, &QAbstractItemModel::modelReset);
        const QString copies = f.server.entryIdOf(playlist, ids.at(2));
        QVERIFY(entries.remove(2));
        QCOMPARE(entries.count(), 2);
        QCOMPARE(entries.total(), 2);
        QCOMPARE(f.rowIds(entries), ordered(ids, {0, 1}));
        QTRY_VERIFY(!entries.writing());
        const auto writes = writeRequests(f.server);
        QCOMPARE(writes.size(), 1);
        QCOMPARE(writes.constFirst().method, QByteArrayLiteral("DELETE"));
        const QByteArray copiesQuery("/Items?entryIds=" + copies.toUtf8());
        QVERIFY2(writes.constFirst().route.endsWith(copiesQuery),
                 writes.constFirst().route.constData());
        QCOMPARE(f.server.playlistItemIds(playlist), ordered(ids, {0, 1}));
        QTRY_COMPARE(readsOf(f.server), 2);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(entries.count(), 2);
        QCOMPARE(entries.total(), 2);
        QVERIFY(entries.canMove(0));
    }

    void anEntryWithoutAValidIdCanNeitherMoveNorGo()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        f.server.playlistById(playlist)->entries[1].entryId = QStringLiteral("../Items");
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(f.rowIds(entries), ids);
        QVERIFY(f.rowName(entries, 1, PlaylistEntriesModel::EntryIdRole).isEmpty());
        QVERIFY(!entries.canMove(1));
        QVERIFY(!entries.canRemove(1));
        QVERIFY(!entries.move(1, 0));
        QVERIFY(!entries.move(1, 2));
        QVERIFY(!entries.remove(1));
        QVERIFY(entries.canMove(0));
        QVERIFY(entries.canMove(2));
        QVERIFY(entries.canRemove(0));
        QTest::qWait(100);
        QCOMPARE(writesOf(f.server), 0);
        QCOMPARE(f.server.playlistItemIds(playlist), ids);
    }

    void anEntryWhoseItemIsInvalidIsDroppedAndNoRowMoves()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        f.server.itemsById.insert(
            ids.at(1),
            QByteArrayLiteral("{\"Id\":\"../Items\",\"Name\":\"Broken\",\"Type\":\"Movie\"}"));
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(f.rowIds(entries), ordered(ids, {0, 2}));
        QCOMPARE(entries.total(), 3);
        for (int row = 0; row < entries.count(); ++row) {
            QVERIFY(!entries.canMove(row));
            QVERIFY(entries.canRemove(row));
        }
        QVERIFY(!entries.move(0, 1));
        QVERIFY(!entries.move(1, 0));
        QCOMPARE(writesOf(f.server), 0);
        QSignalSpy resets(&entries, &QAbstractItemModel::modelReset);
        const QString entry = f.server.entryIdOf(playlist, ids.at(0));
        QVERIFY(entries.remove(0));
        QCOMPARE(f.rowIds(entries), ordered(ids, {2}));
        QCOMPARE(entries.total(), 2);
        QTRY_VERIFY(!entries.writing());
        const auto writes = writeRequests(f.server);
        QCOMPARE(writes.size(), 1);
        const QByteArray entryQuery("/Items?entryIds=" + entry.toUtf8());
        QVERIFY2(writes.constFirst().route.endsWith(entryQuery),
                 writes.constFirst().route.constData());
        QCOMPARE(f.server.playlistItemIds(playlist), ordered(ids, {1, 2}));
        QTRY_COMPARE(readsOf(f.server), 2);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(resets.size(), 0);
        QVERIFY(!entries.canMove(0));
    }

    void editsAreRefusedUntilAReadSucceeds()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        const auto saved = *f.server.playlistById(playlist);
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QCOMPARE(entries.status(), PlaylistEntriesModel::Loading);
        QVERIFY(!entries.canMove(0));
        QVERIFY(!entries.canRemove(0));
        QVERIFY(!entries.move(0, 1));
        QVERIFY(!entries.remove(0));
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QVERIFY(entries.canMove(0));
        QVERIFY(entries.canRemove(0));
        // The playlist went away on the server: the read fails and the rows stay, but nothing can be edited
        f.server.playlists.clear();
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Failed);
        QCOMPARE(entries.count(), 3);
        QVERIFY(!entries.canMove(0));
        QVERIFY(!entries.canRemove(0));
        QVERIFY(!entries.move(0, 1));
        QVERIFY(!entries.remove(0));
        QTest::qWait(100);
        QCOMPARE(writesOf(f.server), 0);
        // A read that succeeds trusts the rows again
        f.server.playlists.append(saved);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QVERIFY(entries.canMove(0));
        QVERIFY(entries.move(0, 1));
        QTRY_VERIFY(!entries.writing());
        QCOMPARE(f.server.playlistItemIds(playlist), ordered(ids, {1, 0, 2}));
    }

    void aReadOfTheSamePlaylistWaitsForTheWrites()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(readsOf(f.server), 1);
        f.server.holdPlaylistWrites = true;
        QVERIFY(entries.move(0, 1));
        QTRY_VERIFY(f.server.heldPlaylistCount() == 1);
        entries.load(playlist);
        QCOMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QTest::qWait(150);
        QCOMPARE(readsOf(f.server), 1);
        QCOMPARE(f.rowIds(entries), ordered(ids, {1, 0, 2}));
        f.server.holdPlaylistWrites = false;
        f.server.releasePlaylistWrites();
        QTRY_COMPARE(readsOf(f.server), 2);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QTest::qWait(100);
        QCOMPARE(readsOf(f.server), 2);
        QCOMPARE(f.rowIds(entries), ordered(ids, {1, 0, 2}));
        QVERIFY(!entries.writing());
    }

    void anEditWhileTheVisitReadIsOnItsWayDropsTheRead()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QSignalSpy resets(&entries, &QAbstractItemModel::modelReset);
        f.server.holdPlaylistWrites = true;
        entries.load(playlist);
        QCOMPARE(entries.status(), PlaylistEntriesModel::Loading);
        QVERIFY(entries.move(0, 1));
        QCOMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QTRY_VERIFY(f.server.heldPlaylistCount() == 1);
        // The old order is not shown while the write is on its way
        QTest::qWait(200);
        QCOMPARE(f.rowIds(entries), ordered(ids, {1, 0, 2}));
        QCOMPARE(resets.size(), 0);
        QCOMPARE(entries.status(), PlaylistEntriesModel::Ready);
        const int readsBefore = readsOf(f.server);
        f.server.holdPlaylistWrites = false;
        f.server.releasePlaylistWrites();
        QTRY_VERIFY(!entries.writing());
        QTRY_VERIFY(readsOf(f.server) > readsBefore);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(f.rowIds(entries), ordered(ids, {1, 0, 2}));
        QCOMPARE(resets.size(), 0);
    }

    void anotherPlaylistLetsTheWritesOfThePreviousFinish()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(4);
        const QString first = f.server.seedPlaylist(QStringLiteral("First"), ids);
        const QString second =
            f.server.seedPlaylist(QStringLiteral("Second"), {ids.at(3), ids.at(2)});
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(first);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        f.server.holdPlaylistWrites = true;
        QVERIFY(entries.move(0, 3));
        QVERIFY(entries.move(0, 2));
        QVERIFY(entries.remove(0));
        QTRY_VERIFY(f.server.heldPlaylistCount() == 1);
        entries.load(second);
        QVERIFY(!entries.writing());
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(f.rowIds(entries), (QStringList{ids.at(3), ids.at(2)}));
        // The edit of the new playlist waits behind the ones of the previous
        QVERIFY(entries.move(0, 1));
        QVERIFY(entries.writing());
        QTest::qWait(100);
        QCOMPARE(writesOf(f.server), 1);
        f.server.holdPlaylistWrites = false;
        f.server.releasePlaylistWrites();
        QTRY_COMPARE(writesOf(f.server), 4);
        QTRY_VERIFY(!entries.writing());
        QCOMPARE(f.server.playlistItemIds(first), ordered(ids, {3, 1, 0}));
        QCOMPARE(f.server.playlistItemIds(second), (QStringList{ids.at(2), ids.at(3)}));
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(f.rowIds(entries), f.server.playlistItemIds(second));
        const auto writes = writeRequests(f.server);
        for (int i = 0; i < 3; ++i)
            QVERIFY2(writes.at(i).route.contains(first.toUtf8()), writes.at(i).route.constData());
        QVERIFY2(writes.at(3).route.contains(second.toUtf8()), writes.at(3).route.constData());
        QCOMPARE(entries.playlistId(), second);
        QCOMPARE(entries.editError(), PlaylistEntriesModel::NoEditError);
    }

    void aPlaylistShownAgainWaitsForItsWriteStillOnItsWay()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        const QString first = f.server.seedPlaylist(QStringLiteral("First"), ids);
        const QString second = f.server.seedPlaylist(QStringLiteral("Second"), {ids.at(0)});
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(first);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        f.server.holdPlaylistWrites = true;
        QVERIFY(entries.move(0, 2));
        QTRY_VERIFY(f.server.heldPlaylistCount() == 1);
        entries.load(second);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        // Back on the first playlist, a read now could still show the order before the move: it waits
        const int reads = readsOf(f.server);
        entries.load(first);
        QVERIFY(entries.writing());
        QCOMPARE(entries.status(), PlaylistEntriesModel::Loading);
        QVERIFY(!entries.canMove(0));
        QTest::qWait(100);
        QCOMPARE(readsOf(f.server), reads);
        f.server.holdPlaylistWrites = false;
        f.server.releasePlaylistWrites();
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(readsOf(f.server), reads + 1);
        QCOMPARE(f.rowIds(entries), ordered(ids, {1, 2, 0}));
        QVERIFY(entries.canMove(0));
    }

    void aWriteThatFailsAfterThePageLeftReadsNothing()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        PlaylistEntriesModel entries(&f.client);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        f.server.holdPlaylistWrites = true;
        f.server.playlistWriteStatus = 500;
        QVERIFY(entries.move(0, 2));
        QTRY_VERIFY(f.server.heldPlaylistCount() == 1);
        entries.close();
        const int reads = readsOf(f.server);
        f.server.releasePlaylistWrites();
        QTRY_VERIFY(!entries.writing());
        QTest::qWait(100);
        QCOMPARE(readsOf(f.server), reads);
        // The next visit reads the server's order, and edits wait for it
        QVERIFY(!entries.canMove(0));
        f.server.holdPlaylistWrites = false;
        f.server.playlistWriteStatus = 0;
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        QCOMPARE(f.rowIds(entries), ids);
        QVERIFY(entries.canMove(0));
    }

    void theSessionEndClearsTheWritesOnTheirWay()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        f.server.holdPlaylistWrites = true;
        QVERIFY(entries.move(0, 1));
        QVERIFY(entries.move(1, 2));
        QVERIFY(entries.writing());
        QTRY_VERIFY(f.server.heldPlaylistCount() == 1);
        f.client.disconnectFromServer();
        QTRY_COMPARE(entries.count(), 0);
        QVERIFY(!entries.writing());
        QVERIFY(entries.playlistId().isEmpty());
        QCOMPARE(entries.status(), PlaylistEntriesModel::Idle);
        QCOMPARE(entries.editError(), PlaylistEntriesModel::NoEditError);
        f.server.holdPlaylistWrites = false;
        f.server.releasePlaylistWrites();
        QTest::qWait(100);
        QVERIFY(!entries.writing());
        QCOMPARE(entries.count(), 0);
        QCOMPARE(entries.editError(), PlaylistEntriesModel::NoEditError);
        QCOMPARE(writesOf(f.server), 1);
    }

    void theSessionEndClearsAnEditError()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QStringList ids = f.movies(3);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Evening"), ids);
        PlaylistEntriesModel entries(&f.client);
        QAbstractItemModelTester tester(&entries);
        entries.load(playlist);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        f.server.playlistWriteStatus = 500;
        QVERIFY(entries.move(0, 1));
        QTRY_COMPARE(entries.editError(), PlaylistEntriesModel::EditFailed);
        QTRY_COMPARE(entries.status(), PlaylistEntriesModel::Ready);
        f.client.disconnectFromServer();
        QTRY_COMPARE(entries.count(), 0);
        QCOMPARE(entries.editError(), PlaylistEntriesModel::NoEditError);
        QVERIFY(!entries.writing());
    }

    void renameSendsTheNormalizedNameAndReloadsTheList()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Old"), {});
        f.server.seedPlaylist(QStringLiteral("Other"), {});
        PlaylistsController playlists(&f.client);
        QAbstractItemModelTester tester(playlists.list());
        playlists.reload();
        QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
        QCOMPARE(playlists.nameOf(playlist), QStringLiteral("Old"));
        const int listed = f.listings();
        QSignalSpy renamed(&playlists, &PlaylistsController::renamed);
        QSignalSpy failed(&playlists, &PlaylistsController::renameFailed);
        QSignalSpy writingChanges(&playlists, &PlaylistsController::writingChanged);
        QVERIFY(!playlists.writing());
        QVERIFY(playlists.rename(playlist, QStringLiteral("  New  ")));
        QVERIFY(playlists.writing());
        QTRY_COMPARE(renamed.size(), 1);
        QCOMPARE(renamed.constFirst().at(0).toString(), playlist);
        QCOMPARE(renamed.constFirst().at(1).toString(), QStringLiteral("New"));
        QCOMPARE(f.server.playlistById(playlist)->name, QStringLiteral("New"));
        QCOMPARE(failed.size(), 0);
        QVERIFY(!playlists.writing());
        QCOMPARE(writingChanges.size(), 2);
        const auto writes = writeRequests(f.server);
        QCOMPARE(writes.size(), 1);
        QCOMPARE(QJsonDocument::fromJson(writes.constFirst().body)
                     .object()
                     .value(QStringLiteral("Name"))
                     .toString(),
                 QStringLiteral("New"));
        QTRY_COMPARE(f.listings(), listed + 1);
        QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
        QCOMPARE(playlists.nameOf(playlist), QStringLiteral("New"));
    }

    void aFailedRenameSaysWhetherItWasForbidden()
    {
        const QList<std::pair<int, bool>> failures{{403, true}, {500, false}};
        for (const auto &failure : failures) {
            Fixture f;
            QVERIFY(f.ok);
            const QString playlist = f.server.seedPlaylist(QStringLiteral("Old"), {});
            PlaylistsController playlists(&f.client);
            playlists.reload();
            QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
            const int listed = f.listings();
            QSignalSpy renamed(&playlists, &PlaylistsController::renamed);
            QSignalSpy failed(&playlists, &PlaylistsController::renameFailed);
            f.server.playlistWriteStatus = failure.first;
            QVERIFY(playlists.rename(playlist, QStringLiteral("New")));
            QTRY_COMPARE(failed.size(), 1);
            QCOMPARE(failed.constFirst().at(0).toString(), playlist);
            QCOMPARE(failed.constFirst().at(1).toBool(), failure.second);
            QCOMPARE(renamed.size(), 0);
            QVERIFY(!playlists.writing());
            QCOMPARE(f.server.playlistById(playlist)->name, QStringLiteral("Old"));
            QTRY_COMPARE(f.listings(), listed + 1);
            QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
            QCOMPARE(playlists.nameOf(playlist), QStringLiteral("Old"));
        }
    }

    void anInvalidNameOrIdIsRefusedWithoutARequest()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Old"), {});
        PlaylistsController playlists(&f.client);
        QSignalSpy renamed(&playlists, &PlaylistsController::renamed);
        QSignalSpy failed(&playlists, &PlaylistsController::renameFailed);
        QSignalSpy deleteFailed(&playlists, &PlaylistsController::deleteFailed);
        for (const QString &name : {QString(), QStringLiteral("   "), QStringLiteral("\n"),
                                    QString(Kinefin::MaxPlaylistNameChars + 1, QLatin1Char('n'))})
            QVERIFY2(!playlists.rename(playlist, name), qPrintable(name));
        QVERIFY(!playlists.rename(QStringLiteral("../Users"), QStringLiteral("New")));
        QVERIFY(!playlists.rename(QString(), QStringLiteral("New")));
        QVERIFY(!playlists.deletePlaylist(QStringLiteral("../Users")));
        QVERIFY(!playlists.deletePlaylist(QString()));
        QVERIFY(!playlists.writing());
        QTest::qWait(100);
        QVERIFY(f.server.playlistRequests.isEmpty());
        QCOMPARE(renamed.size() + failed.size() + deleteFailed.size(), 0);
        QCOMPARE(f.server.playlistById(playlist)->name, QStringLiteral("Old"));
    }

    void validNameTrimsAndRefusesEmptyNames()
    {
        Kinefin::JellyfinClient client;
        PlaylistsController playlists(&client);
        QCOMPARE(playlists.validName(QStringLiteral("  New  ")), QStringLiteral("New"));
        QCOMPARE(playlists.validName(QStringLiteral("Weekend")), QStringLiteral("Weekend"));
        QVERIFY(playlists.validName(QString()).isEmpty());
        QVERIFY(playlists.validName(QStringLiteral("   ")).isEmpty());
        QVERIFY(playlists.validName(QStringLiteral("\t\n")).isEmpty());
    }

    void deleteRemovesThePlaylistAndReloadsTheList()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString doomed = f.server.seedPlaylist(QStringLiteral("Doomed"), {});
        const QString other = f.server.seedPlaylist(QStringLiteral("Other"), {});
        PlaylistsController playlists(&f.client);
        QAbstractItemModelTester tester(playlists.list());
        playlists.reload();
        QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
        QCOMPARE(playlists.list()->count(), 2);
        const int listed = f.listings();
        QSignalSpy deleted(&playlists, &PlaylistsController::deleted);
        QSignalSpy failed(&playlists, &PlaylistsController::deleteFailed);
        QSignalSpy writingChanges(&playlists, &PlaylistsController::writingChanged);
        QVERIFY(playlists.deletePlaylist(doomed));
        QVERIFY(playlists.writing());
        QTRY_COMPARE(deleted.size(), 1);
        QCOMPARE(deleted.constFirst().at(0).toString(), doomed);
        QCOMPARE(failed.size(), 0);
        QVERIFY(!playlists.writing());
        QCOMPARE(writingChanges.size(), 2);
        QVERIFY(!f.server.playlistById(doomed));
        QVERIFY(f.server.playlistById(other));
        QCOMPARE(requestsOf(f.server, "DELETE"), 1);
        QTRY_COMPARE(f.listings(), listed + 1);
        QTRY_COMPARE(playlists.list()->count(), 1);
        QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
        QVERIFY(playlists.nameOf(doomed).isEmpty());
        QCOMPARE(playlists.nameOf(other), QStringLiteral("Other"));
    }

    void aPlaylistTheUserMayNotDeleteStays()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Theirs"), {});
        PlaylistsController playlists(&f.client);
        playlists.reload();
        QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
        const int listed = f.listings();
        QSignalSpy deleted(&playlists, &PlaylistsController::deleted);
        QSignalSpy failed(&playlists, &PlaylistsController::deleteFailed);
        f.server.playlistsDeletable = false;
        QVERIFY(playlists.deletePlaylist(playlist));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().at(0).toString(), playlist);
        QVERIFY(failed.constFirst().at(1).toBool());
        QCOMPARE(deleted.size(), 0);
        QVERIFY(!playlists.writing());
        QCOMPARE(requestsOf(f.server, "DELETE"), 0);
        QVERIFY(f.server.playlistById(playlist));
        QTRY_COMPARE(f.listings(), listed + 1);
        QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
        QCOMPARE(playlists.list()->count(), 1);
        QVERIFY(f.client.isAuthenticated());
    }

    void aFailedDeleteSaysWhetherItWasForbidden()
    {
        const QList<std::pair<int, bool>> failures{{403, true}, {500, false}};
        for (const auto &failure : failures) {
            Fixture f;
            QVERIFY(f.ok);
            const QString playlist = f.server.seedPlaylist(QStringLiteral("Mine"), {});
            PlaylistsController playlists(&f.client);
            playlists.reload();
            QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
            QSignalSpy deleted(&playlists, &PlaylistsController::deleted);
            QSignalSpy failed(&playlists, &PlaylistsController::deleteFailed);
            f.server.playlistWriteStatus = failure.first;
            QVERIFY(playlists.deletePlaylist(playlist));
            QTRY_COMPARE(failed.size(), 1);
            QCOMPARE(failed.constFirst().at(0).toString(), playlist);
            QCOMPARE(failed.constFirst().at(1).toBool(), failure.second);
            QCOMPARE(deleted.size(), 0);
            QVERIFY(!playlists.writing());
            QCOMPARE(requestsOf(f.server, "DELETE"), 1);
            QVERIFY(f.server.playlistById(playlist));
            QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
            QCOMPARE(playlists.list()->count(), 1);
        }
    }

    void theSessionEndClearsTheControllerWrites()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString playlist = f.server.seedPlaylist(QStringLiteral("Old"), {});
        PlaylistsController playlists(&f.client);
        playlists.reload();
        QTRY_COMPARE(playlists.status(), PlaylistsController::Ready);
        QSignalSpy renamed(&playlists, &PlaylistsController::renamed);
        QSignalSpy failed(&playlists, &PlaylistsController::renameFailed);
        QSignalSpy writingChanges(&playlists, &PlaylistsController::writingChanged);
        f.server.holdPlaylistWrites = true;
        QVERIFY(playlists.rename(playlist, QStringLiteral("New")));
        QVERIFY(playlists.writing());
        QTRY_VERIFY(f.server.heldPlaylistCount() == 1);
        f.client.disconnectFromServer();
        QVERIFY(!playlists.writing());
        QCOMPARE(writingChanges.size(), 2);
        QCOMPARE(playlists.list()->count(), 0);
        QCOMPARE(playlists.status(), PlaylistsController::Idle);
        f.server.holdPlaylistWrites = false;
        f.server.releasePlaylistWrites();
        QTest::qWait(100);
        QCOMPARE(renamed.size() + failed.size(), 0);
        QVERIFY(!playlists.writing());
    }
};

QTEST_GUILESS_MAIN(TestPlaylists)
#include "test_playlists.moc"
