// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QPointer>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

#include "FakeJellyfin.h"
#include "MemoryTokenStore.h"
#include "SelfSignedServer.h"
#include "api/PlaybackReport.h"
#include "app/ConnectionController.h"
#include "settings/BrowseStateStore.h"
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

QStringList urlStrings(const QList<QUrl> &urls)
{
    QStringList result;
    for (const QUrl &url : urls)
        result.append(url.toString());
    return result;
}

const QString kCertificateError = QStringLiteral(
    "The server's certificate is not valid: check the certificate, the address and the port, or "
    "turn on Ignore Certificate Errors in Settings");

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

// A GET /Users/Me body for `userId` carrying `configuration`
QByteArray meBody(const QString &userId, const QJsonObject &configuration)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("Id"), userId},
                                     {QStringLiteral("Name"), QStringLiteral("bepi")},
                                     {QStringLiteral("Policy"),
                                      QJsonObject{{QStringLiteral("IsAdministrator"), false}}},
                                     {QStringLiteral("Configuration"), configuration}})
        .toJson(QJsonDocument::Compact);
}

// The `id` of each entry of a library list, in order
QStringList idsOf(const QVariantList &libraries)
{
    QStringList ids;
    for (const QVariant &library : libraries)
        ids.append(library.toMap().value(QStringLiteral("id")).toString());
    return ids;
}

// The library id of each Recently Added row of the Home rows, in order
QStringList recentRowIds(Kinefin::HomeRowsModel *rows)
{
    QStringList ids;
    for (int row = 0; row < rows->rowCount(); ++row) {
        const QModelIndex index = rows->index(row);
        if (rows->data(index, Kinefin::HomeRowsModel::KindRole).toInt()
            == Kinefin::HomeRowsModel::RecentlyAdded)
            ids.append(rows->data(index, Kinefin::HomeRowsModel::LibraryIdRole).toString());
    }
    return ids;
}

// Whether every `/Items/Latest` request of `routes` (from `first` on) carries `isPlayed=false`, and none carries another value
bool latestAsksUnplayedOnly(const QList<QByteArray> &routes, qsizetype first = 0)
{
    for (qsizetype i = first; i < routes.size(); ++i) {
        if (!routes.at(i).contains("isPlayed=false"))
            return false;
    }
    return routes.size() > first;
}

bool latestAsksNoPlayedFilter(const QList<QByteArray> &routes, qsizetype first = 0)
{
    for (qsizetype i = first; i < routes.size(); ++i) {
        if (routes.at(i).contains("isPlayed"))
            return false;
    }
    return routes.size() > first;
}

// Signs the fixture user in on `server`
bool signInFixtureUser(ConnectionController &controller, const FakeJellyfin &server)
{
    controller.connectTo(address(server));
    if (!waitForState(controller, ConnectionController::NeedsLogin))
        return false;
    controller.login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
    return waitForState(controller, ConnectionController::Authenticated);
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
bool saveBothUsers(const FakeJellyfin &server)
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

// A parameter of a captured listing request, decoded
QString wire(const QByteArray &route, const char *name)
{
    return FakeJellyfin::capturedListingQuery(route).queryItemValue(QString::fromLatin1(name),
                                                                    QUrl::FullyDecoded);
}

// The text of a tag or rating the fake server lists, and the digest the store keeps it by
QString digestOf(const char *text)
{
    return Kinefin::BrowseStateStore::hashText(QString::fromLatin1(text));
}

const QString kSciFi = QStringLiteral("aa00000000000000000000000000ff01");

// A movie view with every field away from its default: genres, tags, ratings and years the fake server lists
Kinefin::LibraryItemsModel::ViewState richMovieView()
{
    using namespace Kinefin;
    LibraryItemsModel::ViewState view;
    view.sortBy = QStringLiteral("Runtime");
    view.descending = true;
    view.filters.played = PlayedState::Unplayed;
    view.filters.resumable = true;
    view.filters.favorites = true;
    view.filters.features = LibraryFeature::Subtitles | LibraryFeature::ThemeSong;
    view.filters.quality = VideoQuality::HD;
    view.filters.is4K = true;
    view.filters.is3D = true;
    view.filters.videoTypes = {QStringLiteral("BluRay"), QStringLiteral("Dvd")};
    view.filters.genreIds = {kSciFi};
    view.filters.tags = {QStringLiteral("restored-master")};
    view.filters.officialRatings = {QStringLiteral("PG-13")};
    view.filters.years = {2019, 2024};
    view.viewMode = QStringLiteral("list");
    view.imageType = QStringLiteral("Thumb");
    view.showTitle = false;
    view.showYear = false;
    view.cardLayout = false;
    return view;
}

// A series view: the sections only series have, and the ones its scope keeps
Kinefin::LibraryItemsModel::ViewState richSeriesView()
{
    using namespace Kinefin;
    LibraryItemsModel::ViewState view;
    view.sortBy = QStringLiteral("DateLastContentAdded");
    view.filters.played = PlayedState::Played;
    view.filters.features = LibraryFeature::Trailer;
    view.filters.seriesStatus = {QStringLiteral("Continuing"), QStringLiteral("Unreleased")};
    view.filters.genreIds = {kSciFi};
    view.filters.tags = {QStringLiteral("director-cut")};
    view.filters.officialRatings = {QStringLiteral("R")};
    view.filters.years = {2012};
    view.viewMode = QStringLiteral("list");
    view.imageType = QStringLiteral("Banner");
    return view;
}

// What the user does to a model, edit by edit
void applyView(Kinefin::LibraryItemsModel *model, const Kinefin::LibraryItemsModel::ViewState &view)
{
    model->setSortBy(view.sortBy);
    model->setDescending(view.descending);
    model->setFilterState(view.filters);
    model->setViewMode(view.viewMode);
    model->setImageType(view.imageType);
    model->setShowTitle(view.showTitle);
    model->setShowYear(view.showYear);
    model->setCardLayout(view.cardLayout);
}

// The facets of the open scope loaded, so the model can resolve and prune
bool facetsLoaded(Kinefin::LibraryItemsModel *model)
{
    return QTest::qWaitFor(
        [&] {
            return model->filterOptions()->status() == Kinefin::FilterOptionsModel::Ready
                   && model->filterOptions()->legacyStatus() == Kinefin::FilterOptionsModel::Ready;
        },
        10000);
}

// The id of the fixture movie
const QString kDownloadedMovie = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");

// Signs Anna in and downloads the fixture movie to the end; `server` needs the user table of addTwoUsers. The fake sends 64 KiB from its Download route, so the record expects that file
bool signInWithADownload(ConnectionController &controller, FakeJellyfin &server)
{
    QJsonObject movie = QJsonDocument::fromJson(fixture(QStringLiteral("movie_item")))
                            .object()
                            .value(QStringLiteral("Items"))
                            .toArray()
                            .first()
                            .toObject();
    QJsonArray sources = movie.value(QStringLiteral("MediaSources")).toArray();
    QJsonObject source = sources.first().toObject();
    source.insert(QStringLiteral("Size"), 64 * 1024);
    source.insert(QStringLiteral("ETag"), QStringLiteral("etag-1"));
    sources.replace(0, source);
    movie.insert(QStringLiteral("MediaSources"), sources);
    server.itemsById.insert(kDownloadedMovie, QJsonDocument(movie).toJson(QJsonDocument::Compact));
    controller.connectTo(address(server));
    if (!waitForState(controller, ConnectionController::NeedsLogin)
        || !signInUser(controller, QStringLiteral("Anna"), 1)
        || !controller.downloads()->downloadItem(kDownloadedMovie))
        return false;
    return QTest::qWaitFor(
        [&] {
            return controller.downloads()->downloadState(kDownloadedMovie)
                   == QLatin1String("completed");
        },
        10000);
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

    void theSocketCarriesRemoteCommandsAndAnnouncesThem()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        QSignalSpy keycodes(controller->remoteCommands(),
                            &Kinefin::RemoteCommands::keycodeRequested);
        QSignalSpy seeks(controller->remoteCommands(), &Kinefin::RemoteCommands::seekRequested);
        QVERIFY(signInFixtureUser(*controller, server));
        // The sign-in opens the socket, which announces media control and the commands it takes
        QTRY_COMPARE(controller->socket()->state(),
                     Kinefin::JellyfinSocketClient::State::Authenticated);
        QTRY_VERIFY(QJsonDocument::fromJson(server.lastCapabilitiesBody)
                        .object()
                        .value(QStringLiteral("SupportsMediaControl"))
                        .toBool());
        const QJsonArray commands = QJsonDocument::fromJson(server.lastCapabilitiesBody)
                                        .object()
                                        .value(QStringLiteral("SupportedCommands"))
                                        .toArray();
        QCOMPARE(commands, QJsonArray::fromStringList(Kinefin::supportedGeneralCommands()));
        FakeSocketConnection *connection = server.lastConnection();
        QVERIFY(connection);
        connection->sendText(
            QStringLiteral("{\"MessageType\":\"Playstate\",\"Data\":{\"Command\":\"Pause\"}}"));
        QTRY_COMPARE(keycodes.size(), 1);
        QCOMPARE(keycodes.constFirst().at(0).toString(), QStringLiteral("pause"));
        connection->sendText(QStringLiteral("{\"MessageType\":\"Playstate\",\"Data\":{"
                                            "\"Command\":\"Seek\",\"SeekPositionTicks\":50}}"));
        QTRY_COMPARE(seeks.size(), 1);
        QCOMPARE(seeks.constFirst().at(0).toLongLong(), 50);
        // Signing out closes it: nothing more arrives
        controller->signOut();
        QTRY_COMPARE(controller->socket()->state(),
                     Kinefin::JellyfinSocketClient::State::Disconnected);
    }

    void aUserDataChangedFrameIsReadAgainOnceAndHeldWhileSomethingPlays()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QString first = QStringLiteral("aaaa0000aaaa0000aaaa0000aaaa0001");
        const QString second = QStringLiteral("bbbb0000bbbb0000bbbb0000bbbb0002");
        for (const QString &id : {first, second})
            server.itemsById.insert(
                id, QJsonDocument(QJsonObject{{QStringLiteral("Id"), id},
                                              {QStringLiteral("Type"), QStringLiteral("Movie")},
                                              {QStringLiteral("UserData"),
                                               QJsonObject{{QStringLiteral("Played"), true}}}})
                        .toJson(QJsonDocument::Compact));
        auto controller = makeController();
        QVERIFY(signInFixtureUser(*controller, server));
        QTRY_COMPARE(controller->socket()->state(),
                     Kinefin::JellyfinSocketClient::State::Authenticated);
        FakeSocketConnection *connection = server.lastConnection();
        QVERIFY(connection);
        const auto frame = [&](const QStringList &ids) {
            QJsonArray rows;
            for (const QString &id : ids)
                rows.append(QJsonObject{{QStringLiteral("ItemId"), id}});
            connection->sendText(QString::fromUtf8(
                QJsonDocument(
                    QJsonObject{
                        {QStringLiteral("MessageType"), QStringLiteral("UserDataChanged")},
                        {QStringLiteral("Data"),
                         QJsonObject{{QStringLiteral("UserId"), controller->currentUserId()},
                                     {QStringLiteral("UserDataList"), rows}}}})
                    .toJson(QJsonDocument::Compact)));
        };
        const auto reads = [&] {
            int count = 0;
            for (const QByteArray &route : std::as_const(server.itemsRoutes))
                count += route.contains(first.toUtf8()) || route.contains(second.toUtf8());
            return count;
        };
        QSignalSpy played(&controller->playbackClient(),
                          &Kinefin::JellyfinClient::itemPlayedUpdated);

        // Two frames in a burst are one round that reads the ids once
        frame({first});
        frame({second, first});
        QTRY_COMPARE_WITH_TIMEOUT(played.size(), 2, 5000);
        QCOMPARE(reads(), 1);

        // While something plays the ids wait, and go when it is over
        controller->holdUserDataRefresh(true);
        frame({first});
        QTest::qWait(1500);
        QCOMPARE(reads(), 1);
        QCOMPARE(played.size(), 2);
        controller->holdUserDataRefresh(false);
        QTRY_COMPARE_WITH_TIMEOUT(played.size(), 3, 5000);
        QCOMPARE(reads(), 2);

        // A sign-out drops what waited
        controller->holdUserDataRefresh(true);
        frame({second});
        QTest::qWait(100);
        controller->signOut();
        controller->holdUserDataRefresh(false);
        QTest::qWait(1500);
        QCOMPARE(reads(), 2);
    }

    void aSocketGapLongEnoughAsksThePageToReadAgainAndAShortOneDoesNot()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        // The retry waits the gap that the test sets, so a drop is a gap of that length
        controller->socket()->setTestTimings(0, 0, 0, 400, 400);
        QVERIFY(signInFixtureUser(*controller, server));
        QTRY_COMPARE(controller->socket()->state(),
                     Kinefin::JellyfinSocketClient::State::Authenticated);
        QSignalSpy missed(controller.get(), &ConnectionController::serverEventsMissed);

        // Back within the limit: a network blip, the pages keep their scroll
        controller->setTestSocketGap(60'000);
        server.lastConnection()->kick();
        QTRY_COMPARE_WITH_TIMEOUT(controller->socket()->state(),
                                  Kinefin::JellyfinSocketClient::State::Reconnecting, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller->socket()->state(),
                                  Kinefin::JellyfinSocketClient::State::Authenticated, 5000);
        QCOMPARE(missed.size(), 0);

        // Back after the limit: something may have changed in the meantime
        controller->setTestSocketGap(100);
        server.lastConnection()->kick();
        QTRY_COMPARE_WITH_TIMEOUT(controller->socket()->state(),
                                  Kinefin::JellyfinSocketClient::State::Reconnecting, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller->socket()->state(),
                                  Kinefin::JellyfinSocketClient::State::Authenticated, 5000);
        QCOMPARE(missed.size(), 1);

        // A first connection is not a gap
        controller->signOut();
        QCOMPARE(missed.size(), 1);
    }

    // A test runs only when asked, sends no credentials and says once that it ended
    void aServerTestSendsNoCredentialsAndReportsItsEnd()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        QVERIFY(signInFixtureUser(*controller, server));
        QTRY_COMPARE(controller->servers().size(), 1);
        const QString url =
            controller->servers().constFirst().toMap().value(QStringLiteral("url")).toString();
        const int previous = server.publicInfoRequests;
        QSignalSpy tested(controller.get(), &Kinefin::ConnectionController::serverTested);
        controller->testServer(url);
        QTRY_VERIFY(controller->serverReachability()
                        .value(url)
                        .toMap()
                        .value(QStringLiteral("reachable"))
                        .toBool());
        QCOMPARE(server.publicInfoRequests, previous + 1);
        QVERIFY(server.lastPublicInfoAuth.isEmpty());
        QCOMPARE(tested.size(), 1);
        QCOMPARE(tested.constFirst().at(0).toString(), url);
        QVERIFY(tested.constFirst().at(1).toBool());
        controller->testServer(url);
        QTRY_COMPARE(server.publicInfoRequests, previous + 2);
        QTRY_COMPARE(tested.size(), 2);
        QVERIFY(!controller->serverReachability()
                     .value(url)
                     .toMap()
                     .value(QStringLiteral("testing"))
                     .toBool());
    }

    void malformedServerTestsDoNotSendNetworkRequests()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        for (const QString &url : {QStringLiteral("file:///tmp/server"),
                                   QStringLiteral("http://user:password@127.0.0.1:8096"),
                                   QStringLiteral("https://host.invalid/#fragment"),
                                   QStringLiteral("http://localhost:0")}) {
            controller->testServer(url);
            const auto result = controller->serverReachability().value(url).toMap();
            QVERIFY(!result.value(QStringLiteral("reachable")).toBool());
            QVERIFY(!result.value(QStringLiteral("testing")).toBool());
        }
        QCOMPARE(server.publicInfoRequests, 0);
    }

    void availabilityCheckRejectsRedirectedAndInvalidIdentities()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        const QString url = address(server);
        server.redirectPublicOffHost = true;
        controller->testServer(url);
        QTRY_VERIFY(!controller->serverReachability()
                         .value(url)
                         .toMap()
                         .value(QStringLiteral("testing"))
                         .toBool());
        QVERIFY(!controller->serverReachability()
                     .value(url)
                     .toMap()
                     .value(QStringLiteral("reachable"))
                     .toBool());
        QCOMPARE(server.publicInfoRequests, 1);
        server.redirectPublicOffHost = false;
        server.invalidIdentity = true;
        controller->testServer(url);
        QTRY_COMPARE(server.publicInfoRequests, 2);
        QTRY_VERIFY(!controller->serverReachability()
                         .value(url)
                         .toMap()
                         .value(QStringLiteral("testing"))
                         .toBool());
        QVERIFY(!controller->serverReachability()
                     .value(url)
                     .toMap()
                     .value(QStringLiteral("reachable"))
                     .toBool());
    }

    void candidatesOfAnAddressWithoutSchemeTryHttpsThenHttp()
    {
        const auto candidates = [](const QString &address) {
            return urlStrings(ConnectionController::connectionCandidates(address, true));
        };
        QCOMPARE(candidates(QStringLiteral("jf.example")),
                 (QStringList{QStringLiteral("https://jf.example"),
                              QStringLiteral("https://jf.example:8920"),
                              QStringLiteral("http://jf.example:8096"),
                              QStringLiteral("http://jf.example")}));
        QCOMPARE(candidates(QStringLiteral(" jf.example/jellyfin ")),
                 (QStringList{QStringLiteral("https://jf.example/jellyfin"),
                              QStringLiteral("https://jf.example:8920/jellyfin"),
                              QStringLiteral("http://jf.example:8096/jellyfin"),
                              QStringLiteral("http://jf.example/jellyfin")}));
        QCOMPARE(candidates(QStringLiteral("jf.example:9000")),
                 (QStringList{QStringLiteral("https://jf.example:9000"),
                              QStringLiteral("http://jf.example:9000")}));
        // A typed standard port is still a typed port: the same port on both schemes, nothing else
        QCOMPARE(candidates(QStringLiteral("jf.example:8096")),
                 (QStringList{QStringLiteral("https://jf.example:8096"),
                              QStringLiteral("http://jf.example:8096")}));
        QCOMPARE(candidates(QStringLiteral("[fe80::1]:8096")),
                 (QStringList{QStringLiteral("https://[fe80::1]:8096"),
                              QStringLiteral("http://[fe80::1]:8096")}));
        QCOMPARE(candidates(QStringLiteral("[fe80::1]")),
                 (QStringList{
                     QStringLiteral("https://[fe80::1]"), QStringLiteral("https://[fe80::1]:8920"),
                     QStringLiteral("http://[fe80::1]:8096"), QStringLiteral("http://[fe80::1]")}));
        // Without the fallback the address is what connectTo() would use: https and nothing else
        QCOMPARE(urlStrings(ConnectionController::connectionCandidates(
                     QStringLiteral("jf.example:9000"), false)),
                 (QStringList{QStringLiteral("https://jf.example:9000")}));
    }

    // A saved server keeps its exact endpoint: no other port, no other scheme
    void candidatesOfAnAddressWithSchemeAreThatAddressAlone()
    {
        for (const QString &address :
             {QStringLiteral("https://jf.example"), QStringLiteral("https://jf.example:8920"),
              QStringLiteral("http://jf.example:8096"), QStringLiteral("http://jf.example:9000")}) {
            for (const bool fallback : {true, false})
                QCOMPARE(urlStrings(ConnectionController::connectionCandidates(address, fallback)),
                         QStringList{address});
        }
    }

    void invalidAddressesHaveNoCandidates()
    {
        for (const QString &invalid :
             {QString(), QStringLiteral("   "), QStringLiteral("jf.example:0"),
              QStringLiteral("user:password@jf.example"), QStringLiteral("jf.example/?q=1"),
              QStringLiteral("jf.example/#fragment"), QStringLiteral("[fe80::1"),
              QStringLiteral("ftp://jf.example"), QStringLiteral("file:///tmp/server"),
              QStringLiteral("http://user:password@jf.example"), QStringLiteral("http://"),
              QString(QStringLiteral("jf.example/") + QString(2100, QLatin1Char('a')))}) {
            QVERIFY2(ConnectionController::connectionCandidates(invalid, true).isEmpty(),
                     qPrintable(invalid.left(60)));
            QVERIFY2(ConnectionController::connectionCandidates(invalid, false).isEmpty(),
                     qPrintable(invalid.left(60)));
        }
    }

    void invalidAddressWithoutSchemeIsRejectedBeforeAnyRequest()
    {
        auto controller = makeController();
        controller->connectToWithFallback(QStringLiteral("jf.example:0"));
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QCOMPARE(controller->errorText(), QStringLiteral("Enter a valid server address"));
        const QString typed = QStringLiteral("user:password@jf.example");
        controller->testServer(typed, true);
        const auto result = controller->serverReachability().value(typed).toMap();
        QVERIFY(!result.value(QStringLiteral("reachable")).toBool());
        QVERIFY(!result.value(QStringLiteral("testing")).toBool());
        QCOMPARE(result.value(QStringLiteral("message")).toString(),
                 QStringLiteral("Enter a valid server address"));
    }

    // The typed port serves plain HTTP only: the https attempt on it fails as "not TLS" and the http one answers
    void testWithoutSchemeFallsBackFromHttpsToHttp()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        const QString typed = QStringLiteral("127.0.0.1:%1").arg(server.url().port());
        controller->testServer(typed, true);
        QTRY_VERIFY(controller->serverReachability()
                        .value(typed)
                        .toMap()
                        .value(QStringLiteral("reachable"))
                        .toBool());
        const auto result = controller->serverReachability().value(typed).toMap();
        QVERIFY(!result.value(QStringLiteral("testing")).toBool());
        QCOMPARE(result.value(QStringLiteral("resolvedUrl")).toString(),
                 QStringLiteral("http://") + typed);
        QVERIFY(result.value(QStringLiteral("message")).toString().contains(typed));
        QCOMPARE(server.tlsAttempts, 1);
        QCOMPARE(server.publicInfoRequests, 1);
        QVERIFY(server.lastPublicInfoAuth.isEmpty());
    }

    void testWithoutSchemeAndWithoutFallbackTriesHttpsOnly()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        const QString typed = QStringLiteral("127.0.0.1:%1").arg(server.url().port());
        controller->testServer(typed, false);
        QTRY_VERIFY(!controller->serverReachability()
                         .value(typed)
                         .toMap()
                         .value(QStringLiteral("testing"))
                         .toBool());
        QVERIFY(!controller->serverReachability()
                     .value(typed)
                     .toMap()
                     .value(QStringLiteral("reachable"))
                     .toBool());
        QCOMPARE(server.tlsAttempts, 1);
        QCOMPARE(server.publicInfoRequests, 0);
    }

    void connectWithoutSchemeFallsBackFromHttpsToHttp()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        auto controller = makeController();
        const QString typed = QStringLiteral("127.0.0.1:%1").arg(server.url().port());
        controller->connectToWithFallback(typed);
        QCOMPARE(controller->state(), ConnectionController::Connecting);
        QCOMPARE(controller->serverUrl(), QStringLiteral("https://") + typed);
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        // Loopback is exempt from the insecure-HTTP confirmation
        QCOMPARE(controller->serverUrl(), QStringLiteral("http://") + typed);
        QCOMPARE(server.tlsAttempts, 1);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->servers().size(), 1);
        QCOMPARE(controller->servers().constFirst().toMap().value(QStringLiteral("url")).toString(),
                 QStringLiteral("http://") + typed);
    }

    // The endpoint reached by the fallback is plain HTTP: the warning comes before any credential
    void connectFallbackToHttpOnAnotherHostAsksConfirmation()
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
        const QString typed = QStringLiteral("%1:%2").arg(lan.toString()).arg(server.url().port());
        controller->connectToWithFallback(typed);
        QTRY_VERIFY(controller->state() == ConnectionController::ConfirmInsecure);
        QCOMPARE(controller->serverUrl(), QStringLiteral("http://") + typed);
        QCOMPARE(server.tlsAttempts, 1);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QCOMPARE(controller->state(), ConnectionController::ConfirmInsecure); // refused
        QCOMPARE(server.successfulLogins, 0);
        controller->confirmInsecureHttp();
        QCOMPARE(controller->state(), ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_COMPARE(controller->servers().size(), 1);
        QCOMPARE(controller->servers().constFirst().toMap().value(QStringLiteral("url")).toString(),
                 QStringLiteral("http://") + typed);
    }

    // Every candidate refused: the sequence ends with the usual network error
    void connectWithoutSchemeEndsWhenEveryCandidateIsRefused()
    {
        quint16 port = 0;
        {
            FakeJellyfin gone;
            QVERIFY(gone.listen());
            port = quint16(gone.url().port());
        }
        auto controller = makeController();
        controller->connectToWithFallback(QStringLiteral("127.0.0.1:%1").arg(port));
        QTRY_VERIFY(controller->state() == ConnectionController::Idle);
        QCOMPARE(controller->errorText(),
                 QStringLiteral("Could not reach a Jellyfin server. Check the address and your "
                                "network connection"));
    }

    // A certificate that does not verify is not a network failure: the attempts stop, and HTTP is never tried
    void connectStopsOnACertificateErrorWithoutFallingBackToHttp()
    {
        if (!QSslSocket::supportsSsl())
            QSKIP("no TLS backend");
        if (!SelfSignedServer::fixturesPresent())
            QSKIP("no self-signed certificate fixture in tests/fixtures");
        SelfSignedServer tls;
        QVERIFY(tls.listen());
        auto controller = makeController();
        controller->connectToWithFallback(tls.address());
        QCOMPARE(controller->state(), ConnectionController::Connecting);
        QTRY_VERIFY(controller->state() == ConnectionController::Idle);
        QCOMPARE(controller->errorText(), kCertificateError);
        // A fallback to http on the same port would reach the TLS server a second time
        QTest::qWait(300);
        QCOMPARE(tls.handshakes, 1);
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(controller->servers().isEmpty());
    }

    void connectToAnExactAddressReportsACertificateError()
    {
        if (!QSslSocket::supportsSsl())
            QSKIP("no TLS backend");
        if (!SelfSignedServer::fixturesPresent())
            QSKIP("no self-signed certificate fixture in tests/fixtures");
        SelfSignedServer tls;
        QVERIFY(tls.listen());
        auto controller = makeController();
        controller->connectTo(QStringLiteral("https://") + tls.address());
        QTRY_VERIFY(controller->state() == ConnectionController::Idle);
        QCOMPARE(controller->errorText(), kCertificateError);
        QCOMPARE(tls.handshakes, 1);
    }

    void testStopsOnACertificateErrorWithoutFallingBackToHttp()
    {
        if (!QSslSocket::supportsSsl())
            QSKIP("no TLS backend");
        if (!SelfSignedServer::fixturesPresent())
            QSKIP("no self-signed certificate fixture in tests/fixtures");
        SelfSignedServer tls;
        QVERIFY(tls.listen());
        auto controller = makeController();
        const QString typed = tls.address();
        controller->testServer(typed, true);
        QTRY_VERIFY(!controller->serverReachability()
                         .value(typed)
                         .toMap()
                         .value(QStringLiteral("testing"))
                         .toBool());
        const auto result = controller->serverReachability().value(typed).toMap();
        QVERIFY(!result.value(QStringLiteral("reachable")).toBool());
        QCOMPARE(result.value(QStringLiteral("message")).toString(), kCertificateError);
        QVERIFY(result.value(QStringLiteral("resolvedUrl")).toString().isEmpty());
        QTest::qWait(300);
        QCOMPARE(tls.handshakes, 1);
    }

    // With Ignore Certificate Errors on, the self-signed server is an ordinary server: no certificate error, no fallback to HTTP
    void connectAcceptsASelfSignedCertificateWhenErrorsAreIgnored()
    {
        if (!QSslSocket::supportsSsl())
            QSKIP("no TLS backend");
        if (!SelfSignedServer::fixturesPresent())
            QSKIP("no self-signed certificate fixture in tests/fixtures");
        SelfSignedServer tls;
        tls.body = fixture(QStringLiteral("system_info_public"));
        QVERIFY(tls.listen());
        auto controller = makeController();
        controller->setIgnoreCertificateErrors(true);
        controller->connectToWithFallback(tls.address());
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        QVERIFY(controller->errorText().isEmpty());
        QVERIFY(controller->serverUrl().startsWith(QStringLiteral("https://")));
        QVERIFY(tls.completed >= 1);
    }

    void testAcceptsASelfSignedCertificateWhenErrorsAreIgnored()
    {
        if (!QSslSocket::supportsSsl())
            QSKIP("no TLS backend");
        if (!SelfSignedServer::fixturesPresent())
            QSKIP("no self-signed certificate fixture in tests/fixtures");
        SelfSignedServer tls;
        tls.body = fixture(QStringLiteral("system_info_public"));
        QVERIFY(tls.listen());
        auto controller = makeController();
        controller->setIgnoreCertificateErrors(true);
        const QString typed = tls.address();
        controller->testServer(typed, true);
        QTRY_VERIFY(!controller->serverReachability()
                         .value(typed)
                         .toMap()
                         .value(QStringLiteral("testing"))
                         .toBool());
        const auto result = controller->serverReachability().value(typed).toMap();
        QVERIFY(result.value(QStringLiteral("reachable")).toBool());
        QVERIFY(result.value(QStringLiteral("resolvedUrl"))
                    .toString()
                    .startsWith(QStringLiteral("https://")));
        // The setting is read per request: turned off again, the same server is refused
        controller->setIgnoreCertificateErrors(false);
        controller->testServer(typed, true);
        QTRY_VERIFY(!controller->serverReachability()
                         .value(typed)
                         .toMap()
                         .value(QStringLiteral("testing"))
                         .toBool());
        const auto refused = controller->serverReachability().value(typed).toMap();
        QVERIFY(!refused.value(QStringLiteral("reachable")).toBool());
        QCOMPARE(refused.value(QStringLiteral("message")).toString(), kCertificateError);
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
    bool signInAgain(ConnectionController &controller, const FakeJellyfin &server)
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

    bool signInWithLibraries(ConnectionController &controller, const FakeJellyfin &server,
                             int libraries)
    {
        return signInAgain(controller, server) && waitForLibraries(controller, libraries);
    }

    void subtabsFollowOpenTypeAndTheEnabledSet()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        auto controller = makeController();
        auto *subtabs = controller->subtabs();
        // The settings come before the session: unknown ids are ignored, a type's own set is its own
        controller->setSubtabsEnabled(QStringLiteral("movies"),
                                      {QStringLiteral("favorites"), QStringLiteral("collections"),
                                       QStringLiteral("bogus"), QStringLiteral("episodes")});
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        QVERIFY(!subtabs->isOpen());

        QVERIFY(controller->openType(QStringLiteral("movies")));
        QVERIFY(subtabs->isOpen());
        QCOMPARE(subtabs->type(), QStringLiteral("movies"));
        QVERIFY(subtabs->libraryId().isEmpty());
        QCOMPARE(subtabs->available(),
                 (QStringList{QStringLiteral("main"), QStringLiteral("favorites"),
                              QStringLiteral("collections")}));
        QCOMPARE(subtabs->current(), QStringLiteral("main"));
        // Nothing is asked for a sub-tab that is not showing
        QTRY_COMPARE(controller->movies()->status(), Kinefin::LibraryItemsModel::Ready);
        for (const QByteArray &route : std::as_const(server.itemsRoutes))
            QVERIFY2(!route.contains("BoxSet") && !route.contains("IsFavorite"), route.constData());
        QVERIFY(server.listingRoutes.value(QStringLiteral("MovieRecommendations")).isEmpty());

        // Turning one on shows it in the bar of the open page
        QSignalSpy availableChanged(subtabs, &Kinefin::LibrarySubtabController::availableChanged);
        controller->setSubtabsEnabled(
            QStringLiteral("movies"),
            {QStringLiteral("favorites"), QStringLiteral("collections"), QStringLiteral("genres")});
        QCOMPARE(availableChanged.size(), 1);
        QCOMPARE(subtabs->available().size(), 4);
        QVERIFY(subtabs->select(QStringLiteral("favorites")));
        QCOMPARE(subtabs->current(), QStringLiteral("favorites"));
        // ...and off while it shows: back to the main one
        controller->setSubtabsEnabled(QStringLiteral("movies"), {QStringLiteral("collections")});
        QCOMPARE(subtabs->current(), QStringLiteral("main"));
        QVERIFY(!subtabs->select(QStringLiteral("favorites")));

        // Another tab opens its own sub-tabs, the Home tab closes them, the same scope opens them again
        QVERIFY(controller->openType(QStringLiteral("tvshows")));
        QCOMPARE(subtabs->type(), QStringLiteral("tvshows"));
        QCOMPARE(subtabs->available(), QStringList{QStringLiteral("main")});
        controller->closeSubtabs();
        QVERIFY(!subtabs->isOpen());
        QVERIFY(controller->openType(QStringLiteral("tvshows")));
        QVERIFY(subtabs->isOpen());
        // The end of the session releases everything
        controller->cancel();
        QVERIFY(!subtabs->isOpen());
        QVERIFY(subtabs->available().isEmpty());
    }

    void aLibraryChangeMovesTheSubtabsToTheLibrary()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        auto controller = makeController();
        auto *subtabs = controller->subtabs();
        controller->setSubtabsEnabled(QStringLiteral("movies"), {QStringLiteral("favorites")});
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(controller->libraries().size(), 3);
        QVERIFY(controller->openType(QStringLiteral("movies")));
        QVERIFY(subtabs->libraryId().isEmpty());
        QVERIFY(subtabs->select(QStringLiteral("favorites")));
        QTRY_COMPARE(subtabs->status(), Kinefin::LibrarySubtabController::Ready);
        // Another library keeps the sub-tab and lists its own favorites
        QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
        QCOMPARE(subtabs->libraryId(), kMoviesB);
        QCOMPARE(subtabs->current(), QStringLiteral("favorites"));
        QTRY_COMPARE(subtabs->status(), Kinefin::LibrarySubtabController::Ready);
        QVERIFY(subtabs->activeList());
        QCOMPARE(subtabs->activeList()->libraryId(), kMoviesB);
        // A one-off Recently Added list is the main sub-tab's
        QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies"), kMoviesB));
        QCOMPARE(subtabs->current(), QStringLiteral("main"));
    }

    void subtabListsKeepTheirOwnViewState()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        {
            auto controller = makeController();
            controller->setSubtabsEnabled(
                QStringLiteral("movies"),
                {QStringLiteral("favorites"), QStringLiteral("collections")});
            controller->setSubtabsEnabled(QStringLiteral("tvshows"),
                                          {QStringLiteral("networks"), QStringLiteral("episodes")});
            controller->connectTo(address(server));
            QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
            controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
            QTRY_COMPARE(controller->libraries().size(), 3);
            auto *subtabs = controller->subtabs();
            QVERIFY(controller->openType(QStringLiteral("movies")));
            controller->movies()->setSortBy(QStringLiteral("DateCreated"));

            // Favorites over every library, and over one library, are two views
            QVERIFY(subtabs->select(QStringLiteral("favorites")));
            QVERIFY(subtabs->activeList());
            QCOMPARE(subtabs->scopeKey(), QStringLiteral("movies/favorites"));
            subtabs->activeList()->setSortBy(QStringLiteral("Runtime"));
            subtabs->activeList()->setViewMode(QStringLiteral("list"));
            subtabs->activeList()->setFilterValues(
                QVariantMap{{QStringLiteral("played"), QStringLiteral("Unplayed")}});
            QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
            QCOMPARE(subtabs->current(), QStringLiteral("favorites"));
            QCOMPARE(subtabs->activeList()->sortBy(), QStringLiteral("SortName"));
            QCOMPARE(subtabs->activeList()->viewMode(), QStringLiteral("grid"));
            subtabs->activeList()->setSortBy(QStringLiteral("PremiereDate"));
            // The collections are one view whatever the library
            QVERIFY(subtabs->select(QStringLiteral("collections")));
            subtabs->activeList()->setSortBy(QStringLiteral("CommunityRating"));
            // The main sub-tab's own view is another one, and not touched by any of it
            QVERIFY(subtabs->select(QStringLiteral("main")));
            QCOMPARE(controller->movies()->sortBy(), QStringLiteral("SortName"));

            // A network's series are kept under the network
            QVERIFY(controller->openType(QStringLiteral("tvshows")));
            QVERIFY(subtabs->select(QStringLiteral("networks")));
            QTRY_COMPARE(subtabs->status(), Kinefin::LibrarySubtabController::Ready);
            QVERIFY(subtabs->networks()->count() > 0);
            const QString studio =
                subtabs->networks()
                    ->data(subtabs->networks()->index(0), Kinefin::ItemRowModel::ItemIdRole)
                    .toString();
            QVERIFY(subtabs->selectNetwork(studio));
            QVERIFY(subtabs->activeList());
            QCOMPARE(subtabs->scopeKey(), QStringLiteral("tvshows/network:") + studio);
            subtabs->activeList()->setSortBy(QStringLiteral("SeriesDatePlayed"));
        } // the pending write goes out with the controller
        QFile file(browseFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray stored = file.readAll();
        file.close();
        QVERIFY(stored.contains("movies/favorites\""));
        QVERIFY(stored.contains(QByteArray("movies/favorites:").append(kMoviesB.toLatin1())));
        QVERIFY(stored.contains("movies/collections"));
        QVERIFY(stored.contains("tvshows/network:"));
        // Names of the server are never stored
        QVERIFY(!stored.contains("Library "));

        auto again = makeController();
        again->setSubtabsEnabled(QStringLiteral("movies"),
                                 {QStringLiteral("favorites"), QStringLiteral("collections")});
        QVERIFY(signInWithLibraries(*again, server, 3));
        auto *subtabs = again->subtabs();
        QVERIFY(again->openType(QStringLiteral("movies"), QStringLiteral("*")));
        QVERIFY(subtabs->select(QStringLiteral("favorites")));
        QCOMPARE(subtabs->activeList()->sortBy(), QStringLiteral("Runtime"));
        QCOMPARE(subtabs->activeList()->viewMode(), QStringLiteral("list"));
        QCOMPARE(subtabs->activeList()->filterState().played, Kinefin::PlayedState::Unplayed);
        QVERIFY(again->openType(QStringLiteral("movies"), kMoviesB));
        QCOMPARE(subtabs->activeList()->sortBy(), QStringLiteral("PremiereDate"));
        QVERIFY(subtabs->select(QStringLiteral("collections")));
        QCOMPARE(subtabs->activeList()->sortBy(), QStringLiteral("CommunityRating"));
        // The main list of this library has the view of its own key, which nothing above touched
        QCOMPARE(again->movies()->sortBy(), QStringLiteral("SortName"));
        QVERIFY(again->openType(QStringLiteral("movies"), QStringLiteral("*")));
        QCOMPARE(again->movies()->sortBy(), QStringLiteral("DateCreated"));
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
        // The player's memory of the user goes with the sign-out, and that of every user with the server
        const QString serverId =
            controller->servers().constFirst().toMap().value(QStringLiteral("serverId")).toString();
        const QString userId = controller->currentUserId();
        QSignalSpy userForgotten(controller.get(),
                                 &Kinefin::ConnectionController::storedUserForgotten);
        QSignalSpy serverForgotten(controller.get(),
                                   &Kinefin::ConnectionController::storedServerForgotten);
        // The player leaves the session (and keeps its place) before what it kept is forgotten
        QStringList order;
        connect(controller.get(), &Kinefin::ConnectionController::aboutToLeaveSession, this,
                [&order] { order << QStringLiteral("leave"); });
        connect(controller.get(), &Kinefin::ConnectionController::storedUserForgotten, this,
                [&order] { order << QStringLiteral("forget"); });
        // The end of a session writes what is pending and keeps it
        controller->signOut();
        QCOMPARE(userForgotten.size(), 1);
        QCOMPARE(userForgotten.constFirst().value(0).toString(), serverId);
        QCOMPARE(userForgotten.constFirst().value(1).toString(), userId);
        QCOMPARE(order, QStringList({QStringLiteral("leave"), QStringLiteral("forget")}));
        disconnect(controller.get(), nullptr, this, nullptr);
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
        QCOMPARE(serverForgotten.size(), 1);
        QCOMPARE(serverForgotten.constFirst().value(0).toString(), serverId);
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
            // A collection offers no runtime order: the date added is one of its five
            controller->library()->setSortKey(Kinefin::LibraryItemsModel::DateAdded);
        }
        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 1));
        QVERIFY(again->openLibrary(boxsets));
        QCOMPARE(again->library()->sortKey(), Kinefin::LibraryItemsModel::DateAdded);
    }

    void everyViewFieldSurvivesARestart()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        const Model::ViewState movieView = richMovieView();
        const Model::ViewState seriesView = richSeriesView();
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
            QVERIFY(facetsLoaded(controller->movies()));
            applyView(controller->movies(), movieView);
            QVERIFY(controller->movies()->viewState() == movieView);
            QVERIFY(controller->openType(QStringLiteral("tvshows")));
            QVERIFY(facetsLoaded(controller->shows()));
            applyView(controller->shows(), seriesView);
            QVERIFY(controller->shows()->viewState() == seriesView);
        }
        // The file keeps digests of the tags and ratings, never their text
        const QByteArray bytes = browseBytes();
        for (const char *text : {"restored-master", "director-cut", "PG-13"})
            QVERIFY2(!bytes.contains(text), text);
        for (const char *text : {"restored-master", "director-cut", "PG-13", "R"})
            QVERIFY2(bytes.contains(digestOf(text).toUtf8()), text);

        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 3));
        // Requests the ended session still had in flight reach the server late: count from here
        QTest::qWait(100);
        const int before = int(server.itemsRoutes.size());
        QSignalSpy edits(again->movies(), &Model::viewStateEdited);
        QVERIFY(again->openType(QStringLiteral("movies")));
        QCOMPARE(again->movies()->libraryId(), kMoviesB);
        // The tag and the rating come back as texts once the facets have loaded
        QTRY_VERIFY(!again->movies()->resolvingFacets());
        QVERIFY(again->movies()->viewState() == movieView);
        QTRY_COMPARE(again->movies()->status(), Model::Ready);
        QCOMPARE(edits.size(), 0);
        // The one request of the view carries all of it
        QCOMPARE(int(server.itemsRoutes.size()), before + 1);
        const QByteArray route = server.itemsRoutes.constLast();
        QCOMPARE(wire(route, "tags"), QStringLiteral("restored-master"));
        QCOMPARE(wire(route, "officialRatings"), QStringLiteral("PG-13"));
        QCOMPARE(wire(route, "years"), QStringLiteral("2019,2024"));
        QCOMPARE(wire(route, "genreIds"), kSciFi);
        QCOMPARE(wire(route, "videoTypes"), QStringLiteral("BluRay,Dvd"));
        QCOMPARE(wire(route, "sortBy"), QStringLiteral("Runtime,SortName"));
        QCOMPARE(wire(route, "sortOrder"), QStringLiteral("Descending"));

        QVERIFY(again->openType(QStringLiteral("tvshows")));
        QTRY_VERIFY(!again->shows()->resolvingFacets());
        QVERIFY(again->shows()->viewState() == seriesView);
        // Another scope of the same type has its own, untouched view
        QVERIFY(again->openType(QStringLiteral("movies"), kMoviesA));
        QVERIFY(again->movies()->viewState() == Model::ViewState());
    }

    void editsOfTheViewOptionsAreSavedPerScope()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesB));
            controller->movies()->setViewMode(QStringLiteral("list"));
            controller->movies()->setImageType(QStringLiteral("Disc"));
            controller->movies()->setShowYear(false);
            QVERIFY(controller->openType(QStringLiteral("movies"), kMoviesA));
            controller->movies()->setImageType(QStringLiteral("Logo"));
            controller->movies()->setCardLayout(false);
        }
        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 3));
        QVERIFY(again->openType(QStringLiteral("movies"), kMoviesB));
        QCOMPARE(again->movies()->viewMode(), QStringLiteral("list"));
        QCOMPARE(again->movies()->imageType(), QStringLiteral("Disc"));
        QVERIFY(!again->movies()->showYear());
        QVERIFY(again->movies()->showTitle());
        QVERIFY(again->movies()->cardLayout());
        QVERIFY(again->openType(QStringLiteral("movies"), kMoviesA));
        QCOMPARE(again->movies()->viewMode(), QStringLiteral("grid"));
        QCOMPARE(again->movies()->imageType(), QStringLiteral("Logo"));
        QVERIFY(again->movies()->showYear());
        QVERIFY(!again->movies()->cardLayout());
    }

    void savedTagsAndRatingsWaitForTheFacetsAfterARestart()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            QVERIFY(controller->openType(QStringLiteral("movies")));
            QVERIFY(facetsLoaded(controller->movies()));
            Kinefin::LibraryFilterState filters;
            filters.tags = {QStringLiteral("restored-master")};
            filters.officialRatings = {QStringLiteral("PG-13")};
            controller->movies()->setFilterState(filters);
        }
        server.listingStatus.insert(QStringLiteral("Filters"), 500);
        {
            auto again = makeController();
            QVERIFY(signInWithLibraries(*again, server, 3));
            const int before = int(server.itemsRoutes.size());
            QVERIFY(again->openType(QStringLiteral("movies")));
            QTRY_COMPARE(again->movies()->filterOptions()->status(),
                         Kinefin::FilterOptionsModel::Failed);
            // The facet that names the tag is missing: nothing wider than the saved view is sent, and the view offers a retry
            QCOMPARE(again->movies()->status(), Model::Failed);
            QVERIFY(again->movies()->resolvingFacets());
            QTest::qWait(100);
            QCOMPARE(int(server.itemsRoutes.size()), before);
            // An edit made meanwhile keeps both saved selections
            again->movies()->setSortBy(QStringLiteral("DateCreated"));
            QCOMPARE(again->movies()->viewState().pendingTagHashes,
                     QStringList({digestOf("restored-master")}));
            // The legacy facets did load: the rating is a text already, only the tag still waits, and the rating's digest stays saved
            QVERIFY(again->movies()->viewState().pendingRatingHashes.isEmpty());
            QCOMPARE(again->movies()->filterState().officialRatings,
                     QStringList({QStringLiteral("PG-13")}));
            server.listingStatus.clear();
            again->movies()->retry();
            QTRY_COMPARE(again->movies()->status(), Model::Ready);
            QCOMPARE(int(server.itemsRoutes.size()), before + 1);
            QCOMPARE(wire(server.itemsRoutes.constLast(), "tags"),
                     QStringLiteral("restored-master"));
            QCOMPARE(wire(server.itemsRoutes.constLast(), "officialRatings"),
                     QStringLiteral("PG-13"));
            QCOMPARE(wire(server.itemsRoutes.constLast(), "sortBy"),
                     QStringLiteral("DateCreated,SortName"));
        }
        // What was saved while the facets were missing still names the tag and the rating
        const QByteArray bytes = browseBytes();
        QVERIFY(bytes.contains(digestOf("restored-master").toUtf8()));
        QVERIFY(bytes.contains(digestOf("PG-13").toUtf8()));
        QVERIFY(!bytes.contains("restored-master"));
        auto third = makeController();
        QVERIFY(signInWithLibraries(*third, server, 3));
        QVERIFY(third->openType(QStringLiteral("movies")));
        QTRY_VERIFY(!third->movies()->resolvingFacets());
        QCOMPARE(third->movies()->sortBy(), QStringLiteral("DateCreated"));
        QCOMPARE(third->movies()->filterState().tags,
                 QStringList({QStringLiteral("restored-master")}));
        QCOMPARE(third->movies()->filterState().officialRatings,
                 QStringList({QStringLiteral("PG-13")}));
    }

    void aRenamedTagIsDroppedAfterARestartAndTheFileIsLeftAlone()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            QVERIFY(controller->openType(QStringLiteral("movies")));
            QVERIFY(facetsLoaded(controller->movies()));
            Kinefin::LibraryFilterState filters;
            filters.tags = {QStringLiteral("restored-master"), QStringLiteral("black-and-white")};
            controller->movies()->setFilterState(filters);
        }
        const QByteArray before = browseBytes();
        QVERIFY(!before.isEmpty());
        // The server renamed one tag
        server.listingRawBody.insert(
            QStringLiteral("Filters"),
            QByteArrayLiteral("{\"Genres\":[],\"Tags\":[\"black-and-white\","
                              "\"restored-print\"]}"));
        {
            auto again = makeController();
            QVERIFY(signInWithLibraries(*again, server, 3));
            // Requests the ended session still had in flight reach the server late: count from here
            QTest::qWait(100);
            const int routesBefore = int(server.itemsRoutes.size());
            QSignalSpy edits(again->movies(), &Model::viewStateEdited);
            QVERIFY(again->openType(QStringLiteral("movies")));
            QTRY_VERIFY(!again->movies()->resolvingFacets());
            QCOMPARE(again->movies()->filterState().tags,
                     QStringList({QStringLiteral("black-and-white")}));
            QTRY_COMPARE(again->movies()->status(), Model::Ready);
            QCOMPARE(int(server.itemsRoutes.size()), routesBefore + 1);
            QCOMPARE(wire(server.itemsRoutes.constLast(), "tags"),
                     QStringLiteral("black-and-white"));
            QCOMPARE(edits.size(), 0);
        }
        QCOMPARE(browseBytes(), before);
    }

    void aMissingLegacyEndpointDropsTheSavedRatingAfterARestart()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            QVERIFY(controller->openType(QStringLiteral("movies")));
            QVERIFY(facetsLoaded(controller->movies()));
            Kinefin::LibraryFilterState filters;
            filters.tags = {QStringLiteral("restored-master")};
            filters.officialRatings = {QStringLiteral("PG-13")};
            controller->movies()->setFilterState(filters);
        }
        server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 404);
        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 3));
        // Requests the ended session still had in flight reach the server late: count from here
        QTest::qWait(100);
        const int before = int(server.itemsRoutes.size());
        QVERIFY(again->openType(QStringLiteral("movies")));
        // The view is not blocked for good by a rating nothing can name
        QTRY_COMPARE(again->movies()->status(), Model::Ready);
        QVERIFY(!again->movies()->resolvingFacets());
        QVERIFY(again->movies()->filterState().officialRatings.isEmpty());
        QCOMPARE(int(server.itemsRoutes.size()), before + 1);
        QCOMPARE(wire(server.itemsRoutes.constLast(), "tags"), QStringLiteral("restored-master"));
        QVERIFY(wire(server.itemsRoutes.constLast(), "officialRatings").isEmpty());
    }

    void aVersionOneFileMigratesThroughTheController()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        server.viewsRawBody = twoMovieLibrariesBody();
        const QJsonObject view{{QStringLiteral("sort"), QStringLiteral("DateAdded")},
                               {QStringLiteral("descending"), true},
                               {QStringLiteral("filter"), QStringLiteral("Unplayed")},
                               {QStringLiteral("genreIds"), QJsonArray{kSciFi}},
                               {QStringLiteral("seq"), 1}};
        const QJsonObject user{
            {QStringLiteral("selection"), QJsonObject{{QStringLiteral("movies"), kMoviesB}}},
            {QStringLiteral("views"),
             QJsonObject{{QString(QStringLiteral("movies:") + kMoviesB), view}}}};
        const QJsonObject root{
            {QStringLiteral("version"), 1},
            {QStringLiteral("nextSeq"), 2},
            {QStringLiteral("servers"),
             QJsonObject{{kServerId, QJsonObject{{QStringLiteral("users"),
                                                  QJsonObject{{kFixtureUserId, user}}}}}}}};
        QVERIFY(QDir().mkpath(QFileInfo(browseFile()).absolutePath()));
        QFile file(browseFile());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.close();
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            QVERIFY(controller->openType(QStringLiteral("movies")));
            QCOMPARE(controller->movies()->libraryId(), kMoviesB);
            QCOMPARE(controller->movies()->sortBy(), QStringLiteral("DateCreated"));
            QVERIFY(controller->movies()->descending());
            QCOMPARE(controller->movies()->filter(), Model::Unplayed);
            QCOMPARE(controller->movies()->genreIds(), QStringList({kSciFi}));
            // The new fields start from their defaults
            QCOMPARE(controller->movies()->viewMode(), QStringLiteral("grid"));
            QCOMPARE(controller->movies()->imageType(), QStringLiteral("Primary"));
            QVERIFY(controller->movies()->showTitle() && controller->movies()->showYear()
                    && controller->movies()->cardLayout());
            QTRY_COMPARE(controller->movies()->genreIds(), QStringList({kSciFi}));
            // The next save writes version 2 and keeps what was there
            controller->movies()->setViewMode(QStringLiteral("list"));
        }
        const QJsonObject migrated = QJsonDocument::fromJson(browseBytes()).object();
        QCOMPARE(migrated.value(QStringLiteral("version")).toInt(), 3);
        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 3));
        QVERIFY(again->openType(QStringLiteral("movies")));
        QCOMPARE(again->movies()->sortBy(), QStringLiteral("DateCreated"));
        QCOMPARE(again->movies()->filter(), Model::Unplayed);
        QCOMPARE(again->movies()->genreIds(), QStringList({kSciFi}));
        QCOMPARE(again->movies()->viewMode(), QStringLiteral("list"));
    }

    void oneOffKeepsTheStoredViewOptionsAndFilters()
    {
        using Model = Kinefin::LibraryItemsModel;
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.generatedItemTotal = 5;
        {
            auto controller = makeController();
            QVERIFY(signInWithLibraries(*controller, server, 3));
            QVERIFY(controller->openType(QStringLiteral("movies")));
            controller->movies()->setSortKey(Model::Runtime);
            controller->movies()->setFilter(Model::Unplayed);
            controller->movies()->setViewMode(QStringLiteral("list"));
            controller->movies()->setShowYear(false);
            QVERIFY(controller->openRecentlyAdded(QStringLiteral("movies")));
            QCOMPARE(controller->movies()->sortKey(), Model::DateAdded);
            QCOMPARE(controller->movies()->filter(), Model::All);
            QCOMPARE(controller->movies()->viewMode(), QStringLiteral("list"));
            QVERIFY(!controller->movies()->showYear());
            // Changing how it is shown is not a choice of order or filters: the stored ones stay
            controller->movies()->setViewMode(QStringLiteral("grid"));
        }
        auto again = makeController();
        QVERIFY(signInWithLibraries(*again, server, 3));
        QVERIFY(again->openType(QStringLiteral("movies")));
        QCOMPARE(again->movies()->sortKey(), Model::Runtime);
        QCOMPARE(again->movies()->filter(), Model::Unplayed);
        QCOMPARE(again->movies()->viewMode(), QStringLiteral("grid"));
        QVERIFY(!again->movies()->showYear());
    }

    void scopeKeysCoverTheSubtabs()
    {
        using Controller = Kinefin::ConnectionController;
        const QString dashed = QStringLiteral("B2B2B2B2-B2B2-B2B2-B2B2-B2B2B2B2B2B2");
        const QString id = kMoviesB;
        const QString movies = QStringLiteral("movies");
        const QString shows = QStringLiteral("tvshows");
        // The tab scopes keep the keys they always had
        QCOMPARE(Controller::scopeKey(movies, {}, {}), QStringLiteral("movies"));
        QCOMPARE(Controller::scopeKey(shows, {}, dashed), QString(QStringLiteral("tvshows:") + id));
        QCOMPARE(Controller::scopeKey(QStringLiteral("boxsets"), {}, id),
                 QString(QStringLiteral("boxsets:") + id));
        QCOMPARE(Controller::scopeKey(movies, QStringLiteral("favorites"), {}),
                 QStringLiteral("movies/favorites"));
        QCOMPARE(Controller::scopeKey(movies, QStringLiteral("favorites"), dashed),
                 QString(QStringLiteral("movies/favorites:") + id));
        // Collections belong to no one library
        QCOMPARE(Controller::scopeKey(movies, QStringLiteral("collections"), {}),
                 QStringLiteral("movies/collections"));
        QCOMPARE(Controller::scopeKey(movies, QStringLiteral("collections"), id),
                 QStringLiteral("movies/collections"));
        QCOMPARE(Controller::scopeKey(shows, QStringLiteral("favorites"), {}),
                 QStringLiteral("tvshows/favorites"));
        QCOMPARE(Controller::scopeKey(shows, QStringLiteral("favorites"), dashed),
                 QString(QStringLiteral("tvshows/favorites:") + id));
        // The collections list is the same under both types
        QCOMPARE(Controller::scopeKey(shows, QStringLiteral("collections"), id),
                 QStringLiteral("movies/collections"));
        QCOMPARE(Controller::scopeKey(shows, QStringLiteral("episodes"), {}),
                 QStringLiteral("tvshows/episodes"));
        QCOMPARE(Controller::scopeKey(shows, QStringLiteral("episodes"), id),
                 QString(QStringLiteral("tvshows/episodes:") + id));
        QCOMPARE(Controller::scopeKey(shows, QStringLiteral("network"), dashed),
                 QString(QStringLiteral("tvshows/network:") + id));
        // A network needs its studio, and a sub-tab belongs to its own type
        QVERIFY(Controller::scopeKey(shows, QStringLiteral("network"), {}).isEmpty());
        QVERIFY(Controller::scopeKey(QStringLiteral("boxsets"), QStringLiteral("favorites"), {})
                    .isEmpty());
        QVERIFY(Controller::scopeKey(movies, QStringLiteral("episodes"), {}).isEmpty());
        QVERIFY(Controller::scopeKey(movies, QStringLiteral("unknown"), {}).isEmpty());

        // The store accepts every one of them
        Kinefin::BrowseStateStore store;
        Kinefin::BrowseStateStore::ViewState state;
        state.viewMode = QStringLiteral("list");
        const QList<QString> keys{Controller::scopeKey(movies, QStringLiteral("favorites"), {}),
                                  Controller::scopeKey(movies, QStringLiteral("favorites"), id),
                                  Controller::scopeKey(movies, QStringLiteral("collections"), {}),
                                  Controller::scopeKey(shows, QStringLiteral("favorites"), {}),
                                  Controller::scopeKey(shows, QStringLiteral("favorites"), id),
                                  Controller::scopeKey(shows, QStringLiteral("episodes"), {}),
                                  Controller::scopeKey(shows, QStringLiteral("episodes"), id),
                                  Controller::scopeKey(shows, QStringLiteral("network"), id)};
        for (const QString &key : keys) {
            store.setView(kServerId, kFixtureUserId, key, state);
            QVERIFY2(store.view(kServerId, kFixtureUserId, key).has_value(), qPrintable(key));
        }
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
        // The sign-ins of the first controller each read the user's configuration: let the last one reach the server before counting
        QTest::qWait(300);
        const int logins = server.successfulLogins;
        const int checks = server.usersMeRequests;
        auto controller = makeController();
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::ChoosingUser);
        controller->chooseUser(kAnna);
        QCOMPARE(controller->state(), ConnectionController::Connecting);
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        // One check of the session and one read of the user's configuration for Home
        QTRY_COMPARE(server.usersMeRequests, checks + 2);
        QTest::qWait(100);
        QCOMPARE(server.usersMeRequests, checks + 2);
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
        // The password signs the user back in; a real server issues a new token, the fake reissues the user's fixed one, so the revocation ends here (the configuration read after sign-in would meet it)
        server.deniedUsersMeTokens.clear();
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

    void currentUserIsAdministratorFollowsTheSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.administrator = false;
        auto controller = makeController();
        QSignalSpy changed(controller.get(),
                           &ConnectionController::currentUserIsAdministratorChanged);
        QVERIFY(!controller->currentUserIsAdministrator());
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QVERIFY(!controller->currentUserIsAdministrator());
        QCOMPARE(changed.size(), 0);
        controller->signOut();
        QCOMPARE(changed.size(), 0);

        // An administrator signing in: the flag is there with the session
        server.administrator = true;
        controller->connectTo(address(server));
        QTRY_VERIFY(controller->state() == ConnectionController::NeedsLogin);
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QVERIFY(controller->currentUserIsAdministrator());
        QCOMPARE(changed.size(), 1);

        // Leaving the server clears it, the saved session brings it back once the re-check answered
        controller->cancel();
        QVERIFY(!controller->currentUserIsAdministrator());
        QCOMPARE(changed.size(), 2);
        controller->start();
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QTRY_VERIFY(controller->currentUserIsAdministrator());
        QCOMPARE(changed.size(), 3);

        // A revoked token ends the restored session and nothing of the flag is left
        controller->cancel();
        QCOMPARE(changed.size(), 4);
        server.denyUsersMe = true;
        controller->start();
        QTRY_COMPARE(controller->state(), ConnectionController::NeedsLogin);
        QVERIFY(!controller->currentUserIsAdministrator());
        QCOMPARE(changed.size(), 4);

        // Signing in again, then out
        server.denyUsersMe = false;
        controller->login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(controller->state() == ConnectionController::Authenticated);
        QVERIFY(controller->currentUserIsAdministrator());
        QCOMPARE(changed.size(), 5);
        controller->signOut();
        QVERIFY(!controller->currentUserIsAdministrator());
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

    // Home shows the tiles the configuration orders and does not hide, and Recently Added rows for the libraries it does not exclude; `libraries` and the Movies tab stay whole, and an id that names no library is ignored
    void homeProjectsTheConfigurationOntoTilesAndRows()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.viewsRawBody = twoMovieLibrariesBody();
        const QString unknown = QStringLiteral("f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0");
        server.usersMeRawBody = meBody(
            kFixtureUserId,
            QJsonObject{{QStringLiteral("OrderedViews"), QJsonArray{kShows, unknown, kMoviesB}},
                        {QStringLiteral("MyMediaExcludes"), QJsonArray{kMoviesB}},
                        {QStringLiteral("LatestItemsExcludes"), QJsonArray{kMoviesA, unknown}},
                        {QStringLiteral("HidePlayedInLatest"), true}});
        auto controller = makeController();
        QVERIFY(signInFixtureUser(*controller, server));
        QTRY_COMPARE(controller->homeLibraries().size(), 2);
        QCOMPARE(idsOf(controller->libraries()), (QStringList{kMoviesA, kMoviesB, kShows}));
        // The listed order first (the hidden one dropped), the library the order does not name after them
        QCOMPARE(idsOf(controller->homeLibraries()), (QStringList{kShows, kMoviesA}));
        // Server order; a library either exclude list names loses its Recently Added row
        QCOMPARE(idsOf(controller->recentLibraries()), (QStringList{kShows}));
        QCOMPARE(idsOf(controller->movieLibraries()), (QStringList{kMoviesA, kMoviesB}));
        QTRY_COMPARE(recentRowIds(controller->homeRows()), (QStringList{kShows}));
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Latest")).size(), 1);
        QVERIFY(latestAsksUnplayedOnly(server.listingRoutes.value(QStringLiteral("Latest"))));

        // The libraries change: the projection follows them
        server.viewsRawBody =
            viewsBody({{kMoviesA, QStringLiteral("movies")}, {kShows, QStringLiteral("tvshows")}});
        controller->reloadLibraries();
        QTRY_COMPARE(idsOf(controller->libraries()), (QStringList{kMoviesA, kShows}));
        QCOMPARE(idsOf(controller->homeLibraries()), (QStringList{kShows, kMoviesA}));
        QCOMPARE(idsOf(controller->recentLibraries()), (QStringList{kShows}));
        QTRY_COMPARE(recentRowIds(controller->homeRows()), (QStringList{kShows}));
    }

    void homeAsksForAllItemsWhenPlayedOnesAreKept()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.viewsRawBody = twoMovieLibrariesBody();
        server.usersMeRawBody =
            meBody(kFixtureUserId, QJsonObject{{QStringLiteral("HidePlayedInLatest"), false}});
        auto controller = makeController();
        QVERIFY(signInFixtureUser(*controller, server));
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Latest")).size(), 3);
        QVERIFY(latestAsksNoPlayedFilter(server.listingRoutes.value(QStringLiteral("Latest"))));
        QCOMPARE(controller->homeLibraries().size(), 3);
        QCOMPARE(controller->recentLibraries().size(), 3);
    }

    // No error banner, the unprojected Home, and the next refresh tries the configuration again
    void homeFallsBackWhenTheConfigurationCannotBeLoaded()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.viewsRawBody = twoMovieLibrariesBody();
        server.usersMeStatus = 500;
        auto controller = makeController();
        QVERIFY(signInFixtureUser(*controller, server));
        QTRY_COMPARE(controller->homeLibraries().size(), 3);
        QCOMPARE(idsOf(controller->homeLibraries()), idsOf(controller->libraries()));
        QCOMPARE(idsOf(controller->recentLibraries()), idsOf(controller->libraries()));
        QCOMPARE(controller->userConfiguration()->state(), Kinefin::UserConfigurationModel::Failed);
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Latest")).size(), 3);
        QVERIFY(latestAsksNoPlayedFilter(server.listingRoutes.value(QStringLiteral("Latest"))));
        QVERIFY(controller->errorText().isEmpty());

        server.usersMeStatus = 0;
        server.usersMeRawBody =
            meBody(kFixtureUserId,
                   QJsonObject{{QStringLiteral("MyMediaExcludes"), QJsonArray{kMoviesA}},
                               {QStringLiteral("LatestItemsExcludes"), QJsonArray{kMoviesB}}});
        controller->reloadLibraries();
        QTRY_COMPARE(idsOf(controller->homeLibraries()), (QStringList{kMoviesB, kShows}));
        QCOMPARE(idsOf(controller->recentLibraries()), (QStringList{kShows}));
        QTRY_COMPARE(recentRowIds(controller->homeRows()), (QStringList{kShows}));
        QTest::qWait(200);
        QVERIFY(server.lastListingRoute.value(QStringLiteral("Latest")).contains("isPlayed=false"));
        QVERIFY(controller->errorText().isEmpty());
    }

    // Nothing of the previous user's configuration survives a switch, on the Home side or in the editor
    void switchingUserClearsTheHomeProjection()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        server.viewsRawBody = twoMovieLibrariesBody();
        server.onUsersMe = [&server] {
            if (server.lastUsersMeAuth.contains("annatoken"))
                server.usersMeRawBody = meBody(
                    kAnna,
                    QJsonObject{{QStringLiteral("MyMediaExcludes"), QJsonArray{kMoviesA}},
                                {QStringLiteral("LatestItemsExcludes"), QJsonArray{kShows}}});
            else
                server.usersMeRawBody = meBody(kBen, QJsonObject());
        };
        auto controller = makeController();
        controller->connectTo(address(server));
        QVERIFY(waitForState(*controller, ConnectionController::NeedsLogin));
        QVERIFY(signInUser(*controller, QStringLiteral("Anna"), 1));
        QTRY_COMPARE(controller->homeLibraries().size(), 2);
        QCOMPARE(idsOf(controller->recentLibraries()), (QStringList{kMoviesB}));
        QTRY_COMPARE(recentRowIds(controller->homeRows()), (QStringList{kMoviesB}));

        controller->switchUser();
        QVERIFY(controller->homeLibraries().isEmpty());
        QVERIFY(controller->recentLibraries().isEmpty());
        QCOMPARE(controller->homeRows()->rowCount(), 0);
        QVERIFY(!controller->userConfiguration()->loaded());
        QVERIFY(controller->userConfiguration()->myMediaExcludes().isEmpty());

        controller->addUser();
        QVERIFY(signInUser(*controller, QStringLiteral("Ben"), 2));
        QTRY_COMPARE(controller->homeLibraries().size(), 3);
        QCOMPARE(idsOf(controller->homeLibraries()), idsOf(controller->libraries()));
        QCOMPARE(idsOf(controller->recentLibraries()), idsOf(controller->libraries()));
        QVERIFY(controller->userConfiguration()->myMediaExcludes().isEmpty());
        QTRY_COMPARE(recentRowIds(controller->homeRows()),
                     (QStringList{kMoviesA, kMoviesB, kShows}));
    }

    // A save the editor verified reaches Home without a reload of the libraries
    void aVerifiedSaveUpdatesTheHomeProjection()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.viewsRawBody = twoMovieLibrariesBody();
        QJsonObject remote;
        server.onUsersMe = [&] { server.usersMeRawBody = meBody(kFixtureUserId, remote); };
        server.onUserConfigurationPost = [&](const QByteArray &body) {
            remote = QJsonDocument::fromJson(body).object();
        };
        auto controller = makeController();
        QVERIFY(signInFixtureUser(*controller, server));
        QTRY_COMPARE(controller->homeLibraries().size(), 3);
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Latest")).size(), 3);
        QVERIFY(latestAsksUnplayedOnly(server.listingRoutes.value(QStringLiteral("Latest"))));
        Kinefin::UserConfigurationModel *prefs = controller->userConfiguration();
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Ready);

        prefs->setMyMediaExcluded(kMoviesA, true);
        prefs->setLatestExcluded(kShows, true);
        prefs->setHidePlayedInLatest(false);
        QSignalSpy saved(prefs, &Kinefin::UserConfigurationModel::saved);
        prefs->save();
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(idsOf(controller->homeLibraries()), (QStringList{kMoviesB, kShows}));
        QCOMPARE(idsOf(controller->recentLibraries()), (QStringList{kMoviesB}));
        QTRY_COMPARE(recentRowIds(controller->homeRows()), (QStringList{kMoviesB}));
        QTRY_COMPARE(server.listingRoutes.value(QStringLiteral("Latest")).size(), 4);
        QVERIFY(latestAsksNoPlayedFilter(server.listingRoutes.value(QStringLiteral("Latest")), 3));
    }

    // A user who has to type the password cannot be that user without a server: Work Offline is not offered for them
    void workOfflineIsRefusedForAnAskPasswordUser()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        auto controller = makeController();
        QVERIFY(signInWithADownload(*controller, server));
        QVERIFY(controller->canWorkOffline());
        // The property follows the records at most once a second
        QTRY_VERIFY(controller->property("offlineAvailable").toBool());
        QSignalSpy availability(controller.get(), &ConnectionController::offlineAvailableChanged);

        controller->setUserAskPassword(kAnna, true);
        QVERIFY(userEntry(*controller, kAnna).value(QStringLiteral("askPassword")).toBool());
        QVERIFY(!controller->canWorkOffline());
        QTRY_COMPARE(availability.count(), 1);
        QVERIFY(!controller->property("offlineAvailable").toBool());
        controller->cancel();
        QTRY_COMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(!controller->canWorkOffline());
        QVERIFY(!controller->workOffline());
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(!controller->offline());
    }

    // A user whose token was dropped (not remembered, and not asking for the password either) has no session to fall back on
    void workOfflineIsRefusedForAUserThatIsNotRemembered()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        auto controller = makeController();
        QVERIFY(signInWithADownload(*controller, server));
        QVERIFY(controller->canWorkOffline());

        controller->setUserAskPassword(kAnna, true);
        controller->setUserAskPassword(kAnna, false);
        QVERIFY(!userEntry(*controller, kAnna).value(QStringLiteral("askPassword")).toBool());
        QVERIFY(!userEntry(*controller, kAnna).value(QStringLiteral("remembered")).toBool());
        QVERIFY(!controller->canWorkOffline());
        QVERIFY(!controller->property("offlineAvailable").toBool());
        controller->cancel();
        QTRY_COMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(!controller->workOffline());
        QCOMPARE(controller->state(), ConnectionController::Idle);
    }

    // Cancel and Sign Out from Offline end the state only: the saved user and the downloads stay
    void cancelAndSignOutFromOfflineReturnToIdle()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        addTwoUsers(server);
        auto controller = makeController();
        QVERIFY(signInWithADownload(*controller, server));
        controller->cancel();
        QTRY_COMPARE(controller->state(), ConnectionController::Idle);

        QVERIFY(controller->workOffline());
        QCOMPARE(controller->state(), ConnectionController::Offline);
        QCOMPARE(controller->offlineUserName(), QStringLiteral("Anna"));
        QCOMPARE(controller->downloads()->ownerUserId(), kAnna);
        controller->cancel();
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(!controller->offline());
        QVERIFY(controller->offlineUserName().isEmpty());
        QVERIFY(controller->downloads()->ownerUserId().isEmpty());
        QVERIFY(controller->canWorkOffline());

        QVERIFY(controller->workOffline());
        QCOMPARE(controller->state(), ConnectionController::Offline);
        controller->signOut();
        QCOMPARE(controller->state(), ConnectionController::Idle);
        QVERIFY(controller->offlineUserName().isEmpty());
        QVERIFY(controller->downloads()->ownerUserId().isEmpty());
        QVERIFY(controller->canWorkOffline());
    }

  private:
    std::unique_ptr<QTemporaryDir> m_home;
};

QTEST_GUILESS_MAIN(TestConnectionController)
#include "test_connection_controller.moc"
