// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/CardRoles.h"
#include "models/FilterOptionsModel.h"
#include "models/ItemSummary.h"
#include "models/LibraryFilterState.h"

#include <QAbstractListModel>
#include <QCollator>
#include <QHash>
#include <QJsonArray>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <vector>

namespace Kinefin {
// One favorite of the snapshot: the card data, the libraries it was listed under (in library order) and what the filters match on
struct FavoriteItem {
    ItemSummary summary;
    QStringList libraryIds;
    // The valid genres and tags the server gave: plain text, capped, each once (case-insensitive), at most MaxGenres and MaxTags of them; empty when there are none
    QStringList genres;
    QStringList tags;
    // The parental rating when it is text a filter can carry (isValidFilterText), otherwise empty
    QString officialRating;
};

// The favorites of the signed-in user across the accessible libraries, as one deduplicated snapshot. The libraries come from the `libraries` entries of the connection controller (`id`, `name`, `collectionType`); only movies, tvshows and boxsets libraries are used. They are queried one request at a time on `Channel`, a page of `PageSize` of each in turn (round robin) with `filters=IsFavorite` and the library as parent, so the library an item was seen in is known from the request and never guessed. The same item listed by several libraries is one row that carries all of them. The snapshot holds at most `MaxItems` distinct items and is published in one piece when the scan ends: `Complete` (every library read to its end), `Capped` (the limit was reached while more could follow: the list may be incomplete) or `PartialError` (a library failed, the others still show; `capped` tells whether the limit was reached too). The model is empty while `Loading`, and `Idle` before the first load and after any session loss. A stale reply never reaches the snapshot: a new scan, a changed library list, `close()` and a lost session all release the channel. A lost session (credentialsCleared) also forgets the libraries, the caller sets them again for the next session. Besides its own roles the model has the card roles (see CardRoles.h) with poster images. The Favorites page shows one `FavoritesSectionModel` per media type over this snapshot. The model owns the one filter of the Favorites pages (the Favorites page and every Show All page share it, so the section models follow it): `filters` uses the map format of `LibraryItemsModel::filters`, reduced to the sections the snapshot has data for (`played`, `resumable`, `genreIds` (genre names here), `tags`, `officialRatings`, `years`), the values within one section match with OR and the sections combine with AND. The model itself lists every row of the snapshot whatever the filters say (`count` and the rows are the unfiltered snapshot): the filtered views are the section models, which ask `passesFilters`, and `matchCount` is how many rows pass. The filters work on the session, not on one snapshot: they stay across rescans and visits and are dropped with the rest by a lost session, and they are never saved. `filterOptions` is the dialog's value source, filled from the snapshot (plus the chosen values) when a scan ends, Loading while one runs and empty before, with no request
class FavoritesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // The limit was reached while more favorites may exist: the banner says the list may be incomplete
    Q_PROPERTY(bool capped READ capped NOTIFY noticeChanged)
    // At least one library could not be read: the banner offers a retry
    Q_PROPERTY(bool libraryFailed READ libraryFailed NOTIFY noticeChanged)
    // The libraries a scan reads: zero means the "no libraries" state, not "no favorites"
    Q_PROPERTY(int libraryCount READ libraryCount NOTIFY librariesChanged)
    // The progress of a scan: the libraries read to the end (or failed) and the distinct favorites collected so far
    Q_PROPERTY(int librariesDone READ librariesDone NOTIFY progressChanged)
    Q_PROPERTY(int loadedCount READ loadedCount NOTIFY progressChanged)
    // The filters as a map for QML (the format of LibraryItemsModel::filters): writing replaces every section, a missing key is the section's default, values a filter cannot carry and sections the snapshot has no data for are dropped
    Q_PROPERTY(QVariantMap filters READ filters WRITE setFilters NOTIFY filtersChanged)
    // The values the filters can take: the genres (a row's genreId is its name), tags, official ratings and years of the snapshot
    Q_PROPERTY(Kinefin::FilterOptionsModel *filterOptions READ filterOptions CONSTANT)
    // Whether any filter is set
    Q_PROPERTY(bool filtered READ filtered NOTIFY filtersChanged)
    // How many favorites of the snapshot pass the filters: `count` while none is set, 0 while a scan runs
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchCountChanged)

  public:
    enum Status { Idle, Loading, Complete, Capped, PartialError };
    Q_ENUM(Status)
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        NameRole,
        EpisodeTitleRole,
        SeasonNumberRole,
        EpisodeNumberRole,
        YearRole,
        TypeRole,
        ImageUrlRole,
        PlayedRole,
        UnplayedCountRole,
        PlayedPercentageRole,
        FavoriteRole,
        // The ids and the names of the libraries the item was listed under (string lists, same order)
        LibraryIdsRole,
        LibraryNamesRole
    };

    // The listing channel of the Favorites page; D owns 3072 to 3081
    static constexpr int Channel = 3082;
    static constexpr int PageSize = 100;
    static constexpr int MaxItems = 1000;
    static constexpr int MaxGenres = 16;
    static constexpr int MaxGenreChars = 96;
    static constexpr int MaxTags = 16;
    static constexpr int MaxLibraryNameChars = 128;
    static constexpr int PosterWidth = 400;

    explicit FavoritesModel(JellyfinClient *client, QObject *parent = nullptr);

    // The accessible libraries to scan, in order: the entries of `ConnectionController::libraries`. Entries of another type, with an invalid id or a repeated id are ignored. A different list drops the snapshot and any scan under way, and starts a new scan when the page is open (after load()); the same list changes nothing
    void setLibraries(const QVariantList &libraries);
    // Opens the page: scans when nothing has been loaded yet, otherwise keeps the snapshot or the scan under way
    Q_INVOKABLE void load();
    // Starts a new scan, dropping the snapshot: the Reload and the Retry buttons
    Q_INVOKABLE void reload();
    // Leaves the page: the scan under way is cancelled and its partial result dropped, a finished snapshot stays
    Q_INVOKABLE void close();

    Status status() const { return m_status; }
    int count() const { return int(m_items.size()); }
    bool capped() const { return m_capped; }
    bool libraryFailed() const { return m_libraryFailed; }
    int libraryCount() const { return int(m_libraries.size()); }
    int librariesDone() const;
    int loadedCount() const { return int(m_staging.size()) + count(); }

    // The item of a row, for the per-type views (FavoritesSectionModel)
    const FavoriteItem &itemAt(int row) const { return m_items[size_t(row)]; }
    // The name of a library of the list, empty for an unknown id, and its position in the list (-1 when unknown)
    QString libraryName(const QString &libraryId) const;
    int libraryOrder(const QString &libraryId) const;

    QVariantMap filters() const;
    void setFilters(const QVariantMap &values);
    FilterOptionsModel *filterOptions() { return &m_filterOptions; }
    bool filtered() const { return !m_filters.isEmpty(); }
    int matchCount() const { return m_matchCount; }
    // Whether `item` passes `state`: every section set must match (AND), an item matches a section when it has any of its values (OR). Resumable means a saved position. The sections the snapshot has no data for are ignored
    static bool passes(const FavoriteItem &item, const LibraryFilterState &state);
    // Whether the row passes the filters of now, false for a row that does not exist
    bool passesFilters(int row) const;
    // Whether `values` (the format of `filters`) keeps every list within MaxFilterValues once the values a filter cannot carry and the repeats are dropped: what the filter editor asks before it lets a value in
    Q_INVOKABLE bool filtersFit(const QVariantMap &values) const;
    Q_INVOKABLE void clearFilters();

    // The role names of the model, which its per-type views share
    static QHash<int, QByteArray> itemRoleNames();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return itemRoleNames(); }

  Q_SIGNALS:
    void statusChanged();
    void countChanged();
    void noticeChanged();
    void librariesChanged();
    void progressChanged();
    // The filters changed (a write, clearFilters() or a lost session)
    void filtersChanged();
    void matchCountChanged();

  private:
    struct Library {
        QString id;
        QString name;
        QString includeItemTypes;
        int offset = 0;
        int total = -1;
        bool done = false;
        bool failed = false;
    };

    void startLoad();
    void discardSnapshot();
    // The session is gone: the snapshot and the libraries with it
    void clear();
    void requestNext();
    void finish();
    void fillFilterOptions();
    void assignFilters(const LibraryFilterState &state);
    void refreshMatchCount();
    void failLibrary(int index);
    bool acceptsPage(const Library &library, const QJsonArray &items, int total) const;
    void addItems(const Library &library, const QJsonArray &items);
    void setStatus(Status status);
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);

    JellyfinClient *m_client;
    std::vector<Library> m_libraries;
    // The published snapshot, sorted by name then id
    std::vector<FavoriteItem> m_items;
    // The scan under way: the distinct items so far, by lower-case id
    std::vector<FavoriteItem> m_staging;
    QHash<QString, int> m_stagingIndex;
    QCollator m_collator;
    // Only the sections the snapshot has data for are ever set, cleaned by the same rule as setFilters()
    LibraryFilterState m_filters;
    int m_matchCount = 0;
    FilterOptionsModel m_filterOptions;
    QString m_userId;
    Status m_status = Idle;
    bool m_active = false;
    bool m_capped = false;
    bool m_libraryFailed = false;
    // A new distinct item was turned away because the snapshot is full, or a library was cut at MaxItems with more announced
    bool m_capRejected = false;
    bool m_inFlight = false;
    // The library the request in flight reads, and the generation it was sent in: a reply of an older generation is stale
    int m_pending = -1;
    quint64 m_generation = 0;
    quint64 m_sentGeneration = 0;
    // The library the next request goes to first
    int m_next = 0;
};
} // namespace Kinefin
