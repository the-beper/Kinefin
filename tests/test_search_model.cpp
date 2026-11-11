// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The search against the fake server: debouncing, the wire of the four group requests, a term that is too short, a newer term replacing an older one, group failures and retries, an empty result, and the loss of the session
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/SearchModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::SearchModel;

namespace {
struct Fixture {
    Fixture()
    {
        server.itemsByType.insert(QStringLiteral("Movie"), QStringLiteral("items_page"));
        server.itemsByType.insert(QStringLiteral("Series"), QStringLiteral("search_series"));
        server.itemsByType.insert(QStringLiteral("Episode"), QStringLiteral("episodes_page"));
        server.itemsByType.insert(QStringLiteral("BoxSet"), QStringLiteral("boxset_children"));
        ok = server.listen() && login(client, server);
        // The pause is short in tests
        search.setDebounceInterval(30);
    }

    int searchRequests() const { return server.itemsRoutes.size(); }
    int status(int group) const
    {
        return search.data(search.index(group), SearchModel::StatusRole).toInt();
    }
    int count(int group) const
    {
        return search.data(search.index(group), SearchModel::CountRole).toInt();
    }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    SearchModel search{&client};
    bool ok = false;
};
} // namespace

class TestSearchModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void normalisationTrimsAndDropsControlCharacters()
    {
        QCOMPARE(SearchModel::normalized(QStringLiteral("  abyss \t station\n")),
                 QStringLiteral("abyss station"));
        QCOMPARE(SearchModel::normalized(QStringLiteral("a\x01\x02"
                                                        "b")),
                 QStringLiteral("a b"));
        QCOMPARE(SearchModel::normalized(QString(500, QLatin1Char('x'))).size(),
                 int(SearchModel::MaxTermLength));
        QVERIFY(SearchModel::normalized(QStringLiteral("   ")).isEmpty());
    }

    void debounceCoalescesTypingIntoOneRound()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.search.setDebounceInterval(150);
        for (const QString &typed :
             {QStringLiteral("ab"), QStringLiteral("aby"), QStringLiteral("abys"),
              QStringLiteral("abyss"), QStringLiteral("abyss s")})
            f.search.setTerm(typed);
        QCOMPARE(f.search.state(), SearchModel::Empty);
        QTRY_COMPARE(f.search.state(), SearchModel::Results);
        // Five keystrokes, one round of four requests
        QCOMPARE(f.searchRequests(), 4);
        QVERIFY(QString::fromUtf8(f.server.itemsRoutes.constFirst())
                    .contains(QStringLiteral("abyss%20s")));
    }

    void theFourGroupsAskOnTheirOwnChannelsWithCardImages()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.search.setTerm(QStringLiteral("abyss"));
        QTRY_COMPARE(f.search.state(), SearchModel::Results);
        QSet<QString> types;
        for (const QByteArray &route : f.server.itemsRoutes) {
            const QUrlQuery wire = FakeJellyfin::capturedListingQuery(route);
            types.insert(wire.queryItemValue(QStringLiteral("includeItemTypes")));
            QCOMPARE(wire.queryItemValue(QStringLiteral("searchTerm")), QStringLiteral("abyss"));
            QCOMPARE(wire.queryItemValue(QStringLiteral("recursive")), QStringLiteral("true"));
            QCOMPARE(wire.queryItemValue(QStringLiteral("limit")), QStringLiteral("24"));
            QCOMPARE(wire.queryItemValue(QStringLiteral("fields")),
                     QStringLiteral("PrimaryImageAspectRatio,DateCreated"));
            QVERIFY(wire.queryItemValue(QStringLiteral("enableImageTypes"))
                        .contains(QStringLiteral("Thumb")));
            QCOMPARE(wire.queryItemValue(QStringLiteral("imageTypeLimit")), QStringLiteral("1"));
        }
        QCOMPARE(types, (QSet<QString>{QStringLiteral("Movie"), QStringLiteral("Series"),
                                       QStringLiteral("Episode"), QStringLiteral("BoxSet")}));
        QCOMPARE(f.search.rowCount(), 4);
        for (int group = 0; group < 4; ++group)
            QVERIFY(f.count(group) > 0);
        // Episodes are wide, the rest are posters
        QVERIFY(f.search.data(f.search.index(SearchModel::Episodes), SearchModel::LandscapeRole)
                    .toBool());
        QVERIFY(!f.search.data(f.search.index(SearchModel::Movies), SearchModel::LandscapeRole)
                     .toBool());
    }

    void aShortTermSendsNothing()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.search.setTerm(QStringLiteral("a"));
        QCOMPARE(f.search.state(), SearchModel::TooShort);
        f.search.searchNow();
        QTest::qWait(150);
        QCOMPARE(f.searchRequests(), 0);
        f.search.setTerm(QStringLiteral("   "));
        QCOMPARE(f.search.state(), SearchModel::Empty);
        QCOMPARE(f.searchRequests(), 0);
    }

    void searchNowSkipsTheDebounce()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.search.setDebounceInterval(60000);
        f.search.setTerm(QStringLiteral("abyss"));
        f.search.searchNow();
        QTRY_COMPARE(f.search.state(), SearchModel::Results);
        QCOMPARE(f.searchRequests(), 4);
        // The same words again send nothing
        f.search.setTerm(QStringLiteral("abyss "));
        f.search.searchNow();
        QTest::qWait(100);
        QCOMPARE(f.searchRequests(), 4);
    }

    void aNewTermReplacesTheOldOneAndItsLateAnswers()
    {
        Fixture f;
        QVERIFY(f.ok);
        // The first term's requests are held and never answered in time
        f.server.holdListings = true;
        f.search.setTerm(QStringLiteral("abyss"));
        QTRY_COMPARE(f.searchRequests(), 4);
        QCOMPARE(f.search.state(), SearchModel::Searching);
        // A new term is typed: its requests are answered, the old ones are aborted
        f.server.holdListings = false;
        f.search.setTerm(QStringLiteral("kestrel"));
        QTRY_COMPARE(f.search.state(), SearchModel::Results);
        QCOMPARE(f.searchRequests(), 8);
        const QUrlQuery last = FakeJellyfin::capturedListingQuery(f.server.itemsRoutes.constLast());
        QCOMPARE(last.queryItemValue(QStringLiteral("searchTerm")), QStringLiteral("kestrel"));
        // The held answers of the old term arrive late and change nothing
        const int movies = f.count(SearchModel::Movies);
        f.server.releaseHeld();
        QTest::qWait(250);
        QCOMPARE(f.count(SearchModel::Movies), movies);
        QCOMPARE(f.search.state(), SearchModel::Results);
    }

    void aFailedGroupIsLocalAndRetryable()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.itemsByType.remove(QStringLiteral("Series"));
        f.server.listingRawBody.insert(QStringLiteral("Items"), QByteArrayLiteral("[1,2]"));
        // Series (no fixture) falls to the raw body and fails; the others are answered
        f.search.setTerm(QStringLiteral("abyss"));
        QTRY_COMPARE(f.status(SearchModel::Series), int(SearchModel::Failed));
        QTRY_COMPARE(f.search.state(), SearchModel::Results);
        QCOMPARE(f.status(SearchModel::Movies), int(SearchModel::Ready));
        QVERIFY(
            f.search.data(f.search.index(SearchModel::Series), SearchModel::ShownRole).toBool());
        f.server.itemsByType.insert(QStringLiteral("Series"), QStringLiteral("search_series"));
        f.server.listingRawBody.clear();
        f.search.retry(SearchModel::Series);
        QCOMPARE(f.status(SearchModel::Series), int(SearchModel::Loading));
        QTRY_COMPARE(f.status(SearchModel::Series), int(SearchModel::Ready));
        QCOMPARE(f.count(SearchModel::Series), 2);
        // A group that did not fail is left alone
        const int requests = f.searchRequests();
        f.search.retry(SearchModel::Movies);
        QCOMPARE(f.searchRequests(), requests);
    }

    void everyGroupFailingIsOneState()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        f.search.setTerm(QStringLiteral("abyss"));
        QTRY_COMPARE(f.search.state(), SearchModel::AllFailed);
    }

    void searchNowAsksAgainWhenEveryGroupFailed()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        f.search.setTerm(QStringLiteral("abyss"));
        QTRY_COMPARE(f.search.state(), SearchModel::AllFailed);
        // The way out of the failure: the same words, asked again
        f.server.listingStatus.clear();
        f.search.searchNow();
        QTRY_COMPARE(f.search.state(), SearchModel::Results);
        QCOMPARE(f.searchRequests(), 8);
    }

    void aFailedGroupNextToEmptyOnesIsNotNoResults()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QByteArray none =
            QByteArrayLiteral("{\"Items\":[],\"StartIndex\":0,\"TotalRecordCount\":0}");
        f.server.itemsByType.clear();
        f.server.listingRawBody.insert(QStringLiteral("Items"), none);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        f.search.setTerm(QStringLiteral("zzzz"));
        QTRY_COMPARE(f.search.state(), SearchModel::AllFailed);
        // One group fails, the rest answer empty: its box and Retry must stay reachable
        f.server.listingStatus.clear();
        f.search.retry(SearchModel::Movies);
        QTRY_COMPARE(f.status(SearchModel::Movies), int(SearchModel::Ready));
        QCOMPARE(f.status(SearchModel::Series), int(SearchModel::Failed));
        QCOMPARE(f.search.state(), SearchModel::Results);
        QVERIFY(
            f.search.data(f.search.index(SearchModel::Series), SearchModel::ShownRole).toBool());
    }

    void aFailureDropsTheCardsOfTheOlderTerm()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.search.setTerm(QStringLiteral("abyss"));
        QTRY_COMPARE(f.search.state(), SearchModel::Results);
        QVERIFY(f.count(SearchModel::Movies) > 0);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        f.search.setTerm(QStringLiteral("kestrel"));
        QTRY_COMPARE(f.search.state(), SearchModel::AllFailed);
        for (int group = 0; group < 4; ++group)
            QCOMPARE(f.count(group), 0);
    }

    void aCutTermNeverEndsInALoneSurrogate()
    {
        // 127 letters and an emoji (a surrogate pair): the cut after unit 128 would split it
        const QString term = QString(127, QLatin1Char('a')) + QString::fromUtf8("\xF0\x9F\x98\x80");
        const QString clean = SearchModel::normalized(term);
        QVERIFY(clean.size() <= int(SearchModel::MaxTermLength));
        QVERIFY(!clean.back().isHighSurrogate());
        // Joiners and the bidi marks: the joiner stays, the direction controls go
        QCOMPARE(SearchModel::normalized(QStringLiteral("a\u200Db")), QStringLiteral("a\u200Db"));
        QCOMPARE(SearchModel::normalized(QStringLiteral("a\u202Eb")), QStringLiteral("a b"));
    }

    void noResultsIsItsOwnStateAndEmptyGroupsAreHidden()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QByteArray none =
            QByteArrayLiteral("{\"Items\":[],\"StartIndex\":0,\"TotalRecordCount\":0}");
        f.server.itemsByType.clear();
        f.server.listingRawBody.insert(QStringLiteral("Items"), none);
        f.search.setTerm(QStringLiteral("zzzz"));
        QTRY_COMPARE(f.search.state(), SearchModel::NoResults);
        for (int group = 0; group < 4; ++group)
            QVERIFY(!f.search.data(f.search.index(group), SearchModel::ShownRole).toBool());
    }

    void clearDropsTheTermTheResultsAndTheRequests()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.holdListings = true;
        f.search.setTerm(QStringLiteral("abyss"));
        QTRY_COMPARE(f.searchRequests(), 4);
        QSignalSpy failures(&f.client, &Kinefin::JellyfinClient::listingFailed);
        f.search.clear();
        QCOMPARE(f.search.term(), QString());
        QCOMPARE(f.search.state(), SearchModel::Empty);
        // The held requests are answered anyway: an aborted one stays silent
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTest::qWait(250);
        QCOMPARE(failures.size(), 0);
        for (int group = 0; group < 4; ++group)
            QCOMPARE(f.status(group), int(SearchModel::Idle));
    }

    void aLostSessionEmptiesTheSearch()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.search.setTerm(QStringLiteral("abyss"));
        QTRY_COMPARE(f.search.state(), SearchModel::Results);
        f.client.disconnectFromServer();
        QCOMPARE(f.search.state(), SearchModel::Empty);
        QCOMPARE(f.search.term(), QString());
        for (int group = 0; group < 4; ++group)
            QCOMPARE(f.count(group), 0);
    }

    void theLibraryGridAndTheDetailsAreNotConsumed()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy items(&f.client, &Kinefin::JellyfinClient::itemsReady);
        // A reply on the grid's channel and on a details channel is none of the search's business
        QVERIFY(f.client.fetchItems(Kinefin::ItemQuery(), 1));
        QVERIFY(f.client.fetchItems(Kinefin::ItemQuery(), 65536));
        QTRY_COMPARE(items.size(), 2);
        QCOMPARE(f.search.state(), SearchModel::Empty);
        for (int group = 0; group < 4; ++group)
            QCOMPARE(f.status(group), int(SearchModel::Idle));
    }
};

QTEST_MAIN(TestSearchModel)
#include "test_search_model.moc"
