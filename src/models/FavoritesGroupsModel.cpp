// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FavoritesGroupsModel.h"

#include <QHash>
#include <QVariant>

#include <algorithm>

namespace Kinefin {
namespace {
// The rank of the "more libraries", "no genre" and "no year" groups: after every named one
constexpr int LastRank = 1'000'000;

// The position of an item type in the Type grouping: the known types first, in this order
int typeRank(const QString &type)
{
    static const QStringList order{QStringLiteral("Movie"), QStringLiteral("Series"),
                                   QStringLiteral("Season"), QStringLiteral("Episode"),
                                   QStringLiteral("BoxSet")};
    const qsizetype index = order.indexOf(type);
    return index < 0 ? int(order.size()) : int(index);
}

struct Draft {
    QString key;
    FavoritesGroupsModel::GroupKind kind = FavoritesGroupsModel::AllGroup;
    QString title;
    // Groups sort by rank, then order, then title (locale collation), then key
    int rank = 0;
    int order = 0;
    std::vector<int> rows;
};
} // namespace

FavoritesGroupItemsModel::FavoritesGroupItemsModel(FavoritesModel *source, std::vector<int> rows,
                                                   QObject *parent)
    : QAbstractListModel(parent), m_source(source), m_rows(std::move(rows))
{
}

int FavoritesGroupItemsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> FavoritesGroupItemsModel::roleNames() const
{
    return m_source ? m_source->roleNames() : QHash<int, QByteArray>();
}

QVariant FavoritesGroupItemsModel::data(const QModelIndex &index, int role) const
{
    if (!m_source || !index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= int(m_rows.size()))
        return {};
    // A snapshot that was replaced since: the groups are rebuilt on the same signal, the row may point past the end meanwhile
    const int row = m_rows[size_t(index.row())];
    return row < m_source->count() ? m_source->data(m_source->index(row, 0), role) : QVariant();
}

FavoritesGroupsModel::FavoritesGroupsModel(FavoritesModel *source, QObject *parent)
    : QAbstractListModel(parent), m_source(source)
{
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    if (m_source)
        connect(m_source, &QAbstractItemModel::modelReset, this, [this] { rebuild(); });
    rebuild();
}

QString FavoritesGroupsModel::groupingToken(Grouping grouping)
{
    switch (grouping) {
    case Type:
        return QStringLiteral("Type");
    case Library:
        return QStringLiteral("Library");
    case Genre:
        return QStringLiteral("Genre");
    case Year:
        return QStringLiteral("Year");
    case None:
        return QStringLiteral("None");
    }
    return QStringLiteral("Type");
}

FavoritesGroupsModel::Grouping FavoritesGroupsModel::groupingFromToken(const QString &token)
{
    for (const Grouping grouping : {Type, Library, Genre, Year, None}) {
        if (token == groupingToken(grouping))
            return grouping;
    }
    return Type;
}

bool FavoritesGroupsModel::assignGrouping(Grouping grouping)
{
    if (grouping < Type || grouping > None || grouping == m_grouping)
        return false;
    m_grouping = grouping;
    rebuild();
    Q_EMIT groupingChanged();
    return true;
}

void FavoritesGroupsModel::setGrouping(Grouping grouping)
{
    if (assignGrouping(grouping))
        Q_EMIT groupingEdited();
}

void FavoritesGroupsModel::applyGrouping(Grouping grouping)
{
    assignGrouping(grouping);
}

// Partitions the snapshot: each row goes to exactly one group, whatever the grouping
void FavoritesGroupsModel::rebuild()
{
    std::vector<Draft> drafts;
    QHash<QString, size_t> byKey;
    const int rows = m_source ? m_source->count() : 0;
    for (int row = 0; row < rows; ++row) {
        const FavoriteItem &favorite = m_source->itemAt(row);
        Draft draft;
        switch (m_grouping) {
        case Type:
            draft.key = favorite.summary.type;
            draft.kind = TypeGroup;
            draft.rank = typeRank(favorite.summary.type);
            break;
        case Library:
            if (favorite.libraryIds.size() == 1) {
                const QString &id = favorite.libraryIds.constFirst();
                draft.key = id.toLower();
                draft.kind = LibraryGroup;
                draft.title = m_source->libraryName(id);
                draft.rank = m_source->libraryOrder(id);
            } else {
                draft.key = QStringLiteral("more-libraries");
                draft.kind = MoreLibrariesGroup;
                draft.rank = LastRank;
            }
            break;
        case Genre:
            if (favorite.primaryGenre.isEmpty()) {
                draft.key = QStringLiteral("no-genre");
                draft.kind = NoGenreGroup;
                draft.rank = LastRank;
            } else {
                draft.key = favorite.primaryGenre.toCaseFolded();
                draft.kind = GenreGroup;
                draft.title = favorite.primaryGenre;
            }
            break;
        case Year:
            if (favorite.summary.year <= 0) {
                draft.key = QStringLiteral("no-year");
                draft.kind = NoYearGroup;
                draft.rank = LastRank;
            } else {
                draft.key = QString::number(favorite.summary.year);
                draft.kind = YearGroup;
                draft.title = draft.key;
                draft.order = -favorite.summary.year;
            }
            break;
        case None:
            draft.key = QStringLiteral("all");
            draft.kind = AllGroup;
            break;
        }
        const auto found = byKey.constFind(draft.key);
        if (found == byKey.constEnd()) {
            byKey.insert(draft.key, drafts.size());
            draft.rows.push_back(row);
            drafts.push_back(std::move(draft));
        } else {
            drafts[found.value()].rows.push_back(row);
        }
    }
    std::sort(drafts.begin(), drafts.end(), [this](const Draft &a, const Draft &b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (a.order != b.order)
            return a.order < b.order;
        const int byTitle = m_collator.compare(a.title, b.title);
        return byTitle != 0 ? byTitle < 0 : a.key < b.key;
    });

    const int before = count();
    beginResetModel();
    for (const Group &group : m_groups)
        group.items->deleteLater();
    m_groups.clear();
    for (Draft &draft : drafts) {
        Group group;
        group.key = draft.key;
        group.kind = draft.kind;
        group.title = draft.title;
        group.items = new FavoritesGroupItemsModel(m_source, std::move(draft.rows), this);
        m_groups.push_back(std::move(group));
    }
    endResetModel();
    if (count() != before)
        Q_EMIT countChanged();
}

int FavoritesGroupsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QHash<int, QByteArray> FavoritesGroupsModel::roleNames() const
{
    return {{KeyRole, "key"},
            {KindRole, "kind"},
            {TitleRole, "title"},
            {CountRole, "count"},
            {ItemsRole, "items"}};
}

QVariant FavoritesGroupsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0 || index.row() >= count())
        return {};
    const Group &group = m_groups[size_t(index.row())];
    switch (role) {
    case KeyRole:
        return group.key;
    case KindRole:
        return int(group.kind);
    case TitleRole:
        return group.title;
    case CountRole:
        return group.items->rowCount();
    case ItemsRole:
        return QVariant::fromValue(static_cast<QObject *>(group.items));
    default:
        return {};
    }
}
} // namespace Kinefin
