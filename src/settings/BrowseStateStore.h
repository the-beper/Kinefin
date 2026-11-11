// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <optional>

namespace Kinefin {
// What the movie and TV tabs remember per server and user: which library of a type is selected, and the sort, filter and genres of each scope (a tab over all libraries, or one library). Ids and enum key names only, never server text. Kept in one file with a debounced write; a pending write is flushed by `flush()` (the owner calls it when the session ends) and when the store is destroyed. Whatever cannot be trusted is dropped on load: a link, a non-regular or oversized file goes, a file of a later version is kept aside as `.broken`. At most `MaxViews` views are kept (the one least recently written goes first) and the file stays within `MaxFileBytes`
class BrowseStateStore final : public QObject {
    Q_OBJECT

  public:
    // Enum members are their key names (`DateAdded`, `Unplayed`); the store does not know the enums
    struct ViewState {
        QString sort;
        bool descending = false;
        QString filter;
        QStringList genreIds;
        bool operator==(const ViewState &) const = default;
    };

    static constexpr int MaxViews = 200;
    static constexpr int MaxGenreIds = 64;
    // A stored sequence number above this is a damaged file: counting on from it would overflow
    static constexpr qint64 MaxSeq = qint64(1) << 52;
    static constexpr qint64 MaxFileBytes = 256 * 1024;
    static constexpr int FileVersion = 1;

    explicit BrowseStateStore(QObject *parent = nullptr);
    ~BrowseStateStore() override;

    // The file the state is kept in; read at once, replacing what is in memory (a write pending for the previous file is flushed first). Empty, the default, keeps nothing
    void setStoragePath(const QString &path);
    void setSaveDelay(std::chrono::milliseconds delay);

    // The library selected for a tab type ("movies" or "tvshows"); empty when all libraries are meant or nothing is stored
    QString selection(const QString &serverId, const QString &userId, const QString &type) const;
    // An empty `libraryId` stands for all libraries; an invalid argument is ignored
    void setSelection(const QString &serverId, const QString &userId, const QString &type,
                      const QString &libraryId);

    // The scope key is `movies`, `tvshows`, `tvshows:<libraryId>`, `movies:<libraryId>` or `boxsets:<libraryId>`, lower case without dashes by the caller's convention. `Resumable` is read as `All` in a TV scope: there is no such filter over series
    std::optional<ViewState> view(const QString &serverId, const QString &userId,
                                  const QString &scopeKey) const;
    // Writing the state a scope already has changes nothing; an invalid argument is ignored
    void setView(const QString &serverId, const QString &userId, const QString &scopeKey,
                 const ViewState &state);

    // Drops everything stored for the server, every user's
    void forgetServer(const QString &serverId);

    // Writes now what is pending
    void flush();

  Q_SIGNALS:
    // A write failed (once per streak of failures)
    void saveFailed();

  private:
    struct StoredView {
        ViewState state;
        qint64 seq = 0;
    };
    struct UserState {
        QHash<QString, QString> selection;
        QHash<QString, StoredView> views;
    };
    using Users = QHash<QString, UserState>;

    void load();
    void scheduleSave();
    const UserState *userState(const QString &serverId, const QString &userId) const;
    // Drops the view written longest ago; false when there is none
    bool evictOldest();
    int viewCount() const;
    QByteArray serialize() const;

    QString m_path;
    QHash<QString, Users> m_servers;
    qint64 m_nextSeq = 1;
    QTimer m_saveTimer;
    bool m_dirty = false;
    bool m_saveFailing = false;
};
} // namespace Kinefin
