// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/SyncPlayProtocol.h"
#include "models/ItemSummary.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>

namespace Kinefin {
class JellyfinClient;

// The SyncPlay group's queue as the server last sent it, for the SyncPlay dialog: one row per entry in the group's order, the playing one marked, its item's name and episode facts read over REST (`/Items?ids=` on the listing channel Channel, a hundred at a time; a row stays nameless until then, or when the item cannot be read). The names are remote text for plain-text labels only
class SyncPlayQueueModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)

  public:
    // The listing channel of the item reads (see the channel table of the architecture notes)
    static constexpr int Channel = 5123;
    enum Role {
        PlaylistItemIdRole = Qt::UserRole + 1,
        ItemIdRole,
        NameRole,
        SeriesNameRole,
        // -1 when the item is no episode or the server gave none
        SeasonNumberRole,
        EpisodeNumberRole,
        CurrentRole
    };

    explicit SyncPlayQueueModel(JellyfinClient *client, QObject *parent = nullptr);
    ~SyncPlayQueueModel() override;

    [[nodiscard]] int currentIndex() const { return m_current; }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The group's new queue; the names already read are kept
    void setQueue(const QList<SyncPlay::QueueEntry> &entries, int playingIndex);
    void clear();

  Q_SIGNALS:
    void countChanged();
    void currentIndexChanged();

  private:
    void readNames();
    void onItems(const QJsonArray &items, int channel);

    JellyfinClient *m_client;
    QList<SyncPlay::QueueEntry> m_entries;
    int m_current = -1;
    // What the server said of each item, by item id; the ids of the read in flight
    QHash<QString, ItemSummary> m_items;
    QStringList m_reading;
};

} // namespace Kinefin
