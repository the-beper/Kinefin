// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "models/ItemSummary.h"

#include <QAbstractListModel>
#include <QJsonArray>
#include <QPointer>

#include <vector>

namespace Kinefin {
class JellyfinClient;

// The entries of one playlist in its order, as the Playlist page lists them: the rows of the queue (same roles) plus the server's `entryId` (`PlaylistItemId`, empty when it is not a valid id). Read page by page, at most MaxEntries in MaxPages; the rows already shown stay until the whole new list is read, and stay as they are when it shows the same. One consumer at a time, on its own listing channel
class PlaylistEntriesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString playlistId READ playlistId NOTIFY statusChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int count READ count NOTIFY statusChanged)
    // What the server holds, and whether the cap cut the list
    Q_PROPERTY(int total READ total NOTIFY statusChanged)
    Q_PROPERTY(bool capped READ capped NOTIFY statusChanged)

  public:
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        NameRole,
        SeriesNameRole,
        SeasonNumberRole,
        EpisodeNumberRole,
        YearRole,
        RuntimeMinutesRole,
        TypeRole,
        ImageUrlRole,
        PlayedRole,
        EntryIdRole
    };
    static constexpr int Channel = 5121;
    static constexpr int PageSize = 100;
    static constexpr int MaxEntries = 1000;
    // The pages one read asks for at most, the cap's and two more
    static constexpr int MaxPages = MaxEntries / PageSize + 2;
    static constexpr int ImageWidth = 240;

    explicit PlaylistEntriesModel(JellyfinClient *client, QObject *parent = nullptr);

    QString playlistId() const { return m_playlistId; }
    Status status() const { return m_status; }
    int count() const { return int(m_rows.size()); }
    int total() const { return m_total; }
    bool capped() const { return m_capped; }

    // Reads the playlist from its first entry; another playlist's rows go at once
    Q_INVOKABLE void load(const QString &playlistId);
    // A read in flight is dropped and the channel released; the rows stay
    Q_INVOKABLE void close();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void statusChanged();

  private:
    struct Entry {
        ItemSummary item;
        QString entryId;
    };
    static bool sameEntry(const Entry &a, const Entry &b);
    void onEntries(const QJsonArray &items, int total, int channel);
    void requestPage();
    void publish();
    void reset();

    QPointer<JellyfinClient> m_client;
    QString m_playlistId;
    Status m_status = Idle;
    int m_total = 0;
    std::vector<Entry> m_rows;
    // The read in flight: what it collected and where the next page starts
    std::vector<Entry> m_pending;
    int m_received = 0;
    int m_pages = 0;
    // Where the page in flight started in `m_pending`: a page that adds nothing ends the read
    size_t m_pageStart = 0;
    bool m_capped = false;
};
} // namespace Kinefin
