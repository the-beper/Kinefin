// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The Favorites snapshot and its per-type views against the fake server: one request at a time on channel 3082, dedup across libraries, the 1000 limit and its notice, a failing library, stale replies, the loss of the session, and the section models of the Favorites rows, and the filters of the snapshot (what they match on, how sections and values combine, the facets the dialog offers, and their life across rescans and the session)
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/FavoritesModel.h"
#include "models/FavoritesSectionModel.h"
#include "models/LibraryItemsModel.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <memory>

using Kinefin::FavoritesModel;
using Kinefin::FavoritesSectionModel;
using Kinefin::FilterOptionsModel;
using Kinefin::LibraryItemsModel;

namespace {
const QString kMovies = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
const QString kKids = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd02");
const QString kShows = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd03");
const QString kBoxsets = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd04");

// The titles of fillFilterable()
const QString kAlpha = QStringLiteral("Alpha");
const QString kBravo = QStringLiteral("Bravo");
const QString kCharlie = QStringLiteral("Charlie");
const QString kDelta = QStringLiteral("Delta");
const QString kEcho = QStringLiteral("Echo");

QString idOf(int n)
{
    return QString::number(n).rightJustified(32, QLatin1Char('0'));
}

QJsonObject favorite(int n, const QString &type = QStringLiteral("Movie"), int year = 2000,
                     const QStringList &genres = {}, const QString &name = {})
{
    QJsonObject item{
        {QStringLiteral("Id"), idOf(n)},
        {QStringLiteral("Name"),
         name.isEmpty() ? QStringLiteral("Item %1").arg(n, 5, 10, QLatin1Char('0')) : name},
        {QStringLiteral("Type"), type}};
    if (year > 0)
        item.insert(QStringLiteral("ProductionYear"), year);
    if (!genres.isEmpty())
        item.insert(QStringLiteral("Genres"), QJsonArray::fromStringList(genres));
    return item;
}

// `count` movies numbered from `first`
QJsonArray movies(int first, int count)
{
    QJsonArray items;
    for (int n = first; n < first + count; ++n)
        items.append(favorite(n));
    return items;
}

// A favorite with what the filters match on: tags, parental rating, watched state and a saved position (0 for none)
QJsonObject filterable(int n, const QString &name, int year = 2000, const QStringList &genres = {},
                       const QStringList &tags = {}, const QString &rating = {},
                       bool played = false, qint64 position = 0)
{
    QJsonObject item = favorite(n, QStringLiteral("Movie"), year, genres, name);
    if (!tags.isEmpty())
        item.insert(QStringLiteral("Tags"), QJsonArray::fromStringList(tags));
    if (!rating.isEmpty())
        item.insert(QStringLiteral("OfficialRating"), rating);
    item.insert(QStringLiteral("UserData"),
                QJsonObject{{QStringLiteral("Played"), played},
                            {QStringLiteral("PlaybackPositionTicks"), double(position)}});
    return item;
}

QVariant library(const QString &id, const QString &name,
                 const QString &type = QStringLiteral("movies"))
{
    return QVariantMap{{QStringLiteral("id"), id},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("collectionType"), type}};
}

QString parentOf(const QByteArray &route)
{
    return FakeJellyfin::capturedListingQuery(route).queryItemValue(QStringLiteral("parentId"));
}

int startOf(const QByteArray &route)
{
    return FakeJellyfin::capturedListingQuery(route)
        .queryItemValue(QStringLiteral("startIndex"))
        .toInt();
}

int distinct(const QStringList &ids)
{
    return QSet<QString>(ids.begin(), ids.end()).size();
}

// The requests so far as "parent:start"
QStringList requests(const FakeJellyfin &server)
{
    QStringList list;
    for (const QByteArray &route : server.itemsRoutes)
        list.append(QStringLiteral("%1:%2").arg(parentOf(route)).arg(startOf(route)));
    return list;
}

struct Fixture {
    Fixture()
    {
        ok = server.listen() && login(client, server);
        // Answers an Items request by its parentId from `favorites`, a page at a time. `declaredTotals` overrides the TotalRecordCount, `failing` answers a status instead of a page
        server.itemsResponder = [this](const QByteArray &route) -> std::pair<int, QByteArray> {
            if (server.denyListings)
                return {};
            const QString parent = parentOf(route);
            if (failing.contains(parent))
                return {failing.value(parent), {}};
            if (!favorites.contains(parent))
                return {};
            const QJsonArray all = favorites.value(parent);
            // A repeating library answers the first page whatever the offset
            const int start = repeating.contains(parent) ? 0 : startOf(route);
            const int limit = FakeJellyfin::capturedListingQuery(route)
                                  .queryItemValue(QStringLiteral("limit"))
                                  .toInt();
            QJsonArray page;
            for (int i = start; i < start + limit && i < all.size(); ++i)
                page.append(all.at(i));
            const QByteArray body =
                QJsonDocument(QJsonObject{{QStringLiteral("Items"), page},
                                          {QStringLiteral("StartIndex"), startOf(route)},
                                          {QStringLiteral("TotalRecordCount"),
                                           declaredTotals.value(parent, int(all.size()))}})
                    .toJson(QJsonDocument::Compact);
            return {200, body};
        };
    }

    void start(const QVariantList &libraries)
    {
        model.setLibraries(libraries);
        model.load();
    }

    QStringList itemIds() const
    {
        QStringList ids;
        for (int row = 0; row < model.count(); ++row)
            ids.append(model.itemAt(row).summary.id);
        return ids;
    }

    QVariant value(int row, int role) const { return model.data(model.index(row, 0), role); }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    FavoritesModel model{&client};
    QHash<QString, QJsonArray> favorites;
    QHash<QString, int> failing;
    QHash<QString, int> declaredTotals;
    QSet<QString> repeating;
    bool ok = false;
};

// Eight favorites across three libraries: a repeated one, a genre in two spellings, items without genre or year
void fillMixed(Fixture &f)
{
    const QJsonObject shared = favorite(4, QStringLiteral("Movie"), 2010,
                                        {QStringLiteral("Comedy")}, QStringLiteral("Delta"));
    f.favorites[kMovies] = QJsonArray{
        favorite(1, QStringLiteral("Movie"), 2020,
                 {QStringLiteral("Action"), QStringLiteral("Drama")}, QStringLiteral("Alpha")),
        favorite(2, QStringLiteral("Movie"), 2020, {QStringLiteral("action")},
                 QStringLiteral("Bravo")),
        favorite(3, QStringLiteral("Movie"), 1999, {}, QStringLiteral("Charlie")), shared};
    f.favorites[kKids] =
        QJsonArray{shared, favorite(5, QStringLiteral("Movie"), 2010, {QStringLiteral("Comedy")},
                                    QStringLiteral("Echo"))};
    f.favorites[kShows] =
        QJsonArray{favorite(6, QStringLiteral("Series"), 2015, {QStringLiteral("Drama")},
                            QStringLiteral("Foxtrot")),
                   favorite(7, QStringLiteral("Season"), 0, {}, QStringLiteral("Golf")),
                   favorite(8, QStringLiteral("Episode"), 2015, {QStringLiteral("Drama")},
                            QStringLiteral("Hotel"))};
}

// Five movies the filters can tell apart: Alpha (2020, Action and Drama, tag classic, PG-13, watched), Bravo (2020, Comedy, tags classic and family, PG, in progress), Charlie (1999, Drama, R, watched and started again), Delta (2010, Action, tag family, no rating) and Echo (no year, no genre, PG-13)
void fillFilterable(Fixture &f)
{
    f.favorites[kMovies] =
        QJsonArray{filterable(1, QStringLiteral("Alpha"), 2020,
                              {QStringLiteral("Action"), QStringLiteral("Drama")},
                              {QStringLiteral("classic")}, QStringLiteral("PG-13"), true),
                   filterable(2, QStringLiteral("Bravo"), 2020, {QStringLiteral("Comedy")},
                              {QStringLiteral("classic"), QStringLiteral("family")},
                              QStringLiteral("PG"), false, 600000000),
                   filterable(3, QStringLiteral("Charlie"), 1999, {QStringLiteral("Drama")}, {},
                              QStringLiteral("R"), true, 300000000),
                   filterable(4, QStringLiteral("Delta"), 2010, {QStringLiteral("Action")},
                              {QStringLiteral("family")}),
                   filterable(5, QStringLiteral("Echo"), 0, {}, {}, QStringLiteral("PG-13"))};
}

// A filter map with one section
QVariantMap only(const QString &key, const QVariant &value)
{
    return QVariantMap{{key, value}};
}

// The names of the rows that pass the filters, in order
QStringList passingNames(const Fixture &f)
{
    QStringList names;
    for (int row = 0; row < f.model.count(); ++row) {
        if (f.model.passesFilters(row))
            names.append(f.value(row, FavoritesModel::NameRole).toString());
    }
    return names;
}

// The names of a section's rows, in order
QStringList namesOf(const FavoritesSectionModel &section)
{
    QStringList names;
    for (int row = 0; row < section.rowCount(); ++row)
        names.append(section.data(section.index(row, 0), FavoritesModel::NameRole).toString());
    return names;
}

const QVariantList kMixedLibraries{
    library(kMovies, QStringLiteral("Movies")), library(kKids, QStringLiteral("Kids")),
    library(kShows, QStringLiteral("Shows"), QStringLiteral("tvshows"))};
} // namespace

class TestFavoritesModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void favoriteWriteRefreshesOpenPageAndInvalidatesClosedSnapshot()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 1);
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 1);

        f.favorites[kMovies] = QJsonArray{};
        QVERIFY(f.client.setItemFavorite(idOf(1), false));
        QTRY_COMPARE(f.server.favoriteWrites.size(), 1);
        QTRY_COMPARE(f.server.itemsRoutes.size(), 2); // exactly one rescan
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QTRY_COMPARE(f.model.count(), 0);

        f.model.close();
        f.favorites[kMovies] = movies(2, 1);
        QVERIFY(f.client.setItemFavorite(idOf(2), true));
        QTRY_COMPARE(f.server.favoriteWrites.size(), 2);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Idle);
        QCOMPARE(f.model.count(), 0);
        QCOMPARE(f.server.itemsRoutes.size(), 2); // closed pages do not poll

        f.model.load();
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 1);
        QCOMPARE(f.itemIds(), QStringList{idOf(2)});
    }

    void scansLibrariesInTurnOnePageAtATime()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 250);
        f.favorites[kShows] = movies(1001, 250);
        f.favorites[kKids] = movies(2001, 250);
        QAbstractItemModelTester tester(&f.model);
        f.start({library(kMovies, QStringLiteral("Movies")),
                 library(kShows, QStringLiteral("Shows"), QStringLiteral("tvshows")),
                 library(kKids, QStringLiteral("Kids"))});
        QCOMPARE(f.model.status(), FavoritesModel::Loading);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 750);
        QVERIFY(!f.model.capped());
        QVERIFY(!f.model.libraryFailed());
        QCOMPARE(f.model.librariesDone(), 3);
        // A superseded request would show up as an extra route: exactly one per page, in turn
        QCOMPARE(requests(f.server),
                 (QStringList{kMovies + QStringLiteral(":0"), kShows + QStringLiteral(":0"),
                              kKids + QStringLiteral(":0"), kMovies + QStringLiteral(":100"),
                              kShows + QStringLiteral(":100"), kKids + QStringLiteral(":100"),
                              kMovies + QStringLiteral(":200"), kShows + QStringLiteral(":200"),
                              kKids + QStringLiteral(":200")}));

        const QUrlQuery first = FakeJellyfin::capturedListingQuery(f.server.itemsRoutes.at(0));
        QCOMPARE(first.queryItemValue(QStringLiteral("filters")), QStringLiteral("IsFavorite"));
        QCOMPARE(first.queryItemValue(QStringLiteral("recursive")), QStringLiteral("true"));
        QCOMPARE(first.queryItemValue(QStringLiteral("limit")), QStringLiteral("100"));
        QCOMPARE(first.queryItemValue(QStringLiteral("includeItemTypes"), QUrl::FullyDecoded),
                 QStringLiteral("Movie"));
        const QString fields = first.queryItemValue(QStringLiteral("fields"), QUrl::FullyDecoded);
        QVERIFY(fields.contains(QStringLiteral("Genres")));
        QVERIFY(fields.contains(QStringLiteral("Tags")));
        const QUrlQuery shows = FakeJellyfin::capturedListingQuery(f.server.itemsRoutes.at(1));
        QCOMPARE(shows.queryItemValue(QStringLiteral("includeItemTypes"), QUrl::FullyDecoded),
                 QStringLiteral("Series,Season,Episode"));
    }

    void onlyOneRequestIsInFlight()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 250);
        f.favorites[kKids] = movies(1001, 250);
        f.server.holdListings = true;
        f.start(
            {library(kMovies, QStringLiteral("Movies")), library(kKids, QStringLiteral("Kids"))});
        QTRY_COMPARE(f.server.itemsRoutes.size(), 1);
        QTest::qWait(150);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        QCOMPARE(f.model.status(), FavoritesModel::Loading);
        QCOMPARE(f.model.count(), 0);
    }

    void ignoresLibrariesThatAreNotScanned()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 3);
        f.favorites[kBoxsets] = QJsonArray();
        f.start({library(kMovies, QStringLiteral("Movies")),
                 library(kMovies, QStringLiteral("Again")),
                 library(QStringLiteral("not an id"), QStringLiteral("Bad")),
                 library(kKids, QStringLiteral("Songs"), QStringLiteral("music")),
                 library(kBoxsets, QStringLiteral("Sets\nline\x1b"), QStringLiteral("boxsets"))});
        QCOMPARE(f.model.libraryCount(), 2);
        QCOMPARE(f.model.libraryName(kMovies), QStringLiteral("Movies"));
        QCOMPARE(f.model.libraryName(kBoxsets), QStringLiteral("Sets line"));
        QCOMPARE(f.model.libraryOrder(kBoxsets), 1);
        QCOMPARE(f.model.libraryOrder(kKids), -1);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 3);
        QCOMPARE(f.server.itemsRoutes.size(), 2);
        const QUrlQuery sets = FakeJellyfin::capturedListingQuery(f.server.itemsRoutes.at(1));
        QCOMPARE(sets.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("BoxSet"));
    }

    void noLibrariesIsCompleteWithoutRequests()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.start({});
        QCOMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.libraryCount(), 0);
        QCOMPARE(f.model.count(), 0);
        QCOMPARE(f.server.itemsRoutes.size(), 0);
    }

    void sameLibraryListChangesNothing()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 250);
        f.server.holdListings = true;
        const QVariantList libraries{library(kMovies, QStringLiteral("Movies"))};
        f.start(libraries);
        QTRY_COMPARE(f.server.itemsRoutes.size(), 1);
        QSignalSpy changed(&f.model, &FavoritesModel::librariesChanged);
        f.model.setLibraries(libraries);
        QCOMPARE(changed.size(), 0);
        QTest::qWait(100);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        QCOMPARE(f.model.status(), FavoritesModel::Loading);
    }

    void dedupsAcrossLibraries()
    {
        Fixture f;
        QVERIFY(f.ok);
        fillMixed(f);
        f.start(kMixedLibraries);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 8);
        QCOMPARE(distinct(f.itemIds()), 8);
        // "Delta" is listed by Movies and Kids, in the order of the library list
        int row = -1;
        for (int i = 0; i < f.model.count(); ++i) {
            if (f.value(i, FavoritesModel::NameRole).toString() == QLatin1String("Delta"))
                row = i;
        }
        QVERIFY(row >= 0);
        QCOMPARE(f.value(row, FavoritesModel::LibraryIdsRole).toStringList(),
                 (QStringList{kMovies, kKids}));
        QCOMPARE(f.value(row, FavoritesModel::LibraryNamesRole).toStringList(),
                 (QStringList{QStringLiteral("Movies"), QStringLiteral("Kids")}));
    }

    void limits_data()
    {
        QTest::addColumn<int>("favorites");
        QTest::addColumn<int>("status");
        QTest::addColumn<int>("count");
        QTest::addColumn<int>("requests");
        QTest::addColumn<bool>("capped");
        const int complete = FavoritesModel::Complete;
        const int capped = FavoritesModel::Capped;
        QTest::newRow("0") << 0 << complete << 0 << 1 << false;
        QTest::newRow("100") << 100 << complete << 100 << 1 << false;
        QTest::newRow("101") << 101 << complete << 101 << 2 << false;
        QTest::newRow("1000") << 1000 << complete << 1000 << 10 << false;
        // The limit is reached with more to come: the 11th page is never asked for
        QTest::newRow("1001") << 1001 << capped << 1000 << 10 << true;
        QTest::newRow("1100") << 1100 << capped << 1000 << 10 << true;
    }

    void limits()
    {
        QFETCH(int, favorites);
        QFETCH(int, status);
        QFETCH(int, count);
        QFETCH(int, requests);
        QFETCH(bool, capped);
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, favorites);
        QSignalSpy noticed(&f.model, &FavoritesModel::noticeChanged);
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(int(f.model.status()), status);
        QCOMPARE(f.model.count(), count);
        QCOMPARE(f.model.capped(), capped);
        QVERIFY(!f.model.libraryFailed());
        QCOMPARE(f.model.loadedCount(), count);
        QCOMPARE(noticed.size(), capped ? 1 : 0);
        QTest::qWait(100);
        QCOMPARE(f.server.itemsRoutes.size(), requests);
        QCOMPARE(distinct(f.itemIds()), count);
    }

    void limitIsGlobalAndFairAcrossLibraries()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 700);
        f.favorites[kKids] = movies(1001, 700);
        f.start(
            {library(kMovies, QStringLiteral("Movies")), library(kKids, QStringLiteral("Kids"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Capped);
        QCOMPARE(f.model.count(), 1000);
        QCOMPARE(f.server.itemsRoutes.size(), 10);
        int inMovies = 0, inKids = 0;
        for (int row = 0; row < f.model.count(); ++row) {
            const QStringList libs = f.value(row, FavoritesModel::LibraryIdsRole).toStringList();
            if (libs.contains(kMovies))
                ++inMovies;
            if (libs.contains(kKids))
                ++inKids;
        }
        QCOMPARE(inMovies, 500);
        QCOMPARE(inKids, 500);
    }

    void repeatedItemsCountOnce()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 600);
        f.favorites[kKids] = movies(1, 600);
        f.start(
            {library(kMovies, QStringLiteral("Movies")), library(kKids, QStringLiteral("Kids"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 600);
        QCOMPARE(f.server.itemsRoutes.size(), 12);
        QCOMPARE(f.value(0, FavoritesModel::LibraryIdsRole).toStringList().size(), 2);
    }

    void aLibraryThatRepeatsItsPageStopsAtTheLimit()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 100);
        f.declaredTotals[kMovies] = 1000000;
        f.repeating.insert(kMovies);
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Capped);
        QTest::qWait(100);
        QCOMPARE(f.model.count(), 100);
        QVERIFY(f.model.capped());
        QCOMPARE(f.server.itemsRoutes.size(), FavoritesModel::MaxItems / FavoritesModel::PageSize);
    }

    void failingLibraryGivesPartialResults()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 150);
        f.failing[kKids] = 500;
        f.start(
            {library(kMovies, QStringLiteral("Movies")), library(kKids, QStringLiteral("Kids"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::PartialError);
        QVERIFY(f.model.libraryFailed());
        QVERIFY(!f.model.capped());
        QCOMPARE(f.model.count(), 150);
        QCOMPARE(f.model.librariesDone(), 2);

        // Retry reads everything again, with nothing of the failed scan left
        f.failing.clear();
        f.favorites[kKids] = movies(1001, 50);
        f.model.reload();
        QCOMPARE(f.model.count(), 0);
        QVERIFY(!f.model.libraryFailed());
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 200);
        QVERIFY(!f.model.libraryFailed());
    }

    void everyLibraryFailingIsNotAnEmptyState()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.failing[kMovies] = 500;
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::PartialError);
        QCOMPARE(f.model.count(), 0);
        QVERIFY(f.model.libraryFailed());
    }

    void inconsistentTotalsFailTheLibrary()
    {
        {
            // The total promises more than the library has: the empty page that follows is a failure
            Fixture f;
            QVERIFY(f.ok);
            f.favorites[kMovies] = movies(1, 100);
            f.declaredTotals[kMovies] = 150;
            f.start({library(kMovies, QStringLiteral("Movies"))});
            QTRY_COMPARE(f.model.status(), FavoritesModel::PartialError);
            QCOMPARE(f.model.count(), 100);
            QCOMPARE(f.server.itemsRoutes.size(), 2);
        }
        {
            // The page holds more than the total says
            Fixture f;
            QVERIFY(f.ok);
            f.favorites[kMovies] = movies(1, 100);
            f.declaredTotals[kMovies] = 5;
            f.favorites[kKids] = movies(1001, 3);
            f.start({library(kMovies, QStringLiteral("Movies")),
                     library(kKids, QStringLiteral("Kids"))});
            QTRY_COMPARE(f.model.status(), FavoritesModel::PartialError);
            QCOMPARE(f.model.count(), 3);
        }
    }

    void rejectsItemsThatCannotBeFavorites()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonObject withUserData = favorite(2);
        withUserData.insert(QStringLiteral("UserData"),
                            QJsonObject{{QStringLiteral("IsFavorite"), false}});
        QJsonObject noType = favorite(5);
        noType.remove(QStringLiteral("Type"));
        QJsonObject badId = favorite(3);
        badId.insert(QStringLiteral("Id"), QStringLiteral("../x"));
        f.favorites[kMovies] = QJsonArray{favorite(1), withUserData, badId,
                                          favorite(4, QStringLiteral("Audio")), noType};
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        // A DTO without UserData or with IsFavorite false is kept: the server filter is the authority
        QCOMPARE(f.itemIds(), (QStringList{idOf(1), idOf(2)}));
    }

    void keepsTheSanitisedGenresTagsAndRatingOfEachFavorite()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray manyGenres, manyTags, lateGenres, lateTags;
        for (int i = 0; i < FavoritesModel::MaxGenres + 4; ++i)
            manyGenres.append(QStringLiteral("Genre %1").arg(i));
        for (int i = 0; i < FavoritesModel::MaxTags + 4; ++i)
            manyTags.append(QStringLiteral("tag %1").arg(i));
        // Entries that are no text do not count against the limit
        for (int i = 0; i < FavoritesModel::MaxGenres + 4; ++i) {
            lateGenres.append(QString());
            lateTags.append(QStringLiteral("a|b"));
        }
        lateGenres.append(QStringLiteral("Late"));
        lateTags.append(QStringLiteral("late tag"));

        QJsonObject messy = favorite(1);
        messy.insert(
            QStringLiteral("Genres"),
            QJsonArray{
                QString(), 7,
                QString(QStringLiteral("Sci") + QChar(ushort(0x202e)) + QStringLiteral("Fi\nx")),
                QStringLiteral("Drama"), QStringLiteral("drama"), QStringLiteral("Sci Fi x")});
        messy.insert(QStringLiteral("Tags"),
                     QJsonArray{QStringLiteral("restored"), QStringLiteral("a|b"), QString(),
                                QStringLiteral("  spaced \t out "), QStringLiteral("line\nbreak"),
                                42, QString(200, QLatin1Char('t')), QStringLiteral("RESTORED")});
        messy.insert(QStringLiteral("OfficialRating"), QStringLiteral("PG-13"));
        QJsonObject many = favorite(2);
        many.insert(QStringLiteral("Genres"), manyGenres);
        many.insert(QStringLiteral("Tags"), manyTags);
        many.insert(QStringLiteral("OfficialRating"), QStringLiteral("a|b"));
        QJsonObject late = favorite(3);
        late.insert(QStringLiteral("Genres"), lateGenres);
        late.insert(QStringLiteral("Tags"), lateTags);
        late.insert(QStringLiteral("OfficialRating"), QString(200, QLatin1Char('r')));
        QJsonObject wrongTypes = favorite(4);
        wrongTypes.insert(QStringLiteral("Genres"), QStringLiteral("Drama"));
        wrongTypes.insert(QStringLiteral("Tags"), QJsonObject{{QStringLiteral("a"), 1}});
        wrongTypes.insert(QStringLiteral("OfficialRating"), 7);
        QJsonObject tooLong = favorite(5);
        tooLong.insert(QStringLiteral("Genres"), QJsonArray{QString(200, QLatin1Char('g'))});
        f.favorites[kMovies] = QJsonArray{messy, many, late, wrongTypes, tooLong, favorite(6)};
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 6);

        // Control and format characters become spaces, a text is kept once whatever its case, and a tag the query could not carry is dropped
        QCOMPARE(f.model.itemAt(0).genres,
                 (QStringList{QStringLiteral("Sci Fi x"), QStringLiteral("Drama")}));
        QCOMPARE(f.model.itemAt(0).tags,
                 (QStringList{QStringLiteral("restored"), QStringLiteral("spaced out"),
                              QStringLiteral("line break"), QString(128, QLatin1Char('t'))}));
        QCOMPARE(f.model.itemAt(0).officialRating, QStringLiteral("PG-13"));

        // Only the first MaxGenres and MaxTags texts are kept, and a rating a filter could not carry is dropped
        QStringList firstGenres, firstTags;
        for (int i = 0; i < FavoritesModel::MaxGenres; ++i)
            firstGenres.append(QStringLiteral("Genre %1").arg(i));
        for (int i = 0; i < FavoritesModel::MaxTags; ++i)
            firstTags.append(QStringLiteral("tag %1").arg(i));
        QCOMPARE(f.model.itemAt(1).genres, firstGenres);
        QCOMPARE(f.model.itemAt(1).tags, firstTags);
        QVERIFY(f.model.itemAt(1).officialRating.isEmpty());

        QCOMPARE(f.model.itemAt(2).genres, QStringList{QStringLiteral("Late")});
        QCOMPARE(f.model.itemAt(2).tags, QStringList{QStringLiteral("late tag")});
        QVERIFY(f.model.itemAt(2).officialRating.isEmpty());

        QVERIFY(f.model.itemAt(3).genres.isEmpty());
        QVERIFY(f.model.itemAt(3).tags.isEmpty());
        QVERIFY(f.model.itemAt(3).officialRating.isEmpty());

        // A genre is cut to MaxGenreChars, an item without any has none
        QCOMPARE(f.model.itemAt(4).genres.size(), 1);
        QCOMPARE(f.model.itemAt(4).genres.constFirst().size(), FavoritesModel::MaxGenreChars);
        QVERIFY(f.model.itemAt(5).genres.isEmpty());
        QVERIFY(f.model.itemAt(5).tags.isEmpty());
    }

    void sortsByNameThenIdWhateverTheArrival()
    {
        QStringList orders[2];
        for (int run = 0; run < 2; ++run) {
            Fixture f;
            QVERIFY(f.ok);
            f.favorites[kMovies] =
                QJsonArray{favorite(1, QStringLiteral("Movie"), 2000, {}, QStringLiteral("beta")),
                           favorite(2, QStringLiteral("Movie"), 2000, {}, QStringLiteral("Same")),
                           favorite(3, QStringLiteral("Movie"), 2000, {}, QStringLiteral("Zeta"))};
            f.favorites[kKids] =
                QJsonArray{favorite(4, QStringLiteral("Movie"), 2000, {}, QStringLiteral("Alpha")),
                           favorite(5, QStringLiteral("Movie"), 2000, {}, QStringLiteral("Same"))};
            const QVariant movies = library(kMovies, QStringLiteral("Movies"));
            const QVariant kids = library(kKids, QStringLiteral("Kids"));
            f.start(run == 0 ? QVariantList{movies, kids} : QVariantList{kids, movies});
            QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
            orders[run] = f.itemIds();
        }
        // Case-insensitive names, the id breaks the tie of the two "Same"
        QCOMPARE(orders[0], (QStringList{idOf(4), idOf(1), idOf(2), idOf(5), idOf(3)}));
        QCOMPARE(orders[1], orders[0]);
    }

    void rolesCarryTheCard()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonObject series =
            favorite(1, QStringLiteral("Series"), 2015, {}, QStringLiteral("Show"));
        series.insert(QStringLiteral("ChildCount"), 3);
        series.insert(QStringLiteral("RecursiveItemCount"), 30);
        series.insert(QStringLiteral("ImageTags"),
                      QJsonObject{{QStringLiteral("Primary"), QStringLiteral("tag1")}});
        QJsonObject episode =
            favorite(2, QStringLiteral("Episode"), 2015, {}, QStringLiteral("Pilot"));
        episode.insert(QStringLiteral("SeriesName"), QStringLiteral("Show"));
        episode.insert(QStringLiteral("ParentIndexNumber"), 1);
        episode.insert(QStringLiteral("IndexNumber"), 2);
        f.favorites[kShows] = QJsonArray{series, episode};
        f.start({library(kShows, QStringLiteral("Shows"), QStringLiteral("tvshows"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        const QHash<int, QByteArray> names = f.model.roleNames();
        for (const char *name :
             {"itemId", "name", "episodeTitle", "year", "type", "imageUrl", "played", "favorite",
              "libraryIds", "libraryNames", "seriesId", "rating", "remainingMinutes", "isNew",
              "seasonCount", "episodeCount", "seriesProgress"})
            QVERIFY2(names.values().contains(name), name);
        // The genres are the filters' data, not a role of the card
        QVERIFY(!names.values().contains("genre"));
        QCOMPARE(f.value(0, FavoritesModel::TypeRole).toString(), QStringLiteral("Series"));
        QCOMPARE(f.value(0, Kinefin::SeasonCountRole).toInt(), 3);
        QCOMPARE(f.value(0, Kinefin::EpisodeCountRole).toInt(), 30);
        QVERIFY(!f.value(0, FavoritesModel::ImageUrlRole).toUrl().isEmpty());
        // An episode is listed under its series
        QCOMPARE(f.value(1, FavoritesModel::NameRole).toString(), QStringLiteral("Show"));
        QCOMPARE(f.value(1, FavoritesModel::EpisodeTitleRole).toString(), QStringLiteral("Pilot"));
        QCOMPARE(f.value(1, FavoritesModel::SeasonNumberRole).toInt(), 1);
        QCOMPARE(f.value(1, FavoritesModel::EpisodeNumberRole).toInt(), 2);
        QVERIFY(!f.value(5, FavoritesModel::NameRole).isValid());
    }

    void aSeasonIsListedUnderItsSeriesAndKeepsItsOwnName()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonObject season =
            favorite(3, QStringLiteral("Season"), 0, {}, QStringLiteral("Season 2"));
        season.insert(QStringLiteral("SeriesName"), QStringLiteral("Show"));
        QJsonObject orphan =
            favorite(4, QStringLiteral("Season"), 0, {}, QStringLiteral("Specials"));
        f.favorites[kShows] = QJsonArray{season, orphan};
        f.start({library(kShows, QStringLiteral("Shows"), QStringLiteral("tvshows"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.rowCount(), 2);
        // Sorted by the name the card shows: "Show" (the series' name) before "Specials"
        QCOMPARE(f.value(0, FavoritesModel::NameRole).toString(), QStringLiteral("Show"));
        QCOMPARE(f.value(0, FavoritesModel::EpisodeTitleRole).toString(),
                 QStringLiteral("Season 2"));
        // A season the server gives no series name keeps its own
        QCOMPARE(f.value(1, FavoritesModel::NameRole).toString(), QStringLiteral("Specials"));
        QCOMPARE(f.value(1, FavoritesModel::EpisodeTitleRole).toString(), QString());
    }

    void libraryListChangeDropsStaleReplies()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 250);
        f.favorites[kKids] = movies(1001, 30);
        f.server.holdListings = true;
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.server.itemsRoutes.size(), 1);
        f.server.holdListings = false;
        // The page is open: the new list starts a new scan at once
        f.model.setLibraries({library(kKids, QStringLiteral("Kids"))});
        QCOMPARE(f.model.status(), FavoritesModel::Loading);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 30);
        QCOMPARE(parentOf(f.server.itemsRoutes.at(1)), kKids);
        // The held answer of the old list arrives late: nothing changes
        f.server.releaseHeld();
        QTest::qWait(100);
        QCOMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 30);
        QVERIFY(!f.model.libraryFailed());
        QCOMPARE(f.server.itemsRoutes.size(), 2);
    }

    void libraryListChangeAfterTheScanDropsTheSnapshot()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 10);
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 10);
        f.favorites[kKids] = movies(1001, 4);
        f.model.setLibraries({library(kKids, QStringLiteral("Kids"))});
        QCOMPARE(f.model.count(), 0);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 4);
    }

    void sessionLossMidPagingEmptiesTheModel()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 250);
        f.server.holdListings = true;
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.server.itemsRoutes.size(), 1);
        f.client.disconnectFromServer();
        QCOMPARE(f.model.status(), FavoritesModel::Idle);
        QCOMPARE(f.model.libraryCount(), 0);
        QCOMPARE(f.model.count(), 0);
        f.server.releaseHeld();
        QTest::qWait(100);
        QCOMPARE(f.model.status(), FavoritesModel::Idle);
        QCOMPARE(f.model.count(), 0);
        // Nothing is asked for while there is no session
        f.model.load();
        f.model.reload();
        QCOMPARE(f.model.status(), FavoritesModel::Idle);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
    }

    void sessionLossAfterTheScanEmptiesTheSnapshot()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 20);
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QSignalSpy counts(&f.model, &FavoritesModel::countChanged);
        f.client.disconnectFromServer();
        QCOMPARE(f.model.status(), FavoritesModel::Idle);
        QCOMPARE(f.model.count(), 0);
        QCOMPARE(counts.size(), 1);
    }

    void expiredSessionEmptiesTheModel()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 20);
        f.server.denyListings = true;
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Idle);
        QCOMPARE(f.model.count(), 0);
        QCOMPARE(f.model.libraryCount(), 0);
        QVERIFY(!f.client.isAuthenticated());
    }

    void closeCancelsTheScanAndKeepsAFinishedSnapshot()
    {
        {
            Fixture f;
            QVERIFY(f.ok);
            f.favorites[kMovies] = movies(1, 250);
            f.server.holdListings = true;
            f.start({library(kMovies, QStringLiteral("Movies"))});
            QTRY_COMPARE(f.server.itemsRoutes.size(), 1);
            f.model.close();
            QCOMPARE(f.model.status(), FavoritesModel::Idle);
            f.server.releaseHeld();
            QTest::qWait(100);
            QCOMPARE(f.model.status(), FavoritesModel::Idle);
            QCOMPARE(f.model.count(), 0);
            // A closed page does not start a scan when the libraries change
            f.server.holdListings = false;
            f.model.setLibraries({library(kKids, QStringLiteral("Kids"))});
            QTest::qWait(100);
            QCOMPARE(f.model.status(), FavoritesModel::Idle);
            QCOMPARE(f.server.itemsRoutes.size(), 1);
        }
        {
            Fixture f;
            QVERIFY(f.ok);
            f.favorites[kMovies] = movies(1, 20);
            f.start({library(kMovies, QStringLiteral("Movies"))});
            QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
            f.model.close();
            QCOMPARE(f.model.status(), FavoritesModel::Complete);
            QCOMPARE(f.model.count(), 20);
            // Opening it again keeps the snapshot; reload() is the refresh
            f.model.load();
            QCOMPARE(f.server.itemsRoutes.size(), 1);
            f.favorites[kMovies] = movies(1, 25);
            f.model.reload();
            QTRY_COMPARE(f.model.count(), 25);
        }
    }

    void aSectionListsOneTypeOfTheSnapshotInItsOrder()
    {
        Fixture f;
        QVERIFY(f.ok);
        fillMixed(f);
        struct Expected {
            QString type;
            QStringList names;
        };
        const QList<Expected> expected{
            {QStringLiteral("Movie"),
             {QStringLiteral("Alpha"), QStringLiteral("Bravo"), QStringLiteral("Charlie"),
              QStringLiteral("Delta"), QStringLiteral("Echo")}},
            {QStringLiteral("Series"), {QStringLiteral("Foxtrot")}},
            {QStringLiteral("Season"), {QStringLiteral("Golf")}},
            {QStringLiteral("Episode"), {QStringLiteral("Hotel")}},
            {QStringLiteral("BoxSet"), {}},
            {QStringLiteral("Video"), {}}};
        std::vector<std::unique_ptr<FavoritesSectionModel>> sections;
        for (const Expected &entry : expected) {
            auto section = std::make_unique<FavoritesSectionModel>();
            new QAbstractItemModelTester(section.get(), section.get());
            section->setSource(&f.model);
            section->setType(entry.type);
            // Nothing before the scan
            QCOMPARE(section->count(), 0);
            sections.push_back(std::move(section));
        }
        f.start(kMixedLibraries);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        for (size_t i = 0; i < sections.size(); ++i) {
            const QStringList &names = expected.at(qsizetype(i)).names;
            QCOMPARE(namesOf(*sections[i]), names);
            QCOMPARE(sections[i]->count(), int(names.size()));
            QCOMPARE(sections[i]->total(), int(names.size()));
        }
        // Every role is the snapshot's own, and the same roles
        const FavoritesSectionModel &episodes = *sections[3];
        QCOMPARE(episodes.data(episodes.index(0, 0), FavoritesModel::TypeRole).toString(),
                 QStringLiteral("Episode"));
        QCOMPARE(episodes.data(episodes.index(0, 0), FavoritesModel::ItemIdRole).toString(),
                 idOf(8));
        QVERIFY(!episodes.data(episodes.index(1, 0), FavoritesModel::NameRole).isValid());
        QCOMPARE(episodes.roleNames(), f.model.roleNames());
    }

    void aSectionLimitCapsTheRowsButNotTheTotal()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 30);
        FavoritesSectionModel row;
        QAbstractItemModelTester tester(&row);
        row.setSource(&f.model);
        row.setType(QStringLiteral("Movie"));
        row.setLimit(24);
        QSignalSpy counted(&row, &FavoritesSectionModel::countChanged);
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(row.count(), 24);
        QCOMPARE(row.total(), 30);
        QVERIFY(counted.size() > 0);
        // The first 24 of the snapshot, in its order
        for (int i = 0; i < 24; ++i)
            QCOMPARE(row.data(row.index(i, 0), FavoritesModel::ItemIdRole),
                     f.value(i, FavoritesModel::ItemIdRole));

        // No limit shows them all, one more than the type has changes nothing, a negative one is no limit
        row.setLimit(0);
        QCOMPARE(row.count(), 30);
        row.setLimit(31);
        QCOMPARE(row.count(), 30);
        row.setLimit(-5);
        QCOMPARE(row.limit(), 0);
        QCOMPARE(row.count(), 30);

        // Exactly the limit: every item shows and the total does not exceed it
        row.setLimit(24);
        f.favorites[kMovies] = movies(1, 24);
        f.model.reload();
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(row.count(), 24);
        QCOMPARE(row.total(), 24);
        f.favorites[kMovies] = movies(1, 25);
        f.model.reload();
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(row.count(), 24);
        QCOMPARE(row.total(), 25);
    }

    void aSectionFollowsEveryNewSnapshot()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 3);
        FavoritesSectionModel section;
        QAbstractItemModelTester tester(&section);
        section.setSource(&f.model);
        section.setType(QStringLiteral("Movie"));
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(section.count(), 3);

        // A reload drops the rows at once, and the new scan fills them again
        f.favorites[kMovies] = movies(10, 2);
        f.model.reload();
        QCOMPARE(section.count(), 0);
        QCOMPARE(section.total(), 0);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(section.count(), 2);
        QCOMPARE(section.data(section.index(0, 0), FavoritesModel::ItemIdRole).toString(),
                 idOf(10));

        // A lost session empties it as well
        f.client.disconnectFromServer();
        QCOMPARE(section.count(), 0);
        QCOMPARE(section.total(), 0);

        // Another type, or another source, is a new list
        section.setType(QStringLiteral("Series"));
        QCOMPARE(section.count(), 0);
        section.setSource(nullptr);
        QCOMPARE(section.count(), 0);
    }

    void aSectionOutlivesItsSource()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.favorites[kMovies] = movies(1, 5);
        auto source = std::make_unique<FavoritesModel>(&f.client);
        FavoritesSectionModel section;
        section.setSource(source.get());
        section.setType(QStringLiteral("Movie"));
        source->setLibraries({library(kMovies, QStringLiteral("Movies"))});
        source->load();
        QTRY_COMPARE(source->status(), FavoritesModel::Complete);
        QCOMPARE(section.count(), 5);
        source.reset();
        QCOMPARE(section.count(), 0);
        QVERIFY(section.source() == nullptr);
        QVERIFY(!section.data(section.index(0, 0), FavoritesModel::NameRole).isValid());
    }

    void filtersCombineSectionsWithAndAndValuesWithOr_data()
    {
        QTest::addColumn<QVariantMap>("filters");
        QTest::addColumn<bool>("filtered");
        QTest::addColumn<QStringList>("passing");
        const QStringList all{kAlpha, kBravo, kCharlie, kDelta, kEcho};
        const QString played = QStringLiteral("played");
        const QString resumable = QStringLiteral("resumable");
        const QString genres = QStringLiteral("genreIds");
        const QString tags = QStringLiteral("tags");
        const QString ratings = QStringLiteral("officialRatings");
        const QString years = QStringLiteral("years");

        QTest::newRow("none") << QVariantMap{} << false << all;
        QTest::newRow("played") << only(played, QStringLiteral("Played")) << true
                                << QStringList{kAlpha, kCharlie};
        QTest::newRow("unplayed") << only(played, QStringLiteral("Unplayed")) << true
                                  << QStringList{kBravo, kDelta, kEcho};
        QTest::newRow("resumable means a saved position")
            << only(resumable, true) << true << QStringList{kBravo, kCharlie};
        QTest::newRow("played and resumable")
            << QVariantMap{{played, QStringLiteral("Played")}, {resumable, true}} << true
            << QStringList{kCharlie};
        QTest::newRow("unplayed and resumable")
            << QVariantMap{{played, QStringLiteral("Unplayed")}, {resumable, true}} << true
            << QStringList{kBravo};
        QTest::newRow("a genre") << only(genres, QStringList{QStringLiteral("Action")}) << true
                                 << QStringList{kAlpha, kDelta};
        QTest::newRow("genres are alternatives whatever their case")
            << only(genres, QStringList{QStringLiteral("comedy"), QStringLiteral("ACTION")}) << true
            << QStringList{kAlpha, kBravo, kDelta};
        QTest::newRow("a tag") << only(tags, QStringList{QStringLiteral("family")}) << true
                               << QStringList{kBravo, kDelta};
        QTest::newRow("tags are alternatives")
            << only(tags, QStringList{QStringLiteral("classic"), QStringLiteral("family")}) << true
            << QStringList{kAlpha, kBravo, kDelta};
        QTest::newRow("ratings are alternatives")
            << only(ratings, QStringList{QStringLiteral("PG-13"), QStringLiteral("R")}) << true
            << QStringList{kAlpha, kCharlie, kEcho};
        QTest::newRow("years are alternatives") << only(years, QVariantList{2020, 1999}) << true
                                                << QStringList{kAlpha, kBravo, kCharlie};
        QTest::newRow("an item without a year matches no year")
            << only(years, QVariantList{2020, 2010, 1999}) << true
            << QStringList{kAlpha, kBravo, kCharlie, kDelta};
        QTest::newRow("an item without a rating matches no rating")
            << only(ratings,
                    QStringList{QStringLiteral("PG"), QStringLiteral("PG-13"), QStringLiteral("R")})
            << true << QStringList{kAlpha, kBravo, kCharlie, kEcho};
        QTest::newRow("sections combine with and")
            << QVariantMap{{genres, QStringList{QStringLiteral("Action"), QStringLiteral("Drama")}},
                           {years, QVariantList{2020}},
                           {ratings, QStringList{QStringLiteral("PG-13")}}}
            << true << QStringList{kAlpha};
        QTest::newRow("a genre and a status")
            << QVariantMap{{genres, QStringList{QStringLiteral("Drama")}},
                           {played, QStringLiteral("Unplayed")}}
            << true << QStringList{};
        QTest::newRow("a tag and a rating")
            << QVariantMap{{tags, QStringList{QStringLiteral("classic")}},
                           {ratings, QStringList{QStringLiteral("PG")}}}
            << true << QStringList{kBravo};
        QTest::newRow("a value nobody has")
            << only(genres, QStringList{QStringLiteral("Horror")}) << true << QStringList{};
        QTest::newRow("values a filter cannot carry are dropped")
            << QVariantMap{{years, QVariantList{0, 10000, -3}},
                           {tags, QStringList{QStringLiteral("a|b"), QString()}},
                           {genres, QStringList{QString()}}}
            << false << all;
        QTest::newRow("sections the snapshot has no data for are dropped")
            << QVariantMap{{QStringLiteral("favorites"), true},
                           {QStringLiteral("features"), QStringList{QStringLiteral("Subtitles")}},
                           {QStringLiteral("quality"), QStringLiteral("HD")},
                           {QStringLiteral("is4K"), true},
                           {QStringLiteral("is3D"), true},
                           {QStringLiteral("videoTypes"), QStringList{QStringLiteral("Dvd")}},
                           {QStringLiteral("seriesStatus"), QStringList{QStringLiteral("Ended")}},
                           {QStringLiteral("episodeKind"), QStringLiteral("Specials")}}
            << false << all;
    }

    void filtersCombineSectionsWithAndAndValuesWithOr()
    {
        QFETCH(QVariantMap, filters);
        QFETCH(bool, filtered);
        QFETCH(QStringList, passing);
        Fixture f;
        QVERIFY(f.ok);
        fillFilterable(f);
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 5);
        QVERIFY(!f.model.filtered());
        QCOMPARE(f.model.matchCount(), 5);

        f.model.setFilters(filters);
        QCOMPARE(f.model.filtered(), filtered);
        QCOMPARE(passingNames(f), passing);
        QCOMPARE(f.model.matchCount(), int(passing.size()));
        // The snapshot stays whole, and the rule on the stored state is the one of every row
        QCOMPARE(f.model.count(), 5);
        QCOMPARE(f.model.rowCount(), 5);
        const Kinefin::LibraryFilterState state =
            LibraryItemsModel::filtersFromVariant(f.model.filters());
        for (int row = 0; row < f.model.count(); ++row)
            QCOMPARE(FavoritesModel::passes(f.model.itemAt(row), state),
                     f.model.passesFilters(row));
        QVERIFY(!f.model.passesFilters(-1));
        QVERIFY(!f.model.passesFilters(5));
    }

    void filtersAreCleanedBoundedAndReported()
    {
        FavoritesModel model(nullptr);
        QSignalSpy changed(&model, &FavoritesModel::filtersChanged);
        const QVariantMap none = LibraryItemsModel::filtersToVariant({});
        QCOMPARE(model.filters(), none);
        QVERIFY(!model.filtered());

        // The map is the library's, each value stays once whatever its case, in the order given
        model.setFilters(
            QVariantMap{{QStringLiteral("genreIds"),
                         QStringList{QStringLiteral("Action"), QStringLiteral("action"),
                                     QStringLiteral("ACTION"), QStringLiteral("Drama")}},
                        {QStringLiteral("played"), QStringLiteral("Played")},
                        {QStringLiteral("resumable"), true}});
        QCOMPARE(changed.size(), 1);
        QVERIFY(model.filtered());
        QCOMPARE(model.filters().value(QStringLiteral("genreIds")).toStringList(),
                 (QStringList{QStringLiteral("Action"), QStringLiteral("Drama")}));
        QCOMPARE(model.filters().value(QStringLiteral("played")).toString(),
                 QStringLiteral("Played"));
        QVERIFY(model.filters().value(QStringLiteral("resumable")).toBool());
        // The same filters again, even written back from the property, change nothing
        model.setFilters(model.filters());
        QCOMPARE(changed.size(), 1);
        // A status the model does not know is no status
        model.setFilters(only(QStringLiteral("played"), QStringLiteral("Bogus")));
        QCOMPARE(changed.size(), 2);
        QCOMPARE(model.filters().value(QStringLiteral("played")).toString(), QStringLiteral("Any"));
        QVERIFY(!model.filtered());

        // At most MaxFilterValues in each list
        QStringList tags, genres;
        QVariantList years;
        for (int i = 0; i < Kinefin::MaxFilterValues + 10; ++i) {
            tags.append(QStringLiteral("tag %1").arg(i));
            genres.append(QStringLiteral("Genre %1").arg(i));
            years.append(1900 + i);
        }
        const QVariantMap tooMany{{QStringLiteral("tags"), tags},
                                  {QStringLiteral("genreIds"), genres},
                                  {QStringLiteral("years"), years}};
        QVERIFY(!model.filtersFit(tooMany));
        QVERIFY(!model.filtersFit(only(QStringLiteral("tags"), tags)));
        QVERIFY(!model.filtersFit(only(QStringLiteral("genreIds"), genres)));
        QVERIFY(!model.filtersFit(only(QStringLiteral("years"), years)));
        model.setFilters(tooMany);
        QCOMPARE(model.filters().value(QStringLiteral("tags")).toStringList().size(),
                 Kinefin::MaxFilterValues);
        QCOMPARE(model.filters().value(QStringLiteral("genreIds")).toStringList().size(),
                 Kinefin::MaxFilterValues);
        QCOMPARE(model.filters().value(QStringLiteral("years")).toList().size(),
                 Kinefin::MaxFilterValues);
        QCOMPARE(model.filters().value(QStringLiteral("tags")).toStringList().constFirst(),
                 QStringLiteral("tag 0"));
        const QVariantMap stored = model.filters();

        // Repeats and values a filter cannot carry do not count against the limit, and asking writes nothing
        QStringList padded = tags.mid(0, Kinefin::MaxFilterValues);
        padded += QStringList(20, QStringLiteral("TAG 0"));
        padded += QStringList(20, QStringLiteral("a|b"));
        QVERIFY(model.filtersFit(only(QStringLiteral("tags"), padded)));
        const QStringList fifty = tags.mid(0, Kinefin::MaxFilterValues);
        QVERIFY(model.filtersFit(only(QStringLiteral("tags"), fifty)));
        QCOMPARE(model.filters(), stored);

        model.clearFilters();
        QVERIFY(!model.filtered());
        QCOMPARE(model.filters(), none);
        const int afterClear = int(changed.size());
        model.clearFilters();
        QCOMPARE(changed.size(), afterClear);
    }

    void aSectionCountsAndLimitsOnlyTheFavoritesThatPass()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QString odd = QStringLiteral("Odd");
        const QString even = QStringLiteral("Even");
        QJsonArray movieItems;
        for (int n = 1; n <= 30; ++n) {
            const QString title = QStringLiteral("Movie %1").arg(n, 2, 10, QLatin1Char('0'));
            const QString genre = n % 2 == 1 ? odd : even;
            movieItems.append(filterable(n, title, 2000, {genre}, {}, {}, n == 3));
        }
        f.favorites[kMovies] = movieItems;
        const QString series = QStringLiteral("Series");
        f.favorites[kShows] =
            QJsonArray{favorite(101, series, 2015, {odd}, QStringLiteral("Show A")),
                       favorite(102, series, 2015, {}, QStringLiteral("Show B"))};
        FavoritesSectionModel row;
        FavoritesSectionModel shows;
        QAbstractItemModelTester rowTester(&row);
        QAbstractItemModelTester showsTester(&shows);
        row.setSource(&f.model);
        row.setType(QStringLiteral("Movie"));
        row.setLimit(10);
        shows.setSource(&f.model);
        shows.setType(QStringLiteral("Series"));
        f.start({library(kMovies, QStringLiteral("Movies")),
                 library(kShows, QStringLiteral("Shows"), QStringLiteral("tvshows"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(row.count(), 10);
        QCOMPARE(row.total(), 30);
        QCOMPARE(shows.count(), 2);
        QCOMPARE(shows.total(), 2);

        // The limit applies to the favorites that pass, and so does the total
        QSignalSpy counted(&row, &FavoritesSectionModel::countChanged);
        f.model.setFilters(only(QStringLiteral("genreIds"), QStringList{odd}));
        QCOMPARE(counted.size(), 1);
        QCOMPARE(row.count(), 10);
        QCOMPARE(row.total(), 15);
        for (int i = 0; i < 10; ++i)
            QCOMPARE(row.data(row.index(i, 0), FavoritesModel::ItemIdRole).toString(),
                     idOf(2 * i + 1));
        QCOMPARE(shows.count(), 1);
        QCOMPARE(shows.total(), 1);
        QCOMPARE(shows.data(shows.index(0, 0), FavoritesModel::NameRole).toString(),
                 QStringLiteral("Show A"));
        // The snapshot itself stays whole
        QCOMPARE(f.model.count(), 32);
        QCOMPARE(f.model.rowCount(), 32);
        QCOMPARE(f.model.matchCount(), 16);

        // Fewer than the limit: every one of them shows
        f.model.setFilters(QVariantMap{{QStringLiteral("genreIds"), QStringList{odd}},
                                       {QStringLiteral("played"), QStringLiteral("Played")}});
        QCOMPARE(row.count(), 1);
        QCOMPARE(row.total(), 1);
        QCOMPARE(row.data(row.index(0, 0), FavoritesModel::ItemIdRole).toString(), idOf(3));
        QCOMPARE(shows.count(), 0);
        QCOMPARE(shows.total(), 0);

        // Nothing passes: the sections are empty, the snapshot is not
        f.model.setFilters(only(QStringLiteral("genreIds"), QStringList{QStringLiteral("Horror")}));
        QCOMPARE(row.count(), 0);
        QCOMPARE(row.total(), 0);
        QCOMPARE(shows.total(), 0);
        QCOMPARE(f.model.matchCount(), 0);
        QCOMPARE(f.model.count(), 32);

        f.model.clearFilters();
        QCOMPARE(row.count(), 10);
        QCOMPARE(row.total(), 30);
        QCOMPARE(shows.count(), 2);
        QCOMPARE(f.model.matchCount(), 32);

        // A filter that is set when a scan starts applies to the rows it fills
        f.model.setFilters(only(QStringLiteral("genreIds"), QStringList{odd}));
        f.model.reload();
        QCOMPARE(row.count(), 0);
        QCOMPARE(row.total(), 0);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(row.count(), 10);
        QCOMPARE(row.total(), 15);
        QCOMPARE(shows.count(), 1);
    }

    void filtersSurviveARescanAndGoWithTheSession()
    {
        Fixture f;
        QVERIFY(f.ok);
        fillFilterable(f);
        f.favorites[kKids] = QJsonArray{filterable(6, QStringLiteral("Foxtrot"), 2020,
                                                   {QStringLiteral("Drama")}, {}, {}, true)};
        const QVariant movieLibrary = library(kMovies, QStringLiteral("Movies"));
        const QVariant kidsLibrary = library(kKids, QStringLiteral("Kids"));
        f.start({movieLibrary});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        f.model.setFilters(
            QVariantMap{{QStringLiteral("played"), QStringLiteral("Played")},
                        {QStringLiteral("genreIds"),
                         QStringList{QStringLiteral("Action"), QStringLiteral("Drama")}}});
        const QVariantMap stored = f.model.filters();
        // Alpha and Charlie
        QCOMPARE(f.model.matchCount(), 2);
        QSignalSpy changed(&f.model, &FavoritesModel::filtersChanged);

        // A rescan empties the snapshot and the count, not the filters
        f.model.reload();
        QCOMPARE(f.model.count(), 0);
        QCOMPARE(f.model.matchCount(), 0);
        QVERIFY(f.model.filtered());
        QCOMPARE(f.model.filters(), stored);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.matchCount(), 2);

        // A visit that leaves and comes back
        f.model.close();
        f.model.load();
        QCOMPARE(f.model.filters(), stored);
        QCOMPARE(f.model.matchCount(), 2);

        // Another library list drops the snapshot, and its scan is filtered by the same filters
        f.model.setLibraries({movieLibrary, kidsLibrary});
        QCOMPARE(f.model.count(), 0);
        QCOMPARE(f.model.filters(), stored);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.model.count(), 6);
        QCOMPARE(f.model.matchCount(), 3);
        QCOMPARE(changed.size(), 0);

        // The session is gone and the filters with it
        f.client.disconnectFromServer();
        QCOMPARE(f.model.status(), FavoritesModel::Idle);
        QVERIFY(!f.model.filtered());
        QCOMPARE(f.model.filters(), LibraryItemsModel::filtersToVariant({}));
        QCOMPARE(f.model.matchCount(), 0);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(f.model.filterOptions()->status(), FilterOptionsModel::Idle);
    }

    void theFacetsComeFromTheSnapshotAndKeepTheChosenValues()
    {
        Fixture f;
        QVERIFY(f.ok);
        fillFilterable(f);
        FilterOptionsModel &options = *f.model.filterOptions();
        const auto genreColumn = [&options](int role) {
            QStringList column;
            for (int row = 0; row < options.rowCount(); ++row)
                column.append(options.data(options.index(row), role).toString());
            return column;
        };
        const auto genreNames = [&] { return genreColumn(FilterOptionsModel::NameRole); };
        const auto genreIds = [&] { return genreColumn(FilterOptionsModel::GenreIdRole); };
        QCOMPARE(options.status(), FilterOptionsModel::Idle);
        QCOMPARE(options.legacyStatus(), FilterOptionsModel::Idle);

        // A scan under way: Loading, nothing to offer yet
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QCOMPARE(f.model.status(), FavoritesModel::Loading);
        QCOMPARE(options.status(), FilterOptionsModel::Loading);
        QCOMPARE(options.legacyStatus(), FilterOptionsModel::Loading);
        QVERIFY(!options.available());
        QVERIFY(!options.ratingsAvailable());
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);

        // The values of the snapshot, each once, sorted (the years newest first); a genre's id is its name
        QCOMPARE(options.status(), FilterOptionsModel::Ready);
        QCOMPARE(options.legacyStatus(), FilterOptionsModel::Ready);
        QVERIFY(options.available());
        QVERIFY(options.tagsAvailable());
        QVERIFY(options.ratingsAvailable());
        QVERIFY(options.yearsAvailable());
        QCOMPARE(genreNames(), (QStringList{QStringLiteral("Action"), QStringLiteral("Comedy"),
                                            QStringLiteral("Drama")}));
        QCOMPARE(genreIds(), genreNames());
        QCOMPARE(options.tags(),
                 (QStringList{QStringLiteral("classic"), QStringLiteral("family")}));
        QCOMPARE(options.officialRatings(),
                 (QStringList{QStringLiteral("PG"), QStringLiteral("PG-13"), QStringLiteral("R")}));
        QCOMPARE(options.years(), (QList<int>{2020, 2010, 1999}));
        QVERIFY(options.contains(QStringLiteral("DRAMA")));
        QVERIFY(!options.contains(QStringLiteral("Horror")));

        // The chosen values stay offered, with the spelling the filter has, whatever the next snapshot holds
        f.model.setFilters(
            QVariantMap{{QStringLiteral("genreIds"),
                         QStringList{QStringLiteral("horror"), QStringLiteral("ACTION")}},
                        {QStringLiteral("tags"), QStringList{QStringLiteral("zzz")}},
                        {QStringLiteral("officialRatings"), QStringList{QStringLiteral("NC-17")}},
                        {QStringLiteral("years"), QVariantList{1980}}});
        f.favorites[kMovies] =
            QJsonArray{filterable(1, kAlpha, 2021, {QStringLiteral("action")},
                                  {QStringLiteral("Classic")}, QStringLiteral("PG"))};
        f.model.reload();
        QCOMPARE(options.status(), FilterOptionsModel::Loading);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(options.status(), FilterOptionsModel::Ready);
        QCOMPARE(genreNames(), (QStringList{QStringLiteral("ACTION"), QStringLiteral("horror")}));
        QCOMPARE(genreIds(), genreNames());
        QCOMPARE(options.tags(), (QStringList{QStringLiteral("Classic"), QStringLiteral("zzz")}));
        QCOMPARE(options.officialRatings(),
                 (QStringList{QStringLiteral("NC-17"), QStringLiteral("PG")}));
        QCOMPARE(options.years(), (QList<int>{2021, 1980}));

        // A library that fails leaves the values of the others, and the chosen ones
        f.favorites[kKids] =
            QJsonArray{filterable(2, kBravo, 2005, {QStringLiteral("Comedy")},
                                  {QStringLiteral("family")}, QStringLiteral("R"))};
        f.failing[kKids] = 500;
        f.model.setLibraries(
            {library(kMovies, QStringLiteral("Movies")), library(kKids, QStringLiteral("Kids"))});
        QCOMPARE(options.status(), FilterOptionsModel::Loading);
        QTRY_COMPARE(f.model.status(), FavoritesModel::PartialError);
        QCOMPARE(options.status(), FilterOptionsModel::Ready);
        QCOMPARE(genreNames(), (QStringList{QStringLiteral("ACTION"), QStringLiteral("horror")}));
        QCOMPARE(options.tags(), (QStringList{QStringLiteral("Classic"), QStringLiteral("zzz")}));
        QCOMPARE(options.years(), (QList<int>{2021, 1980}));

        // Leaving while a scan runs drops it, and there is nothing to offer
        f.failing.clear();
        f.server.holdListings = true;
        const int before = int(f.server.itemsRoutes.size());
        f.model.reload();
        QTRY_VERIFY(f.server.itemsRoutes.size() > before);
        f.model.close();
        QCOMPARE(f.model.status(), FavoritesModel::Idle);
        QCOMPARE(options.status(), FilterOptionsModel::Idle);
        QCOMPARE(options.rowCount(), 0);
        QVERIFY(options.tags().isEmpty());
        QVERIFY(options.years().isEmpty());
        f.server.holdListings = false;
        f.server.releaseHeld();

        // A new snapshot fills them again, and a lost session empties them
        f.model.load();
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(options.status(), FilterOptionsModel::Ready);
        QVERIFY(options.available());
        f.client.disconnectFromServer();
        QCOMPARE(options.status(), FilterOptionsModel::Idle);
        QCOMPARE(options.legacyStatus(), FilterOptionsModel::Idle);
        QCOMPARE(options.rowCount(), 0);
        QVERIFY(options.tags().isEmpty());
    }
};

QTEST_MAIN(TestFavoritesModel)
#include "test_favorites_model.moc"
