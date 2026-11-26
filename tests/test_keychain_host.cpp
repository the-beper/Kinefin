// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises KeychainTokenStore against the real keyring of the session bus it is started in. It is skipped unless KINEFIN_TEST_KEYCHAIN=1, because it writes a throw-away entry (a random key and a dummy value, removed again) to the user's own keyring, and an unlock prompt may appear
#include "settings/KeychainTokenStore.h"

#include <QElapsedTimer>
#include <QUuid>
#include <QtTest>

using Kinefin::KeychainTokenStore;

namespace {
struct Result {
    bool done = false;
    bool ok = false;
    QString token;
};

// Runs the event loop until `result` is filled, or fails after the keyring had its time
bool wait(const Result &result, int timeoutMs = 60000)
{
    QElapsedTimer timer;
    timer.start();
    while (!result.done && timer.elapsed() < timeoutMs)
        QTest::qWait(20);
    return result.done;
}
} // namespace

class TestKeychainHost : public QObject {
    Q_OBJECT
  private:
    QString m_key;

  private Q_SLOTS:
    void initTestCase()
    {
        if (qEnvironmentVariable("KINEFIN_TEST_KEYCHAIN") != QLatin1String("1"))
            QSKIP("set KINEFIN_TEST_KEYCHAIN=1 to run against the session's real keyring");
        m_key = QStringLiteral("kinefin-selftest/")
                + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    void cleanupTestCase()
    {
        if (m_key.isEmpty())
            return;
        KeychainTokenStore store;
        Result result;
        store.remove(m_key, [&](bool ok) { result = {true, ok, {}}; });
        wait(result, 20000);
    }

    void writeReadOverwriteRemove()
    {
        KeychainTokenStore store;
        Result result;
        const QString first =
            QStringLiteral("dummy-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        store.write(m_key, first, [&](bool ok) { result = {true, ok, {}}; });
        QVERIFY2(wait(result),
                 "the keyring did not answer in time (locked and unanswered prompt?)");
        QVERIFY(result.ok);

        result = {};
        store.read(m_key, [&](bool ok, const QString &token) { result = {true, ok, token}; });
        QVERIFY(wait(result));
        QVERIFY(result.ok);
        QCOMPARE(result.token, first);

        result = {};
        const QString second = first + QStringLiteral("-2");
        store.write(m_key, second, [&](bool ok) { result = {true, ok, {}}; });
        QVERIFY(wait(result) && result.ok);
        result = {};
        store.read(m_key, [&](bool ok, const QString &token) { result = {true, ok, token}; });
        QVERIFY(wait(result) && result.ok);
        QCOMPARE(result.token, second);

        result = {};
        store.remove(m_key, [&](bool ok) { result = {true, ok, {}}; });
        QVERIFY(wait(result) && result.ok);

        result = {};
        store.read(m_key, [&](bool ok, const QString &token) { result = {true, ok, token}; });
        QVERIFY(wait(result));
        QVERIFY(!result.ok);
        QVERIFY(result.token.isEmpty());
    }

    void operationsQueuedAtOnceRunInOrder()
    {
        // The three calls are queued before the first job starts and must keep their order
        KeychainTokenStore store;
        Result written;
        Result read;
        Result removed;
        const QString value = QStringLiteral("dummy-order");
        store.write(m_key, value, [&](bool ok) { written = {true, ok, {}}; });
        store.read(m_key, [&](bool ok, const QString &token) { read = {true, ok, token}; });
        store.remove(m_key, [&](bool ok) { removed = {true, ok, {}}; });
        QVERIFY(wait(removed));
        QVERIFY(written.done && written.ok);
        QVERIFY(read.done && read.ok);
        QCOMPARE(read.token, value);
        QVERIFY(removed.ok);
    }

    // Removing what is not there is a success, as the TokenStore contract says
    void removingAMissingKeySucceeds()
    {
        KeychainTokenStore store;
        Result result;
        store.remove(m_key, [&](bool ok) { result = {true, ok, {}}; });
        QVERIFY(wait(result));
        QVERIFY(result.ok);
        result = {};
        store.remove(m_key, [&](bool ok) { result = {true, ok, {}}; });
        QVERIFY(wait(result));
        QVERIFY(result.ok);
    }
};
QTEST_GUILESS_MAIN(TestKeychainHost)
#include "test_keychain_host.moc"
