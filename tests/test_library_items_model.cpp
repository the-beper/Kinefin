// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The paging model of the library grid against the fake server: placeholders, one page in flight, the most recently read pages first, least-recently-used eviction, a changing total, failures and the loss of the session
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/ItemSummary.h"
#include "models/LibraryItemsModel.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTimeZone>
#include <QtTest/QtTest>

using Kinefin::LibraryItemsModel;

namespace {
const QString kMoviesLibrary = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
const QString kSciFi = QStringLiteral("aa00000000000000000000000000ff01");
const QString kMystery = QStringLiteral("bb00000000000000000000000000ff02");

struct Fixture {
    Fixture()
    {
        server.generatedItemTotal = 250;
        ok = server.listen() && login(client, server);
    }

    // True once the row is a real item, reading it is what asks for its page
    bool loadedRow(int row)
    {
        return model.data(model.index(row, 0), LibraryItemsModel::LoadedRole).toBool();
    }
    bool openMovies()
    {
        return model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"));
    }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    LibraryItemsModel model{&client, LibraryItemsModel::TileChannel};
    bool ok = false;
};

Kinefin::ItemSummary summaryOf(QJsonObject item)
{
    item.insert(QStringLiteral("Id"), QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f"));
    return Kinefin::parseItemSummary(item);
}

QJsonObject userData(double positionTicks)
{
    return {{QStringLiteral("UserData"),
             QJsonObject{{QStringLiteral("PlaybackPositionTicks"), positionTicks}}}};
}

// How many item requests carried a search term
int searchRequests(const Fixture &f)
{
    int count = 0;
    for (const QByteArray &route : f.server.itemsRoutes)
        count += route.contains("searchTerm=") ? 1 : 0;
    return count;
}
} // namespace

class TestLibraryItemsModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void browseQueryOnlyForBrowsableTypes()
    {
        QVERIFY(LibraryItemsModel::browseQuery(QStringLiteral("movies")));
        QCOMPARE(LibraryItemsModel::browseQuery(QStringLiteral("tvshows"))->includeItemTypes,
                 QStringLiteral("Series"));
        QCOMPARE(LibraryItemsModel::browseQuery(QStringLiteral("boxsets"))->includeItemTypes,
                 QStringLiteral("BoxSet"));
        QVERIFY(!LibraryItemsModel::browseQuery(QStringLiteral("music")));
        QVERIFY(!LibraryItemsModel::browseQuery(QString()));
    }

    void openRefusesWhatCannotBeBrowsed()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(!f.model.open(QStringLiteral("bad id"), QStringLiteral("movies")));
        QVERIFY(!f.model.open(kMoviesLibrary, QStringLiteral("music")));
        QCOMPARE(f.model.status(), LibraryItemsModel::Idle);
        QCOMPARE(f.model.rowCount(), 0);
        QCOMPARE(f.server.itemsRoutes.size(), 0);
    }

    void firstPageSetsTotalAndPlaceholders()
    {
        Fixture f;
        QVERIFY(f.ok);
        QAbstractItemModelTester tester(&f.model);
        QVERIFY(f.openMovies());
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(f.model.totalCount(), 250);
        QCOMPARE(f.model.rowCount(), 250);
        QCOMPARE(f.model.libraryName(), QStringLiteral("Movies"));
        QVERIFY(f.model.resumableAvailable());
        const QModelIndex first = f.model.index(0);
        QVERIFY(f.model.data(first, LibraryItemsModel::LoadedRole).toBool());
        QCOMPARE(f.model.data(first, LibraryItemsModel::NameRole).toString(),
                 QStringLiteral("Item 0"));
        QCOMPARE(f.model.data(first, LibraryItemsModel::YearRole).toInt(), 2000);
        const QModelIndex placeholder = f.model.index(150);
        QVERIFY(!f.model.data(placeholder, LibraryItemsModel::LoadedRole).toBool());
        QVERIFY(f.model.data(placeholder, LibraryItemsModel::NameRole).toString().isEmpty());
        QVERIFY(f.model.data(placeholder, LibraryItemsModel::ImageUrlRole).toUrl().isEmpty());
    }

    void wireCarriesTheLibraryQuery()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QString route = QString::fromUtf8(f.server.itemsRoutes.constFirst());
        const QStringList parts{QString(QStringLiteral("parentId=") + kMoviesLibrary),
                                QStringLiteral("includeItemTypes=Movie"),
                                QStringLiteral("recursive=true"),
                                QStringLiteral("sortBy=SortName"),
                                QStringLiteral("sortOrder=Ascending"),
                                QStringLiteral("startIndex=0"),
                                QStringLiteral("limit=100")};
        for (const QString &part : parts)
            QVERIFY2(route.contains(part), qPrintable(part + QStringLiteral(" in ") + route));
    }

    void imageUrlsAreServerBaseOnly()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QUrl url = f.model.data(f.model.index(3), LibraryItemsModel::ImageUrlRole).toUrl();
        QVERIFY(!url.isEmpty());
        QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(f.client.serverUrl(), url));
        QVERIFY(!url.toString().contains(QString::fromUtf8(kFixtureToken)));
    }

    void readingAnUnloadedRowFetchesItsPageOnce()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        for (int i = 0; i < 5; ++i)
            f.loadedRow(150);
        QTRY_VERIFY(f.loadedRow(150));
        int secondPageRequests = 0;
        for (const QByteArray &route : f.server.itemsRoutes)
            secondPageRequests += route.contains("startIndex=100") ? 1 : 0;
        QCOMPARE(secondPageRequests, 1);
        QCOMPARE(f.model.data(f.model.index(150), LibraryItemsModel::NameRole).toString(),
                 QStringLiteral("Item 150"));
    }

    void onlyOnePageIsInFlight()
    {
        Fixture f;
        f.server.generatedItemTotal = 1000;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const int before = f.server.itemsRoutes.size();
        f.server.holdListings = true;
        f.loadedRow(150);
        f.loadedRow(250);
        QTRY_COMPARE(f.server.itemsRoutes.size(), before + 1);
        QTest::qWait(200);
        QCOMPARE(f.server.itemsRoutes.size(), before + 1);
        f.model.clear();
    }

    void wantedPagesKeepTheMostRecentReads()
    {
        Fixture f;
        f.server.generatedItemTotal = 1000;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const int before = f.server.itemsRoutes.size();
        f.server.holdListings = true;
        for (int page = 1; page <= 6; ++page)
            f.loadedRow(page * 100);
        QTRY_COMPARE(f.server.itemsRoutes.size(), before + 1);
        // The last read page goes first
        QVERIFY(f.server.itemsRoutes.last().contains("startIndex=600"));
        f.model.clear();
    }

    void residentPagesAreEvictedLeastRecentlyUsedFirst()
    {
        Fixture f;
        f.server.generatedItemTotal = 1200;
        QVERIFY(f.ok);
        QAbstractItemModelTester tester(&f.model);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        for (int page = 1; page <= 7; ++page) {
            f.loadedRow(page * 100);
            QTRY_VERIFY(f.loadedRow(page * 100));
        }
        QCOMPARE(f.model.residentPages().size(), 8);
        // Reading page 0 again makes page 1 the least recently used one, so a first-in-first-out eviction would drop page 0 instead
        QVERIFY(f.loadedRow(0));
        f.loadedRow(800);
        QTRY_VERIFY(f.loadedRow(800));
        QVERIFY(f.model.residentPages().contains(0));
        QVERIFY(!f.model.residentPages().contains(1));
        QCOMPARE(f.model.residentPages().size(), 8);
        QCOMPARE(f.model.rowCount(), 1200);
    }

    void totalChangeResizesAndDropsStalePages()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.loadedRow(100);
        QTRY_VERIFY(f.loadedRow(100));
        f.server.generatedItemTotal = 300;
        f.loadedRow(200);
        QTRY_COMPARE(f.model.rowCount(), 300);
        QTRY_COMPARE(f.model.residentPages(), (QList<int>{2}));
        QVERIFY(f.model.data(f.model.index(200), LibraryItemsModel::LoadedRole).toBool());
    }

    void shortPageShrinksInsteadOfLooping()
    {
        Fixture f;
        f.server.generatedAvailable = 130;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.loadedRow(120);
        QTRY_COMPARE(f.model.rowCount(), 130);
        QTest::qWait(300);
        QCOMPARE(f.server.itemsRoutes.size(), 2);
        QCOMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(f.model.data(f.model.index(129), LibraryItemsModel::LoadedRole).toBool());
    }

    void shrinkDropsResidentPagesPastTheEndAndTheirFailures()
    {
        Fixture f;
        f.server.generatedItemTotal = 450;
        QVERIFY(f.ok);
        QAbstractItemModelTester tester(&f.model);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        for (const int row : {250, 350, 420}) {
            f.loadedRow(row);
            QTRY_VERIFY(f.loadedRow(row));
        }
        // The library shrank to 230 rows: page 2 is cut, pages 3 and 4 are gone
        f.server.generatedItemTotal = 230;
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        f.loadedRow(120);
        QTRY_VERIFY(f.model.pageError());
        f.server.listingStatus.clear();
        f.model.retry();
        QTRY_COMPARE(f.model.rowCount(), 230);
        QTRY_VERIFY(!f.model.pageError());
        for (const int page : f.model.residentPages())
            QVERIFY(page * LibraryItemsModel::PageSize < 230);
        QVERIFY(f.loadedRow(120));
    }

    void sortOrFilterChangeResetsAndSupersedes()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QSignalSpy resets(&f.model, &QAbstractItemModel::modelReset);
        f.model.setSortKey(LibraryItemsModel::DateAdded);
        f.model.setDescending(true);
        f.model.setFilter(LibraryItemsModel::Unplayed);
        QCOMPARE(resets.size(), 3);
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        // The superseded requests may or may not have reached the server before their abort: the wanted one is the one that carries all three changes
        bool found = false;
        for (const QByteArray &raw : f.server.itemsRoutes) {
            const QString route = QString::fromUtf8(raw);
            found = found
                    || (route.contains(QStringLiteral("DateCreated"))
                        && route.contains(QStringLiteral("sortOrder=Descending"))
                        && route.contains(QStringLiteral("filters=IsUnplayed"))
                        && route.contains(QStringLiteral("startIndex=0")));
        }
        QVERIFY(found);
        QCOMPARE(f.model.rowCount(), 250);
    }

    void anotherLibraryStartsFromTheDefaultOrder()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.model.setSortKey(LibraryItemsModel::Runtime);
        f.model.setDescending(true);
        f.model.setFilter(LibraryItemsModel::Resumable);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        // The same library keeps what the user chose, another one starts over
        QVERIFY(f.openMovies());
        QCOMPARE(f.model.sortKey(), LibraryItemsModel::Runtime);
        QVERIFY(f.model.open(QStringLiteral("7f1b8f4b3e2148c5a8f2b7c9d0e1f2a3"),
                             QStringLiteral("tvshows"), QStringLiteral("Series")));
        QCOMPARE(f.model.sortKey(), LibraryItemsModel::Name);
        QVERIFY(!f.model.descending());
        QCOMPARE(f.model.filter(), LibraryItemsModel::All);
        QVERIFY(!f.model.resumableAvailable());
    }

    void genreJoinsTheFilterAndTheWire()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.itemsByGenre.insert(kSciFi, QStringLiteral("items_genre_scifi"));
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTRY_VERIFY(f.model.genreOptions()->available());
        QVERIFY(!f.model.filtered());
        QSignalSpy filtered(&f.model, &LibraryItemsModel::filteredChanged);
        f.model.setFilter(LibraryItemsModel::Unplayed);
        f.model.setGenreIds({kSciFi, kMystery});
        QCOMPARE(f.model.genreIds(), QStringList({kSciFi, kMystery}));
        QVERIFY(f.model.filtered());
        QCOMPARE(filtered.size(), 1);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QString route = QString::fromUtf8(f.server.itemsRoutes.constLast());
        QVERIFY2(
            route.contains(QStringLiteral("genreIds=") + kSciFi + QStringLiteral("%7C") + kMystery)
                || route.contains(QStringLiteral("genreIds=") + kSciFi + QLatin1Char('|')
                                  + kMystery),
            qPrintable(route));
        QVERIFY(route.contains(QStringLiteral("filters=IsUnplayed")));
        f.model.setGenreIds({});
        f.model.setFilter(LibraryItemsModel::All);
        QVERIFY(!f.model.filtered());
    }

    void genreIdsAreCleanedAndBounded()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.model.setGenreIds(
            {kSciFi, kSciFi.toUpper(), QStringLiteral("../etc"), QString(), kMystery});
        QCOMPARE(f.model.genreIds(), QStringList({kSciFi, kMystery}));
        QStringList many;
        for (int i = 0; i < 80; ++i)
            many.append(QStringLiteral("%1").arg(i + 1, 32, 16, QLatin1Char('e')));
        f.model.setGenreIds(many);
        QCOMPARE(f.model.genreIds().size(), Kinefin::MaxGenreIds);
    }

    void aFailedOptionsLoadDropsTheGenres()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_VERIFY(f.model.genreOptions()->available());
        f.model.setGenreIds({kSciFi});
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.genreOptions()->status(), Kinefin::FilterOptionsModel::Failed);
        // The menu that clears the genres is hidden while the options are unavailable: the filter goes with them
        QVERIFY(f.model.genreIds().isEmpty());
        QVERIFY(!f.model.filtered());
    }

    void anotherLibraryDropsTheGenreAndTheSameKeepsIt()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTRY_VERIFY(f.model.genreOptions()->available());
        f.model.setGenreIds({kSciFi});
        QVERIFY(f.openMovies());
        QCOMPARE(f.model.genreIds(), QStringList({kSciFi}));
        QTRY_COMPARE(f.model.genreOptions()->status(), Kinefin::FilterOptionsModel::Ready);
        QCOMPARE(f.model.genreIds(), QStringList({kSciFi}));
        QVERIFY(f.model.open(QStringLiteral("7f1b8f4b3e2148c5a8f2b7c9d0e1f2a3"),
                             QStringLiteral("tvshows"), QStringLiteral("Series")));
        QVERIFY(f.model.genreIds().isEmpty());
        QVERIFY(!f.model.filtered());
    }

    void optionsPruneAGenreTheLibraryNoLongerHas()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_VERIFY(f.model.genreOptions()->available());
        const QString gone = QStringLiteral("dd00000000000000000000000000ff09");
        f.model.setGenreIds({kSciFi, gone});
        QCOMPARE(f.model.genreIds().size(), 2);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.genreIds(), QStringList({kSciFi}));
    }

    void clearDropsTheOptions()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_VERIFY(f.model.genreOptions()->available());
        f.model.clear();
        QVERIFY(!f.model.genreOptions()->available());
        QCOMPARE(f.model.genreOptions()->status(), Kinefin::FilterOptionsModel::Idle);
    }

    void allScopeSendsNoParentId()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(!f.model.open(QStringLiteral("bad id"), QStringLiteral("movies")));
        QVERIFY(!f.model.isOpen());
        QVERIFY(f.model.open(QString(), QStringLiteral("movies"), QStringLiteral("Movies")));
        QVERIFY(f.model.isOpen());
        QVERIFY(f.model.libraryId().isEmpty());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(f.model.rowCount(), 250);
        QTRY_VERIFY(f.model.genreOptions()->available());
        const QByteArray itemsRoute = f.server.itemsRoutes.constFirst();
        QVERIFY2(!itemsRoute.contains("parentId="), itemsRoute.constData());
        QVERIFY(itemsRoute.contains("includeItemTypes=Movie"));
        QVERIFY(itemsRoute.contains("recursive=true"));
        const QByteArray filtersRoute =
            f.server.listingRoutes.value(QStringLiteral("Filters")).constFirst();
        QVERIFY2(!filtersRoute.contains("parentId="), filtersRoute.constData());
        f.model.clear();
        QVERIFY(!f.model.isOpen());
        QCOMPARE(f.model.status(), LibraryItemsModel::Idle);
    }

    void allScopeHonoursSortAndReplies()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open(QString(), QStringLiteral("movies")));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.model.setSortKey(LibraryItemsModel::DateAdded);
        f.model.setDescending(true);
        f.model.setFilter(LibraryItemsModel::Unplayed);
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QString route = QString::fromUtf8(f.server.itemsRoutes.constLast());
        QVERIFY2(route.contains(QStringLiteral("DateCreated"))
                     && route.contains(QStringLiteral("sortOrder=Descending"))
                     && route.contains(QStringLiteral("filters=IsUnplayed"))
                     && !route.contains(QStringLiteral("parentId=")),
                 qPrintable(route));
        QCOMPARE(f.model.rowCount(), 250);
        // Reading a placeholder pages in under the all scope as well
        f.loadedRow(150);
        QTRY_VERIFY(f.loadedRow(150));
        QVERIFY(!QString::fromUtf8(f.server.itemsRoutes.constLast())
                     .contains(QStringLiteral("parentId=")));
    }

    void twoModelsOnOneClientSeeOnlyTheirReplies()
    {
        Fixture f;
        QVERIFY(f.ok);
        LibraryItemsModel movies(&f.client, LibraryItemsModel::MoviesChannel);
        LibraryItemsModel shows(&f.client, LibraryItemsModel::ShowsChannel);
        // The movies request is held: the shows answer on the other channel must not complete it
        f.server.holdFirstListings = 1;
        QVERIFY(movies.open(QString(), QStringLiteral("movies")));
        QTRY_COMPARE(f.server.itemsRoutes.size(), 1);
        QVERIFY(shows.open(QString(), QStringLiteral("tvshows")));
        QTRY_COMPARE(shows.status(), LibraryItemsModel::Ready);
        QCOMPARE(shows.rowCount(), 250);
        QCOMPARE(movies.status(), LibraryItemsModel::Loading);
        QCOMPARE(movies.rowCount(), 0);
        // Another request on its own channel supersedes the held one
        movies.setSortKey(LibraryItemsModel::Runtime);
        QTRY_COMPARE(movies.status(), LibraryItemsModel::Ready);
        QCOMPARE(movies.rowCount(), 250);
        // The genre options follow their own channel just the same
        QTRY_VERIFY(movies.genreOptions()->available());
        QTRY_VERIFY(shows.genreOptions()->available());
        // A failure on one channel leaves the other model alone
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        shows.setSortKey(LibraryItemsModel::Runtime);
        QTRY_COMPARE(shows.status(), LibraryItemsModel::Failed);
        QCOMPARE(movies.status(), LibraryItemsModel::Ready);
    }

    void searchIsDebounced()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.model.setSearchDebounceInterval(50);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(!f.model.filtered());
        QSignalSpy filtered(&f.model, &LibraryItemsModel::filteredChanged);
        f.model.setSearchTerm(QStringLiteral("du"));
        f.model.setSearchTerm(QStringLiteral("dun"));
        f.model.setSearchTerm(QStringLiteral("dune"));
        QCOMPARE(f.model.searchTerm(), QStringLiteral("dune"));
        // Nothing is sent before the pause
        QCOMPARE(searchRequests(f), 0);
        QTRY_COMPARE(searchRequests(f), 1);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTest::qWait(200);
        QCOMPARE(searchRequests(f), 1);
        QVERIFY(f.server.itemsRoutes.constLast().contains("searchTerm=dune"));
        QVERIFY(f.model.filtered());
        QCOMPARE(filtered.size(), 1);
    }

    void shortTermReloadsAtOnce()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.model.setSearchDebounceInterval(20);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.model.setSearchTerm(QStringLiteral("dune"));
        QTRY_COMPARE(searchRequests(f), 1);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const int before = f.server.itemsRoutes.size();
        // A pause that long would show if the reload waited for it
        f.model.setSearchDebounceInterval(10000);
        f.model.setSearchTerm(QStringLiteral("d"));
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QVERIFY(!f.model.filtered());
        QTRY_COMPARE(f.server.itemsRoutes.size(), before + 1);
        QVERIFY(!f.server.itemsRoutes.constLast().contains("searchTerm="));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
    }

    void sameNormalisedTermSendsNothing()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.model.setSearchDebounceInterval(20);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const int base = f.server.itemsRoutes.size();
        // Too short from the start: there is nothing to drop and nothing to ask
        f.model.setSearchTerm(QStringLiteral("d"));
        f.model.setSearchTerm(QStringLiteral("  "));
        QTest::qWait(100);
        QCOMPARE(f.server.itemsRoutes.size(), base);
        f.model.setSearchTerm(QStringLiteral("dune"));
        QTRY_COMPARE(f.server.itemsRoutes.size(), base + 1);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        // The same words once normalised
        f.model.setSearchTerm(QStringLiteral("  dune "));
        f.model.setSearchTerm(QStringLiteral("dune"));
        f.model.setSearchTerm(QStringLiteral("dune\t"));
        QTest::qWait(150);
        QCOMPARE(f.server.itemsRoutes.size(), base + 1);
        QCOMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(f.model.searchTerm(), QStringLiteral("dune\t"));
    }

    void searchTermSurvivesAnOpenAndClearDropsIt()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.model.setSearchDebounceInterval(20);
        QVERIFY(f.model.open(QString(), QStringLiteral("movies")));
        f.model.setSearchTerm(QStringLiteral("dune"));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTRY_VERIFY(f.server.itemsRoutes.constLast().contains("searchTerm=dune"));
        // The selector of the tab changes the scope, not the search
        QVERIFY(f.openMovies());
        QCOMPARE(f.model.searchTerm(), QStringLiteral("dune"));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(f.server.itemsRoutes.constLast().contains("searchTerm=dune"));
        QVERIFY(f.server.itemsRoutes.constLast().contains("parentId="));
        QVERIFY(f.model.filtered());
        f.model.clear();
        QVERIFY(f.model.searchTerm().isEmpty());
        QVERIFY(!f.model.filtered());
    }

    void pruneIsNotAnEdit()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        const QString gone = QStringLiteral("dd00000000000000000000000000ff09");
        const LibraryItemsModel::ViewState initial{
            LibraryItemsModel::Runtime, true, LibraryItemsModel::Unplayed, {kSciFi, gone}};
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        // The initial state is applied before the first request and is not an edit
        QCOMPARE(f.model.sortKey(), LibraryItemsModel::Runtime);
        QVERIFY(f.model.descending());
        QCOMPARE(f.model.filter(), LibraryItemsModel::Unplayed);
        QCOMPARE(f.model.genreIds(), QStringList({kSciFi, gone}));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QString route = QString::fromUtf8(f.server.itemsRoutes.constFirst());
        QVERIFY2(route.contains(QStringLiteral("Runtime"))
                     && route.contains(QStringLiteral("sortOrder=Descending"))
                     && route.contains(QStringLiteral("filters=IsUnplayed")),
                 qPrintable(route));
        // The options drop the genre the library no longer has
        QTRY_COMPARE(f.model.genreIds(), QStringList({kSciFi}));
        QCOMPARE(edits.size(), 0);
        QCOMPARE(f.model.viewState().genreIds, QStringList({kSciFi}));
        // The search term is not part of the view state either
        f.model.setSearchDebounceInterval(20);
        f.model.setSearchTerm(QStringLiteral("dune"));
        QTRY_COMPARE(searchRequests(f), 1);
        QCOMPARE(edits.size(), 0);
        // The user's own changes are edits, one each, and a repeated value is none
        f.model.setSortKey(LibraryItemsModel::Name);
        f.model.setSortKey(LibraryItemsModel::Name);
        f.model.setDescending(false);
        f.model.setFilter(LibraryItemsModel::All);
        f.model.setGenreIds({});
        f.model.setGenreIds({});
        QCOMPARE(edits.size(), 4);
        const LibraryItemsModel::ViewState now = f.model.viewState();
        QCOMPARE(now.sort, LibraryItemsModel::Name);
        QVERIFY(!now.descending);
        QCOMPARE(now.filter, LibraryItemsModel::All);
        QVERIFY(now.genreIds.isEmpty());
    }

    void termClearsOnSessionLoss()
    {
        {
            Fixture f;
            QVERIFY(f.ok);
            f.model.setSearchDebounceInterval(20);
            QVERIFY(f.openMovies());
            f.model.setSearchTerm(QStringLiteral("dune"));
            QTRY_COMPARE(searchRequests(f), 1);
            QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
            QSignalSpy terms(&f.model, &LibraryItemsModel::searchTermChanged);
            f.client.disconnectFromServer();
            QVERIFY(f.model.searchTerm().isEmpty());
            QVERIFY(!f.model.filtered());
            QCOMPARE(terms.size(), 1);
            QCOMPARE(f.model.status(), LibraryItemsModel::Idle);
        }
        {
            // A term still waiting for its pause never fires after the session is gone
            Fixture f;
            QVERIFY(f.ok);
            f.model.setSearchDebounceInterval(50);
            QVERIFY(f.openMovies());
            QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
            f.model.setSearchTerm(QStringLiteral("dune"));
            f.client.disconnectFromServer();
            QTest::qWait(200);
            QCOMPARE(searchRequests(f), 0);
            QVERIFY(f.model.searchTerm().isEmpty());
        }
    }

    void totalIsClamped()
    {
        Fixture f;
        f.server.generatedItemTotal = 2000000000;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(f.model.rowCount(), LibraryItemsModel::MaxRows);
    }

    void firstPageFailureAndRetry()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Failed);
        QCOMPARE(f.model.rowCount(), 0);
        f.server.listingStatus.clear();
        f.model.retry();
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(f.model.rowCount(), 250);
    }

    void laterPageFailureFlagsPageError()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        f.loadedRow(100);
        QTRY_VERIFY(f.model.pageError());
        QCOMPARE(f.model.status(), LibraryItemsModel::Ready);
        // A failed page is not asked for again by reading it
        const int attempts = f.server.itemsRoutes.size();
        f.loadedRow(100);
        QTest::qWait(200);
        QCOMPARE(f.server.itemsRoutes.size(), attempts);
        f.server.listingStatus.clear();
        f.model.retry();
        QVERIFY(!f.model.pageError());
        QTRY_VERIFY(f.loadedRow(100));
    }

    void sessionLossAndDisconnectClear()
    {
        {
            Fixture f;
            QVERIFY(f.ok);
            QVERIFY(f.openMovies());
            QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
            f.client.disconnectFromServer();
            QCOMPARE(f.model.status(), LibraryItemsModel::Idle);
            QCOMPARE(f.model.rowCount(), 0);
        }
        {
            // A 401 on a later page expires the session: the model empties itself
            Fixture f;
            QVERIFY(f.ok);
            QVERIFY(f.openMovies());
            QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
            f.server.denyListings = true;
            f.loadedRow(100);
            QTRY_COMPARE(f.model.status(), LibraryItemsModel::Idle);
            QCOMPARE(f.model.rowCount(), 0);
            QVERIFY(!f.model.pageError());
        }
    }

    void parsesCardFields()
    {
        // The server writes seven fractional digits
        QJsonObject json = userData(18e9);
        json.insert(QStringLiteral("CommunityRating"), 7.5);
        json.insert(QStringLiteral("DateCreated"), QStringLiteral("2026-11-10T12:34:56.1234567Z"));
        json.insert(QStringLiteral("RunTimeTicks"), 72e9);
        json.insert(QStringLiteral("ChildCount"), 3);
        json.insert(QStringLiteral("RecursiveItemCount"), 30);
        const Kinefin::ItemSummary item = summaryOf(json);
        QVERIFY(item.valid);
        QCOMPARE(item.communityRating, 7.5);
        QVERIFY(item.dateCreated.isValid());
        QCOMPARE(item.dateCreated,
                 QDateTime(QDate(2026, 11, 10), QTime(12, 34, 56), QTimeZone::utc()));
        QCOMPARE(item.remainingMinutes, 90);
        QCOMPARE(item.childCount, 3);
        QCOMPARE(item.recursiveItemCount, 30);
        // Absent fields mean nothing known
        const Kinefin::ItemSummary bare = summaryOf({});
        QCOMPARE(bare.communityRating, -1.0);
        QVERIFY(!bare.dateCreated.isValid());
        QCOMPARE(bare.remainingMinutes, -1);
        QCOMPARE(bare.childCount, -1);
        QCOMPARE(bare.recursiveItemCount, -1);
        // The extremes that are valid
        json.insert(QStringLiteral("CommunityRating"), 0);
        QCOMPARE(summaryOf(json).communityRating, 0.0);
        json.insert(QStringLiteral("CommunityRating"), 10);
        QCOMPARE(summaryOf(json).communityRating, 10.0);
    }

    void rejectsBadCardFields()
    {
        for (const QJsonValue &rating : {QJsonValue(QStringLiteral("NaN")), QJsonValue(11),
                                         QJsonValue(-1), QJsonValue(10.01), QJsonValue(true)})
            QCOMPARE(summaryOf({{QStringLiteral("CommunityRating"), rating}}).communityRating,
                     -1.0);
        const QStringList badDates{
            QString(10'000, QLatin1Char('2')), QStringLiteral("99999-01-01T00:00:00Z"),
            QStringLiteral("1899-12-31T23:59:59Z"), QStringLiteral("2201-01-01T00:00:00Z"),
            QStringLiteral("not a date")};
        for (const QString &date : badDates)
            QVERIFY2(!summaryOf({{QStringLiteral("DateCreated"), date}}).dateCreated.isValid(),
                     qPrintable(date.left(40)));
        QVERIFY(!summaryOf({{QStringLiteral("DateCreated"), 20261110}}).dateCreated.isValid());
        for (const QJsonValue &count : {QJsonValue(-1), QJsonValue(2.5), QJsonValue(1'000'001),
                                        QJsonValue(QStringLiteral("3"))})
            QCOMPARE(summaryOf({{QStringLiteral("ChildCount"), count}}).childCount, -1);
        for (const QJsonValue &count : {QJsonValue(-5), QJsonValue(0.5), QJsonValue(10'000'001),
                                        QJsonValue(QStringLiteral("3"))})
            QCOMPARE(summaryOf({{QStringLiteral("RecursiveItemCount"), count}}).recursiveItemCount,
                     -1);
        QCOMPARE(summaryOf({{QStringLiteral("ChildCount"), 0}}).childCount, 0);
        QCOMPARE(summaryOf({{QStringLiteral("RecursiveItemCount"), 10'000'000}}).recursiveItemCount,
                 10'000'000);
        // A position outside the runtime, a missing runtime and an absurd one leave no remaining time
        const auto remaining = [](double position, double runtime) {
            QJsonObject json = userData(position);
            json.insert(QStringLiteral("RunTimeTicks"), runtime);
            return summaryOf(json).remainingMinutes;
        };
        QCOMPARE(remaining(7e9, 6e9), -1);
        QCOMPARE(remaining(6e9, 6e9), -1);
        QCOMPARE(remaining(0, 6e9), -1);
        QCOMPARE(remaining(-1, 6e9), -1);
        QCOMPARE(remaining(1e9, 1e14), -1);
        QCOMPARE(remaining(1e9, 0), -1);
        QCOMPARE(summaryOf(userData(1e9)).remainingMinutes, -1);
    }

    void remainingMinutesRoundsUp()
    {
        const auto remaining = [](double position) {
            QJsonObject json = userData(position);
            // Ten minutes
            json.insert(QStringLiteral("RunTimeTicks"), 6e9);
            return summaryOf(json).remainingMinutes;
        };
        QCOMPARE(remaining(1), 10);
        QCOMPARE(remaining(3e9 - 1), 6);
        QCOMPARE(remaining(3e9), 5);
        QCOMPARE(remaining(3e9 + 1), 5);
        // One second left is one minute left
        QCOMPARE(remaining(6e9 - 1e7), 1);
        QCOMPARE(remaining(6e9 - 1), 1);
    }

    void newBadgeWindow()
    {
        const QDateTime now(QDate(2026, 11, 11), QTime(12, 0), QTimeZone::utc());
        const auto added = [&](qint64 secondsAgo, bool played = false) {
            Kinefin::ItemSummary item;
            item.dateCreated = now.addSecs(-secondsAgo);
            item.played = played;
            return Kinefin::isRecentlyAdded(item, now);
        };
        QVERIFY(added(0));
        QVERIFY(added(6 * 86400 + 23 * 3600));
        // Exactly seven days ago is already out
        QVERIFY(!added(7 * 86400));
        QVERIFY(!added(7 * 86400 + 1));
        // A little ahead of the clock still counts as new, a date days ahead does not
        QVERIFY(added(-3600));
        QVERIFY(!added(-2 * 86400));
        QVERIFY(!added(3600, true));
        Kinefin::ItemSummary undated;
        QVERIFY(!Kinefin::isRecentlyAdded(undated, now));
    }

    void cardRolesReachTheGrid()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedItemTotal = -1;
        const QJsonArray items{QJsonObject{
            {QStringLiteral("Id"), QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f")},
            {QStringLiteral("Name"), QStringLiteral("Abyss Station")},
            {QStringLiteral("Type"), QStringLiteral("Series")},
            {QStringLiteral("CommunityRating"), 8.25},
            {QStringLiteral("ChildCount"), 4},
            {QStringLiteral("RecursiveItemCount"), 40},
            {QStringLiteral("DateCreated"),
             QDateTime::currentDateTimeUtc().addSecs(-3600).toString(Qt::ISODateWithMs)},
            {QStringLiteral("UserData"), QJsonObject{{QStringLiteral("UnplayedItemCount"), 10}}}}};
        f.server.listingRawBody.insert(
            QStringLiteral("Items"),
            QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                      {QStringLiteral("TotalRecordCount"), 1}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QModelIndex row = f.model.index(0);
        QCOMPARE(f.model.data(row, Kinefin::RatingRole).toDouble(), 8.25);
        QCOMPARE(f.model.data(row, Kinefin::SeasonCountRole).toInt(), 4);
        QCOMPARE(f.model.data(row, Kinefin::EpisodeCountRole).toInt(), 40);
        QCOMPARE(f.model.data(row, Kinefin::SeriesProgressRole).toDouble(), 0.75);
        QCOMPARE(f.model.data(row, Kinefin::RemainingMinutesRole).toInt(), -1);
        QVERIFY(f.model.data(row, Kinefin::SeriesIdRole).toString().isEmpty());
        QVERIFY(f.model.data(row, Kinefin::IsNewRole).toBool());
        const QHash<int, QByteArray> names = f.model.roleNames();
        QCOMPARE(names.value(Kinefin::SeasonCountRole), QByteArray("seasonCount"));
        QCOMPARE(names.value(Kinefin::IsNewRole), QByteArray("isNew"));
    }

    void untrustedEntriesKeepTheirRow()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedItemTotal = -1;
        const QString longName(1000, QLatin1Char('x'));
        const QJsonArray items{
            QJsonObject{{QStringLiteral("Id"), QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f")},
                        {QStringLiteral("Name"), longName},
                        {QStringLiteral("ProductionYear"), QStringLiteral("not a number")}},
            QJsonObject{{QStringLiteral("Id"), QStringLiteral("../x")},
                        {QStringLiteral("Name"), QStringLiteral("<img src=x>")}},
            QJsonObject{{QStringLiteral("Id"), QStringLiteral("9a8b7c6d5e4f3827160594a3b2c1d0e1")},
                        {QStringLiteral("Name"), 42}}};
        f.server.listingRawBody.insert(
            QStringLiteral("Items"),
            QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                      {QStringLiteral("TotalRecordCount"), 3}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(f.model.rowCount(), 3);
        QCOMPARE(f.model.data(f.model.index(0), LibraryItemsModel::NameRole).toString().size(),
                 512);
        QCOMPARE(f.model.data(f.model.index(0), LibraryItemsModel::YearRole).toInt(), 0);
        // The entry without a valid id keeps its row but is not a loaded item and carries nothing
        QVERIFY(!f.model.data(f.model.index(1), LibraryItemsModel::LoadedRole).toBool());
        QVERIFY(f.model.data(f.model.index(1), LibraryItemsModel::NameRole).toString().isEmpty());
        QVERIFY(f.model.data(f.model.index(2), LibraryItemsModel::LoadedRole).toBool());
        QVERIFY(f.model.data(f.model.index(2), LibraryItemsModel::NameRole).toString().isEmpty());
    }
};

QTEST_MAIN(TestLibraryItemsModel)
#include "test_library_items_model.moc"
