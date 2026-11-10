// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings/ClientSettings.h"

#include <QtQml/qqmlregistration.h>

// Makes the core library's ClientSettings known to QML without linking the core to QtQml
namespace Kinefin {
struct ClientSettingsForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::ClientSettings)
    QML_NAMED_ELEMENT(ClientSettings)
    QML_UNCREATABLE("Created by the application")
};
} // namespace Kinefin
