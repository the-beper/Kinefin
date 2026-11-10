// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/ItemSummary.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QTimer>

#include <optional>
#include <vector>

namespace Kinefin {
// The items of one library as a list that pages in on demand. `rowCount` is the server's `TotalRecordCount` once the first page arrived; a row whose page is not resident is a placeholder (`loaded` false) and reading it queues its page. One page is in flight at a time (the client keeps one `Items` request per family and its own listing channel keeps other consumers of `/Items` out), the most recently read pages go first, and only the most recently used pages stay resident. Nothing retries by itself: a failure waits for `retry()`. The model empties itself whenever the client loses its credentials
class LibraryItemsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY totalCountChanged)
    Q_PROPERTY(bool pageError READ pageError NOTIFY pageErrorChanged)
    Q_PROPERTY(SortKey sortKey READ sortKey WRITE setSortKey NOTIFY sortKeyChanged)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY descendingChanged)
    Q_PROPERTY(Filter filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool resumableAvailable READ resumableAvailable NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryName READ libraryName NOTIFY libraryChanged)

  public:
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
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
    static constexpr int MaxResidentPages = 8;
    static constexpr int MaxWantedPages = 4;
    // The listing channel of the grid: the home rows use others, so neither sees the other's replies
    static constexpr int Channel = 1;
    // A larger TotalRecordCount from the server is clamped to this many rows
    static constexpr int MaxRows = 1'000'000;
    // The width requested for a poster: the tiles show at most about this many device pixels
    static constexpr int ImageWidth = 400;

    explicit LibraryItemsModel(JellyfinClient *client, QObject *parent = nullptr);

    // The query that lists the items of a library of this `CollectionType`, or nothing when the type is not browsable (movies, tvshows and boxsets are)
    static std::optional<ItemQuery> browseQuery(const QString &collectionType);

    // Resets the model and loads the first page. False (and the model stays empty) for a library id the server could not have sent or a type that is not browsable
    bool open(const QString &libraryId, const QString &collectionType,
              const QString &libraryName = QString());
    void clear();
    Q_INVOKABLE void retry();

    Status status() const { return m_status; }
    int totalCount() const { return m_total < 0 ? 0 : m_total; }
    bool pageError() const { return m_pageError; }
    SortKey sortKey() const { return m_sortKey; }
    bool descending() const { return m_descending; }
    Filter filter() const { return m_filter; }
    bool resumableAvailable() const { return m_collectionType == QLatin1String("movies"); }
    QString libraryName() const { return m_libraryName; }
    void setSortKey(SortKey key);
    void setDescending(bool descending);
    void setFilter(Filter filter);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // How many pages are loaded and in what order they were used, for the tests
    QList<int> residentPages() const { return m_residentOrder; }

  Q_SIGNALS:
    void statusChanged();
    void totalCountChanged();
    void pageErrorChanged();
    void sortKeyChanged();
    void descendingChanged();
    void filterChanged();
    void libraryChanged();

  private:
    void resetState();
    void resetOrderAndFilter();
    void setFilterQuietly(Filter filter);
    void restart();
    void requestPage(int page);
    void pump();
    void noteWanted(int page) const;
    void touch(int page) const;
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void failPage(int page);
    void storePage(int page, const QJsonArray &items);
    void evictBeyondLimit(int keep);
    void dropPage(int page);
    void dropOtherPages(int keep);
    void resizeTo(int total);
    void setStatus(Status status);
    void setPageError(bool error);
    int rowsInPage(int page) const;

    JellyfinClient *m_client;
    ItemQuery m_base;
    QString m_libraryId;
    QString m_collectionType;
    QString m_libraryName;
    SortKey m_sortKey = Name;
    bool m_descending = false;
    Filter m_filter = All;
    Status m_status = Idle;
    int m_total = -1;
    bool m_pageError = false;
    int m_inFlight = -1;
    QHash<int, std::vector<ItemSummary>> m_pages;
    QSet<int> m_failedPages;
    // Read-driven bookkeeping touched from the const data(): the pages asked for (newest last) and the resident pages by last use (newest last)
    mutable QList<int> m_wanted;
    mutable QList<int> m_residentOrder;
    mutable QTimer m_pumpTimer;
};
} // namespace Kinefin
