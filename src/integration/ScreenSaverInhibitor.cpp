// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/ScreenSaverInhibitor.h"

#include "player/PlayerController.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <KLocalizedString>

namespace Kinefin {
namespace {
const QString kService = QStringLiteral("org.freedesktop.ScreenSaver");
const QString kPath = QStringLiteral("/org/freedesktop/ScreenSaver");
const QString kInterface = QStringLiteral("org.freedesktop.ScreenSaver");
// The longest the exit waits for the release; the service drops a dead client's inhibitions anyway
constexpr int kReleaseTimeoutMs = 500;
} // namespace

ScreenSaverInhibitor::ScreenSaverInhibitor(PlayerController &player, const QDBusConnection &bus,
                                           QObject *parent)
    : QObject(parent), m_player(player), m_bus(bus)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(0);
    connect(&m_settle, &QTimer::timeout, this, &ScreenSaverInhibitor::update);
    connect(&m_player, &PlayerController::changed, &m_settle, qOverload<>(&QTimer::start));
    update();
}

ScreenSaverInhibitor::~ScreenSaverInhibitor()
{
    if (!m_cookie || !m_bus.isConnected())
        return;
    QDBusMessage call =
        QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("UnInhibit"));
    call << *m_cookie;
    m_bus.call(call, QDBus::Block, kReleaseTimeoutMs);
}

void ScreenSaverInhibitor::update()
{
    // A cast plays on another screen: this one may turn off
    const bool wanted =
        m_player.status() == PlayerController::Status::Playing && !m_player.casting();
    if (!wanted) {
        m_failed = false;
        release();
        return;
    }
    if (m_cookie || m_pending || m_failed || !m_bus.isConnected())
        return;
    QDBusMessage call =
        QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("Inhibit"));
    call << QStringLiteral("Kinefin")
         << i18nc("@info reason the desktop shows for keeping the screen on", "Playing a video");
    m_pending = true;
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *finished) {
                finished->deleteLater();
                m_pending = false;
                const QDBusPendingReply<uint> reply = *finished;
                if (reply.isError()) {
                    if (!m_warned)
                        qWarning("The screen may turn off during playback: %s",
                                 qPrintable(reply.error().message()));
                    m_warned = true;
                    m_failed = true;
                    // Playback may have stopped while the answer was on its way: the next run tries again
                    update();
                    return;
                }
                m_cookie = reply.value();
                // Playback may have paused or stopped while the cookie was on its way
                update();
            });
}

void ScreenSaverInhibitor::release()
{
    if (!m_cookie)
        return;
    QDBusMessage call =
        QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("UnInhibit"));
    call << *m_cookie;
    m_cookie.reset();
    m_bus.asyncCall(call);
}
} // namespace Kinefin
