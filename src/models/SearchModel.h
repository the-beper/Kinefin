// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/ItemRowModel.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <vector>

namespace Kinefin {
// The results of a search as four groups (movies, series, episodes, collections), one `/Items` request per group on a listing channel of its own. Typing sets `term`; a pause of `DebounceMs` (or `searchNow()`) sends the requests, a newer term aborts the ones still in flight, and a term shorter than `MinTermLength` sends nothing. A group is `Loading`, `Ready` (shown only with results) or `Failed` with its own `retry()`. The model empties itself when the client loses its credentials. Everything shown is server data, and the typed term is never echoed back
class SearchModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString term READ term WRITE setTerm NOTIFY termChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(int minTermLength READ minTermLength CONSTANT)

  public:
    enum State { Empty, TooShort, Searching, Results, NoResults, AllFailed };
    Q_ENUM(State)
    enum Kind { Movies, Series, Episodes, Collections };
    Q_ENUM(Kind)
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    enum Role {
        KindRole = Qt::UserRole + 1,
        StatusRole,
        ItemsRole,
        LandscapeRole,
        CountRole,
        ShownRole
    };

    static constexpr int DebounceMs = 400;
    static constexpr int MinTermLength = 2;
    static constexpr int MaxTermLength = 128;
    static constexpr int ItemsPerGroup = 24;
    // The listing channels of the four groups (the library grid uses 1, the details pages 65536 up)
    static constexpr int FirstChannel = 2;
    static constexpr int GroupCount = 4;

    explicit SearchModel(JellyfinClient *client, QObject *parent = nullptr);

    QString term() const { return m_term; }
    State state() const { return m_state; }
    static constexpr int minTermLength() { return MinTermLength; }
    void setTerm(const QString &term);
    // For tests; the default is DebounceMs
    void setDebounceInterval(int milliseconds) { m_debounce.setInterval(milliseconds); }

    // Sends the requests of the term now, without waiting for the pause
    Q_INVOKABLE void searchNow();
    // Asks one failed group again
    Q_INVOKABLE void retry(int row);
    // Drops the term, the results and the requests in flight
    Q_INVOKABLE void clear();

    // The term as it goes on the wire: whitespace simplified, control characters dropped, at most MaxTermLength characters
    static QString normalized(const QString &term);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void termChanged();
    void stateChanged();

  private:
    struct Group {
        Kind kind = Movies;
        Status status = Idle;
        ItemRowModel *items = nullptr;
    };

    void runSearch();
    void request(int group);
    void abortRequests();
    void resetGroups();
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    int groupFor(int channel) const;
    void setGroupStatus(int group, Status status);
    void updateState();
    void setState(State state);

    QPointer<JellyfinClient> m_client;
    QString m_term;
    // The term whose requests were sent last: the same term again sends nothing
    QString m_activeTerm;
    State m_state = Empty;
    QTimer m_debounce;
    std::vector<Group> m_groups;
};
} // namespace Kinefin
