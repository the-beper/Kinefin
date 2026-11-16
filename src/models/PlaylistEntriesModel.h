// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/ItemSummary.h"

#include <QAbstractListModel>
#include <QJsonArray>
#include <QPointer>

#include <deque>
#include <optional>
#include <vector>

namespace Kinefin {
// The entries of one playlist in its order, as the Playlist page lists them: the rows of the queue (same roles) plus the server's `entryId` (`PlaylistItemId`, empty when it is not a valid id). Read page by page, at most MaxEntries in MaxPages; the rows already shown stay until the whole new list is read, and stay as they are when it shows the same. One consumer at a time, on its own listing channel. Edits are optimistic: a row moves or goes at once and its write is sent after the ones before it, one at a time. When the last one succeeds the playlist is read again (the rows stay when the server agrees); a failed write drops the waiting ones, reads the server's order again and sets `editError`
class PlaylistEntriesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString playlistId READ playlistId NOTIFY statusChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int count READ count NOTIFY statusChanged)
    // What the server holds, and whether the cap cut the list
    Q_PROPERTY(int total READ total NOTIFY statusChanged)
    Q_PROPERTY(bool capped READ capped NOTIFY statusChanged)
    // Edits of this playlist on their way to the server
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)
    // Why the last edit failed, until the next edit or another playlist
    Q_PROPERTY(EditError editError READ editError NOTIFY editErrorChanged)

  public:
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    enum EditError { NoEditError, EditForbidden, EditFailed };
    Q_ENUM(EditError)
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        NameRole,
        SeriesNameRole,
        SeasonNumberRole,
        EpisodeNumberRole,
        YearRole,
        RuntimeMinutesRole,
        TypeRole,
        ImageUrlRole,
        PlayedRole,
        EntryIdRole
    };
    static constexpr int Channel = 5121;
    static constexpr int PageSize = 100;
    static constexpr int MaxEntries = 1000;
    // The pages one read asks for at most, the cap's and two more
    static constexpr int MaxPages = MaxEntries / PageSize + 2;
    static constexpr int ImageWidth = 240;
    // The edits that may wait behind the one on its way
    static constexpr int MaxWaitingWrites = 20;

    explicit PlaylistEntriesModel(JellyfinClient *client, QObject *parent = nullptr);

    QString playlistId() const { return m_playlistId; }
    Status status() const { return m_status; }
    int count() const { return int(m_rows.size()); }
    int total() const { return m_total; }
    bool capped() const { return m_capped; }
    bool writing() const;
    EditError editError() const { return m_editError; }

    // Reads the playlist from its first entry; another playlist's rows go at once (the edits of the previous one still go to the server). While edits of this playlist are on their way the read waits for the last of them
    Q_INVOKABLE void load(const QString &playlistId);
    // A read in flight is dropped and the channel released; the rows stay, and so do the edits on their way (without the read that follows them)
    Q_INVOKABLE void close();

    // Whether `row` can move: an entry with an id that no other row shares (the server moves the first copy of an entry), in rows that match the server's positions (no entry was dropped as invalid), while the rows can be edited
    Q_INVOKABLE bool canMove(int row) const;
    // Whether `row` can go: an entry with an id, while the rows can be edited
    Q_INVOKABLE bool canRemove(int row) const;
    // Moves row `from` to `to` (both in the rows) and sends the move; false when refused
    Q_INVOKABLE bool move(int from, int to);
    // Removes the entry of `row`, every copy of it as on the server, and sends the removal; false when refused
    Q_INVOKABLE bool remove(int row);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void statusChanged();
    void writingChanged();
    void editErrorChanged();
    // A row moved or went through move() or remove()
    void edited();

  private:
    struct Entry {
        ItemSummary item;
        QString entryId;
    };
    struct Write {
        JellyfinClient::PlaylistOp op;
        QString playlistId;
        QString entryId;
        int newIndex = -1;
    };
    static bool sameEntry(const Entry &a, const Entry &b);
    // Rows shown from a read that succeeded, with only this model's edits since, and room for one more
    bool canEdit() const;
    void enqueue(Write write);
    void sendNext();
    void onWriteSucceeded(JellyfinClient::PlaylistOp op, const QString &playlistId);
    void onWriteFailed(JellyfinClient::PlaylistOp op, const QString &playlistId, int httpStatus);
    bool isOwnWrite(JellyfinClient::PlaylistOp op, const QString &playlistId) const;
    void setEditError(EditError error);
    void updateWriting();
    void onEntries(const QJsonArray &items, int total, int channel);
    void requestPage();
    void publish();
    void reset();

    QPointer<JellyfinClient> m_client;
    QString m_playlistId;
    Status m_status = Idle;
    int m_total = 0;
    std::vector<Entry> m_rows;
    // The read in flight: what it collected and where the next page starts
    std::vector<Entry> m_pending;
    int m_received = 0;
    int m_pages = 0;
    // Where the page in flight started in `m_pending`: a page that adds nothing ends the read
    size_t m_pageStart = 0;
    bool m_capped = false;
    // An entry of the read was dropped as invalid, so a row's index is not its place on the server
    bool m_pendingGap = false;
    bool m_gap = false;
    // The rows came from a read that succeeded; a failed read or write makes them untrusted until the next one
    bool m_trusted = false;
    // The write on its way, then the ones waiting for it: those of a playlist shown before are still sent
    std::optional<Write> m_sent;
    std::deque<Write> m_waiting;
    // The playlist is read again once the last edit is through
    bool m_readAfterWrites = false;
    // A page reads the model: between load() and close()
    bool m_open = false;
    bool m_wasWriting = false;
    EditError m_editError = NoEditError;
};
} // namespace Kinefin
