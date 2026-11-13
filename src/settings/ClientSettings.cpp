// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings/ClientSettings.h"

#include "api/DeviceProfile.h"
#include "settings/ServerCredentials.h"
#include "settings/SettingDefinitions.h"
#include "settings/SettingsSectionModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <KConfig>
#include <KConfigGroup>

Q_LOGGING_CATEGORY(settingsLog, "kinefin.settings")

namespace Kinefin {
namespace {
const QString kGeneral = QStringLiteral("General");
const QString kVersion = QStringLiteral("version");

QString memoryKey(const QString &section, const QString &key)
{
    return section + QLatin1Char('/') + key;
}

QVariant parseStored(const SettingDefinition &definition, const QString &text)
{
    using T = SettingDefinition::Type;
    bool ok = true;
    QVariant parsed;
    switch (definition.type) {
    case T::Bool:
        if (text == QLatin1String("true"))
            parsed = true;
        else if (text == QLatin1String("false"))
            parsed = false;
        break;
    case T::Int:
        parsed = text.toInt(&ok);
        if (!ok)
            parsed = {};
        break;
    case T::Double:
        parsed = text.toDouble(&ok);
        if (!ok)
            parsed = {};
        break;
    case T::String:
        parsed = text;
        break;
    }
    return parsed.isValid() ? validatedSettingValue(definition, parsed) : QVariant();
}
} // namespace

ClientSettings::ClientSettings(const QString &path, QObject *parent) : QObject(parent), m_path(path)
{
    if (path.isEmpty()) {
        // Without a path KConfig would write the application's default file
        qCWarning(settingsLog) << "no settings file; changes stay in memory";
        return;
    }
    auto config = std::make_unique<KConfig>(path, KConfig::SimpleConfig);
    const int version = config->group(kGeneral).readEntry(kVersion, kFormatVersion);
    if (version <= kFormatVersion) {
        m_config = std::move(config);
        return;
    }
    // Drop the object while it is clean: KConfig writes its dirty data when destroyed
    config.reset();
    const QString broken = path + QStringLiteral(".broken");
    // The older .broken file is replaced only once the newer one has been moved out of the way
    const QString staging = broken + QStringLiteral(".new");
    if (QFile::rename(path, staging)) {
        QFile::remove(broken);
        QFile::rename(staging, broken);
        qCWarning(settingsLog) << "settings file is from a newer version; moved aside to" << broken;
        m_config = std::make_unique<KConfig>(path, KConfig::SimpleConfig);
    } else {
        qCWarning(settingsLog) << "settings file is from a newer version and cannot be moved "
                                  "aside; changes stay in memory";
    }
}

ClientSettings::~ClientSettings() = default;

QString ClientSettings::pathForProfile(const QString &profileId)
{
    const QString directory = profileDataDirectory(profileId);
    return directory.isEmpty() ? QString() : directory + QStringLiteral("/kinefin.conf");
}

QVariant ClientSettings::storedValue(const QString &section, const QString &key) const
{
    const SettingDefinition *definition = findSetting(section, key);
    if (!definition)
        return {};
    const QString id = memoryKey(section, key);
    if (m_memory.contains(id))
        return m_memory.value(id);
    if (!m_config)
        return {};
    const KConfigGroup group = m_config->group(section);
    if (!group.hasKey(key))
        return {};
    const QVariant parsed = parseStored(*definition, group.readEntry(key, QString()));
    if (!parsed.isValid() && !m_warned.contains(id)) {
        m_warned.insert(id);
        qCWarning(settingsLog) << "invalid stored value for" << section << key
                               << "- using the default";
    }
    return parsed;
}

QVariant ClientSettings::value(const QString &section, const QString &key) const
{
    const SettingDefinition *definition = findSetting(section, key);
    if (!definition)
        return {};
    const QVariant stored = storedValue(section, key);
    return stored.isValid() ? stored : definition->defaultValue;
}

bool ClientSettings::isDefault(const QString &section, const QString &key) const
{
    const SettingDefinition *definition = findSetting(section, key);
    return definition && value(section, key) == definition->defaultValue;
}

bool ClientSettings::setValue(const QString &section, const QString &key, const QVariant &newValue)
{
    const SettingDefinition *definition = findSetting(section, key);
    if (!definition || definition->hidden)
        return false;
    const QVariant checked = validatedSettingValue(*definition, newValue);
    if (!checked.isValid())
        return false;
    return store(*definition, checked);
}

bool ClientSettings::store(const SettingDefinition &definition, const QVariant &checked,
                           bool reportFailure)
{
    const QString &section = definition.section;
    const QString &key = definition.key;
    if (checked == value(section, key))
        return true;

    const QString id = memoryKey(section, key);
    bool saved = false;
    if (m_config) {
        KConfigGroup group = m_config->group(section);
        if (checked == definition.defaultValue)
            group.deleteEntry(key);
        else
            group.writeEntry(key, checked.toString());
        m_config->group(kGeneral).writeEntry(kVersion, kFormatVersion);
        saved = m_config->sync();
    }
    if (saved)
        m_memory.remove(id);
    else
        m_memory.insert(id, checked);
    Q_EMIT valueChanged(section, key);
    if (!saved) {
        if (reportFailure)
            Q_EMIT saveFailed();
        else
            qCWarning(settingsLog) << "could not save" << section << key;
    }
    return true;
}

QRect ClientSettings::windowGeometry() const
{
    const QStringList parts = value(QStringLiteral("state"), QStringLiteral("geometry"))
                                  .toString()
                                  .split(QLatin1Char(','));
    if (parts.size() != 4)
        return {};
    int numbers[4];
    for (int index = 0; index < 4; ++index) {
        bool ok = false;
        numbers[index] = parts.at(index).toInt(&ok);
        if (!ok)
            return {};
    }
    // Checked before the QRect exists: its right and bottom edges overflow for values near the int limits
    const auto inRange = [](int number, int low) {
        return number >= low && number <= kMaximumWindowSize;
    };
    const bool plausible =
        inRange(numbers[0], -kMaximumWindowSize) && inRange(numbers[1], -kMaximumWindowSize)
        && inRange(numbers[2], kMinimumWindowSize) && inRange(numbers[3], kMinimumWindowSize);
    return plausible ? QRect(numbers[0], numbers[1], numbers[2], numbers[3]) : QRect();
}

void ClientSettings::setWindowGeometry(const QRect &geometry)
{
    const SettingDefinition *definition =
        findSetting(QStringLiteral("state"), QStringLiteral("geometry"));
    if (!definition || geometry.isEmpty())
        return;
    // Written while the window closes: a failure is only logged, nobody would see a notification
    store(*definition,
          QStringLiteral("%1,%2,%3,%4")
              .arg(geometry.x())
              .arg(geometry.y())
              .arg(geometry.width())
              .arg(geometry.height()),
          false);
}

QObject *ClientSettings::sectionModel(const QString &section, const QString &group)
{
    // One model per section and group; an empty group is the whole section
    const QString id = section + QLatin1Char('/') + group;
    auto it = m_models.find(id);
    if (it == m_models.end())
        it = m_models.insert(id, new SettingsSectionModel(*this, section, group, this));
    return it.value();
}

QStringList ClientSettings::sectionGroups(const QString &section) const
{
    QStringList groups;
    for (const SettingDefinition &definition : settingDefinitions()) {
        if (definition.section != section || definition.hidden)
            continue;
        const QString heading =
            definition.group.isEmpty() ? QString() : definition.group.toString();
        if (!groups.contains(heading))
            groups.append(heading);
    }
    return groups;
}

DeviceProfileOptions ClientSettings::deviceProfileOptions() const
{
    const auto flag = [this](const char *key) {
        return value(QStringLiteral("video"), QLatin1String(key)).toBool();
    };
    DeviceProfileOptions options;
    options.channels = value(QStringLiteral("audio"), QStringLiteral("channels")).toString();
    options.preferTranscodeToH265 = flag("prefer_transcode_to_h265");
    options.allowTranscodeToHevc = flag("allow_transcode_to_hevc");
    options.forceTranscodeHevc = flag("force_transcode_hevc");
    options.forceTranscode4k = flag("force_transcode_4k");
    options.forceTranscodeAv1 = flag("force_transcode_av1");
    options.forceTranscodeHdr = flag("force_transcode_hdr");
    options.forceTranscodeHi10p = flag("force_transcode_hi10p");
    options.forceTranscodeDovi = flag("force_transcode_dovi");
    options.alwaysForceTranscode = flag("always_force_transcode");
    return options;
}
} // namespace Kinefin
