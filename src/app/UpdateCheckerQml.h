// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "integration/UpdateChecker.h"

#include <QtQml/qqmlregistration.h>

namespace Kinefin {
// Register the creatable QObject in the QML module without adding QtQml as a
// dependency of the core library (the same foreign-type pattern as the models).
struct UpdateCheckerForeign {
    Q_GADGET
    QML_FOREIGN(Kinefin::UpdateChecker)
    QML_NAMED_ELEMENT(UpdateChecker)
};
} // namespace Kinefin
