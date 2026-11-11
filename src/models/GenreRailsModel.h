// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/FilterOptionsModel.h"
#include "models/ItemRowModel.h"

#include <QAbstractListModel>
#include <QList>
#include <QPointer>

#include <vector>

namespace Kinefin {
// The genre rails of one library: a list of genres (20 at a time from `GET /Genres`, at most `MaxRails`), each with a row of items of that genre. A rail loads only when the view asks for it (`want()` when its delegate appears, `unwant()` when it goes), at most `MaxInFlight` at once, the newest wanted first, on listing channels `FirstSlotChannel` up that are released when the model clears. A rail that loads empty is hidden, a failed one has its own `retry()`, and a failed genre list has `retryList()`. The genre list rides on `ListChannel` of the `Genres` family: this must stay its only consumer. The model empties itself when the client loses its credentials. Genre names are server text: render them as plain text
class GenreRailsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY statusChanged)
    Q_PROPERTY(QString libraryId READ libraryId NOTIFY libraryChanged)

  public:
    // The state of the genre list itself
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    enum RailStatus { RailIdle, RailLoading, RailReady, RailFailed };
    Q_ENUM(RailStatus)
    enum Role {
        GenreIdRole = Qt::UserRole + 1,
        NameRole,
        RailStatusRole,
        ItemsRole,
        CountRole,
        ShownRole
    };

    static constexpr int GenresPerPage = 20;
    static constexpr int MaxRails = 100;
    static constexpr int ItemsPerRail = 12;
    static constexpr int MaxInFlight = 3;
    static constexpr int MaxNameLength = FilterOptionsModel::MaxNameLength;
    static constexpr int FirstSlotChannel = 1024;
    static constexpr int ListChannel = 1027;

    explicit GenreRailsModel(JellyfinClient *client, QObject *parent = nullptr);
    ~GenreRailsModel() override;

    // Starts the genre list of a library of this `CollectionType` (movies, tvshows or boxsets); an empty `libraryId` spans every library of the type. False for a type that is not browsable or an invalid id
    bool open(const QString &collectionType, const QString &libraryId = {});
    void clear();

    Status status() const { return m_status; }
    bool canLoadMore() const
    {
        return m_status == Ready && m_more && int(m_rails.size()) < MaxRails;
    }
    QString libraryId() const { return m_libraryId; }

    // The next page of genres (the view calls it when it reaches the end)
    Q_INVOKABLE void loadMore();
    // The view needs this rail's items now / no longer needs them
    Q_INVOKABLE void want(const QString &genreId);
    Q_INVOKABLE void unwant(const QString &genreId);
    Q_INVOKABLE void retry(int row);
    Q_INVOKABLE void retryList();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void statusChanged();
    void libraryChanged();

  private:
    struct Rail {
        QString genreId;
        QString name;
        RailStatus status = RailIdle;
        int slot = -1;
        ItemRowModel *items = nullptr;
    };

    void requestGenres();
    void requestRail(int row, int slot);
    void pump();
    int rowFor(const QString &genreId) const;
    int rowForSlot(int slot) const;
    void releaseSlots();
    void onGenresReady(const QJsonArray &items, int total, int channel);
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void setStatus(Status status);
    void setRailStatus(int row, RailStatus status);

    QPointer<JellyfinClient> m_client;
    bool m_open = false;
    QString m_libraryId;
    QString m_includeTypes;
    Status m_status = Idle;
    bool m_more = false;
    int m_nextStart = 0;
    std::vector<Rail> m_rails;
    // Genre ids the view wants, newest last
    QList<QString> m_wanted;
    // The genre id each slot is loading, or empty when free
    QString m_slots[MaxInFlight];
};
} // namespace Kinefin
