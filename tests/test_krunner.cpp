// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The org.kde.krunner1 runner on a private session bus (dbus-run-session): KRunner's side is a
// second connection of this test, the fake Jellyfin server the search behind it
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "integration/KRunnerProvider.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

using Kinefin::JellyfinClient;
using Kinefin::KRunnerAction;
using Kinefin::KRunnerMatch;
using Kinefin::KRunnerProvider;

namespace {
const QString kService = QString::fromLatin1(KRunnerProvider::ServiceName);
const QString kPath = QString::fromLatin1(KRunnerProvider::ObjectPath);
const QString kInterface = QStringLiteral("org.kde.krunner1");

const QString kMovie = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");
const QString kResumable = QStringLiteral("9a8b7c6d5e4f3827160594a3b2c1d0e1");
const QString kSeries = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
const QString kEpisode = QStringLiteral("bb22cc33dd44ee55ff66778899001122");
const QString kBoxSet = QStringLiteral("cc33dd44ee55ff667788990011223344");
const QString kVirtual = QStringLiteral("dd44ee55ff6677889900112233445566");
const QString kHostile = QStringLiteral("ee55ff66778899001122334455667788");
const QString kMissing = QStringLiteral("00112233445566778899aabbccddeeff");

QByteArray searchBody()
{
    const QString hostileName =
        QStringLiteral("<b>Bold</b> & \u0007Danger ") + QString(300, QLatin1Char('x'));
    const QJsonArray items{
        QJsonObject{{QStringLiteral("Id"), kMovie},
                    {QStringLiteral("Name"), QStringLiteral("Arcturus")},
                    {QStringLiteral("Type"), QStringLiteral("Movie")},
                    {QStringLiteral("ProductionYear"), 2012}},
        QJsonObject{{QStringLiteral("Id"), kResumable},
                    {QStringLiteral("Name"), QStringLiteral("Hollow Interval")},
                    {QStringLiteral("Type"), QStringLiteral("Movie")},
                    {QStringLiteral("ProductionYear"), 2019},
                    {QStringLiteral("UserData"),
                     QJsonObject{{QStringLiteral("PlaybackPositionTicks"), 600'000'000}}}},
        QJsonObject{{QStringLiteral("Id"), kSeries},
                    {QStringLiteral("Name"), QStringLiteral("Abyss Station")},
                    {QStringLiteral("Type"), QStringLiteral("Series")},
                    {QStringLiteral("ProductionYear"), 2021}},
        QJsonObject{{QStringLiteral("Id"), kEpisode},
                    {QStringLiteral("Name"), QStringLiteral("Weightless")},
                    {QStringLiteral("Type"), QStringLiteral("Episode")},
                    {QStringLiteral("SeriesName"), QStringLiteral("Abyss Station")},
                    {QStringLiteral("ParentIndexNumber"), 2},
                    {QStringLiteral("IndexNumber"), 6}},
        QJsonObject{{QStringLiteral("Id"), kBoxSet},
                    {QStringLiteral("Name"), QStringLiteral("Favorites")},
                    {QStringLiteral("Type"), QStringLiteral("BoxSet")}},
        // A library entry with no file never reaches KRunner
        QJsonObject{{QStringLiteral("Id"), kVirtual},
                    {QStringLiteral("Name"), QStringLiteral("Virtual Episode")},
                    {QStringLiteral("Type"), QStringLiteral("Episode")},
                    {QStringLiteral("LocationType"), QStringLiteral("Virtual")}},
        // Server text: markup, a control and more than the cap
        QJsonObject{{QStringLiteral("Id"), kHostile},
                    {QStringLiteral("Name"), hostileName},
                    {QStringLiteral("Type"), QStringLiteral("Movie")},
                    {QStringLiteral("ProductionYear"), 2020}}};
    return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                     {QStringLiteral("TotalRecordCount"), items.size()}})
        .toJson(QJsonDocument::Compact);
}

// One signed-in client, its fake server and the runner on its own bus connection; the test calls
// from the default one, as KRunner does from another process (a shared connection would make the
// delayed Match reply a local-loop message)
struct Chain {
    struct Disconnect {
        ~Disconnect() { QDBusConnection::disconnectFromBus(QStringLiteral("kinefin-test-runner")); }
    };
    explicit Chain(bool signIn = true)
        : bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                            QStringLiteral("kinefin-test-runner"))),
          provider(client, bus)
    {
        ok = server.listen() && (!signIn || login(client, server)) && provider.registerOnBus();
        server.listingRawBody.insert(QStringLiteral("Items"), searchBody());
    }
    Disconnect disconnect;
    FakeJellyfin server;
    JellyfinClient client;
    QDBusConnection bus;
    KRunnerProvider provider;
    bool ok = false;
};

QDBusPendingCall startCall(const QString &method, const QVariantList &arguments = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, method);
    message.setArguments(arguments);
    return QDBusConnection::sessionBus().asyncCall(message);
}

QDBusMessage waitFor(QDBusPendingCall pending)
{
    QDBusPendingCallWatcher watcher(pending);
    if (!watcher.isFinished())
        (void)QTest::qWaitFor([&watcher] { return watcher.isFinished(); }, 6000);
    return watcher.isFinished() ? watcher.reply() : QDBusMessage();
}

QDBusMessage call(const QString &method, const QVariantList &arguments = {})
{
    return waitFor(startCall(method, arguments));
}

QList<KRunnerMatch> matchesOf(const QDBusMessage &reply)
{
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1)
        return {};
    return qdbus_cast<QList<KRunnerMatch>>(reply.arguments().constFirst().value<QDBusArgument>());
}

QList<KRunnerAction> actionsOf(const QDBusMessage &reply)
{
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1)
        return {};
    return qdbus_cast<QList<KRunnerAction>>(reply.arguments().constFirst().value<QDBusArgument>());
}
} // namespace

class TestKRunner : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void initTestCase()
    {
        qDBusRegisterMetaType<KRunnerMatch>();
        qDBusRegisterMetaType<KRunnerAction>();
        qDBusRegisterMetaType<QList<KRunnerMatch>>();
        qDBusRegisterMetaType<QList<KRunnerAction>>();
    }

    void registrationAndActions()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(QDBusConnection::sessionBus().interface()->isServiceRegistered(kService));
        const QDBusMessage reply = call(QStringLiteral("Actions"));
        QCOMPARE(reply.signature(), QStringLiteral("a(sss)"));
        QCOMPARE(actionsOf(reply).size(), 2);
        const QList<KRunnerAction> actions = actionsOf(reply);
        QCOMPARE(actions.at(0).id, QStringLiteral("play"));
        QCOMPARE(actions.at(1).id, QStringLiteral("resume"));
        QCOMPARE(actions.at(0).iconName, QStringLiteral("media-playback-start"));
        // A second runner cannot take the object path or the name
        Kinefin::KRunnerProvider second(chain.client, QDBusConnection::sessionBus());
        QVERIFY(!second.registerOnBus());
    }

    void shortQueriesAndSignedOutSendNothing()
    {
        Chain chain(false);
        QVERIFY(chain.ok);
        QVERIFY(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("arct")})).isEmpty());
        QCOMPARE(chain.server.listingRequests, 0);
        QVERIFY(login(chain.client, chain.server));
        // KRunner's own minimum is 3; a single-runner session may skip that check
        QVERIFY(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("ar")})).isEmpty());
        QCOMPARE(chain.server.listingRequests, 0);
    }

    void searchMatchesAndCleaning()
    {
        Chain chain;
        QVERIFY(chain.ok);
        const QDBusMessage reply = call(QStringLiteral("Match"), {QStringLiteral("arct")});
        QCOMPARE(reply.signature(), QStringLiteral("a(sssida{sv})"));
        const QList<KRunnerMatch> matches = matchesOf(reply);
        // Six rows: the virtual episode and the invalid ids stay out
        QCOMPARE(matches.size(), 6);
        QCOMPARE(matches.at(0).id, kMovie);
        QCOMPARE(matches.at(0).text, QStringLiteral("Arcturus"));
        QCOMPARE(matches.at(0).iconName, QStringLiteral("video-x-generic"));
        QCOMPARE(matches.at(0).categoryRelevance, 30);
        QCOMPARE(matches.at(0).properties.value(QStringLiteral("subtext")).toString(),
                 QStringLiteral("Movie, 2012"));
        QCOMPARE(matches.at(0).properties.value(QStringLiteral("actions")).toStringList(),
                 QStringList{QStringLiteral("play")});
        QCOMPARE(matches.at(1).id, kResumable);
        QCOMPARE(matches.at(1).properties.value(QStringLiteral("actions")).toStringList(),
                 QStringList{QStringLiteral("resume")});
        QCOMPARE(matches.at(2).iconName, QStringLiteral("video-television"));
        QCOMPARE(matches.at(2).properties.value(QStringLiteral("subtext")).toString(),
                 QStringLiteral("Series, 2021"));
        QCOMPARE(matches.at(2).properties.value(QStringLiteral("actions")).toStringList(),
                 QStringList{});
        QCOMPARE(matches.at(3).properties.value(QStringLiteral("subtext")).toString(),
                 QStringLiteral("Abyss Station, S2:E6"));
        QCOMPARE(matches.at(4).properties.value(QStringLiteral("subtext")).toString(),
                 QStringLiteral("Collection"));
        QCOMPARE(matches.at(4).iconName, QStringLiteral("folder-videos"));
        // Server text: no tag survives, no control, 200 code points at most
        const QString hostile = matches.at(5).text;
        QVERIFY(!hostile.contains(QLatin1Char('<')));
        QVERIFY(hostile.startsWith(QStringLiteral("\uFF1Cb>Bold")));
        QVERIFY(!hostile.contains(QChar(0x07)));
        QCOMPARE(hostile.size(), 200);
        QVERIFY(!hostile.back().isHighSurrogate());
        // The one request carries the term and the four kinds
        const QByteArray route = chain.server.lastListingRoute.value(QStringLiteral("Items"));
        QVERIFY(route.contains("searchTerm=arct"));
        QVERIFY(route.contains("includeItemTypes=Movie,Series,BoxSet,Episode")
                || route.contains("includeItemTypes=Movie%2CSeries%2CBoxSet%2CEpisode"));
        QCOMPARE(chain.server.listingRequests, 1);
        // The exact title ranks first, in KRunner's own category (a new term asks the server once)
        const QList<KRunnerMatch> exact =
            matchesOf(call(QStringLiteral("Match"), {QStringLiteral("Arcturus")}));
        QCOMPARE(exact.first().categoryRelevance, 50);
        QCOMPARE(exact.first().relevance, 1.0);
        QCOMPARE(chain.server.listingRequests, 2);
    }

    void quickQueriesMakeOneRequestAndTheCacheAnswers()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QDBusPendingCall first = startCall(QStringLiteral("Match"), {QStringLiteral("arct")});
        QDBusPendingCall second = startCall(QStringLiteral("Match"), {QStringLiteral("arct")});
        QVERIFY(matchesOf(waitFor(first)).isEmpty());
        QCOMPARE(matchesOf(waitFor(second)).size(), 6);
        QCOMPARE(chain.server.listingRequests, 1);
        // The same term again comes from the cache
        QCOMPARE(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("arct")})).size(), 6);
        QCOMPARE(chain.server.listingRequests, 1);
    }

    void runRunsOnlyOfferedIds()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("arct")})).size() == 6);
        QSignalSpy open(&chain.provider, &KRunnerProvider::openRequested);
        QSignalSpy play(&chain.provider, &KRunnerProvider::playRequested);
        QVERIFY(call(QStringLiteral("SetActivationToken"), {QStringLiteral("token-open")}).type()
                == QDBusMessage::ReplyMessage);
        QCOMPARE(call(QStringLiteral("Run"), {kMovie, QString()}).type(),
                 QDBusMessage::ReplyMessage);
        QCOMPARE(open.count(), 1);
        QCOMPARE(open.first().at(0).toString(), kMovie);
        QCOMPARE(open.first().at(1).toString(), QStringLiteral("token-open"));
        // A title the runner did not offer, a series (not playable) and an unknown action do nothing
        QVERIFY(call(QStringLiteral("Run"), {kMissing, QString()}).type()
                == QDBusMessage::ReplyMessage);
        QVERIFY(call(QStringLiteral("Run"), {kSeries, QStringLiteral("play")}).type()
                == QDBusMessage::ReplyMessage);
        QVERIFY(call(QStringLiteral("Run"), {kMovie, QStringLiteral("delete")}).type()
                == QDBusMessage::ReplyMessage);
        QCOMPARE(open.count(), 1);
        QCOMPARE(play.count(), 0);
        // Play from the start, Resume at the saved position
        call(QStringLiteral("Run"), {kMovie, QStringLiteral("play")});
        QCOMPARE(play.count(), 1);
        QCOMPARE(play.first().at(0).toString(), kMovie);
        QCOMPARE(play.first().at(1).toLongLong(), 0);
        call(QStringLiteral("Run"), {kResumable, QStringLiteral("resume")});
        QCOMPARE(play.count(), 2);
        QCOMPARE(play.last().at(1).toLongLong(), 600'000'000);
    }

    void activationTokenIsUsedOnceAndExpires()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("arct")})).size() == 6);
        QSignalSpy open(&chain.provider, &KRunnerProvider::openRequested);
        call(QStringLiteral("SetActivationToken"), {QStringLiteral("once")});
        call(QStringLiteral("Run"), {kMovie, QString()});
        call(QStringLiteral("Run"), {kMovie, QString()});
        QCOMPARE(open.count(), 2);
        QCOMPARE(open.at(0).at(1).toString(), QStringLiteral("once"));
        QVERIFY(open.at(1).at(1).toString().isEmpty());
        // A rejected token leaves the previous one alone
        call(QStringLiteral("SetActivationToken"), {QStringLiteral("valid")});
        call(QStringLiteral("SetActivationToken"), {QStringLiteral("not\nprintable")});
        call(QStringLiteral("Run"), {kMovie, QString()});
        QCOMPARE(open.last().at(1).toString(), QStringLiteral("valid"));
        // Ten seconds after it is set, the token is gone
        call(QStringLiteral("SetActivationToken"), {QStringLiteral("stale")});
        QTest::qWait(10'100);
        call(QStringLiteral("Run"), {kMovie, QString()});
        QVERIFY(open.last().at(1).toString().isEmpty());
    }

    void teardownKeepsOfferedIdsAndClearsTheCache()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("arct")})).size() == 6);
        QVERIFY(call(QStringLiteral("Teardown")).type() == QDBusMessage::ReplyMessage);
        // On Wayland Run can arrive after Teardown, waiting for its token: the ids still work
        QSignalSpy open(&chain.provider, &KRunnerProvider::openRequested);
        call(QStringLiteral("Run"), {kMovie, QString()});
        QCOMPARE(open.count(), 1);
        // The cache went with the session: the next query asks again
        QVERIFY(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("arct")})).size() == 6);
        QCOMPARE(chain.server.listingRequests, 2);
    }

    void credentialsClearedForgetsEverything()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("arct")})).size() == 6);
        chain.client.forgetSession();
        // No id is offered any more
        QSignalSpy open(&chain.provider, &KRunnerProvider::openRequested);
        call(QStringLiteral("Run"), {kMovie, QString()});
        QCOMPARE(open.count(), 0);
        // And no query reaches the server
        QVERIFY(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("arct")})).isEmpty());
        QCOMPARE(chain.server.listingRequests, 1);
    }

    void timeoutAnswersEmptyAndReleasesTheChannel()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.holdFirstListings = 1;
        QVERIFY(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("arct")})).isEmpty());
        QCOMPARE(chain.server.listingRequests, 1);
        chain.server.releaseHeld();
        // The channel is free for the next query
        QVERIFY(matchesOf(call(QStringLiteral("Match"), {QStringLiteral("arct")})).size() == 6);
        QCOMPARE(chain.server.listingRequests, 2);
    }
};

QTEST_GUILESS_MAIN(TestKRunner)
#include "test_krunner.moc"
