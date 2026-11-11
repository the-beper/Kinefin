// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

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

const QString kMoviesA = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
const QString kMoviesB = QStringLiteral("b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2");
const QString kMoviesD = QStringLiteral("d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4");
const QString kShows = QStringLiteral("7f1b8f4b3e2148c5a8f2b7c9d0e1f2a3");

// A /UserViews body of libraries given as (id, collection type)
QByteArray viewsBody(const QList<QPair<QString, QString>> &libraries)
{
    QJsonArray items;
    for (const auto &[id, type] : libraries) {
        items.append(
            QJsonObject{{QStringLiteral("Name"), QString(QStringLiteral("Library ") + id.left(4))},
                        {QStringLiteral("Id"), id},
                        {QStringLiteral("CollectionType"), type}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                     {QStringLiteral("TotalRecordCount"), int(items.size())}})
        .toJson(QJsonDocument::Compact);
}

// Two movie libraries and one TV library
QByteArray twoMovieLibrariesBody()
{
    return viewsBody({{kMoviesA, QStringLiteral("movies")},
                      {kMoviesB, QStringLiteral("movies")},
                      {kShows, QStringLiteral("tvshows")}});
}

bool waitForLibraries(ConnectionController &controller, int count)
{
    return QTest::qWaitFor([&] { return controller.libraries().size() == count; }, 10000);
}

bool waitForState(ConnectionController &controller, State state)
{
    return QTest::qWaitFor([&] { return controller.state() == state; }, 10000);
}

// The ids of the system_info_public and authenticate_success fixtures
const QString kServerId = QStringLiteral("9c076c52ba5340d59f6baf5b57cf0a5e");
const QString kFixtureUserId = QStringLiteral("d5ed5d4e2e5447b58e9f2b6f7bd5c96a");

const QString kAnna = QStringLiteral("a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1");
const QString kBen = QStringLiteral("b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3");
const QString kCleo = QStringLiteral("c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5");
const QString kDan = QStringLiteral("d7d7d7d7d7d7d7d7d7d7d7d7d7d7d7d7");
const QString kAvatarTag = QStringLiteral("0123456789abcdef0123456789abcdef");

QString tokenKeyOf(const QString &userId)
{
    return Kinefin::serverTokenKey(kProfile, kServerId, userId);
}

// Anna has an avatar, Ben has none
void addTwoUsers(FakeJellyfin &server)
{
    server.users = {{kAnna, "annatoken", QStringLiteral("Anna"), kAvatarTag, false},
                    {kBen, "bentoken", QStringLiteral("Ben"), {}, false}};
}

// The `users` entry of one user, empty when it is not listed
QVariantMap userEntry(const ConnectionController &controller, const QString &userId)
{
    for (const QVariant &entry : controller.users()) {
        if (entry.toMap().value(QStringLiteral("id")).toString() == userId)
            return entry.toMap();
    }
    return {};
}

// The user listed first is the most recently used one
QString firstUserId(const ConnectionController &controller)
{
    return controller.users().value(0).toMap().value(QStringLiteral("id")).toString();
}

// Signs `name` in on the sign-in page and waits until the profile lists `usersAfter` saved users (the record is written once the token is)
bool signInUser(ConnectionController &controller, const QString &name, int usersAfter)
{
    controller.login(name, QStringLiteral("fixture-password"));
    return waitForState(controller, ConnectionController::Authenticated)
           && QTest::qWaitFor([&] { return controller.users().size() == usersAfter; }, 10000);
}

// Saves Anna, then Ben through Add User, so Ben is the most recently used; `server` needs the user table of addTwoUsers
bool saveBothUsers(FakeJellyfin &server)
{
    auto controller = makeController();
    controller->connectTo(address(server));
    if (!waitForState(*controller, ConnectionController::NeedsLogin)
        || !signInUser(*controller, QStringLiteral("Anna"), 1))
        return false;
    controller->addUser();
    return controller->state() == ConnectionController::NeedsLogin
           && signInUser(*controller, QStringLiteral("Ben"), 2);
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

    void libraryNamesAreOneLineOfText()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QJsonObject view{
            {QStringLiteral("Id"), QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01")},
            {QStringLiteral("Name"), QString::fromUtf8("Movies\nSign Out\t \u202eevil  \u2028x")},
            {QStringLiteral("CollectionType"), QStringLiteral("movies")}};
        server.viewsRawBody = QJsonDocument(QJsonObject{{QStringLiteral("Items"), QJsonArray{view}},
                                                        {QStringLiteral("TotalRecordCount"), 1}})
                                  .toJson(QJsonDocument::Compact);
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 1);
        QCOMPARE(
            controller->libraries().constFirst().toMap().value(QStringLiteral("name")).toString(),
            QStringLiteral("Movies Sign Out evil x"));
    }

    // The queue file of one user of the test profile (the fixture user by default)
    QString queueFile(const QString &userId = kFixtureUserId) const
    {
        return m_home->filePath(QStringLiteral("kinefin/profiles/") + kProfile
                                + QStringLiteral("/queues/") + kServerId.toLower()
                                + QLatin1Char('_') + userId.toLower() + QStringLiteral(".json"));
    }

    // Signs `username` (user `userId`) in against `server` and queues the fixture movie; the queue is saved by the time this returns true
    bool signInAndQueueTheMovie(ConnectionController &controller, FakeJellyfin &server,
                                const QString &username = QStringLiteral("fixture-user"),
                                const QString &userId = kFixtureUserId)
    {
        const QString movie = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");
        server.itemsById.insert(
            movie, QJsonDocument(QJsonObject{{QStringLiteral("Id"), movie},
                                             {QStringLiteral("Name"), QStringLiteral("Arcturus")},
                                             {QStringLiteral("Type"), QStringLiteral("Movie")}})
                       .toJson(QJsonDocument::Compact));
        controller.connectTo(address(server));
        if (!QTest::qWaitFor([&] { return controller.state() == ConnectionController::NeedsLogin; },
                             10000))
            return false;
        controller.login(username, QStringLiteral("fixture-password"));
        if (!QTest::qWaitFor(
                [&] { return controller.state() == ConnectionController::Authenticated; }, 10000))
            return false;
        controller.queue()->setSaveDelay(std::chrono::milliseconds(20));
        QSignalSpy added(controller.queue(), &Kinefin::PlayQueue::added);
        if (!controller.queue()->add(movie, Kinefin::PlayQueue::Last))
            return false;
        return QTest::qWaitFor([&] { return added.size() == 1; }, 10000)
               && QTest::qWaitFor([&] { return QFileInfo::exists(queueFile(userId)); }, 10000);
    }

    // Connects to a server the profile already signed in to: the stored session restores itself, otherwise the fixture user signs in
    bool signInAgain(ConnectionController &controller, FakeJellyfin &server)
    {
        controller.connectTo(address(server));
        if (!QTest::qWaitFor(
                [&] {
                    return controller.state() == ConnectionController::Authenticated
                           || controller.state() == ConnectionController::NeedsLogin;
                },
                10000))
            return false;
        if (controller.state() == ConnectionController::NeedsLogin)
            controller.login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        return QTest::qWaitFor(
            [&] { return controller.state() == ConnectionController::Authenticated; }, 10000);
    }

    void theQueueComesBackAtTheNextSignIn()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        {
            auto controller = makeController();
            QVERIFY(signInAndQueueTheMovie(*controller, server));
        }
        auto again = makeController();
        QVERIFY(signInAgain(*again, server));
        QTRY_COMPARE(again->queue()->count(), 1);
        QCOMPARE(
            again->queue()->data(again->queue()->index(0), Kinefin::PlayQueue::NameRole).toString(),
            QStringLiteral("Arcturus"));
    }

    void switchingServerKeepsTheQueueFileAndSigningOutRemovesIt()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        QVERIFY(signInAndQueueTheMovie(*controller, server));
        controller->cancel();
        QCOMPARE(controller->queue()->count(), 0);
        QVERIFY(QFileInfo::exists(queueFile()));
        // Back in, then out: the file goes with the account
        QVERIFY(signInAgain(*controller, server));
        QTRY_COMPARE(controller->queue()->count(), 1);
        controller->signOut();
        QCOMPARE(controller->queue()->count(), 0);
        QVERIFY(!QFileInfo::exists(queueFile()));
    }

    void switchUserKeepsTokenAndQueue()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        QVERIFY(signInAndQueueTheMovie(*controller, server));
        QCOMPARE(gStore->tokens.size(), 1);
        controller->switchUser();
        // The picker of the same server is next; the token, the record and the saved queue of the account stay
        QCOMPARE(controller->state(), ConnectionController::ChoosingUser);
        QCOMPARE(controller->queue()->count(), 0);
        QCOMPARE(gStore->tokens.size(), 1);
        QCOMPARE(controller->servers().size(), 1);
        QCOMPARE(controller->users().size(), 1);
        QVERIFY(QFileInfo::exists(queueFile()));
        // The same account comes back without a password, queue included
        controller->chooseUser(kFixtureUserId);
        QCOMPARE(controller->state(), ConnectionController::Connecting);
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QCOMPARE(server.successfulLogins, 1);
        QTRY_COMPARE(controller->queue()->count(), 1);
    }

    void removingAServerRemovesItsQueueFile()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        QVERIFY(signInAndQueueTheMovie(*controller, server));
        controller->cancel();
        QVERIFY(QFileInfo::exists(queueFile()));
        QTRY_VERIFY(!controller->servers().isEmpty());
        controller->removeServer(controller->servers()
                                     .constFirst()
                                     .toMap()
                                     .value(QStringLiteral("serverId"))
                                     .toString());
        QVERIFY(!QFileInfo::exists(queueFile()));
    }

    void genreRailsFollowTheOpenLibrary()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        auto controller = makeController();
        QVERIFY(!controller->openGenreRails(controller->library()));
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        QVERIFY(controller->openLibrary(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01")));
        QVERIFY(!controller->openGenreRails(nullptr));
        QVERIFY(controller->openGenreRails(controller->library()));
        QTRY_COMPARE(controller->genreRails()->status(), Kinefin::GenreRailsModel::Ready);
        QCOMPARE(controller->genreRails()->rowCount(), 3);
        // Another library, or the end of the session, drops the rails
        QVERIFY(controller->openLibrary(QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01")));
        QCOMPARE(controller->genreRails()->rowCount(), 0);
        QCOMPARE(controller->genreRails()->status(), Kinefin::GenreRailsModel::Idle);
        QVERIFY(controller->openGenreRails(controller->library()));
        QTRY_COMPARE(controller->genreRails()->status(), Kinefin::GenreRailsModel::Ready);
        controller->cancel();
        QCOMPARE(controller->genreRails()->rowCount(), 0);
    }

    // The browse state file of the test profile
    QString browseFile() const
    {
        return m_home->filePath(QStringLiteral("kinefin/profiles/") + kProfile
                                + QStringLiteral("/browse.json"));
    }

    bool signInWithLibraries(ConnectionController &controller, FakeJellyfin &server, int libraries)
    {
        return signInAgain(controller, server) && waitForLibraries(controller, libraries);
    }

    void typeLibrariesFollowViews()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        auto controller = makeController();
        QVERIFY(!controller->hasType(QStringLiteral("movies")));
        QVERIFY(!controller->openType(QStringLiteral("movies"))); // signed out
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        QCOMPARE(controller->libraries().at(0).toMap().value(QStringLiteral("tab")).toString(),
                 QStringLiteral("movies"));
        QCOMPARE(controller->libraries().at(1).toMap().value(QStringLiteral("tab")).toString(),
                 QStringLiteral("tvshows"));
        QVERIFY(controller->libraries()
                    .at(2)
                    .toMap()
                    .value(QStringLiteral("tab"))
                    .toString()
                    .isEmpty());
        QCOMPARE(controller->movieLibraries().size(), 1);
        QCOMPARE(controller->showLibraries().size(), 1);
        QVERIFY(controller->hasType(QStringLiteral("movies")));
        QVERIFY(controller->hasType(QStringLiteral("tvshows")));
        QVERIFY(!controller->hasType(QStringLiteral("music")));
        QVERIFY(!controller->hasType(QString()));
        QVERIFY(!controller->openType(QStringLiteral("music")));
        QVERIFY(!controller->openType(QString()));

        QVERIFY(controller->openType(QStringLiteral("movies")));
        QVERIFY(controller->openType(QStringLiteral("tvshows")));
        QVERIFY(controller->movies()->isOpen());
        QVERIFY(controller->shows()->isOpen());
        QCOMPARE(controller->movies()->collectionType(), QStringLiteral("movies"));
        QCOMPARE(controller->shows()->collectionType(), QStringLiteral("tvshows"));
        // The one library of a type is never a scope: all of them are listed, with no parent
        QVERIFY(controller->movies()->libraryId().isEmpty());
        QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesA));
        QVERIFY(controller->movies()->libraryId().isEmpty());
        QTRY_COMPARE(controller->movies()->status(), Kinefin::LibraryItemsModel::Ready);
        QTRY_COMPARE(controller->shows()->status(), Kinefin::LibraryItemsModel::Ready);

        // A type with no library left closes its tab and empties its model
        server.viewsRawBody = viewsBody({{kShows, QStringLiteral("tvshows")}});
        QSignalSpy changed(controller.get(), &ConnectionController::librariesChanged);
        controller->reloadLibraries();
        QVERIFY(changed.wait(5000));
        QVERIFY(!controller->hasType(QStringLiteral("movies")));
        QVERIFY(controller->movieLibraries().isEmpty());
        QVERIFY(!controller->movies()->isOpen());
        QCOMPARE(controller->movies()->rowCount(), 0);
        QVERIFY(controller->shows()->isOpen());
        QVERIFY(!controller->openType(QStringLiteral("movies")));
    }

    void openTypeRestoresSelectionAndView()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        {
            auto controller = makeController();
            controller->connectTo(address(server));
            QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
            controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
            QTRY_COMPARE(controller->libraries().size(), 3);
            // The first time: all libraries, the default order
            QVERIFY(controller->openType(QStringLiteral("movies")));
            QVERIFY(controller->movies()->libraryId().isEmpty());
            QCOMPARE(controller->movies()->sortKey(), Kinefin::LibraryItemsModel::Name);
            controller->movies()->setSortKey(Kinefin::LibraryItemsModel::DateAdded);

            QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
            QCOMPARE(controller->movies()->libraryId(), kMoviesB);
            QVERIFY(controller->movies()->libraryName().startsWith(QStringLiteral("Library")));
            QCOMPARE(controller->movies()->sortKey(), Kinefin::LibraryItemsModel::Name);
            controller->movies()->setSortKey(Kinefin::LibraryItemsModel::DateAdded);
            controller->movies()->setDescending(true);
            controller->movies()->setFilter(Kinefin::LibraryItemsModel::Unplayed);

            QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesA));
            QCOMPARE(controller->movies()->libraryId(), kMoviesA);
            controller->movies()->setSortKey(Kinefin::LibraryItemsModel::Runtime);
            // The TV tab has its own scope
            QVERIFY(controller->openType(QStringLiteral("tvshows")));
            QVERIFY(controller->shows()->libraryId().isEmpty());
            QCOMPARE(controller->shows()->sortKey(), Kinefin::LibraryItemsModel::Name);
        } // the pending write goes out with the controller
        QVERIFY(QFileInfo::exists(browseFile()));

        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 3));
        // The library selected last comes back with its own view
        QVERIFY(again->openType(QStringLiteral("movies")));
        QCOMPARE(again->movies()->libraryId(), kMoviesA);
        QCOMPARE(again->movies()->sortKey(), Kinefin::LibraryItemsModel::Runtime);
        QCOMPARE(again->movies()->filter(), Kinefin::LibraryItemsModel::All);
        QVERIFY(again->openType(QStringLiteral("movies"), kMoviesB));
        QCOMPARE(again->movies()->sortKey(), Kinefin::LibraryItemsModel::DateAdded);
        QVERIFY(again->movies()->descending());
        QCOMPARE(again->movies()->filter(), Kinefin::LibraryItemsModel::Unplayed);
        // An id that is no library of the type selects all of them
        QVERIFY(again->openType(QStringLiteral("movies"), QStringLiteral("*")));
        QVERIFY(again->movies()->libraryId().isEmpty());
        QCOMPARE(again->movies()->sortKey(), Kinefin::LibraryItemsModel::DateAdded);
        QVERIFY(!again->movies()->descending());
        // ...and is what an empty id restores next
        QVERIFY(again->openType(QStringLiteral("tvshows")));
        QVERIFY(again->openType(QStringLiteral("movies")));
        QVERIFY(again->movies()->libraryId().isEmpty());
    }

    void vanishedSelectionFallsBackToAll()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
        QCOMPARE(controller->movies()->libraryId(), kMoviesB);

        // The selected library goes, another one is listed: back to all libraries
        server.viewsRawBody = viewsBody({{kMoviesA, QStringLiteral("movies")},
                                         {kMoviesD, QStringLiteral("movies")},
                                         {kShows, QStringLiteral("tvshows")}});
        {
            QSignalSpy changed(controller.get(), &ConnectionController::librariesChanged);
            controller->reloadLibraries();
            QVERIFY(changed.wait(5000));
        }
        QVERIFY(controller->movies()->isOpen());
        QVERIFY(controller->movies()->libraryId().isEmpty());
        QTRY_COMPARE(controller->movies()->status(), Kinefin::LibraryItemsModel::Ready);
        // The stored selection that no longer exists reads as all libraries too
        QVERIFY(controller->openType(QStringLiteral("movies")));
        QVERIFY(controller->movies()->libraryId().isEmpty());

        // One library left: it is no scope
        QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesD));
        QCOMPARE(controller->movies()->libraryId(), kMoviesD);
        server.viewsRawBody = viewsBody({{kMoviesA, QStringLiteral("movies")}});
        {
            QSignalSpy changed(controller.get(), &ConnectionController::librariesChanged);
            controller->reloadLibraries();
            QVERIFY(changed.wait(5000));
        }
        QVERIFY(controller->movies()->libraryId().isEmpty());
        QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesA));
        QVERIFY(controller->movies()->libraryId().isEmpty());
        QVERIFY(!controller->hasType(QStringLiteral("tvshows")));
    }

    void sameScopeNoReload()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        QVERIFY(controller->openType(QStringLiteral("movies")));
        QTRY_COMPARE(controller->movies()->status(), Kinefin::LibraryItemsModel::Ready);
        QSignalSpy reopened(controller->movies(), &Kinefin::LibraryItemsModel::libraryChanged);
        QVERIFY(controller->openType(QStringLiteral("movies")));
        QVERIFY(controller->openType(QStringLiteral("movies"), QStringLiteral("*")));
        // The same libraries listed again leave the scope alone
        {
            QSignalSpy changed(controller.get(), &ConnectionController::librariesChanged);
            controller->reloadLibraries();
            QVERIFY(changed.wait(5000));
        }
        QTest::qWait(100);
        QCOMPARE(reopened.size(), 0);
        QCOMPARE(controller->movies()->status(), Kinefin::LibraryItemsModel::Ready);
        // Another scope does: the model closes the old one and opens the new one
        QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
        QCOMPARE(reopened.size(), 2);
        QTRY_VERIFY(std::any_of(
            server.itemsRoutes.cbegin(), server.itemsRoutes.cend(),
            [](const QByteArray &route) { return route.contains(kMoviesB.toLatin1()); }));

        // The genre rails of a tab over all libraries have no parent either
        QVERIFY(controller->openType(QStringLiteral("movies"), QStringLiteral("*")));
        QVERIFY(!controller->openGenreRails(controller->shows())); // not open
        QVERIFY(controller->openGenreRails(controller->movies()));
        QTRY_COMPARE(controller->genreRails()->status(), Kinefin::GenreRailsModel::Ready);
        QVERIFY(
            !server.listingRoutes.value(QStringLiteral("Genres")).constLast().contains("parentId"));
    }

    void sessionLossClearsTheTabModelsAndTheTerm()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        QVERIFY(controller->openType(QStringLiteral("movies")));
        QVERIFY(controller->openType(QStringLiteral("tvshows")));
        controller->movies()->setSearchTerm(QStringLiteral("alien"));
        QCOMPARE(controller->movies()->searchTerm(), QStringLiteral("alien"));
        controller->switchUser();
        QVERIFY(!controller->movies()->isOpen());
        QVERIFY(!controller->shows()->isOpen());
        QVERIFY(controller->movies()->searchTerm().isEmpty());
        QCOMPARE(controller->movies()->rowCount(), 0);
        QVERIFY(controller->movieLibraries().isEmpty());
        QVERIFY(!controller->openType(QStringLiteral("movies")));
    }

    QByteArray browseBytes() const
    {
        QFile file(browseFile());
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    void recentlyAddedKeepsSavedSort()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
            controller->movies()->setSortKey(Model::Runtime);
        }
        const QByteArray before = browseBytes();
        QVERIFY(!before.isEmpty());
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            // The selection stored for the type picks the library
            QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies")));
            QCOMPARE(controller->movies()->libraryId(), kMoviesB);
            QCOMPARE(controller->movies()->sortKey(), Model::DateAdded);
            QVERIFY(controller->movies()->descending());
            QCOMPARE(controller->movies()->filter(), Model::All);
            QVERIFY(controller->movies()->genreIds().isEmpty());
            QTRY_COMPARE(controller->movies()->status(), Model::Ready);
            QTRY_VERIFY(
                std::any_of(server.itemsRoutes.cbegin(), server.itemsRoutes.cend(),
                            [](const QByteArray &route) { return route.contains("DateCreated"); }));
            controller->signOut(); // flushes what is pending
        }
        QCOMPARE(browseBytes(), before);

        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 3));
        QVERIFY(again->openType(QStringLiteral("movies")));
        QCOMPARE(again->movies()->libraryId(), kMoviesB);
        QCOMPARE(again->movies()->sortKey(), Model::Runtime);
        QVERIFY(!again->movies()->descending());
    }

    void recentlyAddedReopensSameScope()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        auto controller = makeController();
        QVERIFY(signInWithLibraries(*controller, server, 3));
        QVERIFY(controller->openType(QStringLiteral("tvshows")));
        QVERIFY(controller->openRecentlyAdded(QStringLiteral("tvshows")));
        QVERIFY(controller->shows()->libraryId().isEmpty());
        QVERIFY(!controller->openRecentlyAdded(QStringLiteral("music")));
        QVERIFY(controller->openType(QStringLiteral("movies")));
        controller->movies()->setFilter(Model::Unplayed);
        QTRY_COMPARE(controller->movies()->status(), Model::Ready);

        QSignalSpy reopened(controller->movies(), &Model::libraryChanged);
        QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies")));
        QVERIFY(!reopened.isEmpty());
        QCOMPARE(controller->movies()->sortKey(), Model::DateAdded);
        QVERIFY(controller->movies()->descending());
        QCOMPARE(controller->movies()->filter(), Model::All);
        QTRY_COMPARE(controller->movies()->status(), Model::Ready);
        // Asking again reloads the same scope too
        reopened.clear();
        QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies")));
        QVERIFY(!reopened.isEmpty());
        QCOMPARE(controller->movies()->sortKey(), Model::DateAdded);
    }

    void oneOffEndsOnOpenType()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        auto controller = makeController();
        QVERIFY(signInWithLibraries(*controller, server, 3));
        QVERIFY(controller->openType(QStringLiteral("movies")));
        controller->movies()->setSortKey(Model::Runtime);
        controller->movies()->setFilter(Model::Unplayed);

        // The same scope is reopened with the stored view
        QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies")));
        QCOMPARE(controller->movies()->sortKey(), Model::DateAdded);
        QVERIFY(controller->openType(QStringLiteral("movies")));
        QCOMPARE(controller->movies()->sortKey(), Model::Runtime);
        QVERIFY(!controller->movies()->descending());
        QCOMPARE(controller->movies()->filter(), Model::Unplayed);
        // ...and with the one-off gone the same scope no longer reloads
        QSignalSpy reopened(controller->movies(), &Model::libraryChanged);
        QVERIFY(controller->openType(QStringLiteral("movies")));
        QCOMPARE(reopened.size(), 0);

        // Another scope ends it too
        QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies"), kMoviesA));
        QCOMPARE(controller->movies()->libraryId(), kMoviesA);
        QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
        QCOMPARE(controller->movies()->sortKey(), Model::Name);
        controller->movies()->setFilter(Model::Played);
        QCOMPARE(controller->movies()->sortKey(), Model::Name);
        // The session ending drops it as well: the next sign-in opens the stored view
        QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies"), kMoviesB));
        controller->switchUser();
        QVERIFY(!controller->movies()->isOpen());
        QCOMPARE(controller->state(), ConnectionController::ChoosingUser);
        controller->chooseUser(kFixtureUserId);
        QVERIFY(waitForLibraries(*controller, 3));
        QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
        QCOMPARE(controller->movies()->sortKey(), Model::Name);
        QCOMPARE(controller->movies()->filter(), Model::Played);
    }

    void filterEditDuringOneOffKeepsStoredSort()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
            controller->movies()->setSortKey(Model::Runtime);
            controller->movies()->setDescending(true);
            QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies"), kMoviesB));
            controller->movies()->setFilter(Model::Unplayed);
            // The view on screen keeps the one-off order
            QCOMPARE(controller->movies()->sortKey(), Model::DateAdded);
            QVERIFY(controller->movies()->descending());
            // A scope with nothing stored falls back to the name order
            QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies"), QStringLiteral("*")));
            controller->movies()->setFilter(Model::Played);
            QCOMPARE(controller->movies()->sortKey(), Model::DateAdded);
        }
        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 3));
        QVERIFY(again->openType(QStringLiteral("movies"), kMoviesB));
        QCOMPARE(again->movies()->sortKey(), Model::Runtime);
        QVERIFY(again->movies()->descending());
        QCOMPARE(again->movies()->filter(), Model::Unplayed);
        QVERIFY(again->openType(QStringLiteral("movies"), QStringLiteral("*")));
        QCOMPARE(again->movies()->sortKey(), Model::Name);
        QVERIFY(!again->movies()->descending());
        QCOMPARE(again->movies()->filter(), Model::Played);
    }

    void sortEditDuringOneOffIsSaved()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            QVERIFY(controller->openType(QStringLiteral("movies")));
            QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies")));
            controller->movies()->setSortKey(Model::Runtime);
            // The user's pick ended the one-off: the same scope is not reloaded and edits are saved whole
            QSignalSpy reopened(controller->movies(), &Model::libraryChanged);
            QVERIFY(controller->openType(QStringLiteral("movies")));
            QCOMPARE(reopened.size(), 0);
            controller->movies()->setFilter(Model::Unplayed);
            QCOMPARE(controller->movies()->sortKey(), Model::Runtime);
        }
        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 3));
        QVERIFY(again->openType(QStringLiteral("movies")));
        QCOMPARE(again->movies()->sortKey(), Model::Runtime);
        QVERIFY(again->movies()->descending());
        QCOMPARE(again->movies()->filter(), Model::Unplayed);
    }

    void recentlyAddedClearsSearchTerm()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        auto controller = makeController();
        QVERIFY(signInWithLibraries(*controller, server, 3));
        QVERIFY(controller->openType(QStringLiteral("movies")));
        controller->movies()->setSearchTerm(QStringLiteral("alien"));
        QCOMPARE(controller->movies()->searchTerm(), QStringLiteral("alien"));
        QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies")));
        QVERIFY(controller->movies()->searchTerm().isEmpty());
        QVERIFY(!controller->movies()->filtered());
        QCOMPARE(controller->movies()->sortKey(), Model::DateAdded);
    }

    void signOutKeepsTheBrowseStateAndRemoveServerDeletesIt()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        auto controller = makeController();
        QVERIFY(signInWithLibraries(*controller, server, 3));
        QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
        controller->movies()->setSortKey(Kinefin::LibraryItemsModel::Runtime);
        QVERIFY(!QFileInfo::exists(browseFile())); // the write is still pending
        // The end of a session writes what is pending and keeps it
        controller->signOut();
        QVERIFY(QFileInfo::exists(browseFile()));
        QFile file(browseFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QCOMPARE(root.value(QStringLiteral("servers")).toObject().size(), 1);

        QVERIFY(signInWithLibraries(*controller, server, 3));
        controller->cancel();
        QVERIFY(QFileInfo::exists(browseFile()));
        QCOMPARE(controller->servers().size(), 1);
        controller->removeServer(controller->servers()
                                     .constFirst()
                                     .toMap()
                                     .value(QStringLiteral("serverId"))
                                     .toString());
        QVERIFY(!QFileInfo::exists(browseFile()));
    }

    void aCollectionKeepsItsView()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        const QString boxsets = QStringLiteral("c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3");
        server.viewsRawBody = viewsBody({{boxsets, QStringLiteral("boxsets")}});
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 1));
            QVERIFY(controller->openLibrary(boxsets));
            controller->library()->setSortKey(Kinefin::LibraryItemsModel::Runtime);
        }
        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 1));
        QVERIFY(again->openLibrary(boxsets));
        QCOMPARE(again->library()->sortKey(), Kinefin::LibraryItemsModel::Runtime);
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
        QVERIFY(!second->loginUserName().isEmpty());
        // The record stays (only the token is forgotten), so the server is still listed
        QCOMPARE(second->servers().size(), 1);
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
        QVERIFY(!second->loginUserName().isEmpty());
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
        // The record may stay (a user whose token was rejected is asked for the password), but never a token
        QTRY_VERIFY(gStore->tokens.isEmpty());
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

    void startWithTwoUsersShowsPicker()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        gStore->reads = 0;
        auto controller = makeController();
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        controller->setImageOrigin(origin);
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        // The picker is drawn from the profile's records: the wallet is not asked, and the avatars may load
        QCOMPARE(gStore->reads, 0);
        QCOMPARE(origin->get().host(), QStringLiteral("127.0.0.1"));
        QVERIFY(controller->hasSavedUsers());
        QVERIFY(controller->loginUserName().isEmpty());
        QVERIFY(controller->errorText().isEmpty());
        QCOMPARE(controller->servers().size(), 1); // one entry per server, not per user
        QCOMPARE(controller->users().size(), 2);
        const QVariantMap ben = controller->users().at(0).toMap();
        const QVariantMap anna = controller->users().at(1).toMap();
        // The map carries no token and nothing else
        QCOMPARE(ben.keys(), QStringList({QStringLiteral("askPassword"), QStringLiteral("id"),
                                          QStringLiteral("imageUrl"), QStringLiteral("name"),
                                          QStringLiteral("remembered")}));
        QCOMPARE(ben.value(QStringLiteral("id")).toString(), kBen);
        QCOMPARE(ben.value(QStringLiteral("name")).toString(), QStringLiteral("Ben"));
        QVERIFY(ben.value(QStringLiteral("imageUrl")).toUrl().isEmpty());
        QVERIFY(ben.value(QStringLiteral("remembered")).toBool());
        QVERIFY(!ben.value(QStringLiteral("askPassword")).toBool());
        QCOMPARE(anna.value(QStringLiteral("id")).toString(), kAnna);
        QCOMPARE(anna.value(QStringLiteral("name")).toString(), QStringLiteral("Anna"));
        const QString avatar = anna.value(QStringLiteral("imageUrl")).toUrl().toString();
        QVERIFY(avatar.contains(QStringLiteral("/UserImage?")));
        QVERIFY(avatar.contains(kAvatarTag));
        QVERIFY(!avatar.contains(QStringLiteral("token")));
        QCOMPARE(server.successfulLogins, 2);
        // Leaving the picker for another server stops the avatars
        controller->cancel();
        QVERIFY(origin->get().isEmpty());
        QVERIFY(controller->users().isEmpty());
    }

    void startWithOneUserRestoresIt()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        {
            auto first = makeController();
            first->connectTo(address(server));
            QVERIFY(waitForState(*first, ConnectionController::NeedsLogin));
            QVERIFY(signInUser(*first, QStringLiteral("Anna"), 1));
        }
        auto controller = makeController();
        controller->start();
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QCOMPARE(server.successfulLogins, 1); // no password was asked for
        QCOMPARE(controller->users().size(), 1);
        QCOMPARE(firstUserId(*controller), kAnna);
        QTRY_COMPARE(controller->libraries().size(), 3);
    }

    void chooseUserRestoresWithoutPassword()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        const int logins = server.successfulLogins;
        const int checks = server.usersMeRequests;
        auto controller = makeController();
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        controller->chooseUser(kAnna);
        QCOMPARE(controller->state(), ConnectionController::Connecting);
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(server.usersMeRequests, checks + 1);
        QTest::qWait(100);
        QCOMPARE(server.usersMeRequests, checks + 1); // exactly one check of the session
        QCOMPARE(server.successfulLogins, logins);
        QVERIFY(controller->errorText().isEmpty());
        QTRY_COMPARE(controller->libraries().size(), 3);
        // The chosen user is now the most recently used
        QTRY_COMPARE(firstUserId(*controller), kAnna);
    }

    void revokedTokenAsksThatUsersPassword()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        server.deniedUsersMeTokens.insert("annatoken");
        auto controller = makeController();
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        controller->chooseUser(kAnna);
        QTRY_COMPARE(controller->state(), ConnectionController::NeedsLogin);
        QCOMPARE(controller->loginUserName(), QStringLiteral("Anna"));
        QVERIFY(!controller->errorText().isEmpty());
        QVERIFY(!controller->errorText().contains(QStringLiteral("Anna")));
        QVERIFY(controller->libraries().isEmpty());
        // Only that user's token went; both records stay
        QTRY_VERIFY(!gStore->tokens.contains(tokenKeyOf(kAnna)));
        QVERIFY(gStore->tokens.contains(tokenKeyOf(kBen)));
        QCOMPARE(controller->users().size(), 2);
        QVERIFY(!userEntry(*controller, kAnna).value(QStringLiteral("remembered")).toBool());
        QVERIFY(userEntry(*controller, kBen).value(QStringLiteral("remembered")).toBool());
        // The password signs the user back in
        controller->login(QStringLiteral("Anna"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QVERIFY(controller->loginUserName().isEmpty());
    }

    void walletReadFailureAsksPassword()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        const int logins = server.successfulLogins;
        auto controller = makeController();
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        gStore->failReads = true;
        controller->chooseUser(kBen);
        QTRY_COMPARE(controller->state(), ConnectionController::NeedsLogin);
        QCOMPARE(controller->loginUserName(), QStringLiteral("Ben"));
        QVERIFY(!controller->errorText().isEmpty());
        QVERIFY(!controller->errorText().contains(QStringLiteral("Ben")));
        QCOMPARE(server.successfulLogins, logins);
        // The records are kept for the next try
        QCOMPARE(controller->users().size(), 2);
        QVERIFY(userEntry(*controller, kBen).value(QStringLiteral("remembered")).toBool());
    }

    void signOutWithOthersLandsOnPicker()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        auto controller = makeController();
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        controller->chooseUser(kBen);
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->libraries().size(), 3);
        controller->signOut();
        QCOMPARE(controller->state(), ConnectionController::ChoosingUser);
        QVERIFY(controller->libraries().isEmpty());
        QCOMPARE(controller->servers().size(), 1);
        QCOMPARE(controller->users().size(), 1);
        QCOMPARE(firstUserId(*controller), kAnna);
        QTRY_VERIFY(!gStore->tokens.contains(tokenKeyOf(kBen)));
        QVERIFY(gStore->tokens.contains(tokenKeyOf(kAnna)));
        // The other user restores without a password
        controller->chooseUser(kAnna);
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QCOMPARE(server.successfulLogins, 2);
    }

    void signOutOfLastUserReturnsToServers()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->users().size(), 1);
        controller->signOut();
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(controller->servers().isEmpty());
        QVERIFY(controller->users().isEmpty());
        QVERIFY(!controller->hasSavedUsers());
        QTRY_VERIFY(gStore->tokens.isEmpty());
        QVERIFY(Kinefin::loadServerCredentials(kProfile).isEmpty());
    }

    void removeUserDeletesQueueAndToken()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        auto controller = makeController();
        QVERIFY(signInAndQueueTheMovie(*controller, server, QStringLiteral("Anna"), kAnna));
        controller->addUser();
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        QVERIFY(signInUser(*controller, QStringLiteral("Ben"), 2));
        controller->switchUser();
        QCOMPARE(controller->state(), ConnectionController::ChoosingUser);
        QVERIFY(QFileInfo::exists(queueFile(kAnna))); // switching user keeps it
        controller->removeUser(kAnna);
        QCOMPARE(controller->state(), ConnectionController::ChoosingUser);
        QCOMPARE(controller->users().size(), 1);
        QCOMPARE(firstUserId(*controller), kBen);
        QTRY_VERIFY(!QFileInfo::exists(queueFile(kAnna)));
        QTRY_VERIFY(!gStore->tokens.contains(tokenKeyOf(kAnna)));
        QVERIFY(gStore->tokens.contains(tokenKeyOf(kBen)));
        QCOMPARE(Kinefin::loadServerCredentials(kProfile).size(), 1);
        // Removing the last one leaves the server
        controller->removeUser(kBen);
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(controller->servers().isEmpty());
        QTRY_VERIFY(gStore->tokens.isEmpty());
    }

    void removeServerForgetsAllUsers()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        auto controller = makeController();
        QVERIFY(signInAndQueueTheMovie(*controller, server, QStringLiteral("Anna"), kAnna));
        controller->addUser();
        QVERIFY(signInUser(*controller, QStringLiteral("Ben"), 2));
        QCOMPARE(gStore->tokens.size(), 2);
        // The active server, signed in
        controller->removeServer(kServerId);
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(controller->servers().isEmpty());
        QVERIFY(controller->users().isEmpty());
        QTRY_VERIFY(gStore->tokens.isEmpty());
        QVERIFY(!QFileInfo::exists(queueFile(kAnna)));
        QVERIFY(Kinefin::loadServerCredentials(kProfile).isEmpty());
    }

    void removeStoredServerForgetsAllUsers()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        auto controller = makeController();
        QCOMPARE(controller->servers().size(), 1);
        controller->removeServer(kServerId);
        QVERIFY(controller->servers().isEmpty());
        QTRY_VERIFY(gStore->tokens.isEmpty());
        QVERIFY(Kinefin::loadServerCredentials(kProfile).isEmpty());
    }

    void chooseUserIgnoredWhileConnecting()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        auto controller = makeController();
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        gStore->reads = 0;
        gStore->hold = true;
        controller->chooseUser(kBen);
        QCOMPARE(controller->state(), ConnectionController::Connecting);
        QTRY_COMPARE(gStore->reads, 1);
        controller->chooseUser(kAnna);
        QCOMPARE(controller->state(), ConnectionController::Connecting);
        QTest::qWait(100);
        QCOMPARE(gStore->reads, 1);
        gStore->release();
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QCOMPARE(gStore->reads, 1);
        QTRY_COMPARE(firstUserId(*controller), kBen);
    }

    void chooseUserIgnoredWhileSigningIn()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        auto controller = makeController();
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        controller->addUser();
        gStore->reads = 0;
        controller->login(QStringLiteral("Ben"), QStringLiteral("fixture-password"));
        QCOMPARE(controller->state(), ConnectionController::SigningIn);
        controller->chooseUser(kAnna);
        QCOMPARE(controller->state(), ConnectionController::SigningIn);
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QCOMPARE(gStore->reads, 0);
        QTRY_COMPARE(firstUserId(*controller), kBen);
    }

    void chooseUserIgnoresAnUnknownUser()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        auto controller = makeController();
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        controller->chooseUser(kCleo);
        controller->chooseUser(QString());
        QCOMPARE(controller->state(), ConnectionController::ChoosingUser);
    }

    void addUserKeepsTheTokensAndStartsWithoutAName()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        {
            auto first = makeController();
            first->connectTo(address(server));
            QVERIFY(waitForState(*first, ConnectionController::NeedsLogin));
            QVERIFY(signInUser(*first, QStringLiteral("Anna"), 1));
        }
        auto controller = makeController();
        controller->start();
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->libraries().size(), 3);
        controller->addUser();
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        QVERIFY(controller->loginUserName().isEmpty());
        QVERIFY(controller->libraries().isEmpty());
        QVERIFY(controller->hasSavedUsers());
        QVERIFY(gStore->tokens.contains(tokenKeyOf(kAnna)));
        // The sign-in page can go back to the picker, and the picker to the sign-in
        controller->chooseAnotherUser();
        QCOMPARE(controller->state(), ConnectionController::ChoosingUser);
        controller->addUser();
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        QVERIFY(signInUser(*controller, QStringLiteral("Ben"), 2));
        QTRY_COMPARE(gStore->tokens.size(), 2);
        QCOMPARE(firstUserId(*controller), kBen);
    }

    void chooseAnotherUserNeedsSavedUsers()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->chooseAnotherUser();
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        QVERIFY(!controller->hasSavedUsers());
    }

    void askPasswordUserIsNeverRestoredOrStored()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        auto controller = makeController();
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        controller->setUserAskPassword(kBen, true);
        QVERIFY(userEntry(*controller, kBen).value(QStringLiteral("askPassword")).toBool());
        QVERIFY(!userEntry(*controller, kBen).value(QStringLiteral("remembered")).toBool());
        QTRY_VERIFY(!gStore->tokens.contains(tokenKeyOf(kBen)));
        QVERIFY(gStore->tokens.contains(tokenKeyOf(kAnna)));
        // The tile leads to the password prompt without reading the wallet
        gStore->reads = 0;
        controller->chooseUser(kBen);
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        QCOMPARE(controller->loginUserName(), QStringLiteral("Ben"));
        QCOMPARE(gStore->reads, 0);
        // Signing in stores no token
        const int writes = gStore->writes;
        controller->login(QStringLiteral("Ben"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTest::qWait(100);
        QCOMPARE(gStore->writes, writes);
        QVERIFY(!gStore->tokens.contains(tokenKeyOf(kBen)));
        QVERIFY(userEntry(*controller, kBen).value(QStringLiteral("askPassword")).toBool());
        // After a restart both are listed, Ben locked
        controller.reset();
        auto again = makeController();
        again->start();
        QTRY_COMPARE(again->state(), ConnectionController::ChoosingUser);
        QCOMPARE(again->users().size(), 2);
        QVERIFY(userEntry(*again, kBen).value(QStringLiteral("askPassword")).toBool());
        // Turning it off leaves the user locked until the next sign-in
        again->setUserAskPassword(kBen, false);
        QVERIFY(!userEntry(*again, kBen).value(QStringLiteral("askPassword")).toBool());
        QVERIFY(!userEntry(*again, kBen).value(QStringLiteral("remembered")).toBool());
        again->chooseUser(kBen);
        QCOMPARE(again->state(), ConnectionController::NeedsLogin);
    }

    void publicUsersLeaveOutTheSavedAndHiddenUsers()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        server.users.append(
            FakeJellyfin::FakeUser{kCleo, "cleotoken", QStringLiteral("Cleo"), kAvatarTag, false});
        server.users.append(
            FakeJellyfin::FakeUser{kDan, "dantoken", QStringLiteral("Dan"), {}, true});
        {
            auto first = makeController();
            first->connectTo(address(server));
            QVERIFY(waitForState(*first, ConnectionController::NeedsLogin));
            QVERIFY(signInUser(*first, QStringLiteral("Anna"), 1));
        }
        auto controller = makeController();
        controller->start();
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        controller->addUser();
        QVERIFY(controller->publicUsers().isEmpty());
        controller->loadPublicUsers();
        QTRY_COMPARE(controller->publicUsers().size(), 2);
        const QVariantMap ben = controller->publicUsers().at(0).toMap();
        const QVariantMap cleo = controller->publicUsers().at(1).toMap();
        QCOMPARE(ben.value(QStringLiteral("id")).toString(), kBen);
        QVERIFY(ben.value(QStringLiteral("imageUrl")).toUrl().isEmpty());
        QCOMPARE(cleo.value(QStringLiteral("id")).toString(), kCleo);
        QCOMPARE(cleo.value(QStringLiteral("name")).toString(), QStringLiteral("Cleo"));
        QVERIFY(cleo.value(QStringLiteral("imageUrl")).toUrl().toString().contains(kAvatarTag));
        // The list goes with the server
        controller->cancel();
        QVERIFY(controller->publicUsers().isEmpty());
    }

    void currentUserIdFollowsTheSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        auto controller = makeController();
        QSignalSpy changed(controller.get(), &ConnectionController::currentUserIdChanged);
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        QVERIFY(controller->currentUserId().isEmpty());
        QCOMPARE(changed.size(), 0);
        controller->chooseUser(kAnna);
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QCOMPARE(controller->currentUserId(), kAnna);
        QCOMPARE(changed.size(), 1);
        controller->switchUser();
        QVERIFY(controller->currentUserId().isEmpty());
        QCOMPARE(changed.size(), 2);
        controller->chooseUser(kBen);
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QCOMPARE(controller->currentUserId(), kBen);
        QCOMPARE(changed.size(), 3);
        controller->signOut();
        QVERIFY(controller->currentUserId().isEmpty());
        QCOMPARE(changed.size(), 4);
        // A revoked token ends the session the restore started
        server.deniedUsersMeTokens.insert("annatoken");
        controller->chooseUser(kAnna);
        QTRY_COMPARE(controller->state(), ConnectionController::NeedsLogin);
        QVERIFY(controller->currentUserId().isEmpty());
        QCOMPARE(changed.size(), 6);
    }

    // The wallet read of a connection that was cancelled must not move the next one
    void aCancelledRestoreDoesNotStrandTheNextConnect()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        QVERIFY(saveBothUsers(server));
        auto controller = makeController();
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        gStore->hold = true;
        controller->chooseUser(kAnna);
        QCOMPARE(controller->state(), ConnectionController::Connecting);
        controller->cancel();
        QCOMPARE(controller->state(), ConnectionController::Idle);
        controller->connectTo(address(server));
        // The new probe decides as usual while the old read is still held
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        gStore->release();
        QTest::qWait(200);
        QCOMPARE(controller->state(), ConnectionController::ChoosingUser);
        QVERIFY(controller->errorText().isEmpty());
        QVERIFY(controller->currentUserId().isEmpty());
        // And the picker still works
        controller->chooseUser(kBen);
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QCOMPARE(controller->currentUserId(), kBen);
    }

    void removeUserIgnoresAnUnknownUser()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        auto controller = makeController();
        controller->connectTo(address(server));
        QVERIFY(waitForState(*controller, ConnectionController::NeedsLogin));
        QVERIFY(signInUser(*controller, QStringLiteral("Anna"), 1));
        controller->addUser();
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        controller->removeUser(kCleo);
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        QCOMPARE(controller->users().size(), 1);
        QVERIFY(gStore->tokens.contains(tokenKeyOf(kAnna)));
    }

    // A session that could not be saved has no tile: Switch User goes to the sign-in, and Sign Out still ends it
    void switchUserWithoutSavedUsersGoesToSignIn()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        gStore->failWrites = true;
        auto controller = makeController();
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        controller->switchUser();
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        QVERIFY(controller->loginUserName().isEmpty());
        QVERIFY(!controller->hasSavedUsers());
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        controller->signOut();
        QCOMPARE(controller->state(), ConnectionController::Idle);
    }

    void publicUserNamesAreOneLineOfText()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        // A bidirectional mark and a run of spaces
        const QString name = QStringLiteral("Cl") + QChar(0x202e) + QStringLiteral("eo  Q");
        server.users = {{kAnna, "annatoken", QStringLiteral("Anna"), {}, false},
                        {kCleo, "cleotoken", name, {}, false}};
        auto controller = makeController();
        controller->connectTo(address(server));
        QVERIFY(waitForState(*controller, ConnectionController::NeedsLogin));
        controller->loadPublicUsers();
        QTRY_COMPARE(controller->publicUsers().size(), 2);
        QCOMPARE(controller->publicUsers().at(1).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Cl eo Q"));
    }

  private:
    std::unique_ptr<QTemporaryDir> m_home;
};

QTEST_GUILESS_MAIN(TestConnectionController)
#include "test_connection_controller.moc"
