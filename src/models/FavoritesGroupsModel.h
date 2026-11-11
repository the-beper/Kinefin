// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "models/FavoritesModel.h"

#include <QAbstractListModel>
#include <QCollator>
#include <QPointer>

#include <vector>

namespace Kinefin {
// The favorites of one group, as a list of cards: a view over the rows of the snapshot that does not copy the items and answers with the roles of FavoritesModel. It lives as long as the grouping that made it
class FavoritesGroupItemsModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    FavoritesGroupItemsModel(FavoritesModel *source, std::vector<int> rows, QObject *parent);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  private:
    QPointer<FavoritesModel> m_source;
    std::vector<int> m_rows;
};

// The snapshot of FavoritesModel split into groups on the client, with no request: by type (the default), library, main genre, year or not at all. Every favorite is in exactly one group, so the counts of the groups add up to the number of favorites. Groups are never empty and their order is fixed by the grouping: types as Movie, Series, Season, Episode, BoxSet; libraries in the order of the library list, then "more libraries" for the items listed by several of them; genres by name, then "no genre"; years from the newest, then "no year"; one group for "none". The model rebuilds whenever the snapshot is replaced. `title` is remote text (a library name or a genre): show it as plain text; it is empty for the groups whose title the page translates (see `kind`)
class FavoritesGroupsModel final : public QAbstractListModel {
    Q_OBJECT
    // Changing it regroups at once (no request); setting it from the page also tells the controller to save the choice through `groupingEdited`
    Q_PROPERTY(Grouping grouping READ grouping WRITE setGrouping NOTIFY groupingChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

  public:
    enum Grouping { Type, Library, Genre, Year, None };
    Q_ENUM(Grouping)
    // The kind of a group: what its title is. Named groups (type, library, genre, year) carry `key` and `title`; the others have an empty `title` and the page shows its own text
    enum GroupKind {
        TypeGroup,
        LibraryGroup,
        MoreLibrariesGroup,
        GenreGroup,
        NoGenreGroup,
        YearGroup,
        NoYearGroup,
        AllGroup
    };
    Q_ENUM(GroupKind)
    // `key` is stable within a snapshot: the item type, the lower-case library id, the case-folded genre, the year or a fixed word. `items` is the QObject of a FavoritesGroupItemsModel
    enum Role { KeyRole = Qt::UserRole + 1, KindRole, TitleRole, CountRole, ItemsRole };

    explicit FavoritesGroupsModel(FavoritesModel *source, QObject *parent = nullptr);

    Grouping grouping() const { return m_grouping; }
    // The choice of the user: regroups, then emits groupingChanged and groupingEdited. A value outside the enum is ignored
    void setGrouping(Grouping grouping);
    // The saved choice at the start of a session: regroups and emits groupingChanged only
    void applyGrouping(Grouping grouping);
    int count() const { return int(m_groups.size()); }

    // The tokens the browse state store keeps (`Type`, `Library`, `Genre`, `Year`, `None`); an unknown token reads as Type
    static QString groupingToken(Grouping grouping);
    static Grouping groupingFromToken(const QString &token);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void groupingChanged();
    // The user picked another grouping (not applyGrouping)
    void groupingEdited();
    void countChanged();

  private:
    struct Group {
        QString key;
        GroupKind kind = AllGroup;
        QString title;
        FavoritesGroupItemsModel *items = nullptr;
    };

    bool assignGrouping(Grouping grouping);
    void rebuild();

    QPointer<FavoritesModel> m_source;
    QCollator m_collator;
    Grouping m_grouping = Type;
    std::vector<Group> m_groups;
};
} // namespace Kinefin
