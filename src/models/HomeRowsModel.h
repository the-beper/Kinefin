// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/ItemRowModel.h"

#include <QAbstractListModel>
#include <QString>
#include <QVariantList>

#include <vector>

namespace Kinefin {
// The rows of the Home page: Continue Watching, Next Up, Favorites (a random pick of the user's favorite movies and series, only while enabled; every reload, after a playback too, picks again) and Recently Added per movie or series library, each loaded on its own listing channel so they run side by side. A row is `Loading` until its reply, `Ready` (shown only when it has items) or `Failed` with its own retry; nothing retries by itself. The model empties itself when the client loses its credentials. `items` is the row's `ItemRowModel` (the Recently Added ones without season counts: on `/Items/Latest` a series' `ChildCount` is not its seasons)
class HomeRowsModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Kind { ContinueWatching, NextUp, RecentlyAdded, Favorites };
    Q_ENUM(Kind)
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    enum Role {
        KindRole = Qt::UserRole + 1,
        LibraryIdRole,
        LibraryNameRole,
        StatusRole,
        ItemsRole,
        LandscapeRole,
        CountRole,
        ShownRole
    };

    // How many items a row asks for
    static constexpr int ItemsPerRow = 24;
    // Next Up looks back this many days, like jellyfin-web's default
    static constexpr int NextUpDays = 365;
    // The listing channel of Continue Watching and Next Up; Recently Added of the n-th library uses FirstLatestChannel + n
    static constexpr int FixedChannel = 0;
    static constexpr int FirstLatestChannel = 10;
    // The listing channel of the Favorites row (the `Items` family)
    static constexpr int FavoritesChannel = 9;

    explicit HomeRowsModel(JellyfinClient *client, QObject *parent = nullptr);

    // Rebuilds the rows for these libraries (`id`, `name`, `collectionType` entries as ConnectionController lists them: the ones whose Recently Added row shows) and loads them all. With `unplayedOnly` the Recently Added rows ask the server for unplayed items only (the user's HidePlayedInLatest)
    void load(const QVariantList &libraries, bool unplayedOnly = false);
    // Turns the Favorites row (right after Next Up) on or off, off by default: while off no request is made for it. Switching with the rows loaded adds the row and requests it, or removes it, and leaves the other rows alone
    void setFavoritesEnabled(bool enabled);
    // Loads everything again for the libraries of the last load()
    Q_INVOKABLE void reload();
    // Asks for one failed row again
    Q_INVOKABLE void retry(int row);
    void clear();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  private:
    struct Row {
        Kind kind = ContinueWatching;
        QString libraryId;
        QString libraryName;
        Status status = Idle;
        int channel = FixedChannel;
        ItemRowModel *items = nullptr;
    };

    Row favoritesRow();
    void request(int row);
    void deliver(Kind kind, int channel, const QJsonArray &items);
    void fail(const QString &family, int channel);
    int rowFor(Kind kind, int channel) const;
    void setStatus(int row, Status status);
    void clearRows();

    JellyfinClient *m_client;
    QVariantList m_libraries;
    bool m_unplayedOnly = false;
    bool m_favorites = false;
    std::vector<Row> m_rows;
};
} // namespace Kinefin
