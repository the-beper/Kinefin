// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SearchModel.h"

#include <QJsonArray>

#include <algorithm>

namespace Kinefin {
namespace {
QString includeTypesFor(SearchModel::Kind kind)
{
    switch (kind) {
    case SearchModel::Movies:
        return QStringLiteral("Movie");
    case SearchModel::Series:
        return QStringLiteral("Series");
    case SearchModel::Episodes:
        return QStringLiteral("Episode");
    case SearchModel::Collections:
        return QStringLiteral("BoxSet");
    }
    return {};
}
} // namespace

SearchModel::SearchModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(DebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, [this] { runSearch(); });
    // Four fixed groups: the rows never come and go, only their state does
    for (int i = 0; i < GroupCount; ++i) {
        const Kind kind = Kind(i);
        m_groups.push_back({kind, Idle, new ItemRowModel(client, kind == Episodes, this)});
    }
    if (m_client) {
        connect(m_client, &JellyfinClient::itemsReady, this, &SearchModel::onItemsReady);
        connect(m_client, &JellyfinClient::listingFailed, this, &SearchModel::onListingFailed);
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
}

QString SearchModel::normalized(const QString &term)
{
    QString clean;
    clean.reserve(term.size());
    for (const QChar c : term) {
        // Control characters and the invisible direction controls go; joiners stay (they belong to some scripts and to emoji sequences)
        const char16_t u = c.unicode();
        const bool direction = (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069)
                               || u == 0xFEFF || u == 0x200E || u == 0x200F || u == 0x061C;
        if (c.category() == QChar::Other_Control || direction)
            clean.append(QLatin1Char(' '));
        else
            clean.append(c);
    }
    clean = clean.simplified();
    if (clean.size() > MaxTermLength) {
        clean.truncate(MaxTermLength);
        // A cut between the halves of a surrogate pair would leave a lone surrogate on the wire
        if (clean.back().isHighSurrogate())
            clean.chop(1);
    }
    return clean.simplified();
}

void SearchModel::setTerm(const QString &term)
{
    if (term == m_term)
        return;
    m_term = term;
    Q_EMIT termChanged();
    const QString clean = normalized(term);
    if (clean.size() < MinTermLength) {
        // Nothing worth asking for: the results of the older term go with their requests
        m_debounce.stop();
        abortRequests();
        resetGroups();
        m_activeTerm.clear();
        setState(clean.isEmpty() ? Empty : TooShort);
        return;
    }
    if (clean == m_activeTerm) {
        // The same words again (a trailing space, say): what is shown or in flight stays
        m_debounce.stop();
        return;
    }
    m_debounce.start();
}

void SearchModel::searchNow()
{
    const QString clean = normalized(m_term);
    if (clean.size() < MinTermLength)
        return;
    m_debounce.stop();
    if (clean == m_activeTerm) {
        // The same words: what is shown stays, and only the groups that failed are asked again
        for (int group = 0; group < int(m_groups.size()); ++group) {
            if (m_groups[size_t(group)].status == Failed)
                request(group);
        }
        updateState();
        return;
    }
    runSearch();
}

void SearchModel::runSearch()
{
    const QString clean = normalized(m_term);
    if (clean.size() < MinTermLength)
        return;
    m_activeTerm = clean;
    for (int group = 0; group < int(m_groups.size()); ++group)
        request(group);
    updateState();
}

void SearchModel::request(int group)
{
    ItemQuery query;
    query.searchTerm = m_activeTerm;
    query.includeItemTypes = includeTypesFor(m_groups[size_t(group)].kind);
    query.recursive = true;
    query.limit = ItemsPerGroup;
    query.fields = {QStringLiteral("PrimaryImageAspectRatio")};
    query.cardImages = true;
    setGroupStatus(group, Loading);
    if (!m_client || !m_client->fetchItems(query, FirstChannel + group)) {
        m_groups[size_t(group)].items->setItems({});
        setGroupStatus(group, Failed);
    }
}

void SearchModel::retry(int row)
{
    if (row < 0 || row >= int(m_groups.size()) || m_groups[size_t(row)].status != Failed
        || m_activeTerm.isEmpty())
        return;
    request(row);
    updateState();
}

void SearchModel::clear()
{
    m_debounce.stop();
    abortRequests();
    resetGroups();
    m_activeTerm.clear();
    if (!m_term.isEmpty()) {
        m_term.clear();
        Q_EMIT termChanged();
    }
    setState(Empty);
}

void SearchModel::abortRequests()
{
    if (!m_client)
        return;
    for (int i = 0; i < GroupCount; ++i)
        m_client->releaseListingChannel(FirstChannel + i);
}

void SearchModel::resetGroups()
{
    for (int group = 0; group < int(m_groups.size()); ++group) {
        m_groups[size_t(group)].items->setItems({});
        setGroupStatus(group, Idle);
    }
}

int SearchModel::groupFor(int channel)
{
    const int group = channel - FirstChannel;
    return group >= 0 && group < GroupCount ? group : -1;
}

void SearchModel::onItemsReady(const QJsonArray &items, int, int channel)
{
    const int group = groupFor(channel);
    if (group < 0 || m_groups[size_t(group)].status != Loading)
        return;
    m_groups[size_t(group)].items->setItems(items);
    setGroupStatus(group, Ready);
    updateState();
}

void SearchModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    const int group = groupFor(channel);
    if (family != QLatin1String("Items") || group < 0 || m_groups[size_t(group)].status != Loading)
        return;
    // The cards of an older term must not stay under the error of the new one
    m_groups[size_t(group)].items->setItems({});
    setGroupStatus(group, Failed);
    updateState();
}

void SearchModel::setGroupStatus(int group, Status status)
{
    Group &entry = m_groups[size_t(group)];
    if (entry.status == status)
        return;
    entry.status = status;
    const QModelIndex changed = index(group);
    Q_EMIT dataChanged(changed, changed, {StatusRole, CountRole, ShownRole});
}

void SearchModel::updateState()
{
    if (m_activeTerm.isEmpty())
        return;
    bool loading = false;
    bool anyResults = false;
    bool anyFailed = false;
    bool allFailed = true;
    for (const Group &group : m_groups) {
        loading = loading || group.status == Loading;
        anyResults = anyResults || group.items->count() > 0;
        anyFailed = anyFailed || group.status == Failed;
        allFailed = allFailed && group.status == Failed;
    }
    if (loading)
        setState(Searching);
    else if (allFailed)
        setState(AllFailed);
    else if (anyResults || anyFailed)
        // A failed group is unknown, not empty: its box and Retry stay visible next to the others
        setState(Results);
    else
        setState(NoResults);
}

void SearchModel::setState(State state)
{
    if (state == m_state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

int SearchModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_groups.size());
}

QHash<int, QByteArray> SearchModel::roleNames() const
{
    return {{KindRole, "kind"},           {StatusRole, "status"}, {ItemsRole, "items"},
            {LandscapeRole, "landscape"}, {CountRole, "count"},   {ShownRole, "shown"}};
}

QVariant SearchModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= int(m_groups.size()))
        return {};
    const Group &group = m_groups[size_t(index.row())];
    switch (role) {
    case KindRole:
        return group.kind;
    case StatusRole:
        return group.status;
    case ItemsRole:
        return QVariant::fromValue<QObject *>(group.items);
    case LandscapeRole:
        return group.items->landscape();
    case CountRole:
        return group.items->count();
    case ShownRole:
        // An empty group is hidden once it is loaded; a loading or failed one stays visible
        return group.status == Loading || group.status == Failed || group.items->count() > 0;
    default:
        return {};
    }
}
} // namespace Kinefin
