// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/CardRoles.h"
#include "models/ItemRowModel.h"
#include "models/ItemSummary.h"
#include "models/PersonEpisodesModel.h"

#include <QAbstractListModel>

#include <functional>

namespace Kinefin {
// What one person appears in, as landscape cards sorted by name: the movies and the series they are in the main cast of, plus one card per other series in which they have episodes (the guest roles), with the episode count. Two requests, each on a listing channel of its own, are read page by page (`PageSize`) up to `Limit` items; the rows appear when both have answered. `open()` loads a person id; the page that shows it calls `close()` when it goes. `showSeries()` fills `seriesEpisodes` with the person's episodes of one series, for the page behind a guest-series card
class PersonItemsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY totalCountChanged)
    Q_PROPERTY(bool pageError READ pageError NOTIFY pageErrorChanged)
    Q_PROPERTY(Kinefin::PersonEpisodesModel *seriesEpisodes READ seriesEpisodes CONSTANT)

  public:
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)

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
        // The episodes of a series the person is in only through episodes, 0 for any other card
        GuestEpisodesRole,
        LoadedRole
    };

    // Each of the two lists holds this many items at most: a person in more shows the first ones
    static constexpr int Limit = 1000;
    // The lists come in pages of this many: a thousand episodes with their overviews would pass the client's cap on the size of a reply
    static constexpr int PageSize = 200;
    // The movies and series of the person, and their episodes
    static constexpr int Channel = 6146;
    static constexpr int EpisodesChannel = 6148;
    static constexpr int ImageWidth = ItemRowModel::LandscapeWidth;

    explicit PersonItemsModel(JellyfinClient *client, QObject *parent = nullptr);

    // Resets the model and loads the person; false (and the model stays empty) when the request is refused
    Q_INVOKABLE bool open(const QString &personId);
    // Empties the model and releases both listing channels
    Q_INVOKABLE void close();
    // Asks again for the requests that failed
    Q_INVOKABLE void retry();
    // Fills `seriesEpisodes` with the person's episodes of a series; false when there are none
    Q_INVOKABLE bool showSeries(const QString &seriesId);

    Status status() const { return m_status; }
    int totalCount() const { return int(m_rows.size()); }
    bool pageError() const { return m_pageError; }
    PersonEpisodesModel *seriesEpisodes() { return &m_seriesEpisodes; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void statusChanged();
    void totalCountChanged();
    void pageErrorChanged();

  private:
    struct Row {
        ItemSummary item;
        int guestEpisodes = 0;
    };

    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    bool sendTitles();
    bool sendEpisodes();
    // Both requests have answered: builds the rows and the status
    void settle();
    void update(const QString &itemId, const std::function<void(ItemSummary &)> &change);
    void setStatus(Status status);
    void setPageError(bool error);

    JellyfinClient *m_client;
    PersonEpisodesModel m_seriesEpisodes;
    QString m_personId;
    bool m_open = false;
    Status m_status = Idle;
    bool m_pageError = false;
    bool m_titlesPending = false;
    bool m_episodesPending = false;
    bool m_titlesFailed = false;
    bool m_episodesFailed = false;
    // The start of the next page of each list: a failed page is asked again from there
    int m_titlesNext = 0;
    int m_episodesNext = 0;
    QList<ItemSummary> m_titles;
    QList<ItemSummary> m_episodes;
    QList<Row> m_rows;
};
} // namespace Kinefin
