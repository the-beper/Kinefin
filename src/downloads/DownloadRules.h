// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <optional>

namespace Kinefin {
// One automatic download rule of a series: the next N unwatched episodes, a whole season's unwatched episodes, or new episodes of an airing series. Rules only add unwatched items; the cursor is the newest episode already handled
struct DownloadRule {
    QString serverId;
    QString userId;
    QString seriesId;
    // The series name at the time the rule was made, for the tracked list
    QString seriesName;
    // nextN, season or newEpisodes
    QString kind;
    int seasonNumber = -1;
    int count = 3;
    bool enabled = true;
    // The newest DateCreated/id pair the scanner handled, empty before the first scan
    QString cursorDate;
    QString cursorId;

    bool operator==(const DownloadRule &) const = default;
    QString key() const;
};

// `<data>/downloads/rules.json`: versioned, atomic 0600, one rule per series and owner, at most `MaxRules`; a malformed file is kept aside as `.broken`, an invalid rule dropped alone
class DownloadRules final : public QObject {
    Q_OBJECT

  public:
    static constexpr int FileVersion = 1;
    static constexpr int MaxRules = 1000;

    explicit DownloadRules(QObject *parent = nullptr);
    ~DownloadRules() override;

    void setStoragePath(const QString &path);
    QString storagePath() const { return m_path; }
    QList<DownloadRule> rules() const { return m_rules; }
    std::optional<DownloadRule> find(const QString &serverId, const QString &userId,
                                     const QString &seriesId) const;
    bool upsert(const DownloadRule &rule);
    bool remove(const QString &serverId, const QString &userId, const QString &seriesId);
    void flush();

  Q_SIGNALS:
    void changed();
    void saveFailed();

  private:
    void load();
    bool write();

    QString m_path;
    QList<DownloadRule> m_rules;
    bool m_dirty = false;
    bool m_saveFailing = false;
};
} // namespace Kinefin
