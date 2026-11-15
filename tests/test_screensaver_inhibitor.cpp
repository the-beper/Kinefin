// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Runs under dbus-run-session: a fake org.freedesktop.ScreenSaver on a second connection answers the inhibitor over the wire
#include "FakeJellyfin.h"

#include "integration/ScreenSaverInhibitor.h"
#include "models/PlayQueue.h"
#include "player/PlayerController.h"
#include "settings/ClientSettings.h"

#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <memory>

using Kinefin::ScreenSaverInhibitor;

namespace {
const QString kMovie = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");
const QString kService = QStringLiteral("org.freedesktop.ScreenSaver");

// The desktop's screensaver: it counts the calls and can hold its answer to Inhibit back
class FakeScreenSaver : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
  public:
    QStringList inhibits;
    QList<uint> uninhibits;
    bool holdReplies = false;
    QList<QDBusMessage> held;
    uint nextCookie = 41;

    void releaseHeld()
    {
        for (const QDBusMessage &message : std::as_const(held))
            QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                          QStringLiteral("screensaver"))
                .send(message.createReply(QVariant::fromValue(++nextCookie)));
        held.clear();
    }

  public Q_SLOTS:
    uint Inhibit(const QString &application, const QString &reason)
    {
        inhibits.append(application + QLatin1Char('|') + reason);
        if (holdReplies) {
            setDelayedReply(true);
            held.append(message());
            return 0;
        }
        return ++nextCookie;
    }
    void UnInhibit(uint cookie) { uninhibits.append(cookie); }
};

struct Chain {
    Chain()
        : queue(&client), settings(dir.filePath(QStringLiteral("kinefin.conf"))),
          controller(client, queue, settings, QString()),
          desktop(QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                QStringLiteral("screensaver")))
    {
        ok = server.listen() && login(client, server) && desktop.isConnected();
    }
    ~Chain()
    {
        desktop.unregisterService(kService);
        desktop.unregisterObject(QStringLiteral("/org/freedesktop/ScreenSaver"));
        QDBusConnection::disconnectFromBus(QStringLiteral("screensaver"));
    }
    bool serveScreenSaver()
    {
        return desktop.registerObject(QStringLiteral("/org/freedesktop/ScreenSaver"), &screenSaver,
                                      QDBusConnection::ExportAllSlots)
               && desktop.registerService(kService);
    }

    QTemporaryDir dir;
    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    Kinefin::PlayQueue queue;
    Kinefin::ClientSettings settings;
    Kinefin::PlayerController controller;
    QDBusConnection desktop;
    FakeScreenSaver screenSaver;
    bool ok = false;
};
} // namespace

class TestScreenSaverInhibitor : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void itInhibitsOnlyWhilePlaying()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.serveScreenSaver());
        ScreenSaverInhibitor inhibitor(chain.controller, QDBusConnection::sessionBus());
        QVERIFY(!inhibitor.inhibiting());
        QCOMPARE(chain.screenSaver.inhibits.size(), 0);
        // Loading counts as playing
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTRY_VERIFY(inhibitor.inhibiting());
        QCOMPARE(chain.screenSaver.inhibits.size(), 1);
        QVERIFY(chain.screenSaver.inhibits.constFirst().startsWith(QStringLiteral("Kinefin|")));
        // The many changes of a running playback ask for nothing more
        QTest::qWait(200);
        QCOMPARE(chain.screenSaver.inhibits.size(), 1);
        chain.controller.stop();
        QTRY_COMPARE(chain.screenSaver.uninhibits.size(), 1);
        QCOMPARE(chain.screenSaver.uninhibits.constFirst(), chain.screenSaver.nextCookie);
        QVERIFY(!inhibitor.inhibiting());
    }

    void aCookieThatComesAfterTheStopIsReleased()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.serveScreenSaver());
        chain.screenSaver.holdReplies = true;
        ScreenSaverInhibitor inhibitor(chain.controller, QDBusConnection::sessionBus());
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTRY_COMPARE(chain.screenSaver.held.size(), 1);
        chain.controller.stop();
        chain.screenSaver.releaseHeld();
        QTRY_COMPARE(chain.screenSaver.uninhibits.size(), 1);
        QVERIFY(!inhibitor.inhibiting());
    }

    void aMissingServiceIsWarnedAboutOnce()
    {
        Chain chain;
        QVERIFY(chain.ok);
        ScreenSaverInhibitor inhibitor(chain.controller, QDBusConnection::sessionBus());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(
                                               "^The screen may turn off during playback")));
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTest::qWait(300);
        QVERIFY(!inhibitor.inhibiting());
        // A later run tries again, quietly
        QTest::failOnWarning(QRegularExpression(QStringLiteral("screen may turn off")));
        chain.controller.stop();
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTest::qWait(300);
        QVERIFY(!inhibitor.inhibiting());
    }

    void destructionReleasesTheCookie()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.serveScreenSaver());
        auto inhibitor =
            std::make_unique<ScreenSaverInhibitor>(chain.controller, QDBusConnection::sessionBus());
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTRY_VERIFY(inhibitor->inhibiting());
        inhibitor.reset();
        QTRY_COMPARE(chain.screenSaver.uninhibits.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestScreenSaverInhibitor)
#include "test_screensaver_inhibitor.moc"
