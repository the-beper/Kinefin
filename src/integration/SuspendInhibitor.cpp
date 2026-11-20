// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/SuspendInhibitor.h"

#include "player/PlayerController.h"

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>
#include <KLocalizedString>

namespace Kinefin {
namespace {
const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kInhibit = QStringLiteral("org.freedesktop.portal.Inhibit");
const QString kRequest = QStringLiteral("org.freedesktop.portal.Request");
// The Inhibit portal's flags: 4 is Suspend (1 Logout, 2 User Switch, 8 Idle)
constexpr uint kSuspend = 4;
// The longest the exit waits for the release; the portal drops a dead client's inhibitions anyway
constexpr int kReleaseTimeoutMs = 500;
} // namespace

SuspendInhibitor::SuspendInhibitor(PlayerController &player, const QDBusConnection &bus,
                                   QObject *parent)
    : QObject(parent), m_player(player), m_bus(bus)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(0);
    connect(&m_settle, &QTimer::timeout, this, &SuspendInhibitor::update);
    connect(&m_player, &PlayerController::changed, &m_settle, qOverload<>(&QTimer::start));
    update();
}

SuspendInhibitor::~SuspendInhibitor()
{
    if (m_handle.isEmpty() || !m_bus.isConnected())
        return;
    const QDBusMessage call =
        QDBusMessage::createMethodCall(kService, m_handle, kRequest, QStringLiteral("Close"));
    m_bus.call(call, QDBus::Block, kReleaseTimeoutMs);
}

void SuspendInhibitor::update()
{
    const bool wanted =
        m_player.casting() && m_player.status() == PlayerController::Status::Playing;
    if (!wanted) {
        m_failed = false;
        release();
        return;
    }
    if (!m_handle.isEmpty() || m_pending || m_failed || !m_bus.isConnected())
        return;
    QDBusMessage call =
        QDBusMessage::createMethodCall(kService, kPath, kInhibit, QStringLiteral("Inhibit"));
    call << QString() << kSuspend
         << QVariantMap{
                {QStringLiteral("reason"), i18nc("@info reason the desktop shows for staying awake",
                                                 "Casting a video to another device")}};
    m_pending = true;
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *finished) {
                finished->deleteLater();
                m_pending = false;
                const QDBusPendingReply<QDBusObjectPath> reply = *finished;
                if (reply.isError()) {
                    if (!m_warned)
                        qWarning("The computer may suspend while casting: %s",
                                 qPrintable(reply.error().message()));
                    m_warned = true;
                    m_failed = true;
                    update();
                    return;
                }
                m_handle = reply.value().path();
                // The cast may have paused or ended while the answer was on its way
                update();
            });
}

void SuspendInhibitor::release()
{
    if (m_handle.isEmpty())
        return;
    const QDBusMessage call =
        QDBusMessage::createMethodCall(kService, m_handle, kRequest, QStringLiteral("Close"));
    m_handle.clear();
    m_bus.asyncCall(call);
}
} // namespace Kinefin
