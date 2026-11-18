// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The mapping of NetworkManager's Metered property: an unknown network counts as metered, so automatic downloads fail closed
#include "integration/NetworkMeter.h"

#include <QtTest/QtTest>

using Kinefin::NetworkMeter;

class TestNetworkMeter : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void mapsNetworkManagerValues()
    {
        QVERIFY(NetworkMeter::meteredFor(1) == true);
        QVERIFY(NetworkMeter::meteredFor(3) == true);
        QVERIFY(NetworkMeter::meteredFor(2) == false);
        QVERIFY(NetworkMeter::meteredFor(4) == false);
        QVERIFY(!NetworkMeter::meteredFor(0).has_value());
        QVERIFY(!NetworkMeter::meteredFor(5).has_value());
        QVERIFY(!NetworkMeter::meteredFor(-1).has_value());
    }

    void startsMeteredWithoutABus()
    {
        // A connection that is not open answers nothing: the meter stays metered and unknown
        NetworkMeter meter{QDBusConnection(QString())};
        QVERIFY(meter.metered());
        QVERIFY(!meter.known());
    }
};

QTEST_GUILESS_MAIN(TestNetworkMeter)
#include "test_network_meter.moc"
