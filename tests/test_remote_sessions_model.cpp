// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The Remote Sessions dialog's bound, read-only session snapshot and authenticated command path
#include "FakeJellyfin.h"

#include "models/RemoteSessionsModel.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

using Kinefin::RemoteSessionsModel;

namespace {
const QString kSessionId = QStringLiteral("11111111222233334444555566667777");
const QString kIdleSessionId = QStringLiteral("88888888999900001111222233334444");

struct Fixture {
    Fixture() { ok = server.listen() && login(client, server); }

    QVariant row(int index, int role) const { return model.data(model.index(index, 0), role); }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    RemoteSessionsModel model{&client};
    bool ok = false;
};
} // namespace

class TestRemoteSessionsModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void fetchesControllableSessionsAndExposesSafeRoles()
    {
        Fixture f;
        QVERIFY(f.ok);
        QAbstractItemModelTester tester(&f.model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QCOMPARE(f.model.count(), 2);
        QCOMPARE(f.row(0, RemoteSessionsModel::SessionIdRole).toString(), kSessionId);
        QCOMPARE(f.row(0, RemoteSessionsModel::DeviceNameRole).toString(),
                 QStringLiteral("Kinefin test"));
        QCOMPARE(f.row(0, RemoteSessionsModel::NowPlayingRole).toString(),
                 QStringLiteral("Arcturus (2012)"));
        QVERIFY(f.row(0, RemoteSessionsModel::CanControlRole).toBool());
        QVERIFY(f.row(0, RemoteSessionsModel::PlayingRole).toBool());
        QVERIFY(!f.row(0, RemoteSessionsModel::PausedRole).toBool());
        QCOMPARE(f.row(1, RemoteSessionsModel::SessionIdRole).toString(), kIdleSessionId);
        QVERIFY(!f.row(1, RemoteSessionsModel::CanControlRole).toBool());
        QVERIFY(!f.row(1, RemoteSessionsModel::PlayingRole).toBool());
        QVERIFY(f.server.lastListingRoute.value(QStringLiteral("Sessions"))
                    .contains("controllableByUserId="));
        QCOMPARE(f.model.rowCount(f.model.index(0, 0)), 0);
    }

    void stripsUnsafeNamesAndCapsTheList()
    {
        Fixture f;
        QVERIFY(f.ok);
        QJsonArray sessions;
        sessions.append(QJsonObject{{QStringLiteral("Id"), QStringLiteral("bad/id")}});
        sessions.append(
            QJsonObject{{QStringLiteral("Id"), kSessionId},
                        {QStringLiteral("DeviceName"), QStringLiteral("<b>bad</b>\n device")},
                        {QStringLiteral("Client"), QStringLiteral("client\u202e reversed")}});
        sessions.append(QJsonObject{{QStringLiteral("Id"), kSessionId.toUpper()}});
        for (int i = 0; i < RemoteSessionsModel::MaxSessions + 2; ++i) {
            sessions.append(QJsonObject{{QStringLiteral("Id"), QStringLiteral("device-%1").arg(i)},
                                        {QStringLiteral("DeviceName"), QStringLiteral("device")}});
        }
        f.server.listingRawBody.insert(QStringLiteral("Sessions"),
                                       QJsonDocument(sessions).toJson(QJsonDocument::Compact));
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QCOMPARE(f.model.count(), RemoteSessionsModel::MaxSessions);
        QVERIFY(f.model.truncated());
        QCOMPARE(f.row(0, RemoteSessionsModel::DeviceNameRole).toString(),
                 QStringLiteral("<b>bad</b> device"));
        QCOMPARE(f.row(0, RemoteSessionsModel::ClientNameRole).toString(),
                 QStringLiteral("client reversed"));
        QVERIFY(!f.model.control(kSessionId, QStringLiteral("Pause")));
    }

    void permitsOnlyCommandsForPlayingControllableRows()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QVERIFY(!f.model.control(kIdleSessionId, QStringLiteral("Pause")));
        QVERIFY(!f.model.control(QStringLiteral("wrong-id"), QStringLiteral("Pause")));
        QVERIFY(!f.model.control(kSessionId, QStringLiteral("Seek")));
        QVERIFY(!f.model.control(kSessionId, QStringLiteral("Unpause")));
        QVERIFY(!f.client.sendSessionPlaystate(kSessionId, QStringLiteral("NextTrack")));
        QVERIFY(!f.client.sendSessionPlaystate(QStringLiteral("../bad"), QStringLiteral("Pause")));
        QCOMPARE(f.server.remoteCommandRoutes.size(), 0);
    }

    void sendsAuthenticatedCommandsAndRefreshesAfterSuccess()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QVERIFY(f.model.control(kSessionId, QStringLiteral("Pause")));
        QVERIFY(f.model.commandPending());
        QVERIFY(!f.model.control(kSessionId, QStringLiteral("Stop")));
        QTRY_VERIFY(!f.model.commandPending());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QCOMPARE(f.server.remoteCommandRoutes.size(), 1);
        QCOMPARE(
            f.server.remoteCommandRoutes.first(),
            QByteArrayLiteral("/jellyfin/Sessions/11111111222233334444555566667777/Playing/Pause"));
        QVERIFY(f.server.remoteCommandAuthorizations.first().contains("MediaBrowser"));
        QVERIFY(!f.model.commandFailed());
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Sessions")).size(), 2);
    }

    void failedOrRedirectedCommandsStayVisibleToTheUser()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        f.server.remoteCommandStatus = 403;
        QVERIFY(f.model.control(kSessionId, QStringLiteral("Stop")));
        QTRY_VERIFY(!f.model.commandPending());
        QVERIFY(f.model.commandFailed());
        QCOMPARE(f.server.listingRoutes.value(QStringLiteral("Sessions")).size(), 1);
        f.server.redirectRemoteCommands = true;
        QVERIFY(f.model.control(kSessionId, QStringLiteral("Stop")));
        QTRY_VERIFY(!f.model.commandPending());
        QVERIFY(f.model.commandFailed());
        QCOMPARE(f.server.remoteCommandRoutes.size(), 2);
    }

    void closingDuringACommandPreventsDuplicateRequestsOnReopen()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        f.server.holdRemoteCommands = true;
        QVERIFY(f.model.control(kSessionId, QStringLiteral("Pause")));
        QTRY_COMPARE(f.server.remoteCommandRoutes.size(), 1);
        f.model.close();
        QVERIFY(f.model.commandPending());
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QVERIFY(!f.model.control(kSessionId, QStringLiteral("Pause")));
        QCOMPARE(f.server.remoteCommandRoutes.size(), 1);
        f.server.holdRemoteCommands = false;
        f.server.releaseHeld();
        QTRY_VERIFY(!f.model.commandPending());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QVERIFY(!f.model.commandFailed());
    }

    void aFailedRefreshDropsStaleControls()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        f.server.listingRawBody.insert(QStringLiteral("Sessions"), QByteArrayLiteral("{}"));
        f.model.reload();
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Failed);
        QCOMPARE(f.model.count(), 0);
        QVERIFY(!f.model.control(kSessionId, QStringLiteral("Stop")));
        f.server.listingRawBody.remove(QStringLiteral("Sessions"));
        f.model.reload();
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QCOMPARE(f.model.count(), 2);
    }

    void closingThePageIgnoresLateRepliesAndClearsControls()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.holdListings = true;
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.server.listingRoutes.value(QStringLiteral("Sessions")).size(), 1);
        f.model.close();
        f.server.releaseHeld();
        QTest::qWait(80);
        QCOMPARE(f.model.status(), RemoteSessionsModel::Idle);
        QCOMPARE(f.model.count(), 0);
        QVERIFY(!f.model.control(kSessionId, QStringLiteral("Stop")));
        f.server.holdListings = false;
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QCOMPARE(f.model.count(), 2);
    }

    void aReloadKeepsRowsInPlace()
    {
        Fixture f;
        QVERIFY(f.ok);
        QAbstractItemModelTester tester(&f.model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QSignalSpy resets(&f.model, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&f.model, &QAbstractItemModel::dataChanged);
        QSignalSpy inserts(&f.model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removals(&f.model, &QAbstractItemModel::rowsRemoved);

        f.model.reload();
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(f.model.count(), 2);

        // The first session pauses, a new one appears, the idle one goes: the answer lists the new one first
        const QString newId = QStringLiteral("aaaaaaaabbbbccccddddeeeeffff0000");
        const auto playing = [](const QString &id, bool paused) {
            return QJsonObject{
                {QStringLiteral("Id"), id},
                {QStringLiteral("DeviceName"), QStringLiteral("device")},
                {QStringLiteral("SupportsMediaControl"), true},
                {QStringLiteral("NowPlayingItem"),
                 QJsonObject{
                     {QStringLiteral("Name"), QStringLiteral("Title")},
                     {QStringLiteral("Id"), QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f")}}},
                {QStringLiteral("PlayState"), QJsonObject{{QStringLiteral("IsPaused"), paused}}}};
        };
        f.server.listingRawBody.insert(
            QStringLiteral("Sessions"),
            QJsonDocument(QJsonArray{playing(newId, false), playing(kSessionId, true)})
                .toJson(QJsonDocument::Compact));
        f.model.reload();
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QCOMPARE(resets.size(), 0);
        QVERIFY(changes.size() >= 1);
        QCOMPARE(inserts.size(), 1);
        QCOMPARE(removals.size(), 1);
        QCOMPARE(f.model.count(), 2);
        // The kept row keeps its place, the new one follows it
        QCOMPARE(f.row(0, RemoteSessionsModel::SessionIdRole).toString(), kSessionId);
        QVERIFY(f.row(0, RemoteSessionsModel::PausedRole).toBool());
        QCOMPARE(f.row(0, RemoteSessionsModel::NowPlayingRole).toString(), QStringLiteral("Title"));
        QCOMPARE(f.row(1, RemoteSessionsModel::SessionIdRole).toString(), newId);
    }

    void thisClientIsNoRemoteSession()
    {
        Fixture f;
        QVERIFY(f.ok);
        const auto session = [](const QString &id, const QString &deviceId) {
            return QJsonObject{{QStringLiteral("Id"), id},
                               {QStringLiteral("DeviceId"), deviceId},
                               {QStringLiteral("DeviceName"), QStringLiteral("A device")},
                               {QStringLiteral("Client"), QStringLiteral("Kinefin")}};
        };
        // Our own device id, in another spelling, is left out
        f.server.listingRawBody.insert(
            QStringLiteral("Sessions"),
            QJsonDocument(QJsonArray{session(QStringLiteral("a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"),
                                             f.client.deviceId().toUpper()),
                                     session(QStringLiteral("b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2"),
                                             QStringLiteral("another-device"))})
                .toJson(QJsonDocument::Compact));
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        QCOMPARE(f.model.count(), 1);
        QCOMPARE(f.row(0, RemoteSessionsModel::SessionIdRole).toString(),
                 QStringLiteral("b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2"));
        QCOMPARE(f.row(0, RemoteSessionsModel::DeviceKindRole).toString(),
                 QStringLiteral("computer"));
    }

    void theKindOfDeviceComesFromItsClientAndName_data()
    {
        QTest::addColumn<QString>("client");
        QTest::addColumn<QString>("device");
        QTest::addColumn<QString>("kind");
        QTest::newRow("web") << QStringLiteral("Jellyfin Web") << QStringLiteral("Firefox")
                             << QStringLiteral("browser");
        QTest::newRow("android phone") << QStringLiteral("Jellyfin Android")
                                       << QStringLiteral("Pixel 8") << QStringLiteral("phone");
        QTest::newRow("android tv") << QStringLiteral("Jellyfin Android TV")
                                    << QStringLiteral("SHIELD") << QStringLiteral("tv");
        QTest::newRow("webos") << QStringLiteral("Jellyfin for WebOS") << QStringLiteral("LG")
                               << QStringLiteral("tv");
        QTest::newRow("kodi") << QStringLiteral("Kodi") << QStringLiteral("Living room")
                              << QStringLiteral("tv");
        QTest::newRow("ipad") << QStringLiteral("Swiftfin iOS") << QStringLiteral("iPad")
                              << QStringLiteral("tablet");
        QTest::newRow("iphone") << QStringLiteral("Swiftfin iOS") << QStringLiteral("iPhone")
                                << QStringLiteral("phone");
        QTest::newRow("desktop") << QStringLiteral("Jellyfin Media Player")
                                 << QStringLiteral("workstation") << QStringLiteral("computer");
        QTest::newRow("kinefin") << QStringLiteral("Kinefin") << QStringLiteral("laptop")
                                 << QStringLiteral("computer");
        QTest::newRow("unknown") << QStringLiteral("Some Client") << QStringLiteral("Box")
                                 << QStringLiteral("other");
    }
    void theKindOfDeviceComesFromItsClientAndName()
    {
        QFETCH(QString, client);
        QFETCH(QString, device);
        QFETCH(QString, kind);
        QCOMPARE(RemoteSessionsModel::deviceKind(client, device), kind);
    }

    void expiredAuthenticationClearsTheSessionList()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.model.open());
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Ready);
        f.server.denyListings = true;
        f.model.reload();
        QTRY_COMPARE(f.model.status(), RemoteSessionsModel::Idle);
        QCOMPARE(f.model.count(), 0);
        QVERIFY(!f.model.commandPending());
    }
};

QTEST_GUILESS_MAIN(TestRemoteSessionsModel)
#include "test_remote_sessions_model.moc"
