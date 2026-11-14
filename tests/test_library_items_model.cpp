// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The paging model of the library grid against the fake server: placeholders, one page in flight, the most recently read pages first, least-recently-used eviction, a changing total, failures and the loss of the session; the typed filters, sorts, the Random page and the view options as they reach the wire
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/ItemSummary.h"
#include "models/LibraryItemsModel.h"
#include "models/SearchModel.h"
#include "settings/BrowseStateStore.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtTest/QtTest>

using Kinefin::LibraryFeature;
using Kinefin::LibraryFilterState;
using Kinefin::LibraryItemsModel;
using Kinefin::PlayedState;
using Kinefin::VideoQuality;

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

// A parameter of a captured request, decoded; empty when it is absent
QString wireValue(const QByteArray &route, const char *name)
{
    return FakeJellyfin::capturedListingQuery(route).queryItemValue(QString::fromLatin1(name),
                                                                    QUrl::FullyDecoded);
}

bool hasWire(const QByteArray &route, const char *name)
{
    return FakeJellyfin::capturedListingQuery(route).hasQueryItem(QString::fromLatin1(name));
}

// A page of the Items family with the image tags of its items: `tags` is one object of ImageTags per item
QByteArray itemsWithImageTags(const QList<QJsonObject> &tags)
{
    QJsonArray items;
    for (qsizetype i = 0; i < tags.size(); ++i) {
        items.append(QJsonObject{
            {QStringLiteral("Id"), QStringLiteral("%1").arg(i + 1, 32, 16, QLatin1Char('0'))},
            {QStringLiteral("Name"), QStringLiteral("Item %1").arg(i)},
            {QStringLiteral("Type"), QStringLiteral("Movie")},
            {QStringLiteral("ImageTags"), tags.at(i)}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                     {QStringLiteral("TotalRecordCount"), int(tags.size())}})
        .toJson(QJsonDocument::Compact);
}

// The digest the browse state store keeps a tag or rating by
QString digestOf(const char *text)
{
    return Kinefin::BrowseStateStore::hashText(QString::fromLatin1(text));
}

// How many item requests carried a search term
int searchRequests(const Fixture &f)
{
    int count = 0;
    for (const QByteArray &route : f.server.itemsRoutes)
        count += route.contains("searchTerm=") ? 1 : 0;
    return count;
}

// A valid tag of the longest length whose every character encodes to six characters on the wire: about eight of them fill a query
QString longTag(int index)
{
    return QString(125, QChar(0x00e9)) + QStringLiteral("%1").arg(index, 3, 10, QLatin1Char('0'));
}

// Adds long tags through the editor's setter until one is refused: the state the model kept
LibraryFilterState fillWithTags(LibraryItemsModel &model)
{
    LibraryFilterState state;
    for (int i = 0; i < Kinefin::MaxFilterValues; ++i) {
        LibraryFilterState next = state;
        next.tags.append(longTag(i));
        model.setFilterState(next);
        if (!(model.filterState() == next))
            break;
        state = next;
    }
    return state;
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

    void aFavoriteAnswerUpdatesTheResidentRowOnly()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QModelIndex first = f.model.index(0);
        const QString id = f.model.data(first, LibraryItemsModel::ItemIdRole).toString();
        QVERIFY(!f.model.data(first, LibraryItemsModel::FavoriteRole).toBool());
        QSignalSpy changed(&f.model, &LibraryItemsModel::dataChanged);
        QVERIFY(f.client.setItemFavorite(id, true));
        QTRY_VERIFY(f.model.data(first, LibraryItemsModel::FavoriteRole).toBool());
        QVERIFY(!changed.isEmpty());
        QCOMPARE(changed.constLast().at(2).value<QList<int>>(),
                 QList<int>{LibraryItemsModel::FavoriteRole});
        // Another row keeps its flag
        QVERIFY(!f.model.data(f.model.index(1), LibraryItemsModel::FavoriteRole).toBool());
        QVERIFY(f.client.setItemFavorite(id, false));
        QTRY_VERIFY(!f.model.data(first, LibraryItemsModel::FavoriteRole).toBool());
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

    void theFilterEditorCannotBuildAQueryTheClientRefuses()
    {
        Fixture f;
        QVERIFY(f.ok);
        // A failed facet list prunes nothing, so the tags below stay
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTRY_COMPARE(f.model.filterOptions()->status(), Kinefin::FilterOptionsModel::Failed);
        LibraryFilterState state = fillWithTags(f.model);
        QVERIFY(!state.tags.isEmpty() && state.tags.size() < Kinefin::MaxFilterValues);
        // The tag that did not fit was refused and the state is the last one that did
        QVERIFY(f.model.filterState() == state);
        QVERIFY(f.model.filtersFit(LibraryItemsModel::filtersToVariant(state)));
        LibraryFilterState more = state;
        more.tags.append(longTag(900));
        QVERIFY(!f.model.filtersFit(LibraryItemsModel::filtersToVariant(more)));
        // Trading the last tag for a short year fits
        LibraryFilterState year = state;
        year.tags.removeLast();
        year.years = {2020};
        QVERIFY(f.model.filtersFit(LibraryItemsModel::filtersToVariant(year)));
        // What the editor let in is sent, never refused
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(!f.model.queryTooLong());
    }

    void aQueryThatIsTooLongFailsWithItsReasonAndRetryResendsNothing()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        // A saved view past the budget (the editor would not have let it in)
        LibraryItemsModel::ViewState initial =
            LibraryItemsModel::defaultViewState(Kinefin::LibraryScope::Movies);
        for (int i = 0; i < Kinefin::MaxFilterValues; ++i)
            initial.filters.tags.append(longTag(i));
        QSignalSpy tooLong(&f.model, &LibraryItemsModel::queryTooLongChanged);
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        QCOMPARE(f.model.status(), LibraryItemsModel::Failed);
        QVERIFY(f.model.queryTooLong());
        QCOMPARE(tooLong.size(), 1);
        QTest::qWait(100);
        QVERIFY(f.server.itemsRoutes.isEmpty());

        // Retry would send the same refused query: it does nothing
        f.model.retry();
        QCOMPARE(f.model.status(), LibraryItemsModel::Failed);
        QTest::qWait(100);
        QVERIFY(f.server.itemsRoutes.isEmpty());

        // An edit that only shortens the query is accepted while it is still too long
        LibraryFilterState fewer = initial.filters;
        fewer.tags.removeLast();
        f.model.setFilterState(fewer);
        QCOMPARE(f.model.filterState().tags.size(), Kinefin::MaxFilterValues - 1);
        QCOMPARE(f.model.status(), LibraryItemsModel::Failed);
        QVERIFY(f.model.queryTooLong());
        // An edit that lengthens it is not
        fewer.tags.append(longTag(900));
        f.model.setFilterState(fewer);
        QCOMPARE(f.model.filterState().tags.size(), Kinefin::MaxFilterValues - 1);

        // Clearing the filters is the way out
        f.model.clearFilters();
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(!f.model.queryTooLong());
        QCOMPARE(tooLong.size(), 2);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
    }

    void aLongSearchTermWithFiltersIsReportedAndShorteningItRecovers()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.model.setSearchDebounceInterval(0);
        // The editor leaves less room than the longest term takes
        QVERIFY(!fillWithTags(f.model).tags.isEmpty());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(!f.model.queryTooLong());
        // The longest term (128 characters of three bytes each) does not fit with the tags
        f.model.setSearchTerm(QString(Kinefin::SearchModel::MaxTermLength, QChar(0x20ac)));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Failed);
        QVERIFY(f.model.queryTooLong());
        f.model.setSearchTerm(QString());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(!f.model.queryTooLong());
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

    void aFailedOptionsLoadKeepsTheSelection()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_VERIFY(f.model.genreOptions()->available());
        f.model.setGenreIds({kSciFi});
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.genreOptions()->status(), Kinefin::FilterOptionsModel::Failed);
        // A source that did not answer says nothing about the genre: the selection stays, and the request keeps carrying it
        QCOMPARE(f.model.genreIds(), QStringList({kSciFi}));
        QVERIFY(f.model.filtered());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(f.server.itemsRoutes.constLast().contains(kSciFi.toUtf8()));
        f.server.listingStatus.clear();
        f.model.genreOptions()->retry();
        QTRY_COMPARE(f.model.genreOptions()->status(), Kinefin::FilterOptionsModel::Ready);
        QCOMPARE(f.model.genreIds(), QStringList({kSciFi}));
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
        shows.setSortKey(LibraryItemsModel::DateAdded);
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
        LibraryItemsModel::ViewState initial;
        initial.sortBy = QStringLiteral("Runtime");
        initial.descending = true;
        initial.filters.played = PlayedState::Unplayed;
        initial.filters.genreIds = {kSciFi, gone};
        initial.filters.tags = {QStringLiteral("restored-master"), QStringLiteral("gone-tag")};
        initial.filters.officialRatings = {QStringLiteral("PG"), QStringLiteral("XX")};
        initial.filters.years = {2019, 1999};
        initial.viewMode = QStringLiteral("list");
        initial.imageType = QStringLiteral("Disc");
        initial.showYear = false;
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        // The initial state is applied before the first request and is not an edit
        QCOMPARE(f.model.sortBy(), QStringLiteral("Runtime"));
        QCOMPARE(f.model.sortKey(), LibraryItemsModel::Runtime);
        QVERIFY(f.model.descending());
        QCOMPARE(f.model.filter(), LibraryItemsModel::Unplayed);
        QCOMPARE(f.model.genreIds(), QStringList({kSciFi, gone}));
        QCOMPARE(f.model.viewMode(), QStringLiteral("list"));
        QCOMPARE(f.model.imageType(), QStringLiteral("Disc"));
        QVERIFY(!f.model.showYear());
        QVERIFY(f.model.showTitle());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QByteArray first = f.server.itemsRoutes.constFirst();
        QCOMPARE(wireValue(first, "sortBy"), QStringLiteral("Runtime,SortName"));
        QCOMPARE(wireValue(first, "sortOrder"), QStringLiteral("Descending"));
        QCOMPARE(wireValue(first, "filters"), QStringLiteral("IsUnplayed"));
        QCOMPARE(wireValue(first, "enableImageTypes"),
                 QStringLiteral("Primary,Backdrop,Thumb,Disc"));
        // The options drop what the library no longer has, whichever source it comes from
        QTRY_COMPARE(f.model.genreIds(), QStringList({kSciFi}));
        QTRY_COMPARE(f.model.filterState().tags, QStringList({QStringLiteral("restored-master")}));
        QTRY_COMPARE(f.model.filterState().officialRatings, QStringList({QStringLiteral("PG")}));
        QTRY_COMPARE(f.model.filterState().years, QList<int>({2019}));
        QCOMPARE(edits.size(), 0);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        // The search term is not part of the view state either
        f.model.setSearchDebounceInterval(20);
        f.model.setSearchTerm(QStringLiteral("dune"));
        QTRY_COMPARE(searchRequests(f), 1);
        QCOMPARE(edits.size(), 0);
        // The user's own changes are edits, one each, and a repeated value is none
        f.model.setSortBy(QStringLiteral("SortName"));
        f.model.setSortBy(QStringLiteral("SortName"));
        f.model.setDescending(false);
        f.model.clearFilters();
        f.model.clearFilters();
        f.model.setViewMode(QStringLiteral("grid"));
        f.model.setImageType(QStringLiteral("Primary"));
        f.model.setShowYear(true);
        QCOMPARE(edits.size(), 6);
        QVERIFY(f.model.viewState() == LibraryItemsModel::ViewState());
    }

    void filterOptionsIsTheGenreOptions()
    {
        Fixture f;
        QVERIFY(f.ok);
        QCOMPARE(f.model.filterOptions(), f.model.genreOptions());
    }

    void activeSectionsAllReachTheWire()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTRY_VERIFY(f.model.filterOptions()->available());
        QTRY_COMPARE(f.model.filterOptions()->legacyStatus(), Kinefin::FilterOptionsModel::Ready);
        LibraryFilterState state;
        state.played = PlayedState::Unplayed;
        state.resumable = true;
        state.favorites = true;
        state.features = LibraryFeature::Subtitles | LibraryFeature::ThemeSong;
        state.quality = VideoQuality::HD;
        state.is4K = true;
        state.is3D = true;
        state.videoTypes = {QStringLiteral("BluRay"), QStringLiteral("Dvd")};
        state.genreIds = {kSciFi, kMystery};
        state.tags = {QStringLiteral("restored-master")};
        state.officialRatings = {QStringLiteral("PG-13")};
        state.years = {2019, 2024};
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        f.model.setFilterState(state);
        QCOMPARE(edits.size(), 1);
        QVERIFY(f.model.filtered());
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QByteArray route = f.server.itemsRoutes.constLast();
        QCOMPARE(wireValue(route, "filters"), QStringLiteral("IsUnplayed,IsResumable,IsFavorite"));
        QCOMPARE(wireValue(route, "hasSubtitles"), QStringLiteral("true"));
        QCOMPARE(wireValue(route, "hasThemeSong"), QStringLiteral("true"));
        QVERIFY(!hasWire(route, "hasTrailer"));
        QVERIFY(!hasWire(route, "hasSpecialFeature"));
        QVERIFY(!hasWire(route, "hasThemeVideo"));
        QCOMPARE(wireValue(route, "isHd"), QStringLiteral("true"));
        QCOMPARE(wireValue(route, "is4K"), QStringLiteral("true"));
        QCOMPARE(wireValue(route, "is3D"), QStringLiteral("true"));
        QCOMPARE(wireValue(route, "videoTypes"), QStringLiteral("BluRay,Dvd"));
        QCOMPARE(wireValue(route, "genreIds"), QString(kSciFi + QLatin1Char('|') + kMystery));
        QCOMPARE(wireValue(route, "tags"), QStringLiteral("restored-master"));
        QCOMPARE(wireValue(route, "officialRatings"), QStringLiteral("PG-13"));
        QCOMPARE(wireValue(route, "years"), QStringLiteral("2019,2024"));
        QVERIFY(!hasWire(route, "seriesStatus"));
        QVERIFY(!hasWire(route, "parentIndexNumber"));
        // Clearing sends nothing of it and leaves the sort and the search alone
        f.model.setSortBy(QStringLiteral("DateCreated"));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.model.clearFilters();
        QVERIFY(!f.model.filtered());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QByteArray cleared = f.server.itemsRoutes.constLast();
        for (const char *name : {"filters", "hasSubtitles", "isHd", "is4K", "videoTypes",
                                 "genreIds", "tags", "officialRatings", "years"})
            QVERIFY2(!hasWire(cleared, name), name);
        QCOMPARE(wireValue(cleared, "sortBy"), QStringLiteral("DateCreated,SortName"));
    }

    void scopeDecidesWhichSectionsAreSent()
    {
        Fixture f;
        QVERIFY(f.ok);
        LibraryFilterState everything;
        everything.played = PlayedState::Played;
        everything.resumable = true;
        everything.features = LibraryFeature::Subtitles;
        everything.quality = VideoQuality::HD;
        everything.is4K = true;
        everything.videoTypes = {QStringLiteral("Dvd")};
        everything.seriesStatus = {QStringLiteral("Ended")};
        everything.episodeKind = Kinefin::EpisodeKind::Missing;

        QVERIFY(f.model.open(QString(), QStringLiteral("movies")));
        QVERIFY(f.model.resumableAvailable());
        QVERIFY(f.model.featuresAvailable());
        QVERIFY(f.model.videoQualityAvailable());
        QVERIFY(!f.model.seriesStatusAvailable());
        QVERIFY(!f.model.episodeKindAvailable());

        QVERIFY(f.model.open(QString(), QStringLiteral("tvshows")));
        QVERIFY(!f.model.resumableAvailable());
        QVERIFY(f.model.featuresAvailable());
        QVERIFY(!f.model.videoQualityAvailable());
        QVERIFY(f.model.seriesStatusAvailable());
        QVERIFY(!f.model.episodeKindAvailable());
        f.model.setFilterState(everything);
        const LibraryFilterState kept = f.model.filterState();
        QVERIFY(!kept.resumable);
        QVERIFY(kept.quality == VideoQuality::Any);
        QVERIFY(!kept.is4K);
        QVERIFY(kept.videoTypes.isEmpty());
        QVERIFY(kept.episodeKind == Kinefin::EpisodeKind::All);
        QVERIFY(kept.played == PlayedState::Played);
        QVERIFY(kept.features == LibraryFeature::Subtitles);
        QCOMPARE(kept.seriesStatus, QStringList({QStringLiteral("Ended")}));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTRY_VERIFY(f.model.filterOptions()->available());
        const QByteArray route = f.server.itemsRoutes.constLast();
        QCOMPARE(wireValue(route, "filters"), QStringLiteral("IsPlayed"));
        QCOMPARE(wireValue(route, "seriesStatus"), QStringLiteral("Ended"));
        QCOMPARE(wireValue(route, "hasSubtitles"), QStringLiteral("true"));
        for (const char *name : {"isHd", "is4K", "videoTypes", "parentIndexNumber", "isMissing"})
            QVERIFY2(!hasWire(route, name), name);

        QVERIFY(f.model.open(QString(), QStringLiteral("boxsets")));
        QVERIFY(!f.model.resumableAvailable());
        QVERIFY(!f.model.featuresAvailable());
        QVERIFY(!f.model.videoQualityAvailable());
        QVERIFY(!f.model.seriesStatusAvailable());
        // Another scope starts over: nothing of the series filters is carried along
        QVERIFY(f.model.filterState().isEmpty());
        f.model.clear();
        QVERIFY(!f.model.resumableAvailable());
        QVERIFY(!f.model.featuresAvailable());
        QVERIFY(!Kinefin::LibraryItemsModel::scopeFor(QStringLiteral("music")));
        QVERIFY(Kinefin::LibraryItemsModel::scopeFor(QStringLiteral("boxsets"))
                == Kinefin::LibraryScope::Collections);
    }

    void anInitialStateKeepsOnlyWhatTheScopeOffers()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        LibraryItemsModel::ViewState initial;
        initial.sortBy = QStringLiteral("Runtime");
        initial.filters.played = PlayedState::Played;
        initial.filters.resumable = true;
        initial.filters.quality = VideoQuality::HD;
        initial.filters.is3D = true;
        initial.filters.seriesStatus = {QStringLiteral("Ended")};
        initial.filters.episodeKind = Kinefin::EpisodeKind::Missing;
        QVERIFY(
            f.model.open(QString(), QStringLiteral("tvshows"), QStringLiteral("Series"), initial));
        // Series have no runtime order, no resumable filter and no video quality
        QCOMPARE(f.model.sortBy(), QStringLiteral("SortName"));
        const LibraryFilterState kept = f.model.filterState();
        QVERIFY(kept.played == PlayedState::Played);
        QVERIFY(!kept.resumable);
        QVERIFY(!kept.is3D);
        QVERIFY(kept.quality == VideoQuality::Any);
        QVERIFY(kept.episodeKind == Kinefin::EpisodeKind::All);
        QCOMPARE(kept.seriesStatus, QStringList({QStringLiteral("Ended")}));
        QCOMPARE(edits.size(), 0);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QByteArray route = f.server.itemsRoutes.constFirst();
        QCOMPARE(wireValue(route, "sortBy"), QStringLiteral("SortName"));
        QCOMPARE(wireValue(route, "filters"), QStringLiteral("IsPlayed"));
        QVERIFY(!hasWire(route, "is3D"));
        QVERIFY(!hasWire(route, "isMissing"));
    }

    void exclusiveChoicesNeverReachTheWireTogether()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const auto lastRoute = [&f] { return f.server.itemsRoutes.constLast(); };
        f.model.setFilter(LibraryItemsModel::Unplayed);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.model.setFilter(LibraryItemsModel::Played);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(wireValue(lastRoute(), "filters"), QStringLiteral("IsPlayed"));
        // The map form: one played choice, favorites and resumable beside it
        f.model.setFilterValues({{QStringLiteral("played"), QStringLiteral("Unplayed")},
                                 {QStringLiteral("favorites"), true}});
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(wireValue(lastRoute(), "filters"), QStringLiteral("IsUnplayed,IsFavorite"));
        // HD and SD exclude each other; 4K is HD, so it drops SD
        f.model.setFilterValues({{QStringLiteral("quality"), QStringLiteral("SD")}});
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(wireValue(lastRoute(), "isHd"), QStringLiteral("false"));
        f.model.setFilterValues(
            {{QStringLiteral("quality"), QStringLiteral("SD")}, {QStringLiteral("is4K"), true}});
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QByteArray fourK = lastRoute();
        QVERIFY(!hasWire(fourK, "isHd"));
        QCOMPARE(wireValue(fourK, "is4K"), QStringLiteral("true"));
        f.model.setFilterValues({{QStringLiteral("quality"), QStringLiteral("HD")}});
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(wireValue(lastRoute(), "isHd"), QStringLiteral("true"));
    }

    void filterValuesBridgeRoundTripsAndSanitises()
    {
        Fixture f;
        QVERIFY(f.ok);
        LibraryFilterState state;
        state.played = PlayedState::Unplayed;
        state.resumable = true;
        state.favorites = true;
        state.features = LibraryFeature::Subtitles | LibraryFeature::ThemeVideo;
        state.quality = VideoQuality::SD;
        state.is3D = true;
        state.videoTypes = {QStringLiteral("Dvd")};
        state.genreIds = {kSciFi};
        state.tags = {QStringLiteral("a tag")};
        state.officialRatings = {QStringLiteral("TV-MA")};
        state.years = {2024, 1999};
        state.seriesStatus = {QStringLiteral("Continuing")};
        state.episodeKind = Kinefin::EpisodeKind::Unaired;
        const QVariantMap values = LibraryItemsModel::filtersToVariant(state);
        QCOMPARE(values.value(QStringLiteral("played")).toString(), QStringLiteral("Unplayed"));
        QCOMPARE(values.value(QStringLiteral("features")).toStringList(),
                 QStringList({QStringLiteral("Subtitles"), QStringLiteral("ThemeVideo")}));
        QCOMPARE(values.value(QStringLiteral("years")).toList().size(), 2);
        QVERIFY(LibraryItemsModel::filtersFromVariant(values) == state);
        // The model hands the same map to QML and takes it back
        f.model.setFilterValues(values);
        QCOMPARE(f.model.filterValues(), values);
        QVERIFY(f.model.filterState() == state);

        // What QML sends may hold anything: unknown tokens and keys are ignored, numbers may be text
        const LibraryFilterState loose = LibraryItemsModel::filtersFromVariant(
            {{QStringLiteral("played"), QStringLiteral("Bogus")},
             {QStringLiteral("quality"), QStringLiteral("4K")},
             {QStringLiteral("episodeKind"), QStringLiteral("Nope")},
             {QStringLiteral("features"),
              QStringList({QStringLiteral("Subtitles"), QStringLiteral("Nope")})},
             {QStringLiteral("years"),
              QVariantList{2019, QStringLiteral("2024"), QStringLiteral("x")}},
             {QStringLiteral("unknown"), 1}});
        QVERIFY(loose.played == PlayedState::Any);
        QVERIFY(loose.quality == VideoQuality::Any);
        QVERIFY(loose.episodeKind == Kinefin::EpisodeKind::All);
        QVERIFY(loose.features == LibraryFeature::Subtitles);
        QCOMPARE(loose.years, QList<int>({2019, 2024}));

        // The values a query cannot carry never enter the state
        f.model.setFilterState({});
        LibraryFilterState bad;
        bad.tags = {QStringLiteral("a|b"), QString(), QString(200, QLatin1Char('x')),
                    QStringLiteral("ok"), QStringLiteral("ok")};
        bad.officialRatings = {QStringLiteral("PG\n"), QStringLiteral("PG")};
        bad.videoTypes = {QStringLiteral("Dvd"), QStringLiteral("Vhs"), QStringLiteral("Dvd")};
        bad.seriesStatus = {QStringLiteral("Bogus")};
        bad.years = {0, 10000, 2020, 2020};
        bad.genreIds = {kSciFi, QStringLiteral("../x"), kSciFi.toUpper()};
        f.model.setFilterState(bad);
        const LibraryFilterState clean = f.model.filterState();
        QCOMPARE(clean.tags, QStringList({QStringLiteral("ok")}));
        QCOMPARE(clean.officialRatings, QStringList({QStringLiteral("PG")}));
        QCOMPARE(clean.videoTypes, QStringList({QStringLiteral("Dvd")}));
        QVERIFY(clean.seriesStatus.isEmpty());
        QCOMPARE(clean.years, QList<int>({2020}));
        QCOMPARE(clean.genreIds, QStringList({kSciFi}));
        // A list is cut at the most a query may carry
        LibraryFilterState many;
        for (int i = 0; i < 80; ++i) {
            many.tags.append(QStringLiteral("tag %1").arg(i));
            many.years.append(1900 + i);
        }
        f.model.setFilterState(many);
        QCOMPARE(f.model.filterState().tags.size(), Kinefin::MaxFilterValues);
        QCOMPARE(f.model.filterState().years.size(), Kinefin::MaxFilterValues);
    }

    void aMissingLegacyEndpointKeepsTheStoredRatingsAndYears()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 404);
        LibraryItemsModel::ViewState initial;
        initial.filters.officialRatings = {QStringLiteral("PG")};
        initial.filters.years = {1999};
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        QTRY_COMPARE(f.model.filterOptions()->legacyStatus(),
                     Kinefin::FilterOptionsModel::Unavailable);
        QTRY_VERIFY(f.model.filterOptions()->available());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        // The endpoint says nothing about them: they stay selected and keep narrowing the list
        QCOMPARE(f.model.filterState().officialRatings, QStringList({QStringLiteral("PG")}));
        QCOMPARE(f.model.filterState().years, QList<int>({1999}));
        const QByteArray route = f.server.itemsRoutes.constFirst();
        QCOMPARE(wireValue(route, "officialRatings"), QStringLiteral("PG"));
        QCOMPARE(wireValue(route, "years"), QStringLiteral("1999"));
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("FiltersLegacy")).size(), 1);
    }

    void pendingHashesResolveOnceTheFacetsLoad()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        QSignalSpy resolving(&f.model, &LibraryItemsModel::resolvingFacetsChanged);
        LibraryItemsModel::ViewState initial;
        initial.pendingTagHashes = {digestOf("restored-master")};
        initial.pendingRatingHashes = {digestOf("PG-13")};
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        // The texts are not known yet: the list waits, narrowed by what is pending
        QVERIFY(f.model.resolvingFacets());
        QVERIFY(f.model.filtered());
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QVERIFY(f.model.filterState().tags.isEmpty());
        QCOMPARE(f.model.viewState().pendingTagHashes, initial.pendingTagHashes);
        QCOMPARE(f.model.viewState().pendingRatingHashes, initial.pendingRatingHashes);
        QTRY_VERIFY(!f.model.resolvingFacets());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(f.model.filterState().tags, QStringList({QStringLiteral("restored-master")}));
        QCOMPARE(f.model.filterState().officialRatings, QStringList({QStringLiteral("PG-13")}));
        QVERIFY(f.model.viewState().pendingTagHashes.isEmpty());
        QVERIFY(f.model.viewState().pendingRatingHashes.isEmpty());
        // The one request of the view is the resolved one, never a wider one first
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        const QByteArray route = f.server.itemsRoutes.constFirst();
        QCOMPARE(wireValue(route, "tags"), QStringLiteral("restored-master"));
        QCOMPARE(wireValue(route, "officialRatings"), QStringLiteral("PG-13"));
        QCOMPARE(edits.size(), 0);
        QCOMPARE(resolving.size(), 2);
    }

    void aRenamedTagIsDroppedWithoutAnEdit()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        LibraryItemsModel::ViewState initial;
        initial.pendingTagHashes = {digestOf("old-name"), digestOf("director-cut")};
        initial.pendingRatingHashes = {digestOf("NC-17")};
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(!f.model.resolvingFacets());
        QCOMPARE(f.model.filterState().tags, QStringList({QStringLiteral("director-cut")}));
        QVERIFY(f.model.filterState().officialRatings.isEmpty());
        const QByteArray route = f.server.itemsRoutes.constFirst();
        QCOMPARE(wireValue(route, "tags"), QStringLiteral("director-cut"));
        QVERIFY(!hasWire(route, "officialRatings"));
        QCOMPARE(edits.size(), 0);

        // Nothing left to match: the view carries no tag and is no longer narrowed
        LibraryItemsModel::ViewState lost;
        lost.pendingTagHashes = {digestOf("old-name")};
        QVERIFY(
            f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"), lost));
        QTRY_COMPARE(f.server.itemsRoutes.size(), 2);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(!f.model.filtered());
        QVERIFY(!hasWire(f.server.itemsRoutes.constLast(), "tags"));
        QCOMPARE(edits.size(), 0);
    }

    void aFailedTagSourceSendsNoQueryUntilRetrySucceeds()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        LibraryItemsModel::ViewState initial;
        initial.filters.favorites = true;
        initial.pendingTagHashes = {digestOf("restored-master")};
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        QTRY_COMPARE(f.model.filterOptions()->status(), Kinefin::FilterOptionsModel::Failed);
        QCOMPARE(f.model.status(), LibraryItemsModel::Failed);
        QVERIFY(f.model.resolvingFacets());
        QVERIFY(f.model.filtered());
        QCOMPARE(f.model.rowCount(), 0);
        QTest::qWait(100);
        QVERIFY(f.server.itemsRoutes.isEmpty());

        // An edit made meanwhile reloads nothing and keeps what is pending
        f.model.setSortBy(QStringLiteral("DateCreated"));
        QCOMPARE(edits.size(), 1);
        QCOMPARE(f.model.status(), LibraryItemsModel::Failed);
        QCOMPARE(f.model.viewState().pendingTagHashes, initial.pendingTagHashes);

        // Retry asks the source again: still failing, still nothing
        f.model.retry();
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Failed);
        QTest::qWait(100);
        QVERIFY(f.server.itemsRoutes.isEmpty());

        f.server.listingStatus.clear();
        f.model.retry();
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(!f.model.resolvingFacets());
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        const QByteArray route = f.server.itemsRoutes.constFirst();
        QCOMPARE(wireValue(route, "tags"), QStringLiteral("restored-master"));
        QCOMPARE(wireValue(route, "filters"), QStringLiteral("IsFavorite"));
        QCOMPARE(wireValue(route, "sortBy"), QStringLiteral("DateCreated,SortName"));
        QCOMPARE(edits.size(), 1);
    }

    void aFailedRatingSourceSendsNoQueryUntilRetrySucceeds()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 500);
        LibraryItemsModel::ViewState initial;
        initial.pendingRatingHashes = {digestOf("R")};
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        QTRY_COMPARE(f.model.filterOptions()->legacyStatus(), Kinefin::FilterOptionsModel::Failed);
        // The tag source answering says nothing about a rating
        QTRY_COMPARE(f.model.filterOptions()->status(), Kinefin::FilterOptionsModel::Ready);
        QCOMPARE(f.model.status(), LibraryItemsModel::Failed);
        QTest::qWait(100);
        QVERIFY(f.server.itemsRoutes.isEmpty());

        f.server.listingStatus.clear();
        f.model.retry();
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        QCOMPARE(wireValue(f.server.itemsRoutes.constFirst(), "officialRatings"),
                 QStringLiteral("R"));
        QCOMPARE(f.model.filterState().officialRatings, QStringList({QStringLiteral("R")}));
    }

    void aMissingLegacyEndpointDropsThePendingRatings()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 404);
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        LibraryItemsModel::ViewState initial;
        initial.pendingTagHashes = {digestOf("restored-master")};
        initial.pendingRatingHashes = {digestOf("PG")};
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        QTRY_COMPARE(f.model.filterOptions()->legacyStatus(),
                     Kinefin::FilterOptionsModel::Unavailable);
        // Waiting for an endpoint that does not exist would block the view for good: the rating is unavailable
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QVERIFY(!f.model.resolvingFacets());
        QVERIFY(f.model.filterState().officialRatings.isEmpty());
        QCOMPARE(f.model.filterState().tags, QStringList({QStringLiteral("restored-master")}));
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        const QByteArray route = f.server.itemsRoutes.constFirst();
        QCOMPARE(wireValue(route, "tags"), QStringLiteral("restored-master"));
        QVERIFY(!hasWire(route, "officialRatings"));
        QCOMPARE(edits.size(), 0);
    }

    void clearingTheFiltersEndsTheWaitForTheFacets()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        LibraryItemsModel::ViewState initial;
        initial.pendingTagHashes = {digestOf("restored-master")};
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Failed);
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        f.model.clearFilters();
        QCOMPARE(edits.size(), 1);
        QVERIFY(!f.model.resolvingFacets());
        QVERIFY(!f.model.filtered());
        QVERIFY(f.model.viewState().pendingTagHashes.isEmpty());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        QVERIFY(!hasWire(f.server.itemsRoutes.constFirst(), "tags"));
    }

    void pendingSelectionsBelongToTheOpenView()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        LibraryItemsModel::ViewState initial;
        initial.pendingTagHashes = {digestOf("restored-master")};
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Failed);
        // The same library again keeps its filters and what is pending
        QVERIFY(f.openMovies());
        QVERIFY(f.model.resolvingFacets());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Failed);
        QVERIFY(f.server.itemsRoutes.isEmpty());
        // Another one starts from none
        QVERIFY(f.model.open(QStringLiteral("7f1b8f4b3e2148c5a8f2b7c9d0e1f2a3"),
                             QStringLiteral("tvshows"), QStringLiteral("Series")));
        QVERIFY(!f.model.resolvingFacets());
        QVERIFY(f.model.viewState().pendingTagHashes.isEmpty());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        // A session that ends leaves nothing to wait for
        f.model.clear();
        QCOMPARE(f.model.status(), LibraryItemsModel::Idle);
    }

    void pendingHashesAreCleanedAndBounded()
    {
        Fixture f;
        QVERIFY(f.ok);
        LibraryItemsModel::ViewState initial;
        initial.pendingTagHashes = {QStringLiteral("not a digest"), digestOf("a"), digestOf("a")};
        for (int i = 0; i < Kinefin::MaxFilterValues + 5; ++i)
            initial.pendingRatingHashes.append(digestOf(QByteArray::number(i).constData()));
        QVERIFY(f.model.open(kMoviesLibrary, QStringLiteral("movies"), QStringLiteral("Movies"),
                             initial));
        QCOMPARE(f.model.pendingTagHashes(), QStringList({digestOf("a")}));
        QCOMPARE(f.model.pendingRatingHashes().size(), Kinefin::MaxFilterValues);
    }

    void everyScopeOffersItsOwnSortKeys()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.sortKeys().isEmpty());
        QVERIFY(f.openMovies());
        QCOMPARE(f.model.sortKeys().size(), 10);
        QCOMPARE(f.model.sortKeys().constFirst(), QStringLiteral("SortName"));
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        f.model.setDescending(true);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.model.setSortBy(QStringLiteral("PlayCount"));
        QCOMPARE(f.model.sortBy(), QStringLiteral("PlayCount"));
        // The older enum has no name for it
        QCOMPARE(f.model.sortKey(), LibraryItemsModel::Name);
        QCOMPARE(edits.size(), 2);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QByteArray route = f.server.itemsRoutes.constLast();
        QCOMPARE(wireValue(route, "sortBy"), QStringLiteral("PlayCount,SortName"));
        QCOMPARE(wireValue(route, "sortOrder"), QStringLiteral("Descending"));

        QVERIFY(f.model.open(QString(), QStringLiteral("tvshows")));
        QCOMPARE(f.model.sortKeys().size(), 8);
        const int editsBefore = edits.size();
        // Not offered for series, or no sort key at all: refused, nothing changes
        f.model.setSortBy(QStringLiteral("Runtime"));
        f.model.setSortKey(LibraryItemsModel::Runtime);
        f.model.setSortBy(QStringLiteral("SortName,Runtime"));
        f.model.setSortBy(QStringLiteral("Bogus"));
        f.model.setSortBy(QString());
        QCOMPARE(f.model.sortBy(), QStringLiteral("SortName"));
        f.model.setSortBy(QStringLiteral("DateLastContentAdded"));
        QCOMPARE(f.model.sortBy(), QStringLiteral("DateLastContentAdded"));
        QCOMPARE(edits.size(), editsBefore + 1);

        QVERIFY(f.model.open(QString(), QStringLiteral("boxsets")));
        QCOMPARE(f.model.sortKeys().size(), 5);
        f.model.setSortBy(QStringLiteral("PlayCount"));
        QCOMPARE(f.model.sortBy(), QStringLiteral("SortName"));
    }

    void showAllOfAGenreIsOneEditThatDropsTheOtherSections()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTRY_VERIFY(f.model.filterOptions()->available());
        QTRY_COMPARE(f.model.filterOptions()->legacyStatus(), Kinefin::FilterOptionsModel::Ready);
        LibraryFilterState state;
        state.played = PlayedState::Unplayed;
        state.favorites = true;
        state.features = LibraryFeature::Subtitles;
        state.quality = VideoQuality::HD;
        state.tags = {QStringLiteral("restored-master")};
        state.officialRatings = {QStringLiteral("PG-13")};
        state.years = {2019};
        state.genreIds = {kMystery};
        f.model.setFilterState(state);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);

        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        f.model.setFilterValues({{QStringLiteral("genreIds"), QStringList{kSciFi}}});
        QCOMPARE(edits.size(), 1);
        LibraryFilterState only;
        only.genreIds = {kSciFi};
        QVERIFY(f.model.filterState() == only);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const QByteArray route = f.server.itemsRoutes.constLast();
        QCOMPARE(wireValue(route, "genreIds"), kSciFi);
        for (const char *name :
             {"filters", "hasSubtitles", "isHd", "tags", "officialRatings", "years"})
            QVERIFY2(!hasWire(route, name), name);
    }

    void episodesStartInTheAiredOrderUnlessAViewWasStored()
    {
        Fixture f;
        QVERIFY(f.ok);
        LibraryItemsModel::Constraint episodes;
        episodes.scope = Kinefin::LibraryScope::Episodes;
        episodes.includeItemTypes = QStringLiteral("Episode");
        const auto openEpisodes = [&](const LibraryItemsModel::ViewState &initial) {
            return f.model.open(kMoviesLibrary, QStringLiteral("tvshows"), QStringLiteral("Shows"),
                                initial, episodes);
        };
        // Nothing stored: the scope's own default
        const auto fresh = LibraryItemsModel::defaultViewState(Kinefin::LibraryScope::Episodes);
        QVERIFY(openEpisodes(fresh));
        QCOMPARE(f.model.sortBy(), QStringLiteral("AiredEpisodeOrder"));
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(wireValue(f.server.itemsRoutes.constLast(), "sortBy"),
                 QStringLiteral("AiredEpisodeOrder,SortName"));
        // A stored choice stays, SortName included
        LibraryItemsModel::ViewState stored;
        stored.sortBy = QStringLiteral("SortName");
        QVERIFY(openEpisodes(stored));
        QCOMPARE(f.model.sortBy(), QStringLiteral("SortName"));
        // The other scopes keep SortName as their default
        for (const Kinefin::LibraryScope scope :
             {Kinefin::LibraryScope::Movies, Kinefin::LibraryScope::Series,
              Kinefin::LibraryScope::Collections})
            QCOMPARE(LibraryItemsModel::defaultViewState(scope).sortBy, QStringLiteral("SortName"));
    }

    void episodesCarryTheirSeriesAndNumbers()
    {
        Fixture f;
        QVERIFY(f.ok);
        const auto item = [](const char *id, const char *type, const char *name) {
            return QJsonObject{{QStringLiteral("Id"), QString::fromLatin1(id)},
                               {QStringLiteral("Name"), QString::fromLatin1(name)},
                               {QStringLiteral("Type"), QString::fromLatin1(type)},
                               {QStringLiteral("SeriesName"), QStringLiteral("Some Show")}};
        };
        QJsonObject episode = item("a0000000000000000000000000000001", "Episode", "Pilot");
        episode.insert(QStringLiteral("ParentIndexNumber"), 1);
        episode.insert(QStringLiteral("IndexNumber"), 3);
        const QJsonObject movie = item("a0000000000000000000000000000002", "Movie", "A Movie");
        const QJsonObject page{{QStringLiteral("Items"), QJsonArray{episode, movie}},
                               {QStringLiteral("TotalRecordCount"), 2}};
        f.server.generatedItemTotal = -1;
        f.server.listingRawBody.insert(QStringLiteral("Items"),
                                       QJsonDocument(page).toJson(QJsonDocument::Compact));
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const auto role = [&f](int row, int which) {
            return f.model.data(f.model.index(row), which);
        };
        QCOMPARE(role(0, LibraryItemsModel::NameRole).toString(), QStringLiteral("Pilot"));
        QCOMPARE(role(0, LibraryItemsModel::SeriesNameRole).toString(),
                 QStringLiteral("Some Show"));
        QCOMPARE(role(0, LibraryItemsModel::SeasonNumberRole).toInt(), 1);
        QCOMPARE(role(0, LibraryItemsModel::EpisodeNumberRole).toInt(), 3);
        // Only an episode has a series line
        QVERIFY(role(1, LibraryItemsModel::SeriesNameRole).toString().isEmpty());
        QCOMPARE(role(1, LibraryItemsModel::EpisodeNumberRole).toInt(), -1);
        const QList<QByteArray> names = f.model.roleNames().values();
        QVERIFY(names.contains("seriesName"));
        QVERIFY(names.contains("seasonNumber"));
        QVERIFY(names.contains("episodeNumber"));
    }

    void storedSortKeysMapToTheWire()
    {
        const auto mapped = [](const char *stored) {
            return LibraryItemsModel::sortByFromStored(QString::fromLatin1(stored));
        };
        QCOMPARE(mapped("Name"), QStringLiteral("SortName"));
        QCOMPARE(mapped("DateAdded"), QStringLiteral("DateCreated"));
        QCOMPARE(mapped("ReleaseDate"), QStringLiteral("PremiereDate"));
        for (const char *wire :
             {"SortName", "Runtime", "CommunityRating", "DatePlayed", "PlayCount", "Random",
              "DateLastContentAdded", "AiredEpisodeOrder"})
            QCOMPARE(mapped(wire), QString::fromLatin1(wire));
        QCOMPARE(mapped("bogus"), QStringLiteral("SortName"));
        QCOMPARE(mapped(""), QStringLiteral("SortName"));
    }

    void randomIsOneBoundedPageAndReshuffleAsksForAnother()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTRY_VERIFY(f.model.filterOptions()->available());
        QVERIFY(!f.model.isRandom());
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        // Nothing to shuffle in a sorted list
        const int sorted = f.server.itemsRoutes.size();
        f.model.reshuffle();
        QTest::qWait(100);
        QCOMPARE(f.server.itemsRoutes.size(), sorted);

        f.model.setFilter(LibraryItemsModel::Unplayed);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        f.model.setSortBy(QStringLiteral("Random"));
        QVERIFY(f.model.isRandom());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(edits.size(), 2);
        // The server has 250 items: a random order shows the first page of them
        QCOMPARE(f.model.totalCount(), 100);
        QCOMPARE(f.model.rowCount(), 100);
        QVERIFY(f.loadedRow(99));
        QByteArray route = f.server.itemsRoutes.constLast();
        QCOMPARE(wireValue(route, "sortBy"), QStringLiteral("Random"));
        QCOMPARE(wireValue(route, "startIndex"), QStringLiteral("0"));
        QCOMPARE(wireValue(route, "limit"), QStringLiteral("100"));
        QCOMPARE(wireValue(route, "filters"), QStringLiteral("IsUnplayed"));
        QTest::qWait(150);
        for (const QByteArray &each : f.server.itemsRoutes)
            QVERIFY2(!each.contains("startIndex=100"), each.constData());

        const int before = f.server.itemsRoutes.size();
        f.model.reshuffle();
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QTRY_COMPARE(f.server.itemsRoutes.size(), before + 1);
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTest::qWait(100);
        QCOMPARE(f.server.itemsRoutes.size(), before + 1);
        // A reshuffle is not an edit, and the filters stay
        QCOMPARE(edits.size(), 2);
        route = f.server.itemsRoutes.constLast();
        QCOMPARE(wireValue(route, "sortBy"), QStringLiteral("Random"));
        QCOMPARE(wireValue(route, "startIndex"), QStringLiteral("0"));
        QCOMPARE(wireValue(route, "filters"), QStringLiteral("IsUnplayed"));
        QCOMPARE(f.model.totalCount(), 100);

        // Back to a sorted list lifts the cap
        f.model.setSortBy(QStringLiteral("SortName"));
        QTRY_COMPARE(f.model.totalCount(), 250);
        QVERIFY(!f.model.isRandom());
    }

    void aNewSortCancelsThePageInFlight()
    {
        Fixture f;
        f.server.generatedItemTotal = 1000;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        const int before = f.server.itemsRoutes.size();
        f.server.holdListings = true;
        f.loadedRow(250);
        QTRY_COMPARE(f.server.itemsRoutes.size(), before + 1);
        f.model.setSortBy(QStringLiteral("DateCreated"));
        QTRY_COMPARE(f.server.itemsRoutes.size(), before + 2);
        const QByteArray route = f.server.itemsRoutes.constLast();
        QCOMPARE(wireValue(route, "startIndex"), QStringLiteral("0"));
        QCOMPARE(wireValue(route, "sortBy"), QStringLiteral("DateCreated,SortName"));
        QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
        QCOMPARE(f.model.rowCount(), 0);
        QVERIFY(!f.model.pageError());
        f.model.clear();
    }

    void viewOptionsAreEditsThatDoNotReloadTheList()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QTRY_VERIFY(f.model.filterOptions()->available());
        QTRY_COMPARE(f.model.filterOptions()->legacyStatus(), Kinefin::FilterOptionsModel::Ready);
        QCOMPARE(f.model.viewMode(), QStringLiteral("grid"));
        QCOMPARE(f.model.imageType(), QStringLiteral("Primary"));
        QVERIFY(f.model.showTitle() && f.model.showYear() && f.model.cardLayout());
        const int requests = f.server.itemsRoutes.size();
        QSignalSpy edits(&f.model, &LibraryItemsModel::viewStateEdited);
        QSignalSpy resets(&f.model, &QAbstractItemModel::modelReset);
        f.model.setViewMode(QStringLiteral("list"));
        f.model.setViewMode(QStringLiteral("list"));
        f.model.setViewMode(QStringLiteral("tiles"));
        f.model.setShowTitle(false);
        f.model.setShowYear(false);
        f.model.setCardLayout(false);
        f.model.setImageType(QStringLiteral("Thumb"));
        f.model.setImageType(QStringLiteral("Backdrop"));
        QCOMPARE(edits.size(), 5);
        QCOMPARE(resets.size(), 0);
        QTest::qWait(100);
        QCOMPARE(f.server.itemsRoutes.size(), requests);
        const LibraryItemsModel::ViewState state = f.model.viewState();
        QCOMPARE(state.viewMode, QStringLiteral("list"));
        QCOMPARE(state.imageType, QStringLiteral("Thumb"));
        QVERIFY(!state.showTitle && !state.showYear && !state.cardLayout);
        // A list row needs no Overview, whatever the mode: the request never asks for it
        const QByteArray first = f.server.itemsRoutes.constFirst();
        QVERIFY2(!wireValue(first, "fields").contains(QStringLiteral("Overview")),
                 first.constData());
        QCOMPARE(wireValue(first, "enableImageTypes"), QStringLiteral("Primary,Backdrop,Thumb"));
    }

    void imageTypeChoosesTheTagAndFallsBackToPrimary()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedItemTotal = -1;
        const auto tags = [](const QStringList &pairs) {
            QJsonObject object;
            for (const QString &pair : pairs)
                object.insert(pair.section(QLatin1Char('='), 0, 0),
                              pair.section(QLatin1Char('='), 1));
            return object;
        };
        f.server.listingRawBody.insert(
            QStringLiteral("Items"),
            itemsWithImageTags({tags({QStringLiteral("Primary=pa1"), QStringLiteral("Banner=ba1"),
                                      QStringLiteral("Disc=da1"), QStringLiteral("Logo=la1"),
                                      QStringLiteral("Thumb=ta1")}),
                                tags({QStringLiteral("Primary=pb1")}), QJsonObject()}));
        const auto imageOf = [&f](int row) {
            return f.model.data(f.model.index(row), LibraryItemsModel::ImageUrlRole).toUrl();
        };
        const auto describe = [](const QUrl &url) {
            return QString(url.path().section(QLatin1Char('/'), -1) + QLatin1Char(':')
                           + QUrlQuery(url).queryItemValue(QStringLiteral("tag")));
        };
        QVERIFY(f.openMovies());
        QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
        QCOMPARE(wireValue(f.server.itemsRoutes.constFirst(), "enableImageTypes"),
                 QStringLiteral("Primary,Backdrop,Thumb"));
        QCOMPARE(describe(imageOf(0)), QStringLiteral("Primary:pa1"));
        QCOMPARE(describe(imageOf(1)), QStringLiteral("Primary:pb1"));
        // No image at all
        QVERIFY(imageOf(2).isEmpty());

        const QList<QPair<QString, QString>> expected{
            {QStringLiteral("Banner"), QStringLiteral("Banner:ba1")},
            {QStringLiteral("Disc"), QStringLiteral("Disc:da1")},
            {QStringLiteral("Logo"), QStringLiteral("Logo:la1")}};
        for (const auto &[type, url] : expected) {
            const int before = f.server.itemsRoutes.size();
            f.model.setImageType(type);
            // The tags of that type were not asked for: the list is requested again, with them
            QCOMPARE(f.model.status(), LibraryItemsModel::Loading);
            QTRY_COMPARE(f.server.itemsRoutes.size(), before + 1);
            QCOMPARE(wireValue(f.server.itemsRoutes.constLast(), "enableImageTypes"),
                     QString(QStringLiteral("Primary,Backdrop,Thumb,") + type));
            QTRY_COMPARE(f.model.status(), LibraryItemsModel::Ready);
            QCOMPARE(describe(imageOf(0)), url);
            // An item without that image shows its Primary
            QCOMPARE(describe(imageOf(1)), QStringLiteral("Primary:pb1"));
            QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(f.client.serverUrl(), imageOf(0)));
        }
        // Thumb and Primary come with every request: no reload, the URLs follow
        const int before = f.server.itemsRoutes.size();
        f.model.setImageType(QStringLiteral("Thumb"));
        QCOMPARE(describe(imageOf(0)), QStringLiteral("Thumb:ta1"));
        QCOMPARE(describe(imageOf(1)), QStringLiteral("Primary:pb1"));
        f.model.setImageType(QStringLiteral("Primary"));
        QCOMPARE(describe(imageOf(0)), QStringLiteral("Primary:pa1"));
        QTest::qWait(100);
        QCOMPARE(f.server.itemsRoutes.size(), before);
        QCOMPARE(f.model.status(), LibraryItemsModel::Ready);
    }

    void theExtraCardImageTypeIsWhitelisted()
    {
        Fixture f;
        QVERIFY(f.ok);
        Kinefin::ItemQuery query;
        query.cardImages = true;
        for (const char *type : {"Banner", "Disc", "Logo", ""}) {
            query.cardImageType = QString::fromLatin1(type);
            QVERIFY2(f.client.fetchItems(query), type);
        }
        for (const char *type : {"Primary", "Bogus", "Banner,Disc", "banner"}) {
            query.cardImageType = QString::fromLatin1(type);
            QVERIFY2(!f.client.fetchItems(query), type);
        }
    }

    void imageTagsAreParsedWithTheOtherCaps()
    {
        const Kinefin::ItemSummary item =
            summaryOf({{QStringLiteral("ImageTags"),
                        QJsonObject{{QStringLiteral("Banner"), QStringLiteral("ba1")},
                                    {QStringLiteral("Disc"), QString(300, QLatin1Char('d'))},
                                    {QStringLiteral("Logo"), 42}}}});
        QCOMPARE(item.bannerTag, QStringLiteral("ba1"));
        QCOMPARE(item.discTag.size(), 64);
        QVERIFY(item.logoTag.isEmpty());
        QVERIFY(summaryOf({}).bannerTag.isEmpty());
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

    void parsesTheResumePosition()
    {
        QCOMPARE(summaryOf(userData(18e9)).positionTicks, qint64(18000000000));
        // Nothing saved, nothing sane: no position
        QCOMPARE(summaryOf({}).positionTicks, qint64(0));
        QCOMPARE(summaryOf(userData(0)).positionTicks, qint64(0));
        QCOMPARE(summaryOf(userData(-5)).positionTicks, qint64(0));
        QCOMPARE(summaryOf(userData(1e14)).positionTicks, qint64(0));
        QJsonObject text;
        text.insert(QStringLiteral("UserData"),
                    QJsonObject{{QStringLiteral("PlaybackPositionTicks"), QStringLiteral("100")}});
        QCOMPARE(summaryOf(text).positionTicks, qint64(0));
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
