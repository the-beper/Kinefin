// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The genre rails against the fake server: the genre pages, rails that load only when wanted and at most three at a time, the rail query, empty and failed rails, the cap and the loss of the session
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/GenreRailsModel.h"
#include "models/ItemRowModel.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::GenreRailsModel;

namespace {
const QString kLibrary = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
const QString kSciFi = QStringLiteral("aa00000000000000000000000000ff01");
const QString kMystery = QStringLiteral("bb00000000000000000000000000ff02");
const QString kDrama = QStringLiteral("cc00000000000000000000000000ff03");

struct Fixture {
    Fixture()
    {
        server.itemsByGenre.insert(kSciFi, QStringLiteral("items_genre_scifi"));
        ok = server.listen() && login(client, server);
    }

    int railStatus(int row) const
    {
        return rails.data(rails.index(row), GenreRailsModel::RailStatusRole).toInt();
    }
    int railCount(int row) const
    {
        return rails.data(rails.index(row), GenreRailsModel::CountRole).toInt();
    }
    bool shown(int row) const
    {
        return rails.data(rails.index(row), GenreRailsModel::ShownRole).toBool();
    }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    GenreRailsModel rails{&client};
    bool ok = false;
};
} // namespace

class TestGenreRailsModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void opensTheFirstGenrePage()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(!f.rails.open(QStringLiteral("music"), kLibrary));
        QVERIFY(f.rails.open(QStringLiteral("movies"), kLibrary));
        QCOMPARE(f.rails.status(), GenreRailsModel::Loading);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        QCOMPARE(f.rails.rowCount(), 3);
        QCOMPARE(f.rails.libraryId(), kLibrary);
        QCOMPARE(f.rails.data(f.rails.index(0), GenreRailsModel::NameRole).toString(),
                 QStringLiteral("Science Fiction"));
        QCOMPARE(f.railStatus(0), int(GenreRailsModel::RailIdle));
        QVERIFY(f.shown(0));
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("Genres")).constFirst());
        QCOMPARE(wire.queryItemValue(QStringLiteral("parentId")), kLibrary);
        QCOMPARE(wire.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("Movie"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("20"));
    }

    void aRailLoadsOnlyWhenItIsWantedWithItsGenreId()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        QTest::qWait(150);
        QCOMPARE(f.server.itemsRoutes.size(), 0);
        f.rails.want(kSciFi);
        QCOMPARE(f.railStatus(0), int(GenreRailsModel::RailLoading));
        QTRY_COMPARE(f.railStatus(0), int(GenreRailsModel::RailReady));
        QCOMPARE(f.railCount(0), 2);
        QCOMPARE(f.server.itemsRoutes.size(), 1);
        const QUrlQuery wire =
            FakeJellyfin::capturedListingQuery(f.server.itemsRoutes.constFirst());
        QCOMPARE(wire.queryItemValue(QStringLiteral("genreIds")), kSciFi);
        QCOMPARE(wire.queryItemValue(QStringLiteral("parentId")), kLibrary);
        QCOMPARE(wire.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("Movie"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("recursive")), QStringLiteral("true"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("12"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("sortBy")), QStringLiteral("Random"));
        QVERIFY(wire.queryItemValue(QStringLiteral("enableImageTypes"))
                    .contains(QStringLiteral("Thumb")));
    }

    void atMostThreeRailsRunAtOnceAndTheNewestGoesFirst()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedGenreTotal = 10;
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        const auto id = [](int row) {
            return QStringLiteral("%1").arg(row + 1, 32, 16, QLatin1Char('d'));
        };
        f.server.holdListings = true;
        for (int row = 0; row < 5; ++row)
            f.rails.want(id(row));
        QTRY_COMPARE(f.server.itemsRoutes.size(), 3);
        QTest::qWait(150);
        QCOMPARE(f.server.itemsRoutes.size(), 3);
        // A rail the view no longer needs is never asked for
        f.rails.unwant(id(3));
        // The held answers fail, the slots free up and the newest wanted rail goes next
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTRY_VERIFY(f.server.itemsRoutes.size() >= 4);
        QVERIFY(f.server.itemsRoutes.at(3).contains(id(4).toUtf8()));
        for (const QByteArray &route : f.server.itemsRoutes)
            QVERIFY(!route.contains(id(3).toUtf8()));
    }

    void anEmptyRailIsHidden()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingRawBody.insert(
            QStringLiteral("Items"),
            QByteArrayLiteral("{\"Items\":[],\"StartIndex\":0,\"TotalRecordCount\":0}"));
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        f.rails.want(kMystery);
        QTRY_COMPARE(f.railStatus(1), int(GenreRailsModel::RailReady));
        QVERIFY(!f.shown(1));
        QVERIFY(f.shown(0));
    }

    void aFailedRailIsAloneAndRetryable()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        f.rails.want(kMystery);
        QTRY_COMPARE(f.railStatus(1), int(GenreRailsModel::RailFailed));
        QVERIFY(f.shown(1));
        QCOMPARE(f.railStatus(0), int(GenreRailsModel::RailIdle));
        f.server.listingStatus.clear();
        f.server.itemsByGenre.insert(kMystery, QStringLiteral("items_genre_scifi"));
        f.rails.retry(1);
        QTRY_COMPARE(f.railStatus(1), int(GenreRailsModel::RailReady));
        QCOMPARE(f.railCount(1), 2);
    }

    void theGenreListPagesAndStopsAtTheCap()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.generatedGenreTotal = 250;
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        QCOMPARE(f.rails.rowCount(), 20);
        while (f.rails.canLoadMore()) {
            f.rails.loadMore();
            QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        }
        QCOMPARE(f.rails.rowCount(), int(GenreRailsModel::MaxRails));
        const QUrlQuery last = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("Genres")).constLast());
        QCOMPARE(last.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("80"));
    }

    void aPageThatAddsNothingEndsTheList()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QJsonArray bad{QJsonObject{{QStringLiteral("Id"), QStringLiteral("../x")},
                                         {QStringLiteral("Name"), QStringLiteral("bad")}},
                             QJsonObject{{QStringLiteral("Id"), kSciFi},
                                         {QStringLiteral("Name"), QStringLiteral("  ")}}};
        f.server.listingRawBody.insert(
            QStringLiteral("Genres"),
            QJsonDocument(QJsonObject{{QStringLiteral("Items"), bad},
                                      {QStringLiteral("TotalRecordCount"), 500}})
                .toJson(QJsonDocument::Compact));
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        QCOMPARE(f.rails.rowCount(), 0);
        QVERIFY(!f.rails.canLoadMore());
        f.rails.loadMore();
        QTest::qWait(150);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Genres")).size(), 1);
    }

    void theNextGenrePageStartsAfterWhatTheServerSent()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray page;
        for (int i = 0; i < 20; ++i) {
            // Every second entry is dropped: the offset must still advance by 20
            page.append(
                QJsonObject{{QStringLiteral("Id"),
                             i % 2 ? QStringLiteral("%1").arg(i + 1, 32, 16, QLatin1Char('f'))
                                   : QStringLiteral("bad id")},
                            {QStringLiteral("Name"), QStringLiteral("G%1").arg(i)}});
        }
        f.server.listingRawBody.insert(
            QStringLiteral("Genres"),
            QJsonDocument(QJsonObject{{QStringLiteral("Items"), page},
                                      {QStringLiteral("TotalRecordCount"), 60}})
                .toJson(QJsonDocument::Compact));
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        QCOMPARE(f.rails.rowCount(), 10);
        QVERIFY(f.rails.canLoadMore());
        f.rails.loadMore();
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Genres")).size(), 2);
        const QUrlQuery second = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("Genres")).constLast());
        QCOMPARE(second.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("20"));
    }

    void aFailedGenreListRetries()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Genres"), 500);
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Failed);
        f.server.listingStatus.clear();
        f.rails.retryList();
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        QCOMPARE(f.rails.rowCount(), 3);
    }

    void untrustedGenresAreDropped()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QJsonArray genres{
            QJsonObject{{QStringLiteral("Id"), kSciFi},
                        {QStringLiteral("Name"), QString(300, QLatin1Char('n'))}},
            QJsonObject{{QStringLiteral("Id"), kSciFi},
                        {QStringLiteral("Name"), QStringLiteral("Duplicate")}},
            QJsonObject{{QStringLiteral("Id"), QStringLiteral("../x")},
                        {QStringLiteral("Name"), QStringLiteral("bad")}},
            QJsonObject{{QStringLiteral("Id"), kMystery},
                        {QStringLiteral("Name"), QStringLiteral("")}},
            QJsonObject{{QStringLiteral("Id"), kDrama},
                        {QStringLiteral("Name"), QStringLiteral("<b>Noir</b> & more")}}};
        f.server.listingRawBody.insert(
            QStringLiteral("Genres"),
            QJsonDocument(QJsonObject{{QStringLiteral("Items"), genres},
                                      {QStringLiteral("TotalRecordCount"), 5}})
                .toJson(QJsonDocument::Compact));
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        QCOMPARE(f.rails.rowCount(), 2);
        QCOMPARE(f.rails.data(f.rails.index(0), GenreRailsModel::NameRole).toString().size(),
                 int(GenreRailsModel::MaxNameLength));
        QCOMPARE(f.rails.data(f.rails.index(1), GenreRailsModel::NameRole).toString(),
                 QStringLiteral("<b>Noir</b> & more"));
    }

    void clearDropsTheRailsAndTheLateReplies()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        QSignalSpy failures(&f.client, &Kinefin::JellyfinClient::listingFailed);
        f.server.holdListings = true;
        f.rails.want(kSciFi);
        QTRY_COMPARE(f.server.itemsRoutes.size(), 1);
        f.rails.clear();
        QCOMPARE(f.rails.rowCount(), 0);
        QCOMPARE(f.rails.status(), GenreRailsModel::Idle);
        // The held request is answered anyway: an aborted one stays silent
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTest::qWait(250);
        QCOMPARE(failures.size(), 0);
        QCOMPARE(f.rails.rowCount(), 0);
    }

    void allScopeSendsNoParentId()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy answers(&f.client, &Kinefin::JellyfinClient::genresReady);
        QVERIFY(!f.rails.open(QStringLiteral("movies"), QStringLiteral("bad id")));
        QVERIFY(f.rails.open(QStringLiteral("movies")));
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        QVERIFY(f.rails.libraryId().isEmpty());
        QCOMPARE(answers.constFirst().at(2).toInt(), int(GenreRailsModel::ListChannel));
        const QByteArray listRoute =
            f.server.listingRoutes.value(QStringLiteral("Genres")).constFirst();
        QVERIFY2(!listRoute.contains("parentId="), listRoute.constData());
        QVERIFY(listRoute.contains("includeItemTypes=Movie"));
        // A rail of the all scope has no parent either
        f.rails.want(kSciFi);
        QTRY_COMPARE(f.railStatus(0), int(GenreRailsModel::RailReady));
        const QByteArray railRoute = f.server.itemsRoutes.constFirst();
        QVERIFY2(!railRoute.contains("parentId="), railRoute.constData());
        QVERIFY(railRoute.contains(QByteArray("genreIds=").append(kSciFi.toUtf8())));
        // A failed list retries without a library id too
        f.server.listingStatus.insert(QStringLiteral("Genres"), 500);
        QVERIFY(f.rails.open(QStringLiteral("tvshows")));
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Failed);
        f.server.listingStatus.clear();
        f.rails.retryList();
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
    }

    void listChannelReleasedOnClear()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy answers(&f.client, &Kinefin::JellyfinClient::genresReady);
        QSignalSpy failures(&f.client, &Kinefin::JellyfinClient::listingFailed);
        f.server.holdListings = true;
        f.rails.open(QStringLiteral("movies"));
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Genres")).size(), 1);
        f.rails.clear();
        QCOMPARE(f.rails.status(), GenreRailsModel::Idle);
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTest::qWait(250);
        QCOMPARE(answers.size(), 0);
        QCOMPARE(failures.size(), 0);
        QCOMPARE(f.rails.rowCount(), 0);
        // A reopen on the same channel answers once
        f.rails.open(QStringLiteral("movies"));
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        QCOMPARE(answers.size(), 1);
    }

    void aLostSessionEmptiesTheRails()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.rails.open(QStringLiteral("movies"), kLibrary);
        QTRY_COMPARE(f.rails.status(), GenreRailsModel::Ready);
        f.client.disconnectFromServer();
        QCOMPARE(f.rails.rowCount(), 0);
        QCOMPARE(f.rails.status(), GenreRailsModel::Idle);
        QVERIFY(f.rails.libraryId().isEmpty());
    }
};

QTEST_MAIN(TestGenreRailsModel)
#include "test_genre_rails_model.moc"
