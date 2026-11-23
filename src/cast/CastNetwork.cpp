// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastNetwork.h"

#include <QAbstractSocket>
#include <QNetworkAddressEntry>

namespace Kinefin {

QList<CastInterface> castInterfaces()
{
    QList<CastInterface> result;
    const QNetworkInterface::InterfaceFlags wanted =
        QNetworkInterface::IsUp | QNetworkInterface::IsRunning | QNetworkInterface::CanMulticast;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlags(wanted))
            continue;
        if (flags.testAnyFlags(QNetworkInterface::IsLoopBack | QNetworkInterface::IsPointToPoint))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                result.append({iface, entry.ip()});
                break;
            }
        }
    }
    return result;
}

} // namespace Kinefin
