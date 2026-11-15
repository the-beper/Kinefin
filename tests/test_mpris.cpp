// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Runs under dbus-run-session: the service sits on the session bus, a second connection plays the desktop and calls it over the wire
#include "FakeJellyfin.h"

#include "imagecache/ImageOrigin.h"
#include "integration/Mpris.h"
#include "integration/MprisArtwork.h"
#include "models/PlayQueue.h"
#include "player/PlayerController.h"
#include "settings/ClientSettings.h"

#include <QBuffer>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Kinefin::Mpris;

namespace {
const QString kMovie = QStringLiteral("f0e1d2c3-b4a5-9687-7869-5a4b3c2d1e0f");
const QString kSeries = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
const QString kService = QStringLiteral("org.mpris.MediaPlayer2.Kinefin.profile_default");
const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayer = QStringLiteral("org.mpris.MediaPlayer2.Player");

QString episodeId(int number)
{
    return QStringLiteral("%1").arg(number, 32, 16, QLatin1Char('e'));
}

QByteArray toBody(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject episodeObject(const QString &id, const QString &name, int season, int number)
{
    return {{QStringLiteral("Id"), id},
            {QStringLiteral("Name"), name},
            {QStringLiteral("Type"), QStringLiteral("Episode")},
            {QStringLiteral("SeriesName"), QStringLiteral("Abyss Station")},
            {QStringLiteral("SeriesId"), kSeries},
            {QStringLiteral("ParentIndexNumber"), season},
            {QStringLiteral("IndexNumber"), number}};
}

// What a desktop applet sees: every PropertiesChanged of the Player interface
class ChangeRecorder : public QObject {
    Q_OBJECT
  public:
    QList<QVariantMap> changes;
  public Q_SLOTS:
    void changed(const QString &interface, const QVariantMap &values, const QStringList &)
    {
        if (interface == kPlayer)
            changes.append(values);
    }
};

// The player of the tests: no video item, so an item stays loading (Playing for the desktop) once asked for
struct Chain {
    Chain()
        : queue(&client), settings(dir.filePath(QStringLiteral("kinefin.conf"))),
          // An invalid profile id keeps the controller's memory files off the disk; MPRIS gets its own id
          controller(client, queue, settings, QString()),
          desktop(
              QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("desktop")))
    {
        ok = server.listen() && login(client, server) && desktop.isConnected();
    }
    ~Chain() { QDBusConnection::disconnectFromBus(QStringLiteral("desktop")); }

    QVariant property(const QString &interface, const QString &name) const
    {
        QDBusMessage call = QDBusMessage::createMethodCall(
            kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("Get"));
        call << interface << name;
        // The service answers on this thread: the wait must keep the event loop turning
        const QDBusMessage reply = desktop.call(call, QDBus::BlockWithGui);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            return {};
        return reply.arguments().constFirst().value<QDBusVariant>().variant();
    }
    QVariantMap metadata() const
    {
        return qdbus_cast<QVariantMap>(property(kPlayer, QStringLiteral("Metadata")));
    }
    bool call(const QString &method, const QVariantList &arguments = {},
              const QString &interface = kPlayer) const
    {
        QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, interface, method);
        message.setArguments(arguments);
        return desktop.call(message, QDBus::BlockWithGui).type() == QDBusMessage::ReplyMessage;
    }

    QTemporaryDir dir;
    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    Kinefin::PlayQueue queue;
    Kinefin::ClientSettings settings;
    Kinefin::PlayerController controller;
    QDBusConnection desktop;
    bool ok = false;
};
} // namespace

class TestMpris : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void serviceNamesComeFromProfileIds()
    {
        QCOMPARE(Kinefin::mprisServiceName(QStringLiteral("default")), kService);
        QCOMPARE(Kinefin::mprisServiceName(QStringLiteral("a.1-b")),
                 QStringLiteral("org.mpris.MediaPlayer2.Kinefin.profile_a_1_b"));
        QVERIFY(Kinefin::mprisServiceName(QString()).isEmpty());
        QVERIFY(Kinefin::mprisServiceName(QStringLiteral("../escape")).isEmpty());
    }

    void trackIdsAreHexadecimalPaths()
    {
        QCOMPARE(
            Kinefin::mprisTrackId(kMovie).path(),
            QStringLiteral("/io/github/the_beper/Kinefin/track/f0e1d2c3b4a5968778695a4b3c2d1e0f"));
        QCOMPARE(Kinefin::mprisTrackId(QString()).path(),
                 QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack"));
        QCOMPARE(Kinefin::mprisTrackId(QStringLiteral("not/a-path")).path(),
                 QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack"));
    }

    void serverTextLosesControlCharacters()
    {
        QCOMPARE(
            Kinefin::mprisText(QStringLiteral("A\u0007B") + QChar(0x202E) + QStringLiteral("C\nD")),
            QStringLiteral("ABCD"));
        QCOMPARE(Kinefin::mprisText(QString(600, QLatin1Char('x'))).size(), 512);
        // Format characters outside the first plane (a tag character) and the line separators go too
        const char32_t tag = 0xE0041;
        QCOMPARE(Kinefin::mprisText(QStringLiteral("A") + QString::fromUcs4(&tag, 1)
                                    + QStringLiteral("B") + QChar(0x2028) + QStringLiteral("C")),
                 QStringLiteral("ABC"));
        // The cap never leaves half a surrogate pair
        const char32_t emoji = 0x1F600;
        const QString capped =
            Kinefin::mprisText(QString(511, QLatin1Char('x')) + QString::fromUcs4(&emoji, 1));
        QCOMPARE(capped.size(), 511);
        QVERIFY(!capped.back().isSurrogate());
    }

    void itOwnsTheNameAndDescribesTheApplication()
    {
        Chain chain;
        QVERIFY(chain.ok);
        Mpris mpris(chain.controller, QStringLiteral("default"), QDBusConnection::sessionBus());
        QVERIFY(mpris.registerOnBus());
        QVERIFY(chain.desktop.interface()->isServiceRegistered(kService));
        const QString root = QStringLiteral("org.mpris.MediaPlayer2");
        QCOMPARE(chain.property(root, QStringLiteral("Identity")).toString(),
                 QStringLiteral("Kinefin"));
        QCOMPARE(chain.property(root, QStringLiteral("DesktopEntry")).toString(),
                 QStringLiteral("io.github.the_beper.Kinefin"));
        QVERIFY(chain.property(root, QStringLiteral("CanRaise")).toBool());
        QVERIFY(chain.property(root, QStringLiteral("CanQuit")).toBool());
        QVERIFY(!chain.property(root, QStringLiteral("HasTrackList")).toBool());
    }

    void aTakenNameLeavesMprisOff()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.desktop.interface()->registerService(kService).isValid());
        QVERIFY(chain.desktop.interface()->isServiceRegistered(kService));
        Mpris mpris(chain.controller, QStringLiteral("default"), QDBusConnection::sessionBus());
        QTest::ignoreMessage(QtWarningMsg, "MPRIS is off: the bus name is taken");
        QVERIFY(!mpris.registerOnBus());
        chain.desktop.interface()->unregisterService(kService);
    }

    void aStoppedPlayerHasNothingToPlay()
    {
        Chain chain;
        QVERIFY(chain.ok);
        Mpris mpris(chain.controller, QStringLiteral("default"), QDBusConnection::sessionBus());
        QVERIFY(mpris.registerOnBus());
        QCOMPARE(chain.property(kPlayer, QStringLiteral("PlaybackStatus")).toString(),
                 QStringLiteral("Stopped"));
        QVERIFY(!chain.property(kPlayer, QStringLiteral("CanPlay")).toBool());
        QVERIFY(!chain.property(kPlayer, QStringLiteral("CanGoNext")).toBool());
        QVERIFY(chain.property(kPlayer, QStringLiteral("CanControl")).toBool());
        QCOMPARE(chain.property(kPlayer, QStringLiteral("Position")).toLongLong(), 0);
        QCOMPARE(
            chain.metadata().value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>().path(),
            QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack"));
    }

    void aPlayingEpisodeReachesTheDesktop()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.generatedEpisodeTotal = 5;
        chain.server.trickplayMetadataBody =
            toBody(episodeObject(episodeId(3), QStringLiteral("Weightless\u0007"), 2, 6));
        Mpris mpris(chain.controller, QStringLiteral("default"), QDBusConnection::sessionBus());
        QVERIFY(mpris.registerOnBus());
        ChangeRecorder recorder;
        QVERIFY(chain.desktop.connect(kService, kPath,
                                      QStringLiteral("org.freedesktop.DBus.Properties"),
                                      QStringLiteral("PropertiesChanged"), &recorder,
                                      SLOT(changed(QString, QVariantMap, QStringList))));
        QVERIFY(chain.controller.playItem(episodeId(3), 0));
        // Loading counts as playing; the title, the series and the episodes around it follow the server's answers
        QTRY_COMPARE(chain.property(kPlayer, QStringLiteral("PlaybackStatus")).toString(),
                     QStringLiteral("Playing"));
        QTRY_COMPARE(chain.metadata().value(QStringLiteral("xesam:title")).toString(),
                     QStringLiteral("S2:E6 — Weightless"));
        const QVariantMap metadata = chain.metadata();
        QCOMPARE(metadata.value(QStringLiteral("xesam:artist")).toStringList(),
                 QStringList{QStringLiteral("Abyss Station")});
        QCOMPARE(metadata.value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>(),
                 Kinefin::mprisTrackId(episodeId(3)));
        QVERIFY(!metadata.contains(QStringLiteral("xesam:url")));
        QTRY_VERIFY(chain.property(kPlayer, QStringLiteral("CanGoNext")).toBool());
        QVERIFY(chain.property(kPlayer, QStringLiteral("CanPlay")).toBool());
        // The applet heard each change, never the position
        QTRY_VERIFY(std::any_of(
            recorder.changes.cbegin(), recorder.changes.cend(), [](const QVariantMap &values) {
                return values.value(QStringLiteral("PlaybackStatus")) == QStringLiteral("Playing");
            }));
        QTRY_VERIFY(std::any_of(
            recorder.changes.cbegin(), recorder.changes.cend(),
            [](const QVariantMap &values) { return values.contains(QStringLiteral("Metadata")); }));
        for (const QVariantMap &values : std::as_const(recorder.changes))
            QVERIFY(!values.contains(QStringLiteral("Position")));

        // Next and Stop over the bus reach the player
        QVERIFY(chain.call(QStringLiteral("Next")));
        QTRY_COMPARE(chain.controller.itemId(), episodeId(4));
        QTRY_COMPARE(
            chain.metadata().value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>(),
            Kinefin::mprisTrackId(episodeId(4)));
        QVERIFY(chain.call(QStringLiteral("Stop")));
        QTRY_COMPARE(chain.property(kPlayer, QStringLiteral("PlaybackStatus")).toString(),
                     QStringLiteral("Stopped"));
        QTRY_VERIFY(chain.controller.itemId().isEmpty());
    }

    void callsThatCannotApplyDoNothing()
    {
        Chain chain;
        QVERIFY(chain.ok);
        Mpris mpris(chain.controller, QStringLiteral("default"), QDBusConnection::sessionBus());
        QVERIFY(mpris.registerOnBus());
        QSignalSpy seeked(&chain.controller, &Kinefin::PlayerController::seeked);
        // Nothing plays: no next item, nothing to resume or seek, no track to place
        for (const QString &method :
             {QStringLiteral("Next"), QStringLiteral("Previous"), QStringLiteral("Play"),
              QStringLiteral("PlayPause"), QStringLiteral("Pause")})
            QVERIFY(chain.call(method));
        QVERIFY(chain.call(QStringLiteral("Seek"), {qlonglong(5000000)}));
        QVERIFY(chain.call(QStringLiteral("SetPosition"),
                           {QVariant::fromValue(Kinefin::mprisTrackId(kMovie)), qlonglong(0)}));
        QVERIFY(chain.call(QStringLiteral("OpenUri"), {QStringLiteral("file:///etc/passwd")}));
        QVERIFY(chain.controller.itemId().isEmpty());
        QCOMPARE(seeked.size(), 0);
        QCOMPARE(chain.property(kPlayer, QStringLiteral("PlaybackStatus")).toString(),
                 QStringLiteral("Stopped"));
    }

    void raiseAndQuitAreTheApplicationsToDo()
    {
        Chain chain;
        QVERIFY(chain.ok);
        Mpris mpris(chain.controller, QStringLiteral("default"), QDBusConnection::sessionBus());
        QVERIFY(mpris.registerOnBus());
        QSignalSpy raised(&mpris, &Mpris::raiseRequested);
        QSignalSpy quit(&mpris, &Mpris::quitRequested);
        QVERIFY(chain.call(QStringLiteral("Raise"), {}, QStringLiteral("org.mpris.MediaPlayer2")));
        QCOMPARE(raised.size(), 1);
        QCOMPARE(quit.size(), 0);
        QVERIFY(chain.call(QStringLiteral("Quit"), {}, QStringLiteral("org.mpris.MediaPlayer2")));
        QCOMPARE(quit.size(), 1);
    }

    void theArtworkIsAPrivateLocalCopyOfThePrimaryImage()
    {
        Chain chain;
        QVERIFY(chain.ok);
        // A large poster: the desktop gets it scaled down, as a file of its own
        QImage poster(1000, 1500, QImage::Format_RGB32);
        poster.fill(Qt::darkCyan);
        QBuffer encoded(&chain.server.imageBody);
        QVERIFY(encoded.open(QIODevice::WriteOnly) && poster.save(&encoded, "PNG"));
        chain.server.trickplayMetadataBody = toBody(
            QJsonObject{{QStringLiteral("Id"), kMovie},
                        {QStringLiteral("Name"), QStringLiteral("Big Movie")},
                        {QStringLiteral("Type"), QStringLiteral("Movie")},
                        {QStringLiteral("ImageTags"),
                         QJsonObject{{QStringLiteral("Primary"), QStringLiteral("abc123")}}}});
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        origin->set(chain.server.url(QStringLiteral("/jellyfin")));
        QTemporaryDir cache;
        const QString directory = cache.filePath(QStringLiteral("mpris"));
        Mpris mpris(chain.controller, QStringLiteral("default"), QDBusConnection::sessionBus());
        QVERIFY(mpris.registerOnBus());
        {
            Kinefin::MprisArtwork artwork(mpris, chain.controller, origin, directory);
            QVERIFY(chain.controller.playItem(kMovie, 0));
            QString art;
            QTRY_VERIFY(!(art = chain.metadata().value(QStringLiteral("mpris:artUrl")).toString())
                             .isEmpty());
            const QUrl file(art);
            QVERIFY(file.isLocalFile());
            QVERIFY(file.toLocalFile().startsWith(directory + QLatin1Char('/')));
            const QImage stored(file.toLocalFile());
            QCOMPARE(stored.size(), QSize(341, 512));
            // Only the owner reads it, and nothing in it names the server
            QCOMPARE(QFile::permissions(file.toLocalFile())
                         & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                     QFileDevice::Permissions());
            QVERIFY(!art.contains(chain.server.url(QString()).host() + QLatin1Char(':')));
            // The item goes and its artwork with it
            chain.controller.stop();
            QTRY_VERIFY(!chain.metadata().contains(QStringLiteral("mpris:artUrl")));
        }
        // The folder is emptied at exit
        QVERIFY(!QDir(directory).exists() || QDir(directory).isEmpty());
    }

    void anImageInAnotherFormatIsNeverDecoded()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.imageBody = QByteArrayLiteral(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"/>");
        chain.server.trickplayMetadataBody = toBody(
            QJsonObject{{QStringLiteral("Id"), kMovie},
                        {QStringLiteral("Name"), QStringLiteral("Big Movie")},
                        {QStringLiteral("Type"), QStringLiteral("Movie")},
                        {QStringLiteral("ImageTags"),
                         QJsonObject{{QStringLiteral("Primary"), QStringLiteral("abc123")}}}});
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        origin->set(chain.server.url(QStringLiteral("/jellyfin")));
        QTemporaryDir cache;
        Mpris mpris(chain.controller, QStringLiteral("default"), QDBusConnection::sessionBus());
        QVERIFY(mpris.registerOnBus());
        Kinefin::MprisArtwork artwork(mpris, chain.controller, origin,
                                      cache.filePath(QStringLiteral("mpris")));
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTRY_COMPARE(chain.metadata().value(QStringLiteral("xesam:title")).toString(),
                     QStringLiteral("Big Movie"));
        QTest::qWait(300);
        QVERIFY(!chain.metadata().contains(QStringLiteral("mpris:artUrl")));
        QVERIFY(QDir(cache.filePath(QStringLiteral("mpris"))).isEmpty());
    }

    void aChangeThatChangesNothingIsNotSent()
    {
        Chain chain;
        QVERIFY(chain.ok);
        chain.server.trickplayMetadataBody =
            toBody(QJsonObject{{QStringLiteral("Id"), kMovie},
                               {QStringLiteral("Name"), QStringLiteral("Big Movie")},
                               {QStringLiteral("Type"), QStringLiteral("Movie")}});
        Mpris mpris(chain.controller, QStringLiteral("default"), QDBusConnection::sessionBus());
        QVERIFY(mpris.registerOnBus());
        ChangeRecorder recorder;
        QVERIFY(chain.desktop.connect(kService, kPath,
                                      QStringLiteral("org.freedesktop.DBus.Properties"),
                                      QStringLiteral("PropertiesChanged"), &recorder,
                                      SLOT(changed(QString, QVariantMap, QStringList))));
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTRY_COMPARE(chain.metadata().value(QStringLiteral("xesam:title")).toString(),
                     QStringLiteral("Big Movie"));
        QTRY_VERIFY(!recorder.changes.isEmpty());
        QTest::qWait(200);
        recorder.changes.clear();
        // The player says it changed (the position does, many times a second): nothing the desktop holds did
        for (int i = 0; i < 5; ++i)
            QVERIFY(QMetaObject::invokeMethod(&chain.controller, "changed"));
        QTest::qWait(200);
        QCOMPARE(recorder.changes.size(), 0);
    }

    void theNameGoesWithTheObject()
    {
        Chain chain;
        QVERIFY(chain.ok);
        {
            Mpris mpris(chain.controller, QStringLiteral("default"), QDBusConnection::sessionBus());
            QVERIFY(mpris.registerOnBus());
            QTRY_VERIFY(chain.desktop.interface()->isServiceRegistered(kService));
        }
        QTRY_VERIFY(!chain.desktop.interface()->isServiceRegistered(kService));
    }
};

QTEST_GUILESS_MAIN(TestMpris)
#include "test_mpris.moc"
