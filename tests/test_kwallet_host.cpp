// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises KWalletTokenStore against the real KWallet of the session bus it is started in. It is skipped unless KINEFIN_TEST_KWALLET=1, because it writes a throw-away entry (a random key and a dummy value, removed again) to the user's own wallet, and an unlock prompt may appear.
#include "settings/KWalletTokenStore.h"

#include <QEventLoop>
#include <QTimer>
#include <QUuid>
#include <QtTest>
#include <KWallet>

using Kinefin::KWalletTokenStore;

namespace {
struct Result {
    bool done = false;
    bool ok = false;
    QString token;
};

// Runs the event loop until `result` is filled, or fails after the wallet had its time.
bool wait(const Result &result, int timeoutMs = 60000)
{
    QElapsedTimer timer;
    timer.start();
    while (!result.done && timer.elapsed() < timeoutMs)
        QTest::qWait(20);
    return result.done;
}
} // namespace

class TestKWalletHost : public QObject {
    Q_OBJECT
  private:
    QString m_key;

    bool entryExists() const
    {
        // Our own throw-away key only: the value of no real entry is read here.
        QScopedPointer<KWallet::Wallet> wallet(KWallet::Wallet::openWallet(
            KWallet::Wallet::LocalWallet(), 0, KWallet::Wallet::Synchronous));
        return wallet && wallet->hasFolder(QStringLiteral("Kinefin"))
               && wallet->setFolder(QStringLiteral("Kinefin")) && wallet->hasEntry(m_key);
    }

  private Q_SLOTS:
    void initTestCase()
    {
        if (qEnvironmentVariable("KINEFIN_TEST_KWALLET") != QLatin1String("1"))
            QSKIP("set KINEFIN_TEST_KWALLET=1 to run against the session's real KWallet");
        if (!KWallet::Wallet::isEnabled())
            QSKIP("KWallet is disabled in this session");
        m_key = QStringLiteral("kinefin-selftest/")
                + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    void cleanupTestCase()
    {
        if (m_key.isEmpty())
            return;
        KWalletTokenStore store;
        Result result;
        store.remove(m_key, [&](bool ok) { result = {true, ok, {}}; });
        wait(result, 20000);
    }

    void writeReadOverwriteRemove()
    {
        KWalletTokenStore store;
        Result result;
        const QString first =
            QStringLiteral("dummy-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        store.write(m_key, first, [&](bool ok) { result = {true, ok, {}}; });
        QVERIFY2(wait(result), "the wallet did not answer in time (locked and unanswered prompt?)");
        QVERIFY(result.ok);
        // The folder and key layout the docs promise: folder `Kinefin`, key `<profile>/<server id>`.
        QVERIFY(entryExists());

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
        QVERIFY(!entryExists());

        result = {};
        store.read(m_key, [&](bool ok, const QString &token) { result = {true, ok, token}; });
        QVERIFY(wait(result));
        QVERIFY(!result.ok);
        QVERIFY(result.token.isEmpty());

        // Removing what is not there is a success, as the TokenStore contract says.
        result = {};
        store.remove(m_key, [&](bool ok) { result = {true, ok, {}}; });
        QVERIFY(wait(result));
        QVERIFY(result.ok);
    }

    void operationsQueuedBeforeTheWalletIsOpenRunInOrder()
    {
        // A fresh store has no open wallet: the three calls wait for it and must keep their order.
        KWalletTokenStore store;
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
        QVERIFY(!entryExists());
    }
};
QTEST_GUILESS_MAIN(TestKWalletHost)
#include "test_kwallet_host.moc"
