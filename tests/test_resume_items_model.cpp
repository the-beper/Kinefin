// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The Continue Watching list against the fake server: pages by start index, replies of other channels, a failed page and its retry, the loss of the session and the release of the channel
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/ResumeItemsModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::ResumeItemsModel;

namespace {
constexpr int kForeignChannel = 9;

struct Fixture {
    Fixture()
    {
        server.generatedResumeTotal = 250;
        ok = server.listen() && login(client, server);
    }

    // True once the row is a real item, reading it is what asks for its page
    bool loadedRow(int row)
    {
        return model.data(model.index(row, 0), ResumeItemsModel::LoadedRole).toBool();
    }

    // The start indexes of the Resume requests in arrival order
    QList<int> starts() const
    {
        QList<int> result;
        for (const QByteArray &route : server.listingRoutes.value(QStringLiteral("Resume")))
            result.append(FakeJellyfin::capturedListingQuery(route)
                              .queryItemValue(QStringLiteral("startIndex"))
                              .toInt());
        return result;
    }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    ResumeItemsModel model{&client};
    bool ok = false;
};
} // namespace

class TestResumeItemsModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void pagesByStartIndex()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open());
        QCOMPARE(f.model.status(), ResumeItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), ResumeItemsModel::Ready);
        QCOMPARE(f.model.totalCount(), 250);
        QCOMPARE(f.model.rowCount(), 250);
        QVERIFY(f.loadedRow(0));
        QVERIFY(!f.loadedRow(120));
        QTRY_VERIFY(f.loadedRow(120));
        QVERIFY(!f.loadedRow(249));
        QTRY_VERIFY(f.loadedRow(249));
        QCOMPARE(f.starts(), (QList<int>{0, 100, 200}));
        for (const QByteArray &route : f.server.listingRoutes.value(QStringLiteral("Resume")))
            QVERIFY2(route.contains("limit=100"), route.constData());
        QCOMPARE(f.model.data(f.model.index(249), ResumeItemsModel::NameRole).toString(),
                 QStringLiteral("Item 249"));
    }

    void exposesTheCardRoles()
    {
        Fixture f;
        QVERIFY(f.ok);
        const QHash<int, QByteArray> names = f.model.roleNames();
        for (const char *name : {"seriesId", "rating", "remainingMinutes", "isNew", "seasonCount",
                                 "episodeCount", "seriesProgress", "itemId", "loaded"})
            QVERIFY2(names.values().contains(name), name);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), ResumeItemsModel::Ready);
        QCOMPARE(f.model.data(f.model.index(0), Kinefin::RemainingMinutesRole).toInt(), -1);
        QVERIFY(!f.model.data(f.model.index(0), Kinefin::IsNewRole).toBool());
    }

    void openFailsWhenSignedOut()
    {
        Kinefin::JellyfinClient client;
        ResumeItemsModel model{&client};
        QVERIFY(!model.open());
        QCOMPARE(model.status(), ResumeItemsModel::Failed);
        QCOMPARE(model.rowCount(), 0);
    }

    void ignoresForeignChannel()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.holdListings = true;
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Resume")).size(), 1);
        // The page is in flight: what other consumers receive, and a failure of another family, must not reach the model
        const QJsonArray foreign{
            QJsonObject{{QStringLiteral("Id"), QStringLiteral("0000000000000000000000000000abcd")},
                        {QStringLiteral("Name"), QStringLiteral("Foreign")},
                        {QStringLiteral("Type"), QStringLiteral("Movie")}}};
        Q_EMIT f.client.resumeItemsReady(foreign, 1, kForeignChannel);
        Q_EMIT f.client.listingFailed(QStringLiteral("Resume"), 500, QStringLiteral("x"),
                                      kForeignChannel);
        Q_EMIT f.client.listingFailed(QStringLiteral("Items"), 500, QStringLiteral("x"),
                                      ResumeItemsModel::Channel);
        QCOMPARE(f.model.status(), ResumeItemsModel::Loading);
        QCOMPARE(f.model.rowCount(), 0);
        // Its own channel and family are taken
        Q_EMIT f.client.resumeItemsReady(foreign, 1, ResumeItemsModel::Channel);
        QCOMPARE(f.model.status(), ResumeItemsModel::Ready);
        QCOMPARE(f.model.rowCount(), 1);
        QCOMPARE(f.model.data(f.model.index(0), ResumeItemsModel::NameRole).toString(),
                 QStringLiteral("Foreign"));
    }

    void failureThenRetry()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.listingStatus.insert(QStringLiteral("Resume"), 500);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), ResumeItemsModel::Failed);
        QCOMPARE(f.model.rowCount(), 0);
        f.server.listingStatus.clear();
        f.model.retry();
        QCOMPARE(f.model.status(), ResumeItemsModel::Loading);
        QTRY_COMPARE(f.model.status(), ResumeItemsModel::Ready);
        QCOMPARE(f.model.rowCount(), 250);
        // A later page that fails is retried on its own
        f.server.listingStatus.insert(QStringLiteral("Resume"), 500);
        f.loadedRow(120);
        QTRY_VERIFY(f.model.pageError());
        f.server.listingStatus.clear();
        f.model.retry();
        QTRY_VERIFY(f.loadedRow(120));
        QVERIFY(!f.model.pageError());
    }

    void clearsOnCredentialsCleared()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), ResumeItemsModel::Ready);
        f.client.disconnectFromServer();
        QCOMPARE(f.model.rowCount(), 0);
        QCOMPARE(f.model.status(), ResumeItemsModel::Idle);
    }

    void closeReleasesChannel()
    {
        Fixture f;
        QVERIFY(f.ok);
        QSignalSpy answers(&f.client, &Kinefin::JellyfinClient::resumeItemsReady);
        QSignalSpy failures(&f.client, &Kinefin::JellyfinClient::listingFailed);
        f.server.holdListings = true;
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Resume")).size(), 1);
        f.model.close();
        QCOMPARE(f.model.status(), ResumeItemsModel::Idle);
        QCOMPARE(f.model.rowCount(), 0);
        f.server.holdListings = false;
        f.server.releaseHeld();
        QTest::qWait(250);
        QCOMPARE(answers.size(), 0);
        QCOMPARE(failures.size(), 0);
        QCOMPARE(f.model.rowCount(), 0);
        QCOMPARE(f.model.status(), ResumeItemsModel::Idle);
        // A reopen on the same channel answers once
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), ResumeItemsModel::Ready);
        QCOMPARE(answers.size(), 1);
        QCOMPARE(f.model.rowCount(), 250);
    }
};

QTEST_MAIN(TestResumeItemsModel)
#include "test_resume_items_model.moc"
