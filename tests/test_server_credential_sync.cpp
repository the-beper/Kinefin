// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "FakeJellyfin.h"
#include "MemoryTokenStore.h"
#include "settings/ServerCredentialSync.h"

using Kinefin::JellyfinClient;
using Kinefin::ServerCredential;
using Kinefin::ServerCredentialSync;

namespace {
const QString kProfile = QStringLiteral("default");
// The server id of tests/fixtures/system_info_public.json, and the wallet key it gives
const QString kServerId = QStringLiteral("9c076c52ba5340d59f6baf5b57cf0a5e");
const QString kKey = kProfile + QStringLiteral("/") + kServerId;

QList<ServerCredential> stored()
{
    QString error;
    const auto list = Kinefin::loadServerCredentials(kProfile, &error);
    if (!error.isEmpty())
        qWarning() << "stored(): load error" << error;
    return list;
}

QByteArray credentialsFile()
{
    QFile file(Kinefin::serverCredentialsPath(kProfile));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The restart: a fresh client with the same device identity probes the server; returns once the probe answered
bool probe(JellyfinClient &client, FakeJellyfin &server)
{
    QSignalSpy ready(&client, &JellyfinClient::serverReady);
    if (!prepare(client, server))
        return false;
    return QTest::qWaitFor([&] { return ready.size() == 1; }, 5000);
}

// A first run that stores a session, then closes the app
void storeASession(FakeJellyfin &server, MemoryTokenStore &store)
{
    JellyfinClient first;
    ServerCredentialSync sync(&first, &store, kProfile);
    QVERIFY(login(first, server));
    QTRY_COMPARE(stored().size(), 1);
}
} // namespace

class TestServerCredentialSync : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void init()
    {
        m_home = std::make_unique<QTemporaryDir>();
        QVERIFY(m_home->isValid());
        qputenv("XDG_DATA_HOME", m_home->path().toUtf8());
    }

    // The token goes to the store; the file only ever holds the record
    void loginStoresTheTokenInTheStoreAndNeverInTheFile()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QSignalSpy changed(&sync, &ServerCredentialSync::credentialsChanged);
        QVERIFY(login(client, server));
        QTRY_COMPARE(changed.size(), 1);
        QCOMPARE(store.tokens.value(kKey).toUtf8(), kFixtureToken);
        const auto saved = stored();
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().serverId, kServerId);
        QCOMPARE(saved.first().deviceId, QStringLiteral("stable-test-id"));
        QVERIFY(!credentialsFile().contains(kFixtureToken));
        QVERIFY(!credentialsFile().contains("token"));
    }

    void aRestartRestoresWithoutAPassword()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server,
                      store); // the app closes: credentialsCleared fires, nothing is forgotten
        QCOMPARE(store.tokens.size(), 1);

        JellyfinClient second;
        ServerCredentialSync sync(&second, &store, kProfile);
        QCOMPARE(sync.credentials().size(), 1);
        QSignalSpy loggedIn(&second, &JellyfinClient::loginSucceeded);
        QSignalSpy finished(&sync, &ServerCredentialSync::restoreFinished);
        QVERIFY(prepare(second, server));
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(second.isAuthenticated());
        QCOMPARE(finished.size(), 1);
        QVERIFY(!sync.restorePending());
        QCOMPARE(server.successfulLogins, 1); // no second login happened
        // The restored session is re-verified with the stored token
        QTRY_COMPARE(server.usersMeRequests, 1);
        QVERIFY(server.lastUsersMeAuth.contains(kTokenField));
        QCOMPARE(stored().size(), 1);
    }

    void aRestoreIsPendingWhileTheStoreAnswers()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);

        store.hold = true; // the wallet is still being unlocked
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(probe(client, server));
        QVERIFY(sync.restorePending());
        QVERIFY(!client.isAuthenticated());
        QSignalSpy finished(&sync, &ServerCredentialSync::restoreFinished);
        store.release();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(client.isAuthenticated());
        QVERIFY(!sync.restorePending());
    }

    void aWalletThatCannotBeReadMeansSigningIn()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);

        store.failReads = true;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QSignalSpy finished(&sync, &ServerCredentialSync::restoreFinished);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!client.isAuthenticated());
        QCOMPARE(server.usersMeRequests, 0);
        QCOMPARE(stored().size(), 1); // kept: the wallet may answer next time
    }

    void aRestoreAbandonedWhileTheStoreAnswersDoesNothing()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);

        store.hold = true;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(probe(client, server));
        QVERIFY(sync.restorePending());
        client.disconnectFromServer();
        QSignalSpy finished(&sync, &ServerCredentialSync::restoreFinished);
        store.release();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(!sync.restorePending());
    }

    void aRevokedSessionIsForgottenOnRestore()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);

        server.denyUsersMe = true;
        JellyfinClient second;
        ServerCredentialSync sync(&second, &store, kProfile);
        QSignalSpy expired(&second, &JellyfinClient::sessionExpired);
        QVERIFY(prepare(second, server));
        QTRY_COMPARE(expired.size(), 1);
        QVERIFY(!second.isAuthenticated());
        QVERIFY(sync.credentials().isEmpty());
        QVERIFY(stored().isEmpty());
        QTRY_VERIFY(store.tokens.isEmpty()); // the token left the wallet too
    }

    void aSessionExpiredWhileRunningIsForgotten()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(login(client, server));
        QTRY_COMPARE(stored().size(), 1);
        server.denyViews = true;
        QSignalSpy expired(&client, &JellyfinClient::sessionExpired);
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(expired.size(), 1);
        QVERIFY(stored().isEmpty());
        QTRY_VERIFY(store.tokens.isEmpty());
    }

    void plainDisconnectKeepsTheSessionButSignOutForgetsIt()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(login(client, server));
        QTRY_COMPARE(stored().size(), 1);
        client.disconnectFromServer();
        QCOMPARE(stored().size(), 1);
        QCOMPARE(store.tokens.size(), 1);
        // Reconnecting restores the kept session
        QVERIFY(probe(client, server));
        QTRY_VERIFY(client.isAuthenticated());
        sync.signOut();
        QVERIFY(!client.isAuthenticated());
        QVERIFY(stored().isEmpty());
        QVERIFY(sync.credentials().isEmpty());
        QTRY_VERIFY(store.tokens.isEmpty());
    }

    // A forget that overtakes a save still in flight leaves nothing behind
    void signingOutBeforeTheSaveFinishedLeavesNothing()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        store.hold = true;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(login(client, server));
        sync.signOut();
        store.release();
        QTRY_COMPARE(store.writes, 1);
        QTRY_VERIFY(store.tokens.isEmpty());
        QVERIFY(stored().isEmpty());
        QVERIFY(sync.credentials().isEmpty());
    }

    // The sign-out's removal and the second login's save may reach the wallet in any order after a slow unlock: the newer token must survive the older save's cleanup
    void aNewerSaveSurvivesTheCleanupOfAnOlderOne()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.rotateSecondLoginToken = true;
        MemoryTokenStore store;
        store.hold = true;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(login(client, server)); // save 1, held
        sync.signOut();                 // removal, held
        QVERIFY(login(client, server)); // save 2 with the rotated token, held
        store.release();
        QTRY_COMPARE(store.tokens.value(kKey).toUtf8(), server.secondLoginToken);
        QTRY_COMPARE(stored().size(), 1);
        QTest::qWait(100); // nothing may take the new token out afterwards
        QCOMPARE(store.tokens.value(kKey).toUtf8(), server.secondLoginToken);
    }

    // Forgetting another server must not discard this server's save
    void forgettingAnotherServerDoesNotDiscardASaveInFlight()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        store.hold = true;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(login(client, server));
        sync.forgetServer(QStringLiteral("another-server"));
        store.release();
        QTRY_COMPARE(stored().size(), 1);
        QCOMPARE(store.tokens.value(kKey).toUtf8(), kFixtureToken);
    }

    // No plaintext fallback: when the token cannot be stored nothing is persisted and the user is told
    void aWalletThatRefusesTheTokenPersistsNothing()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        store.failWrites = true;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QSignalSpy failed(&sync, &ServerCredentialSync::saveFailed);
        QVERIFY(login(client, server));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(client.isAuthenticated()); // the session itself lives on, in memory
        QVERIFY(stored().isEmpty());
        QVERIFY(!QFileInfo::exists(Kinefin::serverCredentialsPath(kProfile))
                || !credentialsFile().contains(kFixtureToken));
        QVERIFY(sync.credentials().isEmpty());
    }

    // A login that is not a restore (here: another device id) replaces the entry of the same server
    void aLaterLoginReplacesTheEntryOfTheSameServer()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.rotateSecondLoginToken = true;
        MemoryTokenStore store;
        storeASession(server, store);

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(client.setDeviceIdentity(QStringLiteral("Kinefin test"),
                                         QStringLiteral("another-device"),
                                         QStringLiteral("0.0.1")));
        QSignalSpy ready(&client, &JellyfinClient::serverReady);
        QSignalSpy loggedIn(&client, &JellyfinClient::loginSucceeded);
        QVERIFY(client.connectToServer(server.url(QStringLiteral("/jellyfin"))));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QTRY_COMPARE(store.tokens.value(kKey).toUtf8(), server.secondLoginToken);
        QTRY_COMPARE(stored().first().deviceId, QStringLiteral("another-device"));
        QCOMPARE(stored().size(), 1);
    }

    // The last entry is the most recently used one: a restore moves its entry there
    void aRestoreMakesTheServerTheMostRecentlyUsed()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        auto saved = stored();
        ServerCredential other = saved.first();
        other.serverId = QStringLiteral("another-server");
        other.url = QStringLiteral("http://other.invalid:8096");
        saved.append(other); // order: fake server, other
        QVERIFY(Kinefin::saveServerCredentials(kProfile, saved));

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(probe(client, server));
        QTRY_VERIFY(client.isAuthenticated());
        QTRY_COMPARE(stored().last().serverId, kServerId);
        QCOMPARE(stored().size(), 2);
    }

    // The token is read and sent only for the server and device it was issued for
    void aStoredSessionIsNotRestoredForAnotherHostOrDevice()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        {
            // Another device id: not restored, and the wallet is not even asked
            JellyfinClient other;
            ServerCredentialSync sync(&other, &store, kProfile);
            QVERIFY(other.setDeviceIdentity(QStringLiteral("Kinefin test"),
                                            QStringLiteral("another-device"),
                                            QStringLiteral("0.0.1")));
            QSignalSpy ready(&other, &JellyfinClient::serverReady);
            QVERIFY(other.connectToServer(server.url(QStringLiteral("/jellyfin"))));
            QTRY_COMPARE(ready.size(), 1);
            QVERIFY(!other.isAuthenticated());
        }
        // Same id and device but another host, port, scheme or path in the entry
        const QStringList others{
            QStringLiteral("http://elsewhere.invalid:8096"),
            server.url(QStringLiteral("/other")).toString(),
            QStringLiteral("https://127.0.0.1:%1/jellyfin").arg(server.url().port()),
            QStringLiteral("http://127.0.0.1:1/jellyfin")};
        for (const QString &url : others) {
            auto saved = stored();
            QCOMPARE(saved.size(), 1);
            saved.first().url = url;
            QVERIFY(Kinefin::saveServerCredentials(kProfile, saved));
            JellyfinClient moved;
            ServerCredentialSync sync(&moved, &store, kProfile);
            QVERIFY(probe(moved, server));
            QVERIFY2(!moved.isAuthenticated(), qPrintable(url));
        }
        QCOMPARE(store.reads, 0);
        QCOMPARE(server.usersMeRequests, 0);
    }

    void anUnreadableFileIsDeletedAndLoginStillPersists()
    {
        const QString path = Kinefin::serverCredentialsPath(kProfile);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not json");
        }
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(sync.credentials().isEmpty());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".broken")));
        QVERIFY(login(client, server));
        QTRY_COMPARE(stored().size(), 1);
    }

    // A token left in the file by an older snapshot is rewritten away as soon as the file is read
    void aLegacyTokenInTheFileIsDroppedAtOnce()
    {
        const QString path = Kinefin::serverCredentialsPath(kProfile);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"servers\":[{\"serverId\":\"a\",\"url\":\"http://x:8096\","
                       "\"token\":\"legacy-secret\"}]}");
        }
        FakeJellyfin server;
        MemoryTokenStore store;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QCOMPARE(sync.credentials().size(), 1);
        QVERIFY(!credentialsFile().contains("legacy-secret"));
        QVERIFY(!credentialsFile().contains("token"));
    }

    void anInvalidProfileIdStoresNothingAnywhere()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, QStringLiteral("../escape"));
        QSignalSpy failed(&sync, &ServerCredentialSync::saveFailed);
        QVERIFY(login(client, server));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(store.writes, 0);
        QVERIFY(store.tokens.isEmpty());
        QVERIFY(!QFileInfo::exists(m_home->path() + QStringLiteral("/kinefin")));
        QVERIFY(!QFileInfo::exists(m_home->path() + QStringLiteral("/escape")));
    }

  private:
    std::unique_ptr<QTemporaryDir> m_home;
};

QTEST_GUILESS_MAIN(TestServerCredentialSync)
#include "test_server_credential_sync.moc"
