// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/EarlyUiScale.h"

#include "app/ProfileCommands.h"
#include "settings/ClientSettings.h"
#include "settings/ProfileIndex.h"
#include "settings/ProfilePaths.h"
#include "settings/SettingDefinitions.h"

#include <QFileInfo>
#include <KConfig>
#include <KConfigGroup>

#include <optional>

namespace Kinefin {
namespace {
const QString kGeneral = QStringLiteral("General");
const QString kVersion = QStringLiteral("version");
const QString kMain = QStringLiteral("main");
const QString kUiScale = QStringLiteral("uiScale");

// The last value of `--name value` or `--name=value`, as QCommandLineParser takes it; what follows a lone `--` is not an option
std::optional<QString> optionValue(const QStringList &arguments, const QString &name)
{
    const QString option = QStringLiteral("--") + name;
    const QString prefix = option + QLatin1Char('=');
    std::optional<QString> value;
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == QLatin1String("--"))
            break;
        if (argument == option && i + 1 < arguments.size())
            value = arguments.at(++i);
        else if (argument.startsWith(prefix))
            value = argument.mid(prefix.size());
    }
    return value;
}

// The settings file of the profile this run starts with, or empty. The profile root of `--config-dir` is only tried here, since main validates it later, so the root in force before is put back
QString settingsPathForRun(const QStringList &arguments)
{
    const QString previousRoot = profileRoot();
    const auto root = optionValue(arguments, QStringLiteral("config-dir"));
    if (root && !root->isEmpty())
        setProfileRoot(QFileInfo(*root).absoluteFilePath());
    QString profileId;
    if (const auto index = loadProfileIndex()) {
        const auto requested = optionValue(arguments, QStringLiteral("profile"));
        profileId = requested ? resolveProfile(*index, *requested) : index->defaultId;
    }
    const QString path = ClientSettings::pathForProfile(profileId);
    setProfileRoot(previousRoot);
    return path;
}
} // namespace

int startupUiScalePercent(const QStringList &arguments)
{
    const SettingDefinition *definition = findSetting(kMain, kUiScale);
    const int fallback = definition->defaultValue.toInt();
    const QString path = settingsPathForRun(arguments);
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return fallback;
    KConfig config(path, KConfig::SimpleConfig);
    // The same rule as ClientSettings: a file of a newer format is not read
    const int version = config.group(kGeneral).readEntry(kVersion, ClientSettings::kFormatVersion);
    if (version > ClientSettings::kFormatVersion)
        return fallback;
    bool ok = false;
    const int percent = config.group(kMain).readEntry(kUiScale, QString()).toInt(&ok);
    return ok && validatedSettingValue(*definition, percent).isValid() ? percent : fallback;
}
} // namespace Kinefin
