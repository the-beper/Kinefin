// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QHostAddress>
#include <QNetworkInterface>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "FakeJellyfin.h"
#include "MemoryTokenStore.h"
#include "app/ConnectionController.h"
#include "settings/ServerCredentials.h"

using Kinefin::ConnectionController;
using State = Kinefin::ConnectionController::State;

namespace {
const QString kProfile = QStringLiteral("default");
const QString kDevice = QStringLiteral("test-device");

// The wallet of the current test: shared by every controller of it, like the user's KWallet across restarts
std::unique_ptr<MemoryTokenStore> gStore;

std::unique_ptr<ConnectionController> makeController()
{
    return std::make_unique<ConnectionController>(kProfile, kDevice, QStringLiteral("0.0.1"),
                                                  gStore.get());
}

QString address(const FakeJellyfin &server)
{
    return server.url(QStringLiteral("/jellyfin")).toString();
}
} // namespace

class TestConnectionController : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void init()
    {
        m_home = std::make_unique<QTemporaryDir>();
        QVERIFY(m_home->isValid());
        qputenv("XDG_DATA_HOME", m_home->path().toUtf8());
        gStore = std::make_unique<MemoryTokenStore>();
    }

    void cleanup() { gStore.reset(); }

    void startWithoutServersStaysIdle()
    {
        auto controller = makeController();
        controller->start();
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(controller->servers().isEmpty());
    }

    void connectSignInAndLibraries()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        controller->connectTo(address(server));
        QCOMPARE(controller->state(), ConnectionController::Connecting);
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QCOMPARE(controller->state(), ConnectionController::SigningIn);
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->servers().size(), 1);
        QVERIFY(controller->errorText().isEmpty());
        QCOMPARE(controller->servers().size(), 1);
        QTRY_COMPARE(controller->libraries().size(), 3);
        const QVariantMap first = controller->libraries().first().toMap();
        QCOMPARE(first.value(QStringLiteral("name")).toString(), QStringLiteral("Movies"));
        QCOMPARE(first.value(QStringLiteral("collectionType")).toString(),
                 QStringLiteral("movies"));
    }

    void librariesCarryTheImageAndWhetherTheyCanBeBrowsed()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        controller->setImageOrigin(origin);
        QVERIFY(origin->get().isEmpty());
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        // Images may be fetched from the server while signed in, and from nowhere else
        QCOMPARE(origin->get().host(), QStringLiteral("127.0.0.1"));
        const QVariantMap movies = controller->libraries().at(0).toMap();
        QVERIFY(movies.value(QStringLiteral("browsable")).toBool());
        QCOMPARE(movies.value(QStringLiteral("collectionType")).toString(),
                 QStringLiteral("movies"));
        QVERIFY(movies.value(QStringLiteral("imageUrl"))
                    .toString()
                    .contains(QStringLiteral("/Items/")
                              + movies.value(QStringLiteral("id")).toString()
                              + QStringLiteral("/Images/Primary")));
        // No image tag: no URL
        QVERIFY(controller->libraries()
                    .at(1)
                    .toMap()
                    .value(QStringLiteral("imageUrl"))
                    .toString()
                    .isEmpty());
        QVERIFY(!controller->libraries().at(2).toMap().value(QStringLiteral("browsable")).toBool());
        controller->cancel();
        QVERIFY(origin->get().isEmpty());
    }

    void openLibraryOnlyAcceptsBrowsableListedLibraries()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        QVERIFY(!controller->openLibrary(QStringLiteral("ffffffffffffffffffffffffffffffff")));
        // The music library is listed but not browsable
        QVERIFY(!controller->openLibrary(QStringLiteral("2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f")));
        QCOMPARE(controller->library()->status(), Kinefin::LibraryItemsModel::Idle);
        QVERIFY(controller->openLibrary(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01")));
        QTRY_COMPARE(controller->library()->status(), Kinefin::LibraryItemsModel::Ready);
        QCOMPARE(controller->library()->rowCount(), 5);
        // Leaving the session empties the model
        controller->cancel();
        QCOMPARE(controller->library()->status(), Kinefin::LibraryItemsModel::Idle);
        QCOMPARE(controller->library()->rowCount(), 0);
    }

    void aLibraryPageFailureKeepsErrorTextEmpty()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 250;
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        server.listingStatus.insert(QStringLiteral("Items"), 500);
        QVERIFY(controller->openLibrary(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01")));
        QTRY_COMPARE(controller->library()->status(), Kinefin::LibraryItemsModel::Failed);
        // The Home page's own error is about the libraries only
        QVERIFY(controller->errorText().isEmpty());
        QCOMPARE(controller->state(), ConnectionController::Authenticated);
    }

    void itemDetailsAreOwnedByTheirPage()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QString id = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");
        auto controller = makeController();
        QObject page;
        // Not signed in: nothing is created
        QVERIFY(controller->createItemDetails(id, &page) == nullptr);
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QVERIFY(controller->createItemDetails(QStringLiteral("bad id"), &page) == nullptr);
        QVERIFY(controller->createItemDetails(id, nullptr) == nullptr);
        QPointer<Kinefin::ItemDetailsModel> first = controller->createItemDetails(id, &page);
        QPointer<Kinefin::ItemDetailsModel> second = controller->createItemDetails(id, &page);
        QVERIFY(first && second);
        QCOMPARE(first->parent(), &page);
        QCOMPARE(first->itemId(), id);
        // Every page has its own listing channel
        QVERIFY(first.data() != second.data());
        QVERIFY(first->channel() != second->channel());
        QVERIFY(first->channel() >= Kinefin::ItemDetailsModel::FirstChannel);
        // The model lives exactly as long as its page
        auto *owner = new QObject;
        QPointer<Kinefin::ItemDetailsModel> owned = controller->createItemDetails(id, owner);
        QVERIFY(owned);
        delete owner;
        QVERIFY(!owned);
        QVERIFY(first);
    }

    void wrongPasswordReturnsToSignInWithAFixedMessage()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.denyLogin = true;
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("pw-canary-7f3"));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        QVERIFY(!controller->errorText().isEmpty());
        QVERIFY(!controller->errorText().contains(QStringLiteral("pw-canary-7f3")));
        QVERIFY(controller->servers().isEmpty());
        // An empty field never starts a request
        controller->login(QString(), QString());
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        QCOMPARE(server.successfulLogins, 0);
    }

    void anUnreachableOrInvalidAddressReturnsToIdle()
    {
        auto controller = makeController();
        controller->connectTo(QStringLiteral("http://127.0.0.1:1"));
        QTRY_VERIFY(controller->state() == ConnectionController::Idle);
        QVERIFY(!controller->errorText().isEmpty());
        controller->connectTo(QStringLiteral("http://user:pass@127.0.0.1:8096"));
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(!controller->errorText().isEmpty());
        QVERIFY(!controller->errorText().contains(QStringLiteral("pass")));
    }

    void aStoredSessionIsRestoredByStart()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        {
            auto first = makeController();
            first->connectTo(address(server));
            QTRY_VERIFY(first->state() == ConnectionController::NeedsLogin);
            first->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
            QTRY_VERIFY(first->state() == ConnectionController::Authenticated);
            QTRY_COMPARE(first->servers().size(), 1);
        }
        auto second = makeController();
        QCOMPARE(second->servers().size(), 1);
        second->start();
        QTRY_VERIFY(second->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(second->servers().size(), 1);
        QCOMPARE(server.successfulLogins, 1); // no password was asked for
        QTRY_COMPARE(second->libraries().size(), 3);
    }

    void aRevokedSessionAsksToSignInAgainOnTheSameServer()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        {
            auto first = makeController();
            first->connectTo(address(server));
            QTRY_VERIFY(first->state() == ConnectionController::NeedsLogin);
            first->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
            QTRY_VERIFY(first->state() == ConnectionController::Authenticated);
            QTRY_COMPARE(first->servers().size(), 1);
        }
        server.denyUsersMe = true;
        auto second = makeController();
        second->start();
        QTRY_VERIFY(second->state() == ConnectionController::NeedsLogin);
        QVERIFY(!second->errorText().isEmpty());
        QVERIFY(second->servers().isEmpty());
        QVERIFY(second->libraries().isEmpty());
        // Signing in again needs no new address
        server.denyUsersMe = false;
        second->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(second->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(second->servers().size(), 1);
    }

    void quickConnectSignsIn()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        controller->setQuickConnectPollInterval(std::chrono::milliseconds(50));
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->startQuickConnect();
        QCOMPARE(controller->state(), ConnectionController::QuickConnectPending);
        QTRY_VERIFY(!controller->quickConnectCode().isEmpty());
        QTest::qWait(150);
        QCOMPARE(controller->state(), ConnectionController::QuickConnectPending);
        server.approveQuickConnect = true;
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->servers().size(), 1);
        QVERIFY(controller->quickConnectCode().isEmpty());
    }

    void quickConnectCanBeCancelledOrBeUnavailable()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        controller->setQuickConnectPollInterval(std::chrono::milliseconds(50));
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->startQuickConnect();
        QTRY_VERIFY(!controller->quickConnectCode().isEmpty());
        controller->cancelQuickConnect();
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        QVERIFY(controller->quickConnectCode().isEmpty());
        QTest::qWait(150); // the stopped timer sends nothing
        const int polls = server.quickStatusRequests;
        QTest::qWait(150);
        QCOMPARE(server.quickStatusRequests, polls);

        server.disableQuickConnect = true;
        controller->startQuickConnect();
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        QVERIFY(!controller->errorText().isEmpty());
    }

    void switchingServerKeepsTheSessionButSignOutAndRemoveForgetIt()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->servers().size(), 1);
        QTRY_COMPARE(controller->libraries().size(), 3);

        controller->cancel();
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QCOMPARE(controller->servers().size(), 1);
        QVERIFY(controller->libraries().isEmpty());

        controller->start(); // restored without a password
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->servers().size(), 1);
        QCOMPARE(server.successfulLogins, 1);

        controller->signOut();
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(controller->servers().isEmpty());
        QVERIFY(Kinefin::loadServerCredentials(kProfile).isEmpty());

        // Removing a stored server that is not the active one
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->servers().size(), 1);
        controller->cancel();
        const QString serverId =
            controller->servers().first().toMap().value(QStringLiteral("serverId")).toString();
        controller->removeServer(serverId);
        QVERIFY(controller->servers().isEmpty());
        QVERIFY(Kinefin::loadServerCredentials(kProfile).isEmpty());
    }

    // The wallet may take as long as the user needs to unlock it: the controller keeps connecting, it neither shows the sign-in nor loses the session
    void aSlowWalletKeepsConnectingUntilItAnswers()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        {
            auto first = makeController();
            first->connectTo(address(server));
            QTRY_VERIFY(first->state() == ConnectionController::NeedsLogin);
            first->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
            QTRY_VERIFY(first->state() == ConnectionController::Authenticated);
            QTRY_COMPARE(first->servers().size(), 1);
        }
        gStore->hold = true;
        auto second = makeController();
        second->start();
        QTRY_COMPARE(gStore->reads, 1);
        QTest::qWait(300);
        QCOMPARE(second->state(), ConnectionController::Connecting);
        gStore->release();
        QTRY_VERIFY(second->state() == ConnectionController::Authenticated);
        QCOMPARE(server.successfulLogins, 1);
    }

    void aWalletThatCannotBeReadFallsBackToSignIn()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        {
            auto first = makeController();
            first->connectTo(address(server));
            QTRY_VERIFY(first->state() == ConnectionController::NeedsLogin);
            first->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
            QTRY_VERIFY(first->state() == ConnectionController::Authenticated);
            QTRY_COMPARE(first->servers().size(), 1);
        }
        gStore->failReads = true;
        auto second = makeController();
        second->start();
        QTRY_VERIFY(second->state() == ConnectionController::NeedsLogin);
        QCOMPARE(server.successfulLogins, 1);
        QCOMPARE(second->servers().size(), 1); // kept for the next try
    }

    void aWalletThatRefusesTheTokenIsReportedAndNothingIsKept()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        gStore->failWrites = true;
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_VERIFY(!controller->errorText().isEmpty());
        QVERIFY(controller->servers().isEmpty());
        QVERIFY(Kinefin::loadServerCredentials(kProfile).isEmpty());
    }

    // A later library load must not wipe the notice that the token could not be saved
    void aLibraryLoadKeepsAnUnrelatedError()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        gStore->failWrites = true;
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        QTRY_VERIFY(!controller->errorText().isEmpty());
        controller->reloadLibraries(); // a second, successful load
        QTRY_COMPARE(server.viewRequests, 2);
        QTest::qWait(100);
        QVERIFY(!controller->errorText().isEmpty());
    }

    void removingTheActiveServerSignsOut()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->servers().size(), 1);
        const QString serverId =
            controller->servers().first().toMap().value(QStringLiteral("serverId")).toString();
        controller->removeServer(serverId);
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(controller->servers().isEmpty());
        QVERIFY(Kinefin::loadServerCredentials(kProfile).isEmpty());
    }

    // Late replies of an abandoned request change nothing
    void cancelAndSignOutWhileARequestIsInFlightStayIdle()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        controller->connectTo(address(server));
        controller->cancel();
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QTest::qWait(300);
        QCOMPARE(controller->state(), ConnectionController::Idle);

        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        controller->signOut();
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QTest::qWait(300);
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(controller->servers().isEmpty());
        QVERIFY(Kinefin::loadServerCredentials(kProfile).isEmpty());
    }

    void aSessionLostMidwayAsksToSignInAgain()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        server.denyViews = true; // the library request after the login answers 401
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        QTRY_VERIFY(!controller->errorText().isEmpty());
        QVERIFY(controller->servers().isEmpty());
        QVERIFY(controller->libraries().isEmpty());
    }

    void anInvalidDeviceIdentityIsReportedNotSilent()
    {
        ConnectionController controller(kProfile, QStringLiteral("bad\"id"),
                                        QStringLiteral("0.0.1"), gStore.get());
        controller.connectTo(QStringLiteral("https://example.invalid"));
        QCOMPARE(controller.state(), ConnectionController::Idle);
        QVERIFY(!controller.errorText().isEmpty());
        QVERIFY(controller.errorText() != QStringLiteral("Enter a valid server address"));
    }

    // Plain HTTP to a non-loopback host needs the user's explicit opt-in before any credential is sent
    void plainHttpToAnotherHostNeedsConfirmation()
    {
        QHostAddress lan;
        for (const QHostAddress &candidate : QNetworkInterface::allAddresses()) {
            if (candidate.protocol() == QAbstractSocket::IPv4Protocol && !candidate.isLoopback()) {
                lan = candidate;
                break;
            }
        }
        if (lan.isNull())
            QSKIP("no non-loopback IPv4 address on this machine");
        FakeJellyfin server;
        QVERIFY(server.listen(QHostAddress::AnyIPv4));
        auto controller = makeController();
        controller->connectTo(
            QStringLiteral("http://%1:%2/jellyfin").arg(lan.toString()).arg(server.url().port()));
        QTRY_VERIFY(controller->state() == ConnectionController::ConfirmInsecure);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QCOMPARE(controller->state(), ConnectionController::ConfirmInsecure); // refused
        QCOMPARE(server.successfulLogins, 0);
        controller->confirmInsecureHttp();
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->servers().size(), 1);

        // A stored plain-HTTP session is not restored behind the user's back: the prompt comes back, and cancelling it returns to Idle with nothing sent
        controller->cancel();
        controller->connectTo(
            QStringLiteral("http://%1:%2/jellyfin").arg(lan.toString()).arg(server.url().port()));
        QTRY_VERIFY(controller->state() == ConnectionController::ConfirmInsecure);
        controller->cancel();
        QCOMPARE(controller->state(), ConnectionController::Idle);
    }

  private:
    std::unique_ptr<QTemporaryDir> m_home;
};

QTEST_GUILESS_MAIN(TestConnectionController)
#include "test_connection_controller.moc"
