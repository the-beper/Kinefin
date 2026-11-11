// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/CardRoles.h"
#include "models/ItemSummary.h"

#include <QAbstractListModel>
#include <QCollator>
#include <QHash>
#include <QJsonArray>
#include <QStringList>
#include <QVariantList>

#include <vector>

namespace Kinefin {
// One favorite of the snapshot: the card data, the libraries it was listed under (in library order) and its main genre
struct FavoriteItem {
    ItemSummary summary;
    QStringList libraryIds;
    // The first valid genre the server gave (plain text, capped), empty when there is none
    QString primaryGenre;
};

// The favorites of the signed-in user across the accessible libraries, as one deduplicated snapshot. The libraries come from the `libraries` entries of the connection controller (`id`, `name`, `collectionType`); only movies, tvshows and boxsets libraries are used. They are queried one request at a time on `Channel`, a page of `PageSize` of each in turn (round robin) with `filters=IsFavorite` and the library as parent, so the library an item was seen in is known from the request and never guessed. The same item listed by several libraries is one row that carries all of them. The snapshot holds at most `MaxItems` distinct items and is published in one piece when the scan ends: `Complete` (every library read to its end), `Capped` (the limit was reached while more could follow: the list may be incomplete) or `PartialError` (a library failed, the others still show; `capped` tells whether the limit was reached too). The model is empty while `Loading`, and `Idle` before the first load and after any session loss. A stale reply never reaches the snapshot: a new scan, a changed library list, `close()` and a lost session all release the channel. A lost session (credentialsCleared) also forgets the libraries, the caller sets them again for the next session. Besides its own roles the model has the card roles (see CardRoles.h) with poster images
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
        LibraryNamesRole,
        // The main genre, empty when the item has none
        GenreRole
    };

    // The listing channel of the Favorites page; D owns 3072 to 3081
    static constexpr int Channel = 3082;
    static constexpr int PageSize = 100;
    static constexpr int MaxItems = 1000;
    static constexpr int MaxGenres = 16;
    static constexpr int MaxGenreChars = 96;
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

    // The item of a row, for the grouping
    const FavoriteItem &itemAt(int row) const { return m_items[size_t(row)]; }
    // The name of a library of the list, empty for an unknown id, and its position in the list (-1 when unknown)
    QString libraryName(const QString &libraryId) const;
    int libraryOrder(const QString &libraryId) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void statusChanged();
    void countChanged();
    void noticeChanged();
    void librariesChanged();
    void progressChanged();

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
    QString m_userId;
    Status m_status = Idle;
    bool m_active = false;
    bool m_capped = false;
    bool m_libraryFailed = false;
    // A new distinct item was turned away because the snapshot is full
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
