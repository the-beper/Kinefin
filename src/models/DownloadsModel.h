// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "downloads/DownloadManager.h"

#include <QAbstractListModel>
#include <QList>

namespace Kinefin {
// The rows of the Downloads page: active records first in queue order, then completed ones newest first. Server text (name, series name) is rendered as plain text by the page. A record is a row whether it is queued, running, paused, waiting, failed or done; the engine owns it, the model only mirrors it
class DownloadsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countsChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY countsChanged)
    Q_PROPERTY(int completedCount READ completedCount NOTIFY countsChanged)
    Q_PROPERTY(qint64 storedBytes READ storedBytes NOTIFY countsChanged)

  public:
    enum Role {
        ServerIdRole = Qt::UserRole + 1,
        UserIdRole,
        ItemIdRole,
        NameRole,
        SeriesNameRole,
        TypeRole,
        SeasonNumberRole,
        EpisodeNumberRole,
        YearRole,
        StateRole,
        ErrorRole,
        SizeRole,
        BytesWrittenRole,
        ProgressRole,
        ActiveRole,
        DownloadedRole,
        SectionRole,
        PrimaryTagRole,
        RuleIdRole
    };

    explicit DownloadsModel(DownloadManager *manager, QObject *parent = nullptr);

    int count() const { return int(m_rows.size()); }
    int activeCount() const;
    int completedCount() const;
    qint64 storedBytes() const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Lists only the records of this owner: the other owners' stay in the engine, unlisted. An empty id matches nothing; without a call every record is listed
    void setOwner(const QString &serverId, const QString &userId);
    // Rebuilds the rows from the engine; called by the engine's signals and by the tests
    void refresh();

  Q_SIGNALS:
    void countsChanged();

  private:
    static bool downloaded(const DownloadRecord &record);
    static bool active(const DownloadRecord &record);
    bool owned(const DownloadRecord &record) const;
    void updateRecord(const DownloadRecord &record);

    DownloadManager *m_manager = nullptr;
    QList<DownloadRecord> m_rows;
    QString m_ownerServerId;
    QString m_ownerUserId;
    bool m_filtered = false;
};
} // namespace Kinefin
