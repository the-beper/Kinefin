// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings/SettingsSectionModel.h"

#include "settings/ClientSettings.h"
#include "settings/SettingDefinitions.h"

#include <QVariantMap>
#include <KLocalizedString>

namespace Kinefin {
namespace {
QString kindOf(const SettingDefinition &definition)
{
    using T = SettingDefinition::Type;
    if (!definition.options.isEmpty())
        return QStringLiteral("choice");
    switch (definition.type) {
    case T::Bool:
        return QStringLiteral("switch");
    case T::Int:
    case T::Double:
        return QStringLiteral("number");
    case T::String:
        return definition.multiline ? QStringLiteral("multiline") : QStringLiteral("text");
    }
    return QStringLiteral("text");
}

QString label(const KLazyLocalizedString &text)
{
    return text.isEmpty() ? QString() : text.toString();
}
} // namespace

SettingsSectionModel::SettingsSectionModel(ClientSettings &settings, const QString &section,
                                           QObject *parent)
    : QAbstractListModel(parent), m_settings(settings), m_section(section)
{
    for (const SettingDefinition &definition : settingDefinitions()) {
        if (definition.section == section && !definition.hidden)
            m_rows.append(&definition);
    }
    connect(&settings, &ClientSettings::valueChanged, this,
            [this](const QString &changedSection, const QString &key) {
                if (changedSection != m_section)
                    return;
                for (int row = 0; row < m_rows.size(); ++row) {
                    if (m_rows.at(row)->key == key)
                        Q_EMIT dataChanged(index(row), index(row), {ValueRole, IsDefaultRole});
                }
            });
}

int SettingsSectionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant SettingsSectionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const SettingDefinition &definition = *m_rows.at(index.row());
    switch (role) {
    case KeyRole:
        return definition.key;
    case LabelRole:
        return label(definition.label);
    case KindRole:
        return kindOf(definition);
    case OptionsRole: {
        QVariantList options;
        for (const SettingDefinition::Option &option : definition.options) {
            const QString text = label(option.label);
            options.append(QVariantMap{
                {QStringLiteral("value"), option.value},
                {QStringLiteral("text"), text.isEmpty() ? option.value.toString() : text}});
        }
        return options;
    }
    case ValueRole:
        return m_settings.value(m_section, definition.key);
    case IsDefaultRole:
        return m_settings.isDefault(m_section, definition.key);
    case MinimumRole:
        return definition.min;
    case MaximumRole:
        return definition.max;
    default:
        return {};
    }
}

bool SettingsSectionModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() >= m_rows.size() || role != ValueRole)
        return false;
    return m_settings.setValue(m_section, m_rows.at(index.row())->key, value);
}

QHash<int, QByteArray> SettingsSectionModel::roleNames() const
{
    return {{KeyRole, "key"},         {LabelRole, "label"},    {KindRole, "kind"},
            {OptionsRole, "options"}, {ValueRole, "value"},    {IsDefaultRole, "isDefault"},
            {MinimumRole, "minimum"}, {MaximumRole, "maximum"}};
}
} // namespace Kinefin
