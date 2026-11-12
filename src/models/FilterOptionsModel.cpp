// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FilterOptionsModel.h"

#include "api/MediaSourceSelection.h"
#include "models/LibraryFilterState.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace Kinefin {

FilterOptionsModel::FilterOptionsModel(JellyfinClient *client, int channel, QObject *parent)
    : QAbstractListModel(parent), m_client(client), m_channel(channel)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::filtersReady, this, &FilterOptionsModel::onFiltersReady);
        connect(m_client, &JellyfinClient::legacyFiltersReady, this,
                &FilterOptionsModel::onLegacyFiltersReady);
        connect(m_client, &JellyfinClient::listingFailed, this,
                &FilterOptionsModel::onListingFailed);
        connect(m_client, &JellyfinClient::legacyFiltersFailed, this,
                &FilterOptionsModel::onLegacyFiltersFailed);
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
}

QString FilterOptionsModel::cappedName(const QString &name)
{
    QString capped = name.left(MaxNameLength);
    if (capped.size() < name.size() && capped.back().isHighSurrogate())
        capped.chop(1);
    return capped.trimmed();
}

void FilterOptionsModel::load(const QString &parentId, const QString &includeItemTypes)
{
    m_parentId = parentId;
    m_includeItemTypes = includeItemTypes;
    loadFilters();
    loadLegacyFilters();
}

void FilterOptionsModel::loadFilters()
{
    setStatus(Loading);
    if (!m_client || !m_client->fetchFilters(m_parentId, m_includeItemTypes, m_channel))
        setStatus(Failed);
}

void FilterOptionsModel::loadLegacyFilters()
{
    setLegacyStatus(Loading);
    if (!m_client || !m_client->fetchLegacyFilters(m_parentId, m_includeItemTypes, m_channel))
        setLegacyStatus(Failed);
}

void FilterOptionsModel::retry()
{
    if (m_status == Failed)
        loadFilters();
    if (m_legacyStatus == Failed)
        loadLegacyFilters();
}

void FilterOptionsModel::clear()
{
    const bool hadOptions = !m_options.empty() || !m_tags.isEmpty() || !m_officialRatings.isEmpty()
                            || !m_years.isEmpty();
    if (!m_options.empty()) {
        beginResetModel();
        m_options.clear();
        endResetModel();
    }
    m_tags.clear();
    m_officialRatings.clear();
    m_years.clear();
    m_parentId.clear();
    m_includeItemTypes.clear();
    setStatus(Idle);
    setLegacyStatus(Idle);
    if (hadOptions)
        Q_EMIT optionsChanged();
}

bool FilterOptionsModel::contains(const QString &genreId) const
{
    for (const Option &option : m_options) {
        if (option.genreId.compare(genreId, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

void FilterOptionsModel::onFiltersReady(const QJsonObject &filters, int channel)
{
    if (channel != m_channel || m_status != Loading)
        return;
    std::vector<Option> options;
    const QJsonValue genres = filters.value(QStringLiteral("Genres"));
    if (genres.isArray()) {
        QSet<QString> seen;
        for (const QJsonValue &entry : genres.toArray()) {
            if (int(options.size()) == MaxOptions)
                break;
            const QJsonObject genre = entry.toObject();
            const QString id = genre.value(QStringLiteral("Id")).toString();
            const QString name = cappedName(genre.value(QStringLiteral("Name")).toString());
            // An id the server could not have sent, a nameless genre or a repeated one is not an option
            if (!isValidJellyfinId(id) || name.isEmpty() || seen.contains(id.toLower()))
                continue;
            seen.insert(id.toLower());
            options.push_back({id, name});
        }
    }
    // A tag is a text the query can carry: the rest (too long, a pipe, control characters) could never be selected
    QStringList tags;
    const QJsonValue tagValues = filters.value(QStringLiteral("Tags"));
    if (tagValues.isArray()) {
        for (const QJsonValue &entry : tagValues.toArray()) {
            if (tags.size() == MaxFacetOptions)
                break;
            const QString tag = entry.toString();
            if (entry.isString() && isValidFilterText(tag) && !tags.contains(tag))
                tags.append(tag);
        }
    }
    beginResetModel();
    m_options = std::move(options);
    endResetModel();
    m_tags = tags;
    setStatus(Ready);
    Q_EMIT optionsChanged();
}

void FilterOptionsModel::onLegacyFiltersReady(const QJsonObject &filters, int channel)
{
    if (channel != m_channel || m_legacyStatus != Loading)
        return;
    const LegacyFacets facets = legacyFacetsFrom(filters);
    m_officialRatings = facets.officialRatings;
    m_years = facets.years;
    setLegacyStatus(Ready);
    Q_EMIT optionsChanged();
}

void FilterOptionsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (family != QLatin1String("Filters") || channel != m_channel || m_status != Loading)
        return;
    setStatus(Failed);
}

void FilterOptionsModel::onLegacyFiltersFailed(int httpStatus, const QString &, int channel)
{
    if (channel != m_channel || m_legacyStatus != Loading)
        return;
    setLegacyStatus(httpStatus == 404 ? Unavailable : Failed);
}

void FilterOptionsModel::setStatus(Status status)
{
    if (status == m_status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void FilterOptionsModel::setLegacyStatus(Status status)
{
    if (status == m_legacyStatus)
        return;
    m_legacyStatus = status;
    Q_EMIT legacyStatusChanged();
}

int FilterOptionsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_options.size());
}

QHash<int, QByteArray> FilterOptionsModel::roleNames() const
{
    return {{GenreIdRole, "genreId"}, {NameRole, "name"}};
}

QVariant FilterOptionsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= int(m_options.size()))
        return {};
    const Option &option = m_options[size_t(index.row())];
    switch (role) {
    case GenreIdRole:
        return option.genreId;
    case Qt::DisplayRole:
    case NameRole:
        return option.name;
    default:
        return {};
    }
}
} // namespace Kinefin
