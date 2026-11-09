// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QList>

namespace Kinefin {
class ClientSettings;
struct SettingDefinition;

// The visible settings of one section for the Settings page: one row per setting, `value` writable through setData, changes made from C++ arrive as dataChanged of that row only.
class SettingsSectionModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        LabelRole,
        KindRole,    // "switch", "choice", "number", "text" or "multiline"
        OptionsRole, // list of {value, text}; empty unless "choice"
        ValueRole,
        IsDefaultRole,
        MinimumRole,
        MaximumRole,
    };

    SettingsSectionModel(ClientSettings &settings, const QString &section, QObject *parent);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QHash<int, QByteArray> roleNames() const override;

  private:
    ClientSettings &m_settings;
    QString m_section;
    QList<const SettingDefinition *> m_rows;
};
} // namespace Kinefin
