// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <utility>

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
// The user of tests/fixtures/authenticate_success.json, and the per-user and legacy wallet keys
const QString kUserId = QStringLiteral("d5ed5d4e2e5447b58e9f2b6f7bd5c96a");
const QString kKey = kProfile + QStringLiteral("/") + kServerId + QStringLiteral("/") + kUserId;
const QString kLegacyKey = kProfile + QStringLiteral("/") + kServerId;
const QString kOtherUserId = QStringLiteral("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
const QString kOtherKey =
    kProfile + QStringLiteral("/") + kServerId + QStringLiteral("/") + kOtherUserId;

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

// Restores the most recently used user of the probed server, as the connection controller does after a probe
bool restoreLast(ServerCredentialSync &sync)
{
    const auto users = sync.restorableUsers();
    return !users.isEmpty() && sync.restoreUser(users.constLast().userId);
}

// Another user of the fake server's server in the stored records, with a token of its own in `store`
// The fake server learns both users, so `/Users/Me` answers by the token of the request
void addOtherUser(MemoryTokenStore &store, FakeJellyfin *server = nullptr)
{
    if (server) {
        server->users = {{kUserId, kFixtureToken, QStringLiteral("fixture-user"), {}, false},
                         {kOtherUserId, "other-token-0123", QStringLiteral("other"), {}, false}};
    }
    auto saved = stored();
    ServerCredential other = saved.first();
    other.userId = kOtherUserId;
    other.username = QStringLiteral("other");
    other.imageTag.clear();
    saved.append(other);
    QVERIFY(Kinefin::saveServerCredentials(kProfile, saved));
    store.tokens.insert(kOtherKey, QStringLiteral("other-token-0123"));
}

// The state an older version left: the record of `userId` has no `remembered` key, so it owns the legacy per-server wallet key
void makeLegacyOwner(const QString &userId = kUserId)
{
    auto saved = stored();
    for (ServerCredential &record : saved)
        record.ownsLegacyKey = record.userId == userId;
    QVERIFY(Kinefin::saveServerCredentials(kProfile, saved));
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
        QCOMPARE(saved.first().userId, kUserId);
        QVERIFY(saved.first().remembered);
        QVERIFY(!saved.first().askPassword);
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
        QVERIFY(probe(second, server));
        QVERIFY(!sync.restorePending()); // a probe restores nothing by itself
        QCOMPARE(store.reads, 0);
        QVERIFY(restoreLast(sync));
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
        QVERIFY(restoreLast(sync));
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
        QVERIFY(probe(client, server));
        QVERIFY(restoreLast(sync));
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
        QVERIFY(restoreLast(sync));
        QVERIFY(sync.restorePending());
        client.disconnectFromServer();
        QSignalSpy finished(&sync, &ServerCredentialSync::restoreFinished);
        store.release();
        QTRY_VERIFY(!sync.restorePending());
        // Nobody waits for the answer of a server the client left: no signal, and nothing restored
        QTest::qWait(50);
        QCOMPARE(finished.size(), 0);
        QVERIFY(!client.isAuthenticated());
    }

    void cancelRestoreIgnoresTheLateAnswer()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);

        store.hold = true;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(probe(client, server));
        QVERIFY(restoreLast(sync));
        QVERIFY(sync.restorePending());
        QSignalSpy finished(&sync, &ServerCredentialSync::restoreFinished);
        sync.cancelRestore();
        QVERIFY(!sync.restorePending());
        store.release();
        QTest::qWait(100);
        // The server is still probed, yet the cancelled read neither restores nor reports
        QVERIFY(!client.isAuthenticated());
        QCOMPARE(finished.size(), 0);
        QVERIFY(!sync.restorePending());
        // A new restore works as before
        QVERIFY(restoreLast(sync));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(client.isAuthenticated());
    }

    // A revoked token takes only that user's token with it: the record stays (asking for the password next time) and the other users are untouched
    void expiryForgetsOnlyThatUsersTokenAndKeepsTheRecord()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        addOtherUser(store, &server);

        server.denyUsersMe = true;
        JellyfinClient second;
        ServerCredentialSync sync(&second, &store, kProfile);
        QSignalSpy expired(&second, &JellyfinClient::sessionExpired);
        QVERIFY(probe(second, server));
        QVERIFY(sync.restoreUser(kUserId));
        QTRY_COMPARE(expired.size(), 1);
        QVERIFY(!second.isAuthenticated());
        QTRY_VERIFY(!store.tokens.contains(kKey)); // the token left the wallet
        QCOMPARE(store.tokens.value(kOtherKey), QStringLiteral("other-token-0123"));
        QCOMPARE(sync.credentials().size(), 2);
        for (const ServerCredential &record : sync.credentials())
            QCOMPARE(record.remembered, record.userId == kOtherUserId);
        QCOMPARE(stored().size(), 2);
        // A user that is not remembered is not restored, and asks the password
        QVERIFY(!sync.restoreUser(kUserId));
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
        QTRY_VERIFY(store.tokens.isEmpty());
        QCOMPARE(stored().size(), 1);
        QVERIFY(!stored().first().remembered);
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
        QVERIFY(restoreLast(sync));
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
        QVERIFY(sync.restoreUser(kUserId));
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
            QVERIFY(sync.restorableUsers().isEmpty());
            QVERIFY(!sync.restoreUser(kUserId));
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
            QVERIFY2(sync.restorableUsers().isEmpty(), qPrintable(url));
            QVERIFY2(!sync.restoreUser(kUserId), qPrintable(url));
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
                       "\"userId\":\"u1\",\"token\":\"legacy-secret\"}]}");
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

    void twoUsersOfOneServerKeepTheirOwnTokens()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        addOtherUser(store, &server);

        // The second user restores with its own token, and the first one's stays where it is
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QCOMPARE(sync.restorableUsers().size(), 0); // not probed yet
        QVERIFY(probe(client, server));
        QCOMPARE(sync.restorableUsers().size(), 2);
        QVERIFY(sync.restoreUser(kOtherUserId));
        QTRY_VERIFY(client.isAuthenticated());
        QCOMPARE(client.userId(), kOtherUserId);
        QTRY_COMPARE(server.usersMeRequests, 1);
        QVERIFY(server.lastUsersMeAuth.contains("Token=\"other-token-0123\""));
        QCOMPARE(store.tokens.value(kKey).toUtf8(), kFixtureToken);
        QCOMPARE(store.tokens.value(kOtherKey), QStringLiteral("other-token-0123"));
        QCOMPARE(stored().size(), 2);
        QCOMPARE(stored().last().userId, kOtherUserId); // the most recently used one

        // Dropping one user leaves the other's token and record
        sync.forgetUser(kServerId, kOtherUserId);
        QTRY_VERIFY(!store.tokens.contains(kOtherKey));
        QCOMPARE(store.tokens.value(kKey).toUtf8(), kFixtureToken);
        QCOMPARE(stored().size(), 1);
        QCOMPARE(stored().first().userId, kUserId);
    }

    void legacyKeyMigratesOnRestore()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        // The state an older version left: the token under the per-server key
        store.tokens.clear();
        store.tokens.insert(kLegacyKey, QString::fromUtf8(kFixtureToken));
        makeLegacyOwner();

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(probe(client, server));
        QVERIFY(restoreLast(sync));
        QTRY_VERIFY(client.isAuthenticated());
        QTRY_VERIFY(!store.tokens.contains(kLegacyKey));
        QCOMPARE(store.tokens.value(kKey).toUtf8(), kFixtureToken);
        QCOMPARE(store.tokens.size(), 1);
        QCOMPARE(server.successfulLogins, 1);
        // Migrated: the record owns nothing any more, and says so in the file
        QTRY_VERIFY(!stored().first().ownsLegacyKey);
        QVERIFY(stored().first().remembered);
    }

    // The legacy key held the token of one user only: another stored user never reads it
    void onlyTheOwnerOfTheLegacyKeyReadsIt()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        addOtherUser(store, &server);
        store.tokens.clear();
        store.tokens.insert(kLegacyKey, QString::fromUtf8(kFixtureToken));
        makeLegacyOwner(kUserId);

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QSignalSpy finished(&sync, &ServerCredentialSync::restoreFinished);
        QVERIFY(probe(client, server));
        // The other user has no token of its own: one read, no fall back to the owner's token
        QVERIFY(sync.restoreUser(kOtherUserId));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(store.reads, 1);
        QVERIFY(!client.isAuthenticated());
        QCOMPARE(store.tokens.value(kLegacyKey).toUtf8(), kFixtureToken);
        // The owner migrates it
        QVERIFY(sync.restoreUser(kUserId));
        QTRY_VERIFY(client.isAuthenticated());
        QCOMPARE(client.userId(), kUserId);
        QTRY_VERIFY(!store.tokens.contains(kLegacyKey));
        QCOMPARE(store.tokens.value(kKey).toUtf8(), kFixtureToken);
    }

    // Every way of forgetting the owner takes the legacy token with it; forgetting another user leaves it
    void forgettingTheOwnerRemovesTheLegacyKey()
    {
        using Forget = std::function<void(ServerCredentialSync &, const QString &)>;
        const QList<std::pair<const char *, Forget>> forgets = {
            {"forgetUser", [](ServerCredentialSync &sync,
                              const QString &user) { sync.forgetUser(kServerId, user); }},
            {"forgetToken", [](ServerCredentialSync &sync,
                               const QString &user) { sync.forgetToken(kServerId, user); }},
            {"setAskPassword", [](ServerCredentialSync &sync, const QString &user) {
                 sync.setAskPassword(kServerId, user, true);
             }}};
        for (const auto &[name, forget] : forgets) {
            MemoryTokenStore store;
            ServerCredential owner;
            owner.serverId = kServerId;
            owner.url = QStringLiteral("http://127.0.0.1:1/jellyfin");
            owner.userId = kUserId;
            owner.deviceId = QStringLiteral("stable-test-id");
            owner.ownsLegacyKey = true;
            ServerCredential other = owner;
            other.userId = kOtherUserId;
            other.ownsLegacyKey = false;
            QVERIFY(Kinefin::saveServerCredentials(kProfile, {owner, other}));
            store.tokens.insert(kLegacyKey, QStringLiteral("legacy-token"));
            store.tokens.insert(kOtherKey, QStringLiteral("other-token-0123"));

            JellyfinClient client;
            ServerCredentialSync sync(&client, &store, kProfile);
            forget(sync, kOtherUserId);
            QTRY_VERIFY2(!store.tokens.contains(kOtherKey), name);
            QTest::qWait(50);
            QVERIFY2(store.tokens.contains(kLegacyKey), name);
            forget(sync, kUserId);
            QTRY_VERIFY2(!store.tokens.contains(kLegacyKey), name);
            for (const ServerCredential &record : stored()) {
                if (record.userId == kUserId)
                    QVERIFY2(!record.ownsLegacyKey, name);
            }
        }
    }

    // A password login of the owner replaces both its tokens, and the legacy key goes
    void loginOfTheLegacyOwnerRemovesTheLegacyKey()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        store.tokens.clear();
        store.tokens.insert(kLegacyKey, QStringLiteral("old-token"));
        makeLegacyOwner();

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(login(client, server));
        QTRY_COMPARE(store.tokens.value(kKey).toUtf8(), kFixtureToken);
        QTRY_VERIFY(!store.tokens.contains(kLegacyKey));
        QCOMPARE(store.tokens.size(), 1);
        QTRY_VERIFY(!stored().first().ownsLegacyKey);
        QVERIFY(stored().first().remembered);
    }

    // The token of a user who signs in again is dead on the server: when the new one cannot be stored, nothing is left to restore
    void relogInWhoseSaveFailsLeavesNoOldToken()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        QVERIFY(store.tokens.contains(kKey));

        store.failWrites = true;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QSignalSpy failed(&sync, &ServerCredentialSync::saveFailed);
        QVERIFY(login(client, server));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(client.isAuthenticated());
        QTRY_VERIFY(!store.tokens.contains(kKey));
        QCOMPARE(stored().size(), 1);
        QVERIFY(!stored().first().remembered);
        QVERIFY(!sync.restoreUser(kUserId));
    }

    void migrationWriteFailureKeepsLegacyToken()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        store.tokens.clear();
        store.tokens.insert(kLegacyKey, QString::fromUtf8(kFixtureToken));
        makeLegacyOwner();
        const int writesBefore = store.writes;
        store.failWrites = true;

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QSignalSpy failed(&sync, &ServerCredentialSync::saveFailed);
        QVERIFY(probe(client, server));
        QVERIFY(restoreLast(sync));
        QTRY_VERIFY(client.isAuthenticated()); // the session itself is fine
        QTRY_COMPARE(store.writes, writesBefore + 1);
        QTest::qWait(100);
        // Kept for the next restart
        QCOMPARE(store.tokens.value(kLegacyKey).toUtf8(), kFixtureToken);
        QVERIFY(!store.tokens.contains(kKey));
        QCOMPARE(failed.size(), 0); // not the user's business
        // Still the owner after a restart, so the next one retries the migration
        QVERIFY(stored().first().ownsLegacyKey);
    }

    // No token is ever written for a user that always asks for the password
    void askPasswordUserNeverWritesTheStore()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        ServerCredential record;
        record.serverId = kServerId;
        record.url = server.url(QStringLiteral("/jellyfin")).toString();
        record.userId = kUserId;
        record.deviceId = QStringLiteral("stable-test-id");
        record.deviceName = QStringLiteral("Kinefin test");
        record.askPassword = true;
        record.remembered = false;
        QVERIFY(Kinefin::saveServerCredentials(kProfile, {record}));

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QSignalSpy changed(&sync, &ServerCredentialSync::credentialsChanged);
        QVERIFY(login(client, server));
        QTRY_COMPARE(changed.size(), 1);
        QCOMPARE(store.writes, 0);
        QVERIFY(store.tokens.isEmpty());
        QCOMPARE(stored().size(), 1);
        QVERIFY(stored().first().askPassword);
        QVERIFY(!stored().first().remembered);
        QVERIFY(!sync.restoreUser(kUserId));
        QCOMPARE(store.reads, 0);
    }

    // A token left from before the choice goes with the next sign-in of that user
    void askPasswordLoginRemovesALeftoverToken()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        ServerCredential record;
        record.serverId = kServerId;
        record.url = server.url(QStringLiteral("/jellyfin")).toString();
        record.userId = kUserId;
        record.deviceId = QStringLiteral("stable-test-id");
        record.askPassword = true;
        record.remembered = false;
        QVERIFY(Kinefin::saveServerCredentials(kProfile, {record}));
        store.tokens.insert(kKey, QStringLiteral("leftover-token"));

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(login(client, server));
        QTRY_VERIFY(store.tokens.isEmpty());
        QCOMPARE(store.writes, 0);
    }

    void turningAskPasswordOnRemovesTheStoredTokenAndOffKeepsItGone()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        sync.setAskPassword(kServerId, kUserId, true);
        QTRY_VERIFY(store.tokens.isEmpty());
        QVERIFY(stored().first().askPassword);
        QVERIFY(!stored().first().remembered);
        sync.setAskPassword(kServerId, kUserId, false);
        QVERIFY(!stored().first().askPassword);
        QVERIFY(!stored().first().remembered); // until the next sign-in stores a token again
        QVERIFY(store.tokens.isEmpty());
    }

    void forgetUserDuringHeldSaveDiscardsIt()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        store.hold = true;
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(login(client, server));
        sync.forgetUser(kServerId, kUserId);
        store.release();
        QTRY_COMPARE(store.writes, 1);
        QTRY_VERIFY(store.tokens.isEmpty());
        QVERIFY(stored().isEmpty());
        QVERIFY(sync.credentials().isEmpty());
    }

    void forgetServerRemovesEveryUserToken()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        addOtherUser(store, &server);
        // Another server's record and token, and a leftover legacy token of this one
        auto saved = stored();
        ServerCredential another = saved.first();
        another.serverId = QStringLiteral("another-server");
        another.url = QStringLiteral("http://other.invalid:8096");
        saved.append(another);
        QVERIFY(Kinefin::saveServerCredentials(kProfile, saved));
        const QString anotherKey = kProfile + QStringLiteral("/another-server/") + kUserId;
        store.tokens.insert(anotherKey, QStringLiteral("keep"));
        store.tokens.insert(kLegacyKey, QStringLiteral("old"));

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QCOMPARE(sync.credentials().size(), 3);
        sync.forgetServer(kServerId);
        QTRY_COMPARE(store.tokens.size(), 1);
        QVERIFY(store.tokens.contains(anotherKey));
        QCOMPARE(stored().size(), 1);
        QCOMPARE(stored().first().serverId, QStringLiteral("another-server"));
    }

    void forgetProfileTokensRemovesPerUserAndLegacyKeys()
    {
        MemoryTokenStore store;
        ServerCredential record;
        record.url = QStringLiteral("http://x.invalid:8096");
        record.deviceId = QStringLiteral("d");
        QList<ServerCredential> records;
        for (const auto &[server, user] :
             {std::pair{"srv1", "u1"}, std::pair{"srv1", "u2"}, std::pair{"srv2", "u1"}}) {
            record.serverId = QString::fromLatin1(server);
            record.userId = QString::fromLatin1(user);
            records.append(record);
        }
        QVERIFY(Kinefin::saveServerCredentials(kProfile, records));
        store.tokens.insert(QStringLiteral("default/srv1/u1"), QStringLiteral("t"));
        store.tokens.insert(QStringLiteral("default/srv1/u2"), QStringLiteral("t"));
        store.tokens.insert(QStringLiteral("default/srv2/u1"), QStringLiteral("t"));
        store.tokens.insert(QStringLiteral("default/srv1"), QStringLiteral("legacy"));
        store.tokens.insert(QStringLiteral("default/srv2"), QStringLiteral("legacy"));
        store.tokens.insert(QStringLiteral("other/srv1/u1"), QStringLiteral("keep"));

        bool done = false;
        bool ok = false;
        Kinefin::forgetProfileTokens(kProfile, store, [&](bool result) {
            ok = result;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(ok);
        QCOMPARE(store.tokens.size(), 1);
        QVERIFY(store.tokens.contains(QStringLiteral("other/srv1/u1")));
    }

    void recordWithInvalidUserIdIsIgnored()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        ServerCredential good;
        good.serverId = kServerId;
        good.url = server.url(QStringLiteral("/jellyfin")).toString();
        good.userId = kUserId;
        good.deviceId = QStringLiteral("stable-test-id");
        QList<ServerCredential> records;
        for (const QString &bad : {QString(), QStringLiteral("a/b"), QStringLiteral("x y")}) {
            ServerCredential record = good;
            record.userId = bad;
            records.append(record);
        }
        ServerCredential badServer = good;
        badServer.serverId = QStringLiteral("a/b");
        records.append(badServer);
        records.append(good);
        QVERIFY(Kinefin::saveServerCredentials(kProfile, records));

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QCOMPARE(sync.credentials().size(), 1);
        QCOMPARE(sync.credentials().first().userId, kUserId);
        QVERIFY(probe(client, server));
        QCOMPARE(sync.restorableUsers().size(), 1);
        QVERIFY(!sync.restoreUser(QStringLiteral("a/b")));
        QCOMPARE(store.reads, 0);
    }

    // The record of the old format that had an invalid user id owned the legacy key: it goes with the record, unless the server id could alias another key
    void recordsWithInvalidIdsLeaveNoLegacyKeyBehind()
    {
        MemoryTokenStore store;
        ServerCredential bad;
        bad.serverId = kServerId;
        bad.url = QStringLiteral("http://127.0.0.1:1/jellyfin");
        bad.userId = QStringLiteral("x y");
        bad.deviceId = QStringLiteral("stable-test-id");
        ServerCredential aliasing = bad;
        aliasing.serverId = QStringLiteral("a/b");
        aliasing.userId = kUserId;
        QVERIFY(Kinefin::saveServerCredentials(kProfile, {bad, aliasing}));
        store.tokens.insert(kLegacyKey, QStringLiteral("old"));
        store.tokens.insert(QStringLiteral("default/a/b"), QStringLiteral("another-key"));

        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QVERIFY(sync.credentials().isEmpty());
        QTRY_VERIFY(!store.tokens.contains(kLegacyKey));
        QTest::qWait(50);
        QVERIFY(store.tokens.contains(QStringLiteral("default/a/b")));
    }

    void updateUserRefreshesTheNameAndTheTagOfOneRecord()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        MemoryTokenStore store;
        storeASession(server, store);
        JellyfinClient client;
        ServerCredentialSync sync(&client, &store, kProfile);
        QSignalSpy changed(&sync, &ServerCredentialSync::credentialsChanged);
        sync.updateUser(kServerId, kUserId, QStringLiteral("Renamed"), QStringLiteral("tag-2"));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(stored().first().username, QStringLiteral("Renamed"));
        QCOMPARE(stored().first().imageTag, QStringLiteral("tag-2"));
        sync.updateUser(kServerId, kUserId, QStringLiteral("Renamed"), QStringLiteral("tag-2"));
        QCOMPARE(changed.size(), 1); // unchanged: not rewritten
        sync.updateUser(kServerId, kUserId, QString(), QStringLiteral("bad tag/"));
        QCOMPARE(stored().first().username, QStringLiteral("Renamed"));
        QVERIFY(stored().first().imageTag.isEmpty());
    }

  private:
    std::unique_ptr<QTemporaryDir> m_home;
};

QTEST_GUILESS_MAIN(TestServerCredentialSync)
#include "test_server_credential_sync.moc"
