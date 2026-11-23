// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings/MpvConf.h"

namespace Kinefin {
// Asks libmpv whether it knows each option and accepts its value, on a throw-away handle that is never started: nothing reaches the player. The entries libmpv refuses come back as problems (`UnknownOption` for a name it does not have, `BadValue` for a value it does not take); the others are not mentioned
QList<MpvConfProblem> validateMpvConf(const QList<MpvConfEntry> &entries);
} // namespace Kinefin
