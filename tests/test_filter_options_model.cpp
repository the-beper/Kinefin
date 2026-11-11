// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The genre options of a library against the fake server: the wire, untrusted entries, an empty or failed answer, and the loss of the session
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/FilterOptionsModel.h"

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
