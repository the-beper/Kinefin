// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "NetworkMeter.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>

namespace Kinefin {
namespace {
const QString kService = QStringLiteral("org.freedesktop.NetworkManager");
const QString kPath = QStringLiteral("/org/freedesktop/NetworkManager");
const QString kInterface = QStringLiteral("org.freedesktop.NetworkManager");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kMetered = QStringLiteral("Metered");
} // namespace

NetworkMeter::NetworkMeter(QObject *parent) : NetworkMeter(QDBusConnection::systemBus(), parent) {}

NetworkMeter::NetworkMeter(const QDBusConnection &bus, QObject *parent)
    : QObject(parent), m_bus(bus)
{
    if (!m_bus.isConnected())
        return;
    m_bus.connect(kService, kPath, kProperties, QStringLiteral("PropertiesChanged"), this,
                  SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    auto *serviceWatcher =
        new QDBusServiceWatcher(kService, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(serviceWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &newOwner) {
                if (newOwner.isEmpty()) {
                    ++m_sequence;
                    apply(std::nullopt);
                    return;
                }
                read();
            });
    read();
}

std::optional<bool> NetworkMeter::meteredFor(int value)
{
    switch (value) {
    case 1:
    case 3:
        return true;
    case 2:
    case 4:
        return false;
    }
    return std::nullopt;
}

void NetworkMeter::read()
{
    QDBusMessage call =
        QDBusMessage::createMethodCall(kService, kPath, kProperties, QStringLiteral("Get"));
    call << kInterface << kMetered;
    const quint64 ticket = ++m_sequence;
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, ticket](QDBusPendingCallWatcher *watcher) {
                watcher->deleteLater();
                if (ticket != m_sequence)
                    return;
                const QDBusMessage reply = watcher->reply();
                if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
                    apply(std::nullopt);
                    return;
                }
                const QVariant value = reply.arguments().constFirst();
                const QVariant inner = value.metaType() == QMetaType::fromType<QDBusVariant>()
                                           ? value.value<QDBusVariant>().variant()
                                           : value;
                apply(meteredFor(inner.toInt()));
            });
}

void NetworkMeter::apply(const std::optional<bool> &metered)
{
    m_known = metered.has_value();
    const bool value = metered.value_or(true);
    if (value == m_metered)
        return;
    m_metered = value;
    Q_EMIT meteredChanged(m_metered);
}

void NetworkMeter::onPropertiesChanged(const QString &interface, const QVariantMap &changed,
                                       const QStringList &invalidated)
{
    if (interface != kInterface)
        return;
    if (invalidated.contains(kMetered)) {
        read();
        return;
    }
    const auto value = changed.constFind(kMetered);
    if (value != changed.constEnd()) {
        ++m_sequence;
        apply(meteredFor(value.value().toInt()));
    }
}
} // namespace Kinefin
