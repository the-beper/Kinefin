// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Runs under dbus-run-session: a fake Inhibit portal on a second connection answers the inhibitor over the wire, and a fake renderer plays the cast
#include "FakeJellyfin.h"
#include "FakeRenderer.h"

#include "cast/CastController.h"
#include "integration/SuspendInhibitor.h"
#include "models/PlayQueue.h"
#include "player/PlayerController.h"
#include "settings/ClientSettings.h"

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <memory>
#include <vector>

using Kinefin::PlayerController;
using Kinefin::SuspendInhibitor;

namespace {
const QString kMovie = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");
const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");

// One inhibition the portal granted: closing it ends it
class FakeRequest : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
  public:
    explicit FakeRequest(int &closes) : m_closes(closes) {}

  public Q_SLOTS:
    void Close() { ++m_closes; }

  private:
    int &m_closes;
};

// The desktop's Inhibit portal: it records each call and hands out a request object per inhibition
class FakePortal : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Inhibit")
  public:
    explicit FakePortal(QDBusConnection &bus) : m_bus(bus) {}

    QStringList windows;
    QList<uint> flags;
    QStringList reasons;
    int closes = 0;

  public Q_SLOTS:
    QDBusObjectPath Inhibit(const QString &window, uint flag, const QVariantMap &options)
    {
        windows.append(window);
        flags.append(flag);
        reasons.append(options.value(QStringLiteral("reason")).toString());
        const QString path =
            kPath + QStringLiteral("/request/test/inhibit%1").arg(m_requests.size() + 1);
        m_requests.push_back(std::make_unique<FakeRequest>(closes));
        m_bus.registerObject(path, m_requests.back().get(), QDBusConnection::ExportAllSlots);
        return QDBusObjectPath(path);
    }

  private:
    QDBusConnection &m_bus;
    std::vector<std::unique_ptr<FakeRequest>> m_requests;
};

struct Chain {
    Chain()
        : queue(&client), settings(dir.filePath(QStringLiteral("kinefin.conf"))),
          controller(client, queue, settings, QString()),
          desktop(
              QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("portal"))),
          portal(desktop)
    {
        ok = server.listen() && login(client, server) && desktop.isConnected() && renderer.listen();
        controller.castController()->addRenderer(renderer.rendererInfo(QStringLiteral("TV")));
    }
    ~Chain()
    {
        desktop.unregisterService(kService);
        desktop.unregisterObject(kPath, QDBusConnection::UnregisterTree);
        QDBusConnection::disconnectFromBus(QStringLiteral("portal"));
    }
    bool servePortal()
    {
        return desktop.registerObject(kPath, &portal, QDBusConnection::ExportAllSlots)
               && desktop.registerService(kService);
    }
    // The movie playing on the renderer: the server heard its Start
    bool cast()
    {
        if (!controller.playItemOn(QStringLiteral("uuid:test-renderer"), kMovie, 0))
            return false;
        return QTest::qWaitFor([this] {
            return controller.casting() && controller.status() == PlayerController::Status::Playing
                   && renderer.transport == QLatin1StringView("PLAYING");
        });
    }

    QTemporaryDir dir;
    FakeJellyfin server;
    FakeRenderer renderer;
    Kinefin::JellyfinClient client;
    Kinefin::PlayQueue queue;
    Kinefin::ClientSettings settings;
    PlayerController controller;
    QDBusConnection desktop;
    FakePortal portal;
    bool ok = false;
};
} // namespace

class TestSuspendInhibitor : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void aCastHoldsSuspendOnlyWhileItPlays()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.servePortal());
        SuspendInhibitor inhibitor(chain.controller, QDBusConnection::sessionBus());
        // A local playback is the screensaver inhibitor's business, not this one's
        QVERIFY(chain.controller.playItem(kMovie, 0));
        QTest::qWait(300);
        QCOMPARE(chain.portal.flags.size(), 0);
        QVERIFY(!inhibitor.inhibiting());
        QVERIFY(chain.cast());
        QTRY_VERIFY(inhibitor.inhibiting());
        // Suspend only: the screen may turn off, the video plays elsewhere
        QCOMPARE(chain.portal.flags, QList<uint>{4});
        QCOMPARE(chain.portal.windows, QStringList{QString()});
        QVERIFY(!chain.portal.reasons.constFirst().isEmpty());
        // The many changes of a running cast ask for nothing more
        QTest::qWait(500);
        QCOMPARE(chain.portal.flags.size(), 1);
        // A pause on the renderer releases it, playing again takes it again
        chain.controller.pause();
        QTRY_COMPARE(chain.portal.closes, 1);
        QVERIFY(!inhibitor.inhibiting());
        chain.controller.resume();
        QTRY_VERIFY(inhibitor.inhibiting());
        QCOMPARE(chain.portal.flags.size(), 2);
        chain.controller.stopCasting();
        QTRY_COMPARE(chain.portal.closes, 2);
        QVERIFY(!inhibitor.inhibiting());
    }

    void aMissingPortalIsWarnedAboutOnce()
    {
        Chain chain;
        QVERIFY(chain.ok);
        SuspendInhibitor inhibitor(chain.controller, QDBusConnection::sessionBus());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(
                                               "^The computer may suspend while casting")));
        QVERIFY(chain.cast());
        QTest::qWait(300);
        QVERIFY(!inhibitor.inhibiting());
        // A later cast tries again, quietly
        QTest::failOnWarning(QRegularExpression(QStringLiteral("may suspend while casting")));
        chain.controller.stopCasting();
        QVERIFY(chain.cast());
        QTest::qWait(300);
        QVERIFY(!inhibitor.inhibiting());
    }

    void destructionClosesTheRequest()
    {
        Chain chain;
        QVERIFY(chain.ok);
        QVERIFY(chain.servePortal());
        auto inhibitor =
            std::make_unique<SuspendInhibitor>(chain.controller, QDBusConnection::sessionBus());
        QVERIFY(chain.cast());
        QTRY_VERIFY(inhibitor->inhibiting());
        inhibitor.reset();
        QTRY_COMPARE(chain.portal.closes, 1);
    }
};

QTEST_GUILESS_MAIN(TestSuspendInhibitor)
#include "test_suspend_inhibitor.moc"
