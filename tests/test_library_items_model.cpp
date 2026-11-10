// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The paging model of the library grid against the fake server: placeholders, one page in flight, the most recently read pages first, least-recently-used eviction, a changing total, failures and the loss of the session
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/LibraryItemsModel.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::LibraryItemsModel;

namespace {
const QString kMoviesLibrary = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");

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
    LibraryItemsModel model{&client};
    bool ok = false;
};
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
