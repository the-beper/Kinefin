// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHostAddress>
#include <QList>
#include <QNetworkInterface>

namespace Kinefin {

struct CastInterface {
    QNetworkInterface interface;
    // The first IPv4 address of the interface
    QHostAddress address;
};

// The interfaces a multicast search goes out on: up, running, multicast-capable, neither loopback nor point-to-point, with an IPv4 address
QList<CastInterface> castInterfaces();

} // namespace Kinefin
