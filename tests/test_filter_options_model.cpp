// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The filter options of a library against the fake server: genres and tags from Filters2, official ratings and years from the legacy endpoint, each source loading and failing on its own, the wire, untrusted entries, an empty or failed answer, and the loss of the session
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/FilterOptionsModel.h"
#include "models/LibraryFilterState.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::FilterOptionsModel;

namespace {
const QString kLibrary = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");

QByteArray filtersBody(const QJsonArray &genres)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("Genres"), genres}})
        .toJson(QJsonDocument::Compact);
}

QByteArray legacyBody(const QJsonArray &ratings, const QJsonArray &years)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("OfficialRatings"), ratings},
                                     {QStringLiteral("Years"), years}})
        .toJson(QJsonDocument::Compact);
}

struct Fixture {
    Fixture() { ok = server.listen() && login(client, server); }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    FilterOptionsModel options{&client, 6};
    bool ok = false;
};
} // namespace

class TestFilterOptionsModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void loadsTheGenresOfALibrary()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QCOMPARE(f.options.status(), FilterOptionsModel::Loading);
        QVERIFY(!f.options.available());
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QVERIFY(f.options.available());
        QCOMPARE(f.options.rowCount(), 3);
        QCOMPARE(f.options.data(f.options.index(0), FilterOptionsModel::NameRole).toString(),
                 QStringLiteral("Science Fiction"));
        QCOMPARE(f.options.data(f.options.index(0), FilterOptionsModel::GenreIdRole).toString(),
                 QStringLiteral("aa00000000000000000000000000ff01"));
        QVERIFY(f.options.contains(QStringLiteral("BB00000000000000000000000000FF02")));
        QVERIFY(!f.options.contains(QStringLiteral("dd00000000000000000000000000ff09")));
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("Filters")).constFirst());
        QCOMPARE(wire.queryItemValue(QStringLiteral("parentId")), kLibrary);
        QCOMPARE(wire.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("Movie"));
        QCOMPARE(wire.queryItemValue(QStringLiteral("recursive")), QStringLiteral("true"));
    }

    void untrustedEntriesAreDroppedAndBounded()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray genres;
        for (int i = 0; i < 600; ++i)
            genres.append(QJsonObject{
                {QStringLiteral("Id"), QStringLiteral("%1").arg(i + 1, 32, 16, QLatin1Char('c'))},
                {QStringLiteral("Name"), QStringLiteral("G%1").arg(i)}});
        genres.append(QJsonObject{{QStringLiteral("Id"), QStringLiteral("../etc")},
                                  {QStringLiteral("Name"), QStringLiteral("bad id")}});
        f.server.listingRawBody.insert(QStringLiteral("Filters"), filtersBody(genres));
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QCOMPARE(f.options.rowCount(), int(FilterOptionsModel::MaxOptions));

        const QString id = QStringLiteral("aa00000000000000000000000000ff01");
        const QJsonArray mixed{
            QJsonObject{{QStringLiteral("Id"), id},
                        {QStringLiteral("Name"), QString(300, QLatin1Char('n'))}},
            QJsonObject{{QStringLiteral("Id"), id},
                        {QStringLiteral("Name"), QStringLiteral("Duplicate")}},
            QJsonObject{{QStringLiteral("Id"), QStringLiteral("bb00000000000000000000000000ff02")},
                        {QStringLiteral("Name"), QStringLiteral("   ")}},
            QJsonObject{{QStringLiteral("Id"), 42},
                        {QStringLiteral("Name"), QStringLiteral("number id")}},
            QJsonObject{{QStringLiteral("Id"), QStringLiteral("cc00000000000000000000000000ff03")},
                        {QStringLiteral("Name"), QStringLiteral("<b>Noir</b> & <img src=x>")}}};
        f.server.listingRawBody.insert(QStringLiteral("Filters"), filtersBody(mixed));
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.rowCount(), 2);
        // The name is capped, the duplicate, the nameless and the bad ids are gone, markup stays literal text
        QCOMPARE(f.options.data(f.options.index(0), FilterOptionsModel::NameRole).toString().size(),
                 int(FilterOptionsModel::MaxNameLength));
        QCOMPARE(f.options.data(f.options.index(1), FilterOptionsModel::NameRole).toString(),
                 QStringLiteral("<b>Noir</b> & <img src=x>"));
    }

    void aCutNeverSplitsASurrogatePair()
    {
        const QString name = QString(FilterOptionsModel::MaxNameLength - 1, QLatin1Char('a'))
                             + QString::fromUcs4(U"\U0001F3AC") + QStringLiteral("tail");
        const QString capped = FilterOptionsModel::cappedName(name);
        QCOMPARE(capped.size(), FilterOptionsModel::MaxNameLength - 1);
        QVERIFY(!capped.back().isHighSurrogate());
        QCOMPARE(FilterOptionsModel::cappedName(QStringLiteral("  Drama  ")),
                 QStringLiteral("Drama"));
    }

    void anEmptyAnswerIsNotAvailable()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingRawBody.insert(QStringLiteral("Filters"), filtersBody({}));
        f.options.load(kLibrary, QStringLiteral("Series"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QVERIFY(!f.options.available());
        QCOMPARE(f.options.rowCount(), 0);
    }

    void aFailureHidesTheOptionsAndRetryRecovers()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Failed);
        QVERIFY(!f.options.available());
        f.server.listingStatus.clear();
        f.options.retry();
        QCOMPARE(f.options.status(), FilterOptionsModel::Loading);
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QVERIFY(f.options.available());
    }

    void anotherChannelsFiltersAreIgnored()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy answers(&f.client, &Kinefin::JellyfinClient::filtersReady);
        QSignalSpy failures(&f.client, &Kinefin::JellyfinClient::listingFailed);
        f.server.holdFirstListings = 1;
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Filters")).size(), 1);
        // Another consumer's answer and failure do not reach this model
        QVERIFY(f.client.fetchFilters(kLibrary, QStringLiteral("Movie"), 7));
        QTRY_COMPARE(answers.size(), 1);
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        QVERIFY(f.client.fetchFilters(kLibrary, QStringLiteral("Movie"), 7));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(f.options.status(), FilterOptionsModel::Loading);
        QCOMPARE(f.options.rowCount(), 0);
        // Its own channel is the one that counts
        f.server.listingStatus.clear();
        QVERIFY(f.client.fetchFilters(kLibrary, QStringLiteral("Movie"), 6));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QCOMPARE(f.options.rowCount(), 3);
    }

    void retryWithoutParentId()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        f.options.load(QString(), QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Failed);
        f.server.listingStatus.clear();
        f.options.retry();
        QCOMPARE(f.options.status(), FilterOptionsModel::Loading);
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QVERIFY(f.options.available());
        const QByteArray route =
            f.server.listingRoutes.value(QStringLiteral("Filters")).constLast();
        QVERIFY2(!route.contains("parentId="), route.constData());
        QVERIFY(route.contains("includeItemTypes=Movie"));
    }

    void aLateAnswerAfterAClearChangesNothing()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.holdListings = true;
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Filters")).size(), 1);
        f.options.clear();
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTest::qWait(200);
        QCOMPARE(f.options.status(), FilterOptionsModel::Idle);
        QCOMPARE(f.options.rowCount(), 0);
    }

    void loadsTagsRatingsAndYears()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QCOMPARE(f.options.status(), FilterOptionsModel::Loading);
        QCOMPARE(f.options.legacyStatus(), FilterOptionsModel::Loading);
        QVERIFY(!f.options.tagsAvailable());
        QVERIFY(!f.options.ratingsAvailable());
        QVERIFY(!f.options.yearsAvailable());
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QTRY_COMPARE(f.options.legacyStatus(), FilterOptionsModel::Ready);
        QVERIFY(f.options.tagsAvailable());
        QCOMPARE(f.options.tags(),
                 QStringList({QStringLiteral("restored-master"), QStringLiteral("black-and-white"),
                              QStringLiteral("director-cut")}));
        QVERIFY(f.options.ratingsAvailable());
        QCOMPARE(f.options.officialRatings(),
                 QStringList({QStringLiteral("PG"), QStringLiteral("PG-13"), QStringLiteral("R")}));
        QVERIFY(f.options.yearsAvailable());
        QCOMPARE(f.options.years(), QList<int>({2024, 2019, 2012}));
        // The legacy endpoint is asked for the same library and kind, on the channel of the model
        const QUrlQuery wire = FakeJellyfin::capturedListingQuery(
            f.server.listingRoutes.value(QStringLiteral("FiltersLegacy")).constFirst());
        QCOMPARE(wire.queryItemValue(QStringLiteral("parentId")), kLibrary);
        QCOMPARE(wire.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("Movie"));
        QVERIFY(f.server.lastListingRoute.value(QStringLiteral("FiltersLegacy"))
                    .contains("/Items/Filters?"));
    }

    void aFailingSourceLeavesTheOtherFacets()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 500);
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QTRY_COMPARE(f.options.legacyStatus(), FilterOptionsModel::Failed);
        QVERIFY(f.options.available());
        QVERIFY(f.options.tagsAvailable());
        QVERIFY(!f.options.ratingsAvailable());
        QVERIFY(!f.options.yearsAvailable());
        // Retry asks only for the source that failed
        f.server.listingStatus.clear();
        f.options.retry();
        QCOMPARE(f.options.legacyStatus(), FilterOptionsModel::Loading);
        QCOMPARE(f.options.status(), FilterOptionsModel::Ready);
        QTRY_COMPARE(f.options.legacyStatus(), FilterOptionsModel::Ready);
        QVERIFY(f.options.ratingsAvailable());
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Filters")).size(), 1);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("FiltersLegacy")).size(), 2);
    }

    void theGenreSourceFailingLeavesRatingsAndYears()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        f.options.load(kLibrary, QStringLiteral("Series"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Failed);
        QTRY_COMPARE(f.options.legacyStatus(), FilterOptionsModel::Ready);
        QVERIFY(!f.options.available());
        QVERIFY(!f.options.tagsAvailable());
        QVERIFY(f.options.ratingsAvailable());
        QVERIFY(f.options.yearsAvailable());
        f.server.listingStatus.clear();
        f.options.retry();
        QCOMPARE(f.options.status(), FilterOptionsModel::Loading);
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("FiltersLegacy")).size(), 1);
    }

    void aMissingLegacyEndpointHidesRatingsAndYearsOnly()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 404);
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.legacyStatus(), FilterOptionsModel::Unavailable);
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QVERIFY(f.options.available());
        QVERIFY(f.options.tagsAvailable());
        QVERIFY(!f.options.ratingsAvailable());
        QVERIFY(!f.options.yearsAvailable());
        // Nothing to retry: no request is ever repeated for a server without the endpoint
        f.options.retry();
        QTest::qWait(150);
        QCOMPARE(f.options.legacyStatus(), FilterOptionsModel::Unavailable);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("FiltersLegacy")).size(), 1);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Filters")).size(), 1);
        // Another failure is not a missing endpoint
        f.server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 503);
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.legacyStatus(), FilterOptionsModel::Failed);
    }

    void legacyValuesAreBoundedAndTagsAreText()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray ratings;
        for (int i = 0; i < 400; ++i)
            ratings.append(QStringLiteral("R-%1").arg(i));
        ratings.append(QStringLiteral("a|b"));
        QJsonArray years;
        for (int i = 0; i < 400; ++i)
            years.append(1900 + i);
        f.server.listingRawBody.insert(QStringLiteral("FiltersLegacy"), legacyBody(ratings, years));
        QJsonArray tags;
        for (int i = 0; i < 400; ++i)
            tags.append(QStringLiteral("tag %1").arg(i));
        f.server.listingRawBody.insert(
            QStringLiteral("Filters"),
            QJsonDocument(QJsonObject{{QStringLiteral("Tags"), tags}}).toJson());
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QTRY_COMPARE(f.options.legacyStatus(), FilterOptionsModel::Ready);
        // Every tag is kept (up to the memory cap), the ratings and years stay at the facet cap
        QCOMPARE(f.options.tags().size(), 400);
        QCOMPARE(f.options.officialRatings().size(), Kinefin::MaxFacetOptions);
        QCOMPARE(f.options.years().size(), Kinefin::MaxFacetOptions);

        // Untrusted entries: only text a query can carry stays, once each, in the server's order
        f.server.listingRawBody.insert(
            QStringLiteral("Filters"),
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("Tags"),
                     QJsonArray{QStringLiteral("good"), QStringLiteral("a|b"), QStringLiteral(""),
                                QStringLiteral("good"), 42, QString(200, QLatin1Char('t')),
                                QStringLiteral("line\nbreak"), QStringLiteral("<b>x</b>")}}})
                .toJson());
        f.server.listingRawBody.insert(
            QStringLiteral("FiltersLegacy"),
            legacyBody({QStringLiteral("PG"), QStringLiteral("a|b"), QStringLiteral("PG"), 7,
                        QString(200, QLatin1Char('r'))},
                       {2020, 0, 10000, -5, 2020, 1.5, QStringLiteral("2021"), 2019}));
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.tags(),
                     QStringList({QStringLiteral("good"), QStringLiteral("<b>x</b>")}));
        QTRY_COMPARE(f.options.officialRatings(), QStringList({QStringLiteral("PG")}));
        QCOMPARE(f.options.years(), QList<int>({2020, 2019}));
    }

    void tagsAreKeptUpToTheMemoryCap()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray tags;
        for (int i = 0; i < Kinefin::MaxTagOptions + 50; ++i)
            tags.append(QStringLiteral("tag %1").arg(i));
        f.server.listingRawBody.insert(
            QStringLiteral("Filters"),
            QJsonDocument(QJsonObject{{QStringLiteral("Tags"), tags}}).toJson());
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QCOMPARE(f.options.tags().size(), Kinefin::MaxTagOptions);
        QCOMPARE(f.options.tagCount(), Kinefin::MaxTagOptions);
        // The server's order is kept, the ones past the cap are the ones that go
        QCOMPARE(f.options.tags().constFirst(), QStringLiteral("tag 0"));
        QCOMPARE(f.options.tags().constLast(),
                 QStringLiteral("tag %1").arg(Kinefin::MaxTagOptions - 1));
    }

    void tagRowsListTheFirstTagsAndASearchFindsTheOthers()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray tags;
        for (int i = 0; i < 2225; ++i)
            tags.append(QStringLiteral("Tag %1").arg(i, 4, 10, QLatin1Char('0')));
        tags.append(QStringLiteral("Noir Classics"));
        f.server.listingRawBody.insert(
            QStringLiteral("Filters"),
            QJsonDocument(QJsonObject{{QStringLiteral("Tags"), tags}}).toJson());
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QCOMPARE(f.options.tagCount(), 2226);

        // No search: the first 300, in the server's order
        QList<int> rows = f.options.tagRows(QString(), {});
        QCOMPARE(rows.size(), FilterOptionsModel::MaxShownTags);
        QCOMPARE(rows.constFirst(), 0);
        QCOMPARE(rows.constLast(), FilterOptionsModel::MaxShownTags - 1);
        QCOMPARE(f.options.tagRows(QStringLiteral("  \t"), {}), rows);

        // A search finds the tags past the first 300, whatever the case, and trims its text
        rows = f.options.tagRows(QStringLiteral("  noIR clas "), {});
        QCOMPARE(rows, QList<int>({2225}));
        rows = f.options.tagRows(QStringLiteral("tag 2200"), {});
        QCOMPARE(rows, QList<int>({2200}));
        // A search that matches more than 300 lists them all
        QCOMPARE(f.options.tagRows(QStringLiteral("tag 1"), {}).size(), 1000);
        QVERIFY(f.options.tagRows(QStringLiteral("nothing like it"), {}).isEmpty());

        // A tag to keep stays listed whether or not the search matches it, once, in the server's order
        rows = f.options.tagRows(QStringLiteral("noir"), {QStringLiteral("Tag 0005")});
        QCOMPARE(rows, QList<int>({5, 2225}));
        rows = f.options.tagRows(QStringLiteral("tag 0005"), {QStringLiteral("Tag 0005")});
        QCOMPARE(rows, QList<int>({5}));
        // Without a search a kept tag past the first 300 comes after them
        rows = f.options.tagRows(QString(), {QStringLiteral("Tag 1500"), QStringLiteral("gone")});
        QCOMPARE(rows.size(), FilterOptionsModel::MaxShownTags + 1);
        QCOMPARE(rows.constLast(), 1500);
    }

    void anotherChannelsLegacyAnswersAreIgnored()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy answers(&f.client, &Kinefin::JellyfinClient::legacyFiltersReady);
        QSignalSpy failures(&f.client, &Kinefin::JellyfinClient::legacyFiltersFailed);
        // The two requests of this model are held, the ones of another consumer are not
        f.server.holdFirstListings = 2;
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("FiltersLegacy")).size(), 1);
        QVERIFY(f.client.fetchLegacyFilters(kLibrary, QStringLiteral("Movie"), 7));
        QTRY_COMPARE(answers.size(), 1);
        f.server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 500);
        QVERIFY(f.client.fetchLegacyFilters(kLibrary, QStringLiteral("Movie"), 7));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(f.options.legacyStatus(), FilterOptionsModel::Loading);
        QVERIFY(f.options.years().isEmpty());
        // Its own channel is the one that counts
        f.server.listingStatus.clear();
        QVERIFY(f.client.fetchLegacyFilters(kLibrary, QStringLiteral("Movie"), 6));
        QTRY_COMPARE(f.options.legacyStatus(), FilterOptionsModel::Ready);
        QCOMPARE(f.options.years().size(), 3);
    }

    void clearAndALostSessionEmptyEveryFacet()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QTRY_COMPARE(f.options.legacyStatus(), FilterOptionsModel::Ready);
        f.options.clear();
        QCOMPARE(f.options.status(), FilterOptionsModel::Idle);
        QCOMPARE(f.options.legacyStatus(), FilterOptionsModel::Idle);
        QVERIFY(f.options.tags().isEmpty());
        QVERIFY(f.options.officialRatings().isEmpty());
        QVERIFY(f.options.years().isEmpty());
        QCOMPARE(f.options.rowCount(), 0);

        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.legacyStatus(), FilterOptionsModel::Ready);
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        f.client.disconnectFromServer();
        QCOMPARE(f.options.legacyStatus(), FilterOptionsModel::Idle);
        QVERIFY(!f.options.ratingsAvailable());
        QVERIFY(!f.options.tagsAvailable());
    }

    void localValuesAreReadyWithoutARequestAndKeepTheCaps()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy options(&f.options, &FilterOptionsModel::optionsChanged);
        f.options.setLocalLoading();
        QCOMPARE(f.options.status(), FilterOptionsModel::Loading);
        QCOMPARE(f.options.legacyStatus(), FilterOptionsModel::Loading);
        QVERIFY(!f.options.available());
        QVERIFY(!f.options.ratingsAvailable());
        QCOMPARE(options.size(), 0);

        // Nothing to offer is still an answer: Ready, with no facet available
        f.options.setLocalValues({}, {}, {}, {});
        QCOMPARE(f.options.status(), FilterOptionsModel::Ready);
        QCOMPARE(f.options.legacyStatus(), FilterOptionsModel::Ready);
        QVERIFY(!f.options.available());
        QVERIFY(!f.options.tagsAvailable());
        QVERIFY(!f.options.ratingsAvailable());
        QVERIFY(!f.options.yearsAvailable());
        QCOMPARE(f.options.rowCount(), 0);

        // The caps of the requests apply, in the order given: a name is cut and trimmed, the repeats (any case), the empty and the unfit are dropped
        QStringList genres{QStringLiteral("Drama"), QStringLiteral("drama"), QString(),
                           QStringLiteral("   "), QString(300, QLatin1Char('n'))};
        QStringList ratings{QStringLiteral("R-0"), QStringLiteral("a|b"), QString(),
                            QString(200, QLatin1Char('r')), QStringLiteral("R-0")};
        QStringList tags{QStringLiteral("tag 0"), QStringLiteral("a|b"), QString(),
                         QStringLiteral("tag 0")};
        QList<int> years{0, 10000, -5, 1900};
        for (int i = 1; i < 600; ++i)
            genres.append(QStringLiteral("G%1").arg(i));
        for (int i = 1; i < 400; ++i)
            ratings.append(QStringLiteral("R-%1").arg(i));
        for (int i = 1; i < Kinefin::MaxTagOptions + 50; ++i)
            tags.append(QStringLiteral("tag %1").arg(i));
        for (int i = 1; i < 400; ++i)
            years.append(1900 + i);
        years.append(1900);
        f.options.setLocalValues(genres, ratings, years, tags);

        QCOMPARE(f.options.status(), FilterOptionsModel::Ready);
        QCOMPARE(f.options.legacyStatus(), FilterOptionsModel::Ready);
        QVERIFY(f.options.available());
        QVERIFY(f.options.tagsAvailable());
        QVERIFY(f.options.ratingsAvailable());
        QVERIFY(f.options.yearsAvailable());
        QCOMPARE(options.size(), 2);
        QCOMPARE(f.options.rowCount(), int(FilterOptionsModel::MaxOptions));
        QCOMPARE(f.options.officialRatings().size(), Kinefin::MaxFacetOptions);
        QCOMPARE(f.options.years().size(), Kinefin::MaxFacetOptions);
        QCOMPARE(f.options.tags().size(), Kinefin::MaxTagOptions);
        // A genre row's id is its name
        for (int row = 0; row < f.options.rowCount(); ++row)
            QCOMPARE(f.options.data(f.options.index(row), FilterOptionsModel::GenreIdRole),
                     f.options.data(f.options.index(row), FilterOptionsModel::NameRole));
        QCOMPARE(f.options.data(f.options.index(0), FilterOptionsModel::NameRole).toString(),
                 QStringLiteral("Drama"));
        QCOMPARE(f.options.data(f.options.index(1), FilterOptionsModel::NameRole).toString().size(),
                 int(FilterOptionsModel::MaxNameLength));
        QCOMPARE(f.options.data(f.options.index(2), FilterOptionsModel::NameRole).toString(),
                 QStringLiteral("G1"));
        QCOMPARE(f.options
                     .data(f.options.index(FilterOptionsModel::MaxOptions - 1),
                           FilterOptionsModel::NameRole)
                     .toString(),
                 QStringLiteral("G%1").arg(FilterOptionsModel::MaxOptions - 2));
        QVERIFY(f.options.contains(QStringLiteral("DRAMA")));
        QVERIFY(!f.options.contains(QStringLiteral("G%1").arg(FilterOptionsModel::MaxOptions - 1)));
        QCOMPARE(f.options.officialRatings().constFirst(), QStringLiteral("R-0"));
        QCOMPARE(f.options.officialRatings().at(1), QStringLiteral("R-1"));
        QCOMPARE(f.options.years().constFirst(), 1900);
        QCOMPARE(f.options.years().at(1), 1901);
        QCOMPARE(f.options.years().constLast(), 1900 + Kinefin::MaxFacetOptions - 1);
        QCOMPARE(f.options.tags().constFirst(), QStringLiteral("tag 0"));
        QCOMPARE(f.options.tags().at(1), QStringLiteral("tag 1"));
        QCOMPARE(f.options.tags().constLast(),
                 QStringLiteral("tag %1").arg(Kinefin::MaxTagOptions - 1));

        // No request went out: the first the server sees for each source is the one of another consumer, asked after the calls above
        QSignalSpy filtersAnswered(&f.client, &Kinefin::JellyfinClient::filtersReady);
        QSignalSpy legacyAnswered(&f.client, &Kinefin::JellyfinClient::legacyFiltersReady);
        QVERIFY(f.client.fetchFilters(kLibrary, QStringLiteral("Movie"), 7));
        QVERIFY(f.client.fetchLegacyFilters(kLibrary, QStringLiteral("Movie"), 7));
        QTRY_COMPARE(filtersAnswered.size(), 1);
        QTRY_COMPARE(legacyAnswered.size(), 1);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Filters")).size(), 1);
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("FiltersLegacy")).size(), 1);
        // The answers of that other channel do not touch the local values
        QCOMPARE(f.options.status(), FilterOptionsModel::Ready);
        QCOMPARE(f.options.rowCount(), int(FilterOptionsModel::MaxOptions));

        // Local values go like the others: clear() and a lost session empty them
        f.options.clear();
        QCOMPARE(f.options.status(), FilterOptionsModel::Idle);
        QCOMPARE(f.options.rowCount(), 0);
        f.options.setLocalValues({QStringLiteral("Drama")}, {QStringLiteral("PG")}, {2020},
                                 {QStringLiteral("classic")});
        QVERIFY(f.options.available());
        f.client.disconnectFromServer();
        QCOMPARE(f.options.status(), FilterOptionsModel::Idle);
        QCOMPARE(f.options.legacyStatus(), FilterOptionsModel::Idle);
        QCOMPARE(f.options.rowCount(), 0);
        QVERIFY(f.options.tags().isEmpty());
        QVERIFY(f.options.officialRatings().isEmpty());
        QVERIFY(f.options.years().isEmpty());
    }

    void aLostSessionEmptiesTheOptions()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.options.load(kLibrary, QStringLiteral("Movie"));
        QTRY_COMPARE(f.options.status(), FilterOptionsModel::Ready);
        QSignalSpy changed(&f.options, &FilterOptionsModel::optionsChanged);
        f.client.disconnectFromServer();
        QCOMPARE(f.options.status(), FilterOptionsModel::Idle);
        QCOMPARE(f.options.rowCount(), 0);
        QCOMPARE(changed.size(), 1);
    }
};

QTEST_MAIN(TestFilterOptionsModel)
#include "test_filter_options_model.moc"
