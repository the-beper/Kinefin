// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ColorSchemeSetting.h"

#include "settings/ClientSettings.h"

#include <KColorSchemeManager>

namespace Kinefin {
namespace {
const QLatin1String kMain("main");
const QLatin1String kColorScheme("colorScheme");

void applyColorScheme(const ClientSettings &settings, bool atStart)
{
    const QString id = colorSchemeId(settings.value(kMain, kColorScheme).toString());
    // Auto at the start leaves the palette as the platform set it up: the manager is only created once a scheme was picked
    if (atStart && id.isEmpty())
        return;
    auto *manager = KColorSchemeManager::instance();
    // The choice lives in the profile's kinefin.conf only: left on, the manager writes [UiSettings] ColorScheme into the application's default config file
    manager->setAutosaveChanges(false);
    manager->activateSchemeId(id);
}
} // namespace

QString colorSchemeId(const QString &value)
{
    if (value == QLatin1String("light"))
        return QStringLiteral("BreezeLight");
    if (value == QLatin1String("dark"))
        return QStringLiteral("BreezeDark");
    return {};
}

void followColorScheme(ClientSettings &settings)
{
    applyColorScheme(settings, true);
    QObject::connect(&settings, &ClientSettings::valueChanged, &settings,
                     [&settings](const QString &section, const QString &key) {
                         if (section == kMain && key == kColorScheme)
                             applyColorScheme(settings, false);
                     });
}
} // namespace Kinefin
