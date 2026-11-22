// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "models/FavoritesModel.h"

#include <QAbstractListModel>
#include <QString>

#include <vector>

namespace Kinefin {
// The favorites of one media type (`type`, the item's Jellyfin `Type` such as "Movie") of a `FavoritesModel` snapshot, in the snapshot's order, at most `limit` of them (0 for all): the row of a type on the Favorites page and the Show All page of its type are one of these each. It copies no item: it keeps the source rows and answers every role from the source, with the same roles. It follows the source's resets (a new scan, a reload, a lost session), the only way the snapshot changes, and the source's filters (`FavoritesModel::filters`): a favorite that does not pass them is not a row of any section. It is empty without a source. `total` counts every favorite of the type that passes the filters, past the limit as well
class FavoritesSectionModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(Kinefin::FavoritesModel *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString type READ type WRITE setType NOTIFY typeChanged)
    Q_PROPERTY(int limit READ limit WRITE setLimit NOTIFY limitChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int total READ total NOTIFY countChanged)

  public:
    explicit FavoritesSectionModel(QObject *parent = nullptr);

    FavoritesModel *source() const { return m_source; }
    void setSource(FavoritesModel *source);
    const QString &type() const { return m_type; }
    void setType(const QString &type);
    int limit() const { return m_limit; }
    void setLimit(int limit);
    int count() const { return int(m_rows.size()); }
    int total() const { return m_total; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void sourceChanged();
    void typeChanged();
    void limitChanged();
    void countChanged();

  private:
    void fill();
    void refill();

    FavoritesModel *m_source = nullptr;
    QString m_type;
    int m_limit = 0;
    // The rows of the source this model shows, and how many the type has in all
    std::vector<int> m_rows;
    int m_total = 0;
};
} // namespace Kinefin
