// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <optional>

class QJsonObject;

namespace Kinefin {
// What the movie and TV tabs remember per server and user: which library of a type is selected, and the sort, filters and view options of each scope (a tab or sub-tab over all libraries, or one library). Ids, enum key names and SHA-256 digests only, never server text (a tag or rating has no id: it is kept as the digest of its exact text, see `hashText`). Kept in one file with a debounced write; a pending write is flushed by `flush()` (the owner calls it when the session ends) and when the store is destroyed. Whatever cannot be trusted is dropped on load: a link, a non-regular or oversized file goes, a file of a later version is kept aside as `.broken`. At most `MaxViews` views are kept and the file stays within `MaxFileBytes`, the one least recently written going first; `MaxFileBytes` is the limit that binds when the views carry many selections (a view with every selection at its cap is about 11 KB, so roughly two dozen fit)
class BrowseStateStore final : public QObject {
    Q_OBJECT

  public:
    // The sort key is the model's `sortBy` wire token (`SortName`, `DateCreated`) and the other enum members are their key names (`IsUnplayed`, `HasSubtitles`); the store does not know the enums, it checks the shape of the sort key and every other token against a whitelist
    struct ViewState {
        QString sortKey = QStringLiteral("SortName");
        bool descending = false;
        // `IsPlayed` or `IsUnplayed`, with `IsResumable` and `IsFavorite` as well
        QStringList statusFlags;
        // `HasSubtitles`, `HasTrailer`, `HasSpecialFeature`, `HasThemeSong`, `HasThemeVideo`
        QStringList features;
        // Empty, `HD` or `SD`
        QString quality;
        bool is4K = false;
        bool is3D = false;
        // `BluRay`, `Dvd`
        QStringList videoTypes;
        QStringList genreIds;
        // `hashText` of the exact tag and rating texts: the texts themselves are never stored
        QStringList tagHashes;
        QStringList ratingHashes;
        QList<int> years;
        // `Continuing`, `Ended`, `Unreleased`
        QStringList seriesStatus;
        // `All`, `Specials`, `Missing`, `Unaired`
        QString episodeKind = QStringLiteral("All");
        // `grid` or `list`
        QString viewMode = QStringLiteral("grid");
        // `Primary`, `Banner`, `Disc`, `Logo`, `Thumb`
        QString imageType = QStringLiteral("Primary");
        bool showTitle = true;
        bool showYear = true;
        bool cardLayout = true;
        bool operator==(const ViewState &) const = default;
    };

    static constexpr int MaxViews = 200;
    static constexpr int MaxGenreIds = 64;
    static constexpr int MaxHashes = 64;
    static constexpr int MaxYears = 64;
    static constexpr int MinYear = 1800;
    static constexpr int MaxYear = 2200;
    // A stored sequence number above this is a damaged file: counting on from it would overflow
    static constexpr qint64 MaxSeq = qint64(1) << 52;
    static constexpr qint64 MaxFileBytes = 256 * 1024;
    static constexpr int FileVersion = 2;

    // The digest the store keeps a tag or rating by: SHA-256 of the UTF-8 text as is, lower case hex
    static QString hashText(const QString &text);

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

    // The scope key is `movies`, `tvshows`, `movies:<id>`, `tvshows:<id>`, `boxsets:<id>`, `movies/favorites`, `movies/favorites:<id>`, `movies/collections`, `tvshows/episodes`, `tvshows/episodes:<id>` or `tvshows/network:<studioId>`, ids lower case without dashes by the caller's convention. `IsResumable` is dropped from the flags in a scope that lists series (`tvshows`, `tvshows:<id>`, `tvshows/network:<studioId>`): there is no such filter over series
    std::optional<ViewState> view(const QString &serverId, const QString &userId,
                                  const QString &scopeKey) const;
    // Writing the state a scope already has changes nothing; an invalid argument is ignored
    void setView(const QString &serverId, const QString &userId, const QString &scopeKey,
                 const ViewState &state);

    // How the Favorites page groups its items: `Type` (the default), `Library`, `Genre`, `Year` or `None`; an unknown value is ignored
    QString favoritesGrouping(const QString &serverId, const QString &userId) const;
    void setFavoritesGrouping(const QString &serverId, const QString &userId,
                              const QString &grouping);

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
        // Empty until set
        QString favoritesGrouping;
    };
    using Users = QHash<QString, UserState>;

    void load();
    void loadUsers(const QJsonObject &servers, int version, qint64 &highest);
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
