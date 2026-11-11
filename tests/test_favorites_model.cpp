// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The Favorites snapshot and its groupings against the fake server: one request at a time on channel 3082, dedup across libraries, the 1000 limit and its notice, a failing library, stale replies, the loss of the session, and the five groupings
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/FavoritesGroupsModel.h"
#include "models/FavoritesModel.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::FavoritesGroupsModel;
using Kinefin::FavoritesModel;

namespace {
const QString kMovies = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
const QString kKids = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd02");
const QString kShows = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd03");
const QString kBoxsets = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd04");

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
            const int start = startOf(route);
            const int limit = FakeJellyfin::capturedListingQuery(route)
                                  .queryItemValue(QStringLiteral("limit"))
                                  .toInt();
            QJsonArray page;
            for (int i = start; i < start + limit && i < all.size(); ++i)
                page.append(all.at(i));
            const QByteArray body =
                QJsonDocument(QJsonObject{{QStringLiteral("Items"), page},
                                          {QStringLiteral("StartIndex"), start},
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
    FavoritesGroupsModel groups{&model};
    QHash<QString, QJsonArray> favorites;
    QHash<QString, int> failing;
    QHash<QString, int> declaredTotals;
    bool ok = false;
};

// "key:count" of every group
QStringList groupSummary(const FavoritesGroupsModel &groups)
{
    QStringList list;
    for (int row = 0; row < groups.count(); ++row) {
        const QModelIndex index = groups.index(row, 0);
        list.append(QStringLiteral("%1:%2")
                        .arg(groups.data(index, FavoritesGroupsModel::KeyRole).toString())
                        .arg(groups.data(index, FavoritesGroupsModel::CountRole).toInt()));
    }
    return list;
}

int groupedTotal(const FavoritesGroupsModel &groups)
{
    int total = 0;
    for (int row = 0; row < groups.count(); ++row)
        total += groups.data(groups.index(row, 0), FavoritesGroupsModel::CountRole).toInt();
    return total;
}

// The names of the cards of a group
QStringList groupNames(const FavoritesGroupsModel &groups, int row)
{
    const auto *items = qobject_cast<QAbstractItemModel *>(
        groups.data(groups.index(row, 0), FavoritesGroupsModel::ItemsRole).value<QObject *>());
    QStringList names;
    for (int i = 0; items && i < items->rowCount(); ++i)
        names.append(items->data(items->index(i, 0), FavoritesModel::NameRole).toString());
    return names;
}

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

const QVariantList kMixedLibraries{
    library(kMovies, QStringLiteral("Movies")), library(kKids, QStringLiteral("Kids")),
    library(kShows, QStringLiteral("Shows"), QStringLiteral("tvshows"))};
} // namespace

class TestFavoritesModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
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
        QVERIFY(first.queryItemValue(QStringLiteral("fields"), QUrl::FullyDecoded)
                    .contains(QStringLiteral("Genres")));
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
        f.groups.applyGrouping(FavoritesGroupsModel::Library);
        QCOMPARE(groupSummary(f.groups),
                 (QStringList{kMovies + QStringLiteral(":500"), kKids + QStringLiteral(":500")}));
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
        f.groups.applyGrouping(FavoritesGroupsModel::Library);
        QCOMPARE(groupSummary(f.groups), (QStringList{QStringLiteral("more-libraries:600")}));
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

    void keepsOneSanitisedMainGenre()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray lateGenres;
        for (int i = 0; i < FavoritesModel::MaxGenres; ++i)
            lateGenres.append(QString());
        lateGenres.append(QStringLiteral("Late"));
        QJsonObject skipsInvalid = favorite(1);
        skipsInvalid.insert(
            QStringLiteral("Genres"),
            QJsonArray{QString(), 7, (QStringLiteral("Sci") + QChar(ushort(0x202e)) + QStringLiteral("Fi\nx")), QStringLiteral("Drama")});
        QJsonObject tooLate = favorite(2);
        tooLate.insert(QStringLiteral("Genres"), lateGenres);
        QJsonObject tooLong = favorite(3);
        tooLong.insert(QStringLiteral("Genres"), QJsonArray{QString(200, QLatin1Char('g'))});
        QJsonObject wrongType = favorite(4);
        wrongType.insert(QStringLiteral("Genres"), QStringLiteral("Drama"));
        f.favorites[kMovies] = QJsonArray{skipsInvalid, tooLate, tooLong, wrongType};
        f.start({library(kMovies, QStringLiteral("Movies"))});
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        QCOMPARE(f.value(0, FavoritesModel::GenreRole).toString(), QStringLiteral("Sci Fi x"));
        QVERIFY(f.value(1, FavoritesModel::GenreRole).toString().isEmpty());
        QCOMPARE(f.value(2, FavoritesModel::GenreRole).toString().size(),
                 FavoritesModel::MaxGenreChars);
        QVERIFY(f.value(3, FavoritesModel::GenreRole).toString().isEmpty());
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
              "libraryIds", "libraryNames", "genre", "seriesId", "rating", "remainingMinutes",
              "isNew", "seasonCount", "episodeCount", "seriesProgress"})
            QVERIFY2(names.values().contains(name), name);
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
        QCOMPARE(f.groups.count(), 0);
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

    void groupsByTypeLibraryGenreYearOrNone()
    {
        Fixture f;
        QVERIFY(f.ok);
        fillMixed(f);
        f.start(kMixedLibraries);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        const int sent = f.server.itemsRoutes.size();
        QAbstractItemModelTester tester(&f.groups);

        QCOMPARE(f.groups.grouping(), FavoritesGroupsModel::Type);
        QCOMPARE(groupSummary(f.groups),
                 (QStringList{QStringLiteral("Movie:5"), QStringLiteral("Series:1"),
                              QStringLiteral("Season:1"), QStringLiteral("Episode:1")}));
        QCOMPARE(f.groups.data(f.groups.index(0, 0), FavoritesGroupsModel::KindRole).toInt(),
                 int(FavoritesGroupsModel::TypeGroup));
        QVERIFY(f.groups.data(f.groups.index(0, 0), FavoritesGroupsModel::TitleRole)
                    .toString()
                    .isEmpty());
        QCOMPARE(groupedTotal(f.groups), 8);

        f.groups.setGrouping(FavoritesGroupsModel::Library);
        QCOMPARE(groupSummary(f.groups),
                 (QStringList{kMovies + QStringLiteral(":3"), kKids + QStringLiteral(":1"),
                              kShows + QStringLiteral(":3"), QStringLiteral("more-libraries:1")}));
        QCOMPARE(f.groups.data(f.groups.index(0, 0), FavoritesGroupsModel::TitleRole).toString(),
                 QStringLiteral("Movies"));
        QCOMPARE(f.groups.data(f.groups.index(3, 0), FavoritesGroupsModel::KindRole).toInt(),
                 int(FavoritesGroupsModel::MoreLibrariesGroup));
        QVERIFY(f.groups.data(f.groups.index(3, 0), FavoritesGroupsModel::TitleRole)
                    .toString()
                    .isEmpty());
        QCOMPARE(groupNames(f.groups, 0),
                 (QStringList{QStringLiteral("Alpha"), QStringLiteral("Bravo"),
                              QStringLiteral("Charlie")}));
        QCOMPARE(groupNames(f.groups, 3), (QStringList{QStringLiteral("Delta")}));
        QCOMPARE(groupedTotal(f.groups), 8);

        // One main genre per item, in two spellings one group, no genre last
        f.groups.setGrouping(FavoritesGroupsModel::Genre);
        QCOMPARE(groupSummary(f.groups),
                 (QStringList{QStringLiteral("action:2"), QStringLiteral("comedy:2"),
                              QStringLiteral("drama:2"), QStringLiteral("no-genre:2")}));
        QCOMPARE(f.groups.data(f.groups.index(0, 0), FavoritesGroupsModel::TitleRole).toString(),
                 QStringLiteral("Action"));
        QCOMPARE(f.groups.data(f.groups.index(3, 0), FavoritesGroupsModel::KindRole).toInt(),
                 int(FavoritesGroupsModel::NoGenreGroup));
        QCOMPARE(groupedTotal(f.groups), 8);

        f.groups.setGrouping(FavoritesGroupsModel::Year);
        QCOMPARE(groupSummary(f.groups),
                 (QStringList{QStringLiteral("2020:2"), QStringLiteral("2015:2"),
                              QStringLiteral("2010:2"), QStringLiteral("1999:1"),
                              QStringLiteral("no-year:1")}));
        QCOMPARE(f.groups.data(f.groups.index(4, 0), FavoritesGroupsModel::KindRole).toInt(),
                 int(FavoritesGroupsModel::NoYearGroup));
        QCOMPARE(groupedTotal(f.groups), 8);

        f.groups.setGrouping(FavoritesGroupsModel::None);
        QCOMPARE(groupSummary(f.groups), (QStringList{QStringLiteral("all:8")}));
        QCOMPARE(f.groups.data(f.groups.index(0, 0), FavoritesGroupsModel::KindRole).toInt(),
                 int(FavoritesGroupsModel::AllGroup));
        QCOMPARE(groupNames(f.groups, 0).size(), 8);

        // Regrouping is client-side only
        QCOMPARE(f.server.itemsRoutes.size(), sent);
    }

    void groupItemsModelSharesTheCardRoles()
    {
        Fixture f;
        QVERIFY(f.ok);
        fillMixed(f);
        f.start(kMixedLibraries);
        QTRY_COMPARE(f.model.status(), FavoritesModel::Complete);
        const auto *items = qobject_cast<QAbstractItemModel *>(
            f.groups.data(f.groups.index(0, 0), FavoritesGroupsModel::ItemsRole)
                .value<QObject *>());
        QVERIFY(items);
        QCOMPARE(items->roleNames(), f.model.roleNames());
        QCOMPARE(items->rowCount(), 5);
        QVERIFY(!items->data(items->index(5, 0), FavoritesModel::NameRole).isValid());
    }

    void groupingChoiceSignalsAndTokens()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy changed(&f.groups, &FavoritesGroupsModel::groupingChanged);
        QSignalSpy edited(&f.groups, &FavoritesGroupsModel::groupingEdited);
        f.groups.setGrouping(FavoritesGroupsModel::Type);
        QCOMPARE(changed.size(), 0);
        f.groups.setGrouping(FavoritesGroupsModel::Library);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(edited.size(), 1);
        // The saved choice read at the start of a session is not an edit
        f.groups.applyGrouping(FavoritesGroupsModel::Genre);
        QCOMPARE(changed.size(), 2);
        QCOMPARE(edited.size(), 1);
        f.groups.setGrouping(FavoritesGroupsModel::Grouping(42));
        QCOMPARE(f.groups.grouping(), FavoritesGroupsModel::Genre);
        QCOMPARE(changed.size(), 2);

        for (const auto grouping :
             {FavoritesGroupsModel::Type, FavoritesGroupsModel::Library,
              FavoritesGroupsModel::Genre, FavoritesGroupsModel::Year, FavoritesGroupsModel::None})
            QCOMPARE(FavoritesGroupsModel::groupingFromToken(
                         FavoritesGroupsModel::groupingToken(grouping)),
                     grouping);
        QCOMPARE(FavoritesGroupsModel::groupingToken(FavoritesGroupsModel::None),
                 QStringLiteral("None"));
        QCOMPARE(FavoritesGroupsModel::groupingFromToken(QStringLiteral("bogus")),
                 FavoritesGroupsModel::Type);
        QCOMPARE(FavoritesGroupsModel::groupingFromToken(QString()), FavoritesGroupsModel::Type);
    }

    void groupsFollowTheSnapshot()
    {
        Fixture f;
        QVERIFY(f.ok);
        fillMixed(f);
        QSignalSpy counts(&f.groups, &FavoritesGroupsModel::countChanged);
        f.start(kMixedLibraries);
        // Nothing is grouped while the scan runs
        QCOMPARE(f.groups.count(), 0);
        QTRY_COMPARE(f.groups.count(), 4);
        QVERIFY(counts.size() >= 1);
        f.model.setLibraries({});
        QCOMPARE(f.groups.count(), 0);
        QCOMPARE(groupedTotal(f.groups), 0);
    }
};

QTEST_MAIN(TestFavoritesModel)
#include "test_favorites_model.moc"
