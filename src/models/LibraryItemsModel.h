// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/FilterOptionsModel.h"
#include "models/PagedItemsModel.h"

#include <optional>

namespace Kinefin {
// The items of one library as a paged list (see PagedItemsModel): the library's own query with a sort order and a filter, on a listing channel of its own so the home rows and the details pages never see its replies. The model empties itself whenever the client loses its credentials
class LibraryItemsModel final : public PagedItemsModel {
    Q_OBJECT
    Q_PROPERTY(SortKey sortKey READ sortKey WRITE setSortKey NOTIFY sortKeyChanged)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY descendingChanged)
    Q_PROPERTY(Filter filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool resumableAvailable READ resumableAvailable NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryName READ libraryName NOTIFY libraryChanged)
    // The genre ids the grid is filtered by (any of them), chosen among `genreOptions`
    Q_PROPERTY(QStringList genreIds READ genreIds WRITE setGenreIds NOTIFY genreIdsChanged)
    Q_PROPERTY(Kinefin::FilterOptionsModel *genreOptions READ genreOptions CONSTANT)
    // Whether any filter or genre narrows the list (the empty-state text depends on it)
    Q_PROPERTY(bool filtered READ filtered NOTIFY filteredChanged)

  public:
    enum SortKey { Name, DateAdded, ReleaseDate, CommunityRating, DatePlayed, Runtime };
    Q_ENUM(SortKey)
    enum Filter { All, Unplayed, Played, Resumable, Favorites };
    Q_ENUM(Filter)
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        NameRole,
        YearRole,
        TypeRole,
        ImageUrlRole,
        PlayedRole,
        UnplayedCountRole,
        PlayedPercentageRole,
        FavoriteRole,
        LoadedRole
    };

    static constexpr int PageSize = 100;
    // The listing channel of the grid: the home rows use others, so neither sees the other's replies
    static constexpr int Channel = 1;
    // The width requested for a poster: the tiles show at most about this many device pixels
    static constexpr int ImageWidth = 400;

    explicit LibraryItemsModel(JellyfinClient *client, QObject *parent = nullptr);

    // The query that lists the items of a library of this `CollectionType`, or nothing when the type is not browsable (movies, tvshows and boxsets are)
    static std::optional<ItemQuery> browseQuery(const QString &collectionType);

    // Resets the model and loads the first page. False (and the model stays empty) for a library id the server could not have sent or a type that is not browsable
    bool open(const QString &libraryId, const QString &collectionType,
              const QString &libraryName = QString());
    void clear();

    SortKey sortKey() const { return m_sortKey; }
    bool descending() const { return m_descending; }
    Filter filter() const { return m_filter; }
    bool resumableAvailable() const { return m_collectionType == QLatin1String("movies"); }
    QString libraryName() const { return m_libraryName; }
    QString libraryId() const { return m_libraryId; }
    QString collectionType() const { return m_collectionType; }
    QStringList genreIds() const { return m_genreIds; }
    FilterOptionsModel *genreOptions() { return &m_genreOptions; }
    bool filtered() const { return m_filter != All || !m_genreIds.isEmpty(); }
    void setGenreIds(const QStringList &ids);
    void setSortKey(SortKey key);
    void setDescending(bool descending);
    void setFilter(Filter filter);

    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void sortKeyChanged();
    void descendingChanged();
    void filterChanged();
    void libraryChanged();
    void genreIdsChanged();
    void filteredChanged();

  protected:
    bool sendPage(int start, int limit) override;
    QVariant rowData(const ItemSummary &item, bool loaded, int role) const override;

  private:
    void resetOrderAndFilter();
    void setFilterQuietly(Filter filter);
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);

    JellyfinClient *m_client;
    ItemQuery m_base;
    QString m_libraryId;
    QString m_collectionType;
    QString m_libraryName;
    SortKey m_sortKey = Name;
    bool m_descending = false;
    Filter m_filter = All;
    QStringList m_genreIds;
    FilterOptionsModel m_genreOptions;
};
} // namespace Kinefin
