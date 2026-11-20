// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QStringList>

namespace Kinefin {

// The renderers the picker lists: id (UDN), name, detail (model and address) and kind; text is plain and already sanitized
class CastTargetsModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    struct Target {
        QString udn;
        QString name;
        QString detail;
        QString kind;
    };

    enum Role {
        UdnRole = Qt::UserRole + 1,
        NameRole,
        DetailRole,
        KindRole,
    };

    explicit CastTargetsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // A new row, or the row of the same UDN brought up to date
    void add(Target target);
    void remove(const QString &udn);
    QStringList udns() const;
    Q_INVOKABLE int indexOf(const QString &udn) const;

  private:
    QList<Target> m_targets;
};

} // namespace Kinefin
