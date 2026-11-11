// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/CardRoles.h"
#include "models/FilterOptionsModel.h"
#include "models/PagedItemsModel.h"

#include <QTimer>

#include <optional>

namespace Kinefin {
// The items of one library, or of every library of a type, as a paged list (see PagedItemsModel, with the card roles of CardRoles.h): the query of the type with a sort order, a filter, genres and a search term, on a listing channel given at construction (the genre options use the same channel number in their own family) so the home rows, the other grids and the details pages never see its replies. The model empties itself whenever the client loses its credentials
class LibraryItemsModel final : public PagedItemsModel {
    Q_OBJECT
    Q_PROPERTY(SortKey sortKey READ sortKey WRITE setSortKey NOTIFY sortKeyChanged)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY descendingChanged)
    Q_PROPERTY(Filter filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool resumableAvailable READ resumableAvailable NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryName READ libraryName NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryId READ libraryId NOTIFY libraryChanged)
    // The genre ids the grid is filtered by (any of them), chosen among `genreOptions`
    Q_PROPERTY(QStringList genreIds READ genreIds WRITE setGenreIds NOTIFY genreIdsChanged)
    Q_PROPERTY(Kinefin::FilterOptionsModel *genreOptions READ genreOptions CONSTANT)
    // What the user typed in the search box: a pause of `SearchModel::DebounceMs` after the last edit reloads the list with the normalised term, a term that becomes too short reloads at once, and the same normalised term sends nothing. Kept across open(), dropped by clear()
    Q_PROPERTY(QString searchTerm READ searchTerm WRITE setSearchTerm NOTIFY searchTermChanged)
    // Whether any filter, genre or search term narrows the list (the empty-state text depends on it)
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

    // The sort order, filter and genres of a view: what the browse state store keeps per scope
    struct ViewState {
        SortKey sort = Name;
        bool descending = false;
        Filter filter = All;
        QStringList genreIds;
    };

    static constexpr int PageSize = 100;
    // The listing channels of the grids: the tile-opened library (collections), the Movies tab and the TV Shows tab
    static constexpr int TileChannel = 1;
    static constexpr int MoviesChannel = 6;
    static constexpr int ShowsChannel = 7;
    // The width requested for a poster: the tiles show at most about this many device pixels
    static constexpr int ImageWidth = 400;

    LibraryItemsModel(JellyfinClient *client, int channel, QObject *parent = nullptr);

    // The query that lists the items of a library of this `CollectionType`, or nothing when the type is not browsable (movies, tvshows and boxsets are)
    static std::optional<ItemQuery> browseQuery(const QString &collectionType);

    // Resets the model and loads the first page. An empty `libraryId` lists every library of the type (no parent on the wire). False (and the model stays empty) for a library id the server could not have sent or a type that is not browsable
    bool open(const QString &libraryId, const QString &collectionType,
              const QString &libraryName = QString());
    // The same, starting from `initial` instead of the default order and filter (applied without viewStateEdited)
    bool open(const QString &libraryId, const QString &collectionType, const QString &libraryName,
              const ViewState &initial);
    // Empties the model and drops the search term
    void clear();
    // Whether a library (or the whole type) is open
    bool isOpen() const { return !m_collectionType.isEmpty(); }

    SortKey sortKey() const { return m_sortKey; }
    bool descending() const { return m_descending; }
    Filter filter() const { return m_filter; }
    bool resumableAvailable() const { return m_collectionType == QLatin1String("movies"); }
    QString libraryName() const { return m_libraryName; }
    QString libraryId() const { return m_libraryId; }
    QString collectionType() const { return m_collectionType; }
    QStringList genreIds() const { return m_genreIds; }
    FilterOptionsModel *genreOptions() { return &m_genreOptions; }
    bool filtered() const
    {
        return m_filter != All || !m_genreIds.isEmpty() || !m_appliedTerm.isEmpty();
    }
    ViewState viewState() const { return {m_sortKey, m_descending, m_filter, m_genreIds}; }
    QString searchTerm() const { return m_searchTerm; }
    void setSearchTerm(const QString &term);
    // For tests; the default is SearchModel::DebounceMs
    void setSearchDebounceInterval(int milliseconds) { m_searchDebounce.setInterval(milliseconds); }
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
    void searchTermChanged();
    // The user changed the sort order, direction, filter or genres (not an open(), a prune of vanished genres or a search term)
    void viewStateEdited();

  protected:
    bool sendPage(int start, int limit) override;
    QVariant rowData(const ItemSummary &item, bool loaded, int role) const override;

  private:
    bool openScope(const QString &libraryId, const QString &collectionType,
                   const QString &libraryName, const ViewState *initial);
    void clearLibrary();
    void applyViewState(const ViewState &state);
    bool assignGenreIds(const QStringList &ids);
    void setFilterQuietly(Filter filter);
    void applySearchTerm();
    void dropSearchTerm();
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);

    JellyfinClient *m_client;
    const int m_channel;
    ItemQuery m_base;
    QString m_libraryId;
    QString m_collectionType;
    QString m_libraryName;
    SortKey m_sortKey = Name;
    bool m_descending = false;
    Filter m_filter = All;
    QStringList m_genreIds;
    // The typed text, and the normalised term the list was last loaded with (empty when the text is too short)
    QString m_searchTerm;
    QString m_appliedTerm;
    QTimer m_searchDebounce;
    FilterOptionsModel m_genreOptions;
};
} // namespace Kinefin
