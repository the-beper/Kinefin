// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The list of playlists and the entries of one against the fake server: what is asked, the order, the caps, the channels and the end of a session
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/PlaylistEntriesModel.h"
#include "models/PlaylistsController.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::ItemRowModel;
using Kinefin::PlaylistEntriesModel;
using Kinefin::PlaylistsController;

namespace {
QString movieId(int number)
{
    return QStringLiteral("d1%1").arg(number, 30, 16, QLatin1Char('0'));
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
};

QTEST_GUILESS_MAIN(TestPlaylists)
#include "test_playlists.moc"
