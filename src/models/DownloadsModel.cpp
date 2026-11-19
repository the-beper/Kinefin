// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadsModel.h"

#include <algorithm>

namespace Kinefin {
DownloadsModel::DownloadsModel(DownloadManager *manager, QObject *parent)
    : QAbstractListModel(parent), m_manager(manager)
{
    QObject::connect(m_manager, &DownloadManager::recordsChanged, this, &DownloadsModel::refresh);
    QObject::connect(m_manager, &DownloadManager::recordChanged, this,
                     &DownloadsModel::updateRecord);
    refresh();
}

bool DownloadsModel::downloaded(const DownloadRecord &record)
{
    return record.state == QLatin1String("completed") || record.state == QLatin1String("exported");
}

bool DownloadsModel::active(const DownloadRecord &record)
{
    return !downloaded(record);
}

bool DownloadsModel::owned(const DownloadRecord &record) const
{
    return !m_filtered
           || (!m_ownerServerId.isEmpty() && !m_ownerUserId.isEmpty()
               && record.serverId.compare(m_ownerServerId, Qt::CaseInsensitive) == 0
               && record.userId.compare(m_ownerUserId, Qt::CaseInsensitive) == 0);
}

void DownloadsModel::setOwner(const QString &serverId, const QString &userId)
{
    m_filtered = true;
    m_ownerServerId = serverId;
    m_ownerUserId = userId;
    refresh();
}

int DownloadsModel::activeCount() const
{
    return int(std::count_if(m_rows.cbegin(), m_rows.cend(), active));
}

int DownloadsModel::completedCount() const
{
    return int(std::count_if(m_rows.cbegin(), m_rows.cend(), downloaded));
}

qint64 DownloadsModel::storedBytes() const
{
    qint64 bytes = 0;
    for (const DownloadRecord &record : m_rows) {
        if (downloaded(record) && record.size > 0)
            bytes += record.size;
    }
    return bytes;
}

int DownloadsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant DownloadsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const DownloadRecord &record = m_rows.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return record.name;
    case ServerIdRole:
        return record.serverId;
    case UserIdRole:
        return record.userId;
    case ItemIdRole:
        return record.itemId;
    case NameRole:
        return record.name;
    case SeriesNameRole:
        return record.seriesName;
    case TypeRole:
        return record.type;
    case SeasonNumberRole:
        return record.seasonNumber;
    case EpisodeNumberRole:
        return record.episodeNumber;
    case YearRole:
        return record.year;
    case StateRole:
        return record.state;
    case ErrorRole:
        return record.error;
    case SizeRole:
        return record.size;
    case BytesWrittenRole:
        return record.bytesWritten;
    case ProgressRole:
        return record.size > 0 ? qBound(0.0, double(record.bytesWritten) / double(record.size), 1.0)
                               : -1.0;
    case ActiveRole:
        return active(record);
    case DownloadedRole:
        return downloaded(record);
    case SectionRole:
        return downloaded(record) ? QStringLiteral("completed") : QStringLiteral("active");
    case PrimaryTagRole:
        return record.primaryTag;
    case RuleIdRole:
        return record.ruleId;
    }
    return {};
}

QHash<int, QByteArray> DownloadsModel::roleNames() const
{
    return {{ServerIdRole, "serverId"},
            {UserIdRole, "userId"},
            {ItemIdRole, "itemId"},
            {NameRole, "name"},
            {SeriesNameRole, "seriesName"},
            {TypeRole, "type"},
            {SeasonNumberRole, "seasonNumber"},
            {EpisodeNumberRole, "episodeNumber"},
            {YearRole, "year"},
            {StateRole, "recordState"},
            {ErrorRole, "error"},
            {SizeRole, "size"},
            {BytesWrittenRole, "bytesWritten"},
            {ProgressRole, "progress"},
            {ActiveRole, "active"},
            {DownloadedRole, "downloaded"},
            {SectionRole, "section"},
            {PrimaryTagRole, "primaryTag"},
            {RuleIdRole, "ruleId"}};
}

void DownloadsModel::updateRecord(const DownloadRecord &record)
{
    if (!owned(record))
        return;
    const QString key = record.key();
    int row = -1;
    for (int candidate = 0; candidate < m_rows.size(); ++candidate) {
        if (m_rows.at(candidate).key() == key) {
            row = candidate;
            break;
        }
    }
    if (row < 0) {
        refresh();
        return;
    }
    // A record that changed section or queue position is a structural change
    if (active(m_rows.at(row)) != active(record)) {
        refresh();
        return;
    }
    m_rows[row] = record;
    Q_EMIT dataChanged(index(row), index(row));
    Q_EMIT countsChanged();
}

void DownloadsModel::refresh()
{
    QList<DownloadRecord> rows = m_manager->store()->records();
    rows.removeIf([this](const DownloadRecord &record) { return !owned(record); });
    std::sort(rows.begin(), rows.end(),
              [](const DownloadRecord &left, const DownloadRecord &right) {
                  const bool leftActive = active(left);
                  const bool rightActive = active(right);
                  if (leftActive != rightActive)
                      return leftActive;
                  if (leftActive)
                      return left.addedAt == right.addedAt ? left.key() < right.key()
                                                           : left.addedAt < right.addedAt;
                  return left.updatedAt == right.updatedAt ? left.key() < right.key()
                                                           : left.updatedAt > right.updatedAt;
              });
    if (rows == m_rows)
        return;
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    Q_EMIT countsChanged();
}
} // namespace Kinefin
