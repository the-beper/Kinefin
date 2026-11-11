// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"

#include <QAbstractListModel>
#include <QPointer>

#include <vector>

namespace Kinefin {
// The genres a library can be filtered by, from `GET /Items/Filters2` for the kinds the library lists. A row is a genre id (the hex id that goes on the wire in `genreIds`) and its name (server text: render it as plain text, never as a control's text). The list is unavailable until the server has answered with at least one valid genre; invalid ids, empty names and duplicates are dropped, names are capped and there are at most `MaxOptions` rows. Each instance owns one channel of the `Filters` family, given at construction: never share it with another consumer, and only this channel's answers and failures count. The model empties itself when the client loses its credentials
class FilterOptionsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool available READ available NOTIFY statusChanged)

  public:
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    enum Role { GenreIdRole = Qt::UserRole + 1, NameRole };

    static constexpr int MaxOptions = 500;
    static constexpr int MaxNameLength = 128;

    // A genre name from the server: cut to MaxNameLength without splitting a surrogate pair, then trimmed
    static QString cappedName(const QString &name);

    FilterOptionsModel(JellyfinClient *client, int channel, QObject *parent = nullptr);

    // Asks for the genres of `parentId` (empty: every library) among the kinds of `includeItemTypes` (comma-separated BaseItemKind tokens)
    void load(const QString &parentId, const QString &includeItemTypes);
    void clear();
    Q_INVOKABLE void retry();

    Status status() const { return m_status; }
    bool available() const { return m_status == Ready && !m_options.empty(); }
    bool contains(const QString &genreId) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void statusChanged();
    // The options changed (a new answer or a clear)
    void optionsChanged();

  private:
    struct Option {
        QString genreId;
        QString name;
    };

    void onFiltersReady(const QJsonObject &filters, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void setStatus(Status status);

    QPointer<JellyfinClient> m_client;
    int m_channel;
    QString m_parentId;
    QString m_includeItemTypes;
    Status m_status = Idle;
    std::vector<Option> m_options;
};
} // namespace Kinefin
