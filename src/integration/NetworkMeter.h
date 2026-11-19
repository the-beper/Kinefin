// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusConnection>
#include <QObject>

#include <optional>

namespace Kinefin {
// The `Metered` property of org.freedesktop.NetworkManager on the system bus: automatic downloads start only on an unmetered network, and an unknown value (no daemon, no permission, no answer yet, the daemon gone) counts as metered. The daemon's bus name is watched: losing its owner makes the value unknown, a new owner is read again. Manual downloads do not consult it
class NetworkMeter final : public QObject {
    Q_OBJECT

  public:
    explicit NetworkMeter(QObject *parent = nullptr);
    // For tests: a bus other than the system one
    explicit NetworkMeter(const QDBusConnection &bus, QObject *parent = nullptr);

    // True until the answer says otherwise, so an unknown network fails closed
    bool metered() const { return m_metered; }
    bool known() const { return m_known; }
    // NetworkManager's values: 1 (yes) and 3 (yes, guessed) metered, 2 (no) and 4 (no, guessed) unmetered, 0 (unknown) and anything else unknown
    static std::optional<bool> meteredFor(int value);

  Q_SIGNALS:
    void meteredChanged(bool metered);

  private Q_SLOTS:
    void onPropertiesChanged(const QString &interface, const QVariantMap &changed,
                             const QStringList &invalidated);

  private:
    void read();
    void apply(const std::optional<bool> &metered);

    QDBusConnection m_bus;
    // Bumped by every read and every pushed value, so a Get reply that a newer answer overtook is dropped
    quint64 m_sequence = 0;
    bool m_metered = true;
    bool m_known = false;
};
} // namespace Kinefin
