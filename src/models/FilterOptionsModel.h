// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QStringList>

#include <vector>

namespace Kinefin {
// The values a library can be filtered by, from two sources that load and fail on their own. `GET /Items/Filters2` gives the genres (the rows of the list model: a genre id, the hex id that goes on the wire in `genreIds`, and its name) and the tags (plain texts, no ids); `GET /Items/Filters` (the legacy endpoint) gives the official ratings and the production years, and a server without it only hides those two sections (`legacyStatus` Unavailable) without touching the other facets. Everything here is server text: render it as plain text, never as a control's text. A facet is available once its source answered with at least one valid value; invalid ids, empty or unfit names and duplicates are dropped, names are capped and every facet holds at most `MaxOptions` (genres), `MaxTagOptions` (tags) or `MaxFacetOptions` (ratings and years) values. Tag and rating texts live in memory only. Each instance owns one channel, given at construction and used in the `Filters` and `FiltersLegacy` families: never share it with another consumer, and only this channel's answers and failures count; a model filled by `setLocalValues` sends no request and is given -1. The model empties itself when the client loses its credentials
class FilterOptionsModel final : public QAbstractListModel {
    Q_OBJECT
    // The genre and tag source
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    // The official rating and year source
    Q_PROPERTY(Status legacyStatus READ legacyStatus NOTIFY legacyStatusChanged)
    Q_PROPERTY(bool available READ available NOTIFY statusChanged)
    Q_PROPERTY(bool tagsAvailable READ tagsAvailable NOTIFY optionsChanged)
    Q_PROPERTY(bool ratingsAvailable READ ratingsAvailable NOTIFY optionsChanged)
    Q_PROPERTY(bool yearsAvailable READ yearsAvailable NOTIFY optionsChanged)
    Q_PROPERTY(QStringList tags READ tags NOTIFY optionsChanged)
    Q_PROPERTY(int tagCount READ tagCount NOTIFY optionsChanged)
    Q_PROPERTY(QStringList officialRatings READ officialRatings NOTIFY optionsChanged)
    Q_PROPERTY(QList<int> years READ years NOTIFY optionsChanged)

  public:
    // Unavailable is for the legacy source only: the server has no such endpoint, which is not worth a retry
    enum Status { Idle, Loading, Ready, Failed, Unavailable };
    Q_ENUM(Status)
    enum Role { GenreIdRole = Qt::UserRole + 1, NameRole };

    static constexpr int MaxOptions = 500;
    static constexpr int MaxNameLength = 128;
    // The tags the filter dialog lists while no search narrows them
    static constexpr int MaxShownTags = 300;

    // A genre name from the server: cut to MaxNameLength without splitting a surrogate pair, then trimmed
    static QString cappedName(const QString &name);

    FilterOptionsModel(JellyfinClient *client, int channel, QObject *parent = nullptr);

    // Asks for the values of `parentId` (empty: every library) among the kinds of `includeItemTypes` (comma-separated BaseItemKind tokens), from both sources
    void load(const QString &parentId, const QString &includeItemTypes);
    void clear();
    // Asks again for the source or sources that failed
    Q_INVOKABLE void retry();
    // Fills every facet from values the caller holds, with no request: both sources are Ready at once, a genre row's id is its name, and the same caps and checks as the requests apply (the given order is kept, each value once, the others dropped). For a model that has no server to ask, such as the Favorites snapshot
    void setLocalValues(const QStringList &genres, const QStringList &ratings,
                        const QList<int> &years, const QStringList &tags);
    // Both sources Loading while the caller collects its values, with no request; the values held stay until setLocalValues() or clear(), as with load()
    void setLocalLoading();

    Status status() const { return m_status; }
    Status legacyStatus() const { return m_legacyStatus; }
    bool available() const { return m_status == Ready && !m_options.empty(); }
    bool tagsAvailable() const { return m_status == Ready && !m_tags.isEmpty(); }
    bool ratingsAvailable() const
    {
        return m_legacyStatus == Ready && !m_officialRatings.isEmpty();
    }
    bool yearsAvailable() const { return m_legacyStatus == Ready && !m_years.isEmpty(); }
    bool contains(const QString &genreId) const;
    QStringList tags() const { return m_tags; }
    int tagCount() const { return int(m_tags.size()); }
    // The positions in tags() to list for `search`: the tags that hold the trimmed text (case-insensitive), or the first MaxShownTags when it is empty, plus the tags in `keep`, in the server's order
    Q_INVOKABLE QList<int> tagRows(const QString &search, const QStringList &keep) const;
    QStringList officialRatings() const { return m_officialRatings; }
    QList<int> years() const { return m_years; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void statusChanged();
    void legacyStatusChanged();
    // The options changed (a new answer or a clear)
    void optionsChanged();

  private:
    struct Option {
        QString genreId;
        QString name;
    };

    void loadFilters();
    void loadLegacyFilters();
    void onFiltersReady(const QJsonObject &filters, int channel);
    void onLegacyFiltersReady(const QJsonObject &filters, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void onLegacyFiltersFailed(int httpStatus, const QString &reason, int channel);
    void setStatus(Status status);
    void setLegacyStatus(Status status);

    QPointer<JellyfinClient> m_client;
    int m_channel;
    QString m_parentId;
    QString m_includeItemTypes;
    Status m_status = Idle;
    Status m_legacyStatus = Idle;
    std::vector<Option> m_options;
    QStringList m_tags;
    QStringList m_officialRatings;
    QList<int> m_years;
};
} // namespace Kinefin
