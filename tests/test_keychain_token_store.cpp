// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The real KeychainTokenStore with no backend behind it: the contract that holds whatever the backend answers, failing closed, event-loop delivery, order, and no callback from a store that is gone
#include "settings/KeychainTokenStore.h"

#include <QStringList>
#include <QtTest>

using Kinefin::KeychainTokenStore;

class TestKeychainTokenStore : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void initTestCase()
    {
        // A session bus that does not exist and libsecret pinned, set before any job, so that even a direct run never reaches a keyring of the machine
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent");
        qputenv("QTKEYCHAIN_BACKEND", "libsecret");
    }

    void aStoreWithoutABackendFailsClosed()
    {
        KeychainTokenStore store;
        bool readDone = false;
        bool writeDone = false;
        bool removeDone = false;
        bool readOk = true;
        bool writeOk = true;
        bool removeOk = true;
        QString token = QStringLiteral("unset");
        store.read(QStringLiteral("selftest/read"), [&](bool ok, const QString &text) {
            readDone = true;
            readOk = ok;
            token = text;
        });
        QTRY_VERIFY(readDone);
        QVERIFY(!readOk);
        QVERIFY(token.isEmpty());
        store.write(QStringLiteral("selftest/write"), QStringLiteral("dummy"), [&](bool ok) {
            writeDone = true;
            writeOk = ok;
        });
        QTRY_VERIFY(writeDone);
        QVERIFY(!writeOk);
        store.remove(QStringLiteral("selftest/remove"), [&](bool ok) {
            removeDone = true;
            removeOk = ok;
        });
        QTRY_VERIFY(removeDone);
        QVERIFY(!removeOk);
    }

    void callbacksArriveFromTheEventLoop()
    {
        KeychainTokenStore store;
        int calls = 0;
        store.read(QStringLiteral("selftest/a"), [&](bool, const QString &) { ++calls; });
        store.write(QStringLiteral("selftest/a"), QStringLiteral("dummy"), [&](bool) { ++calls; });
        store.remove(QStringLiteral("selftest/a"), [&](bool) { ++calls; });
        QCOMPARE(calls, 0); // nothing ran inside the calls
        QTRY_COMPARE(calls, 3);
    }

    void operationsFinishInTheOrderTheyWereAsked()
    {
        KeychainTokenStore store;
        QStringList order;
        store.read(QStringLiteral("selftest/a"),
                   [&](bool, const QString &) { order.append(QStringLiteral("read 1")); });
        store.write(QStringLiteral("selftest/a"), QStringLiteral("dummy"),
                    [&](bool) { order.append(QStringLiteral("write")); });
        store.remove(QStringLiteral("selftest/a"),
                     [&](bool) { order.append(QStringLiteral("remove")); });
        store.read(QStringLiteral("selftest/a"),
                   [&](bool, const QString &) { order.append(QStringLiteral("read 2")); });
        QTRY_COMPARE(order.size(), 4);
        QCOMPARE(order, (QStringList{QStringLiteral("read 1"), QStringLiteral("write"),
                                     QStringLiteral("remove"), QStringLiteral("read 2")}));
    }

    // A callback that asks for more work is served after the ones already queued
    void workAskedFromACallbackRunsAfterTheQueue()
    {
        KeychainTokenStore store;
        QStringList order;
        store.remove(QStringLiteral("selftest/a"), [&](bool) {
            order.append(QStringLiteral("first"));
            store.remove(QStringLiteral("selftest/a"),
                         [&](bool) { order.append(QStringLiteral("third")); });
        });
        store.remove(QStringLiteral("selftest/a"),
                     [&](bool) { order.append(QStringLiteral("second")); });
        QTRY_COMPARE(order.size(), 3);
        QCOMPARE(order, (QStringList{QStringLiteral("first"), QStringLiteral("second"),
                                     QStringLiteral("third")}));
    }

    void aStoreDestroyedWithWorkQueuedNeverCallsBack()
    {
        int calls = 0;
        {
            // Nothing started yet
            KeychainTokenStore store;
            store.read(QStringLiteral("selftest/a"), [&](bool, const QString &) { ++calls; });
            store.write(QStringLiteral("selftest/a"), QStringLiteral("dummy"),
                        [&](bool) { ++calls; });
        }
        QTest::qWait(300);
        QCOMPARE(calls, 0);
        {
            // The first job may already be in flight when the store goes
            KeychainTokenStore store;
            store.read(QStringLiteral("selftest/a"), [&](bool, const QString &) { ++calls; });
            store.remove(QStringLiteral("selftest/a"), [&](bool) { ++calls; });
            QCoreApplication::processEvents();
        }
        QTest::qWait(1000);
        QCOMPARE(calls, 0);
    }
};
QTEST_GUILESS_MAIN(TestKeychainTokenStore)
#include "test_keychain_token_store.moc"
