// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// What a person appears in against the fake server: the two requests and their cap, the cards of the series the person only has episodes in, the episodes of one series by season, a failed request and its retry, the loss of the session and the release of the channels
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/PersonItemsModel.h"
#include "models/SeasonEpisodesModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::PersonItemsModel;
using Kinefin::SeasonEpisodesModel;

namespace {
const QString kPerson = QStringLiteral("a1b2c3d4e5f60718293a4b5c6d7e8f01");
const QString kAlpha = QStringLiteral("a0000000000000000000000000000001");
const QString kBeta = QStringLiteral("b0000000000000000000000000000002");
const QString kZeta = QStringLiteral("f0000000000000000000000000000003");
const QString kGamma = QStringLiteral("c0000000000000000000000000000004");
const QString kDelta = QStringLiteral("d0000000000000000000000000000005");

QString idOf(char prefix, int number)
{
    return QStringLiteral("%1%2").arg(QLatin1Char(prefix)).arg(number, 31, 16, QLatin1Char('0'));
}

QJsonObject title(const QString &id, const QString &name, const QString &type)
{
    return {{QStringLiteral("Id"), id},
            {QStringLiteral("Name"), name},
            {QStringLiteral("Type"), type},
            {QStringLiteral("ProductionYear"), 2001}};
}

QJsonObject episode(const QString &id, const QString &name, const QString &seriesId,
                    const QString &seriesName, int season, int number)
{
    return {{QStringLiteral("Id"), id},
            {QStringLiteral("Name"), name},
            {QStringLiteral("Type"), QStringLiteral("Episode")},
            {QStringLiteral("SeriesId"), seriesId},
            {QStringLiteral("SeriesName"), seriesName},
            {QStringLiteral("SeriesPrimaryImageTag"), QStringLiteral("seriesprimary")},
            {QStringLiteral("SeriesThumbImageTag"), QStringLiteral("seriesthumb")},
            {QStringLiteral("ParentIndexNumber"), season},
            {QStringLiteral("IndexNumber"), number}};
}

struct Fixture {
    Fixture()
    {
        // The server's SortName order for the titles; the episodes come in any order
        server.personItems.insert(
            QStringLiteral("Movie,Series"),
            QJsonArray{title(kAlpha, QStringLiteral("Alpha Movie"), QStringLiteral("Movie")),
                       title(kBeta, QStringLiteral("Beta Show"), QStringLiteral("Series")),
                       title(kZeta, QStringLiteral("Zeta Film"), QStringLiteral("Movie"))});
        server.personItems.insert(QStringLiteral("Episode"),
                                  QJsonArray{episode(idOf('e', 1), QStringLiteral("Xmas Story"),
                                                     kGamma, QStringLiteral("Gamma Show"), 2, 8),
                                             episode(idOf('e', 2), QStringLiteral("Pilot"), kGamma,
                                                     QStringLiteral("Gamma Show"), 1, 1),
                                             episode(idOf('e', 3), QStringLiteral("Beta guest"),
                                                     kBeta, QStringLiteral("Beta Show"), 1, 4),
                                             episode(idOf('e', 4), QStringLiteral("Only one"),
                                                     kDelta, QStringLiteral("Delta Show"), 3, 2),
                                             episode(idOf('e', 5), QStringLiteral("Second"), kGamma,
                                                     QStringLiteral("Gamma Show"), 1, 2)});
        ok = server.listen() && login(client, server);
    }

    QString nameAt(int row) const
    {
        return model.data(model.index(row, 0), PersonItemsModel::NameRole).toString();
    }
    int guestAt(int row) const
    {
        return model.data(model.index(row, 0), PersonItemsModel::GuestEpisodesRole).toInt();
    }

    QStringList names() const
    {
        QStringList result;
        for (int row = 0; row < model.rowCount(); ++row)
            result.append(nameAt(row));
        return result;
    }

    // The Items requests of the person in arrival order, by the types they ask for
    QList<QUrlQuery> requests(const QString &types) const
    {
        QList<QUrlQuery> result;
        for (const QByteArray &route : server.itemsRoutes) {
            const QUrlQuery query = FakeJellyfin::capturedListingQuery(route);
            if (query.queryItemValue(QStringLiteral("includeItemTypes")) == types)
                result.append(query);
        }
        return result;
    }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    PersonItemsModel model{&client};
    bool ok = false;
};
} // namespace

class TestPersonItemsModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void showsTitlesAndOneCardPerGuestSeriesByName()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open(kPerson));
        QCOMPARE(f.model.status(), PersonItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        // The series in the main cast stays one card, the others sort among the titles by name
        QCOMPARE(f.names(), (QStringList{QStringLiteral("Alpha Movie"), QStringLiteral("Beta Show"),
                                         QStringLiteral("Delta Show"), QStringLiteral("Gamma Show"),
                                         QStringLiteral("Zeta Film")}));
        QCOMPARE(f.model.totalCount(), 5);
        QVERIFY(!f.model.pageError());
        QCOMPARE(f.guestAt(0), 0);
        QCOMPARE(f.guestAt(1), 0);
        QCOMPARE(f.guestAt(2), 1);
        QCOMPARE(f.guestAt(3), 3);
        QCOMPARE(f.guestAt(4), 0);
        const auto roleAt = [&](int row, int role) {
            return f.model.data(f.model.index(row, 0), role);
        };
        QCOMPARE(roleAt(3, PersonItemsModel::TypeRole).toString(), QStringLiteral("Series"));
        // A guest-series card opens nothing of an episode: its id is the series'
        QCOMPARE(roleAt(3, PersonItemsModel::ItemIdRole).toString(), kGamma);
        QVERIFY(roleAt(3, PersonItemsModel::LoadedRole).toBool());
        // The series' own image, built by the client
        const QUrl image = roleAt(3, PersonItemsModel::ImageUrlRole).toUrl();
        QVERIFY2(image.path().contains(kGamma) && image.path().endsWith(QLatin1String("/Thumb")),
                 qPrintable(image.toString()));
        QVERIFY(image.toString().startsWith(f.server.url().toString()));
    }

    void asksForTheTitlesAndTheEpisodesAtOnceFromTheFirstPage()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        const QList<QUrlQuery> titles = f.requests(QStringLiteral("Movie,Series"));
        const QList<QUrlQuery> episodes = f.requests(QStringLiteral("Episode"));
        QCOMPARE(titles.size(), 1);
        QCOMPARE(episodes.size(), 1);
        for (const QUrlQuery &query : {titles.first(), episodes.first()}) {
            QCOMPARE(query.queryItemValue(QStringLiteral("personIds")), kPerson);
            QCOMPARE(query.queryItemValue(QStringLiteral("recursive")), QStringLiteral("true"));
            QCOMPARE(query.queryItemValue(QStringLiteral("limit")),
                     QString::number(PersonItemsModel::PageSize));
            QCOMPARE(query.queryItemValue(QStringLiteral("startIndex")).toInt(), 0);
        }
        QCOMPARE(titles.first().queryItemValue(QStringLiteral("sortBy")),
                 QStringLiteral("SortName"));
        QVERIFY(episodes.first()
                    .queryItemValue(QStringLiteral("fields"))
                    .contains(QStringLiteral("Overview")));
    }

    void capsBothRequests()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray movies;
        for (int number = 0; number < PersonItemsModel::Limit + 1; ++number)
            movies.append(title(idOf('a', number + 16),
                                QStringLiteral("Movie %1").arg(number, 4, 10, QLatin1Char('0')),
                                QStringLiteral("Movie")));
        QJsonArray episodes;
        for (int number = 0; number < PersonItemsModel::Limit + 200; ++number)
            episodes.append(episode(idOf('e', number + 16), QStringLiteral("Ep %1").arg(number),
                                    kGamma, QStringLiteral("Gamma Show"), 1 + number / 100,
                                    number % 100));
        f.server.personItems.insert(QStringLiteral("Movie,Series"), movies);
        f.server.personItems.insert(QStringLiteral("Episode"), episodes);
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        // The first thousand movies, and one card for the first thousand episodes
        QCOMPARE(f.model.rowCount(), PersonItemsModel::Limit + 1);
        int guests = 0;
        int guestRow = -1;
        for (int row = 0; row < f.model.rowCount(); ++row) {
            if (f.guestAt(row) > 0) {
                ++guests;
                guestRow = row;
            }
        }
        QCOMPARE(guests, 1);
        QCOMPARE(f.guestAt(guestRow), PersonItemsModel::Limit);
        QVERIFY(f.model.showSeries(kGamma));
        QCOMPARE(f.model.seriesEpisodes()->rowCount(), PersonItemsModel::Limit);
        // Pages one after the other up to the cap, never past it
        for (const QString &types : {QStringLiteral("Movie,Series"), QStringLiteral("Episode")}) {
            const QList<QUrlQuery> pages = f.requests(types);
            QCOMPARE(pages.size(), PersonItemsModel::Limit / PersonItemsModel::PageSize);
            for (qsizetype page = 0; page < pages.size(); ++page) {
                QCOMPARE(pages.at(page).queryItemValue(QStringLiteral("startIndex")).toInt(),
                         int(page) * PersonItemsModel::PageSize);
                QCOMPARE(pages.at(page).queryItemValue(QStringLiteral("limit")).toInt(),
                         PersonItemsModel::PageSize);
            }
        }
    }

    void aFailedPageKeepsTheEarlierOnesAndRetryGoesOnFromIt()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray episodes;
        for (int number = 0; number < PersonItemsModel::PageSize + 50; ++number)
            episodes.append(episode(idOf('e', number + 16), QStringLiteral("Ep %1").arg(number),
                                    kGamma, QStringLiteral("Gamma Show"), 1, number + 1));
        f.server.personItems.insert(QStringLiteral("Episode"), episodes);
        const QByteArray secondPage =
            "startIndex=" + QByteArray::number(PersonItemsModel::PageSize);
        f.server.itemsResponder = [secondPage](const QByteArray &route) {
            return route.contains("includeItemTypes=Episode") && route.contains(secondPage)
                       ? std::pair<int, QByteArray>{500, {}}
                       : std::pair<int, QByteArray>{};
        };
        const auto gammaEpisodes = [&f] {
            for (int row = 0; row < f.model.rowCount(); ++row) {
                if (f.nameAt(row) == QLatin1String("Gamma Show"))
                    return f.guestAt(row);
            }
            return -1;
        };
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QVERIFY(f.model.pageError());
        QCOMPARE(gammaEpisodes(), PersonItemsModel::PageSize);
        f.server.itemsResponder = nullptr;
        f.model.retry();
        QTRY_COMPARE(gammaEpisodes(), PersonItemsModel::PageSize + 50);
        QVERIFY(!f.model.pageError());
        // The retry asks for the failed page, not for the list again
        const QList<QUrlQuery> pages = f.requests(QStringLiteral("Episode"));
        QCOMPARE(pages.size(), 3);
        QCOMPARE(pages.last().queryItemValue(QStringLiteral("startIndex")).toInt(),
                 PersonItemsModel::PageSize);
        QCOMPARE(f.requests(QStringLiteral("Movie,Series")).size(), 1);
    }

    void showsTheEpisodesOfOneSeriesBySeason()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QVERIFY(f.model.showSeries(kGamma));
        Kinefin::PersonEpisodesModel *rows = f.model.seriesEpisodes();
        QCOMPARE(rows->rowCount(), 3);
        const auto at = [&](int row, int role) { return rows->data(rows->index(row, 0), role); };
        // Season 1 first, in episode order, then season 2
        QCOMPARE(at(0, SeasonEpisodesModel::NameRole).toString(), QStringLiteral("Pilot"));
        QCOMPARE(at(1, SeasonEpisodesModel::NameRole).toString(), QStringLiteral("Second"));
        QCOMPARE(at(2, SeasonEpisodesModel::NameRole).toString(), QStringLiteral("Xmas Story"));
        QCOMPARE(at(0, SeasonEpisodesModel::SeasonNumberRole).toInt(), 1);
        QCOMPARE(at(2, SeasonEpisodesModel::SeasonNumberRole).toInt(), 2);
        QCOMPARE(at(2, SeasonEpisodesModel::EpisodeNumberRole).toInt(), 8);
        QVERIFY(at(0, SeasonEpisodesModel::LoadedRole).toBool());
        // Only the series' own episodes: another series, or one the person has none in
        QVERIFY(f.model.showSeries(kDelta));
        QCOMPARE(rows->rowCount(), 1);
        QVERIFY(!f.model.showSeries(kZeta));
        QCOMPARE(rows->rowCount(), 1);
    }

    void aWatchedStateEditReachesTheRows()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QVERIFY(f.model.showSeries(kGamma));
        Kinefin::PersonEpisodesModel *rows = f.model.seriesEpisodes();
        QVERIFY(!rows->data(rows->index(0, 0), SeasonEpisodesModel::PlayedRole).toBool());
        Q_EMIT f.client.itemPlayedUpdated(idOf('e', 2), true);
        QVERIFY(rows->data(rows->index(0, 0), SeasonEpisodesModel::PlayedRole).toBool());
        // A later look at the series still has it
        QVERIFY(f.model.showSeries(kGamma));
        QVERIFY(rows->data(rows->index(0, 0), SeasonEpisodesModel::PlayedRole).toBool());
    }

    void aFailedEpisodesRequestLeavesTheTitlesAndRetries()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.itemsResponder = [](const QByteArray &route) -> std::pair<int, QByteArray> {
            return route.contains("includeItemTypes=Episode") ? std::pair<int, QByteArray>{500, {}}
                                                              : std::pair<int, QByteArray>{};
        };
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QVERIFY(f.model.pageError());
        QCOMPARE(f.names(), (QStringList{QStringLiteral("Alpha Movie"), QStringLiteral("Beta Show"),
                                         QStringLiteral("Zeta Film")}));
        f.server.itemsResponder = nullptr;
        f.model.retry();
        QTRY_COMPARE(f.model.rowCount(), 5);
        QVERIFY(!f.model.pageError());
        QCOMPARE(f.model.status(), PersonItemsModel::Ready);
        // Only the failed request is asked again
        QCOMPARE(f.requests(QStringLiteral("Movie,Series")).size(), 1);
        QCOMPARE(f.requests(QStringLiteral("Episode")).size(), 2);
    }

    void aFailedRequestWithNothingElseToShowFailsTheModel()
    {
        Fixture f;
        QVERIFY(f.ok);
        // A person with guest roles only: no titles, and the episodes fail
        f.server.personItems.insert(QStringLiteral("Movie,Series"), QJsonArray{});
        f.server.itemsResponder = [](const QByteArray &route) -> std::pair<int, QByteArray> {
            return route.contains("includeItemTypes=Episode") ? std::pair<int, QByteArray>{500, {}}
                                                              : std::pair<int, QByteArray>{};
        };
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Failed);
        QVERIFY(!f.model.pageError());
        QCOMPARE(f.model.rowCount(), 0);
        f.server.itemsResponder = nullptr;
        f.model.retry();
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QCOMPARE(f.names(), (QStringList{QStringLiteral("Beta Show"), QStringLiteral("Delta Show"),
                                         QStringLiteral("Gamma Show")}));
        QCOMPARE(f.requests(QStringLiteral("Movie,Series")).size(), 1);
    }

    void bothRequestsFailingFailTheModelUntilRetry()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Items"), 500);
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Failed);
        QCOMPARE(f.model.rowCount(), 0);
        QVERIFY(!f.model.pageError());
        f.server.listingStatus.remove(QStringLiteral("Items"));
        f.model.retry();
        QCOMPARE(f.model.status(), PersonItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QCOMPARE(f.model.rowCount(), 5);
    }

    void aPersonWithNothingIsReadyAndEmpty()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.personItems.insert(QStringLiteral("Movie,Series"), QJsonArray{});
        f.server.personItems.insert(QStringLiteral("Episode"), QJsonArray{});
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QCOMPARE(f.model.totalCount(), 0);
    }

    void ignoresItemsOfAnotherKindAndEpisodesWithoutASeries()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonObject orphan = episode(idOf('e', 99), QStringLiteral("Orphan"), kGamma,
                                     QStringLiteral("Gamma Show"), 1, 1);
        orphan.remove(QStringLiteral("SeriesId"));
        f.server.personItems.insert(
            QStringLiteral("Episode"),
            QJsonArray{orphan,
                       title(idOf('f', 98), QStringLiteral("A Movie"), QStringLiteral("Movie"))});
        f.server.personItems.insert(QStringLiteral("Movie,Series"),
                                    QJsonArray{title(idOf('f', 97), QStringLiteral("An Episode"),
                                                     QStringLiteral("Episode"))});
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QCOMPARE(f.model.totalCount(), 0);
    }

    void refusesAnIdTheServerCouldNotHaveSent()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(!f.model.open(QString()));
        QVERIFY(!f.model.open(QStringLiteral("not an id!")));
        QCOMPARE(f.model.status(), PersonItemsModel::Idle);
        QCOMPARE(f.model.rowCount(), 0);
        QVERIFY(f.server.itemsRoutes.isEmpty());
    }

    void clearsOnCredentialsCleared()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QVERIFY(f.model.showSeries(kGamma));
        f.client.disconnectFromServer();
        QCOMPARE(f.model.rowCount(), 0);
        QCOMPARE(f.model.seriesEpisodes()->rowCount(), 0);
        QCOMPARE(f.model.status(), PersonItemsModel::Idle);
    }

    void openingAnotherPersonStartsAgain()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        f.server.personItems.insert(QStringLiteral("Movie,Series"), QJsonArray{});
        f.server.personItems.insert(
            QStringLiteral("Episode"),
            QJsonArray{episode(idOf('e', 7), QStringLiteral("Cameo"), kDelta,
                               QStringLiteral("Delta Show"), 1, 1)});
        QVERIFY(f.model.open(QStringLiteral("b1b2c3d4e5f60718293a4b5c6d7e8f02")));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QCOMPARE(f.names(), (QStringList{QStringLiteral("Delta Show")}));
        QVERIFY(!f.model.showSeries(kGamma));
    }

    void closeReleasesTheChannels()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy answers(&f.client, &Kinefin::JellyfinClient::itemsReady);
        QSignalSpy failures(&f.client, &Kinefin::JellyfinClient::listingFailed);
        f.server.holdListings = true;
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.server.itemsRoutes.size(), 2);
        f.model.close();
        QCOMPARE(f.model.status(), PersonItemsModel::Idle);
        QCOMPARE(f.model.rowCount(), 0);
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTest::qWait(250);
        QCOMPARE(answers.size(), 0);
        QCOMPARE(failures.size(), 0);
        QCOMPARE(f.model.rowCount(), 0);
        QCOMPARE(f.model.status(), PersonItemsModel::Idle);
        // A reopen on the same channels answers once each
        QVERIFY(f.model.open(kPerson));
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QCOMPARE(answers.size(), 2);
        QCOMPARE(f.model.rowCount(), 5);
    }

    void repliesOfOtherChannelsAreIgnored()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open(kPerson));
        // No reply can arrive before the event loop runs, so the stray one comes first
        Q_EMIT f.client.itemsReady(
            QJsonArray{title(kAlpha, QStringLiteral("Stray"), QStringLiteral("Movie"))}, 1,
            PersonItemsModel::Channel + 1000);
        QCOMPARE(f.model.status(), PersonItemsModel::Loading);
        QCOMPARE(f.model.rowCount(), 0);
        QTRY_COMPARE(f.model.status(), PersonItemsModel::Ready);
        QCOMPARE(f.model.rowCount(), 5);
    }
};

QTEST_MAIN(TestPersonItemsModel)
#include "test_person_items_model.moc"
