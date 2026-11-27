// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace Kinefin {
class ClientSettings;

// The KColorSchemeManager id for a `main.colorScheme` value: `light` and `dark` give Breeze Light and Breeze Dark, `auto` and anything else give an empty id, the desktop's own scheme
QString colorSchemeId(const QString &value);

// Applies the `main.colorScheme` of `settings` to the application palette and follows its changes for as long as `settings` lives. Needs the QGuiApplication to exist, and runs before the first window so none shows the wrong scheme
void followColorScheme(ClientSettings &settings);
} // namespace Kinefin
