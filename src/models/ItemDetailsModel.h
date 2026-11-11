// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/ItemChildrenModel.h"
#include "models/ItemDetails.h"
#include "models/ItemRowModel.h"

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

namespace Kinefin {
// The data behind one details page: the item itself (one `GET /Items?ids=`) and, for an item that has them, its similar items. One model per open page, so a chain of pages (an item, a similar item, back) keeps each page's own state; it takes a listing channel of its own. A reload keeps the old data on screen until the answer replaces it. The model empties itself when the client loses its credentials. Everything shown is server data: render it as plain text
class ItemDetailsModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString itemId READ itemId CONSTANT)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(Status similarStatus READ similarStatus NOTIFY similarStatusChanged)
    Q_PROPERTY(QString name READ name NOTIFY detailsChanged)
    Q_PROPERTY(QString type READ type NOTIFY detailsChanged)
    Q_PROPERTY(QString overview READ overview NOTIFY detailsChanged)
    Q_PROPERTY(QString tagline READ tagline NOTIFY detailsChanged)
    Q_PROPERTY(int year READ year NOTIFY detailsChanged)
    Q_PROPERTY(int runtimeMinutes READ runtimeMinutes NOTIFY detailsChanged)
    Q_PROPERTY(qreal communityRating READ communityRating NOTIFY detailsChanged)
    Q_PROPERTY(QString officialRating READ officialRating NOTIFY detailsChanged)
    Q_PROPERTY(QStringList genres READ genres NOTIFY detailsChanged)
    Q_PROPERTY(QStringList directors READ directors NOTIFY detailsChanged)
    Q_PROPERTY(QStringList writers READ writers NOTIFY detailsChanged)
    Q_PROPERTY(bool played READ played NOTIFY detailsChanged)
    Q_PROPERTY(bool favorite READ favorite NOTIFY detailsChanged)
    Q_PROPERTY(bool favoriteBusy READ favoriteBusy NOTIFY favoriteWriteStateChanged)
    Q_PROPERTY(bool favoriteError READ favoriteError NOTIFY favoriteWriteStateChanged)
    Q_PROPERTY(bool playedBusy READ playedBusy NOTIFY playedWriteStateChanged)
    Q_PROPERTY(bool playedError READ playedError NOTIFY playedWriteStateChanged)
    Q_PROPERTY(qreal playedPercentage READ playedPercentage NOTIFY detailsChanged)
    Q_PROPERTY(int unplayedCount READ unplayedCount NOTIFY detailsChanged)
    Q_PROPERTY(bool canResume READ canResume NOTIFY detailsChanged)
    Q_PROPERTY(qint64 resumePositionTicks READ resumePositionTicks NOTIFY detailsChanged)
    Q_PROPERTY(bool playable READ playable NOTIFY detailsChanged)
    Q_PROPERTY(QString seriesName READ seriesName NOTIFY detailsChanged)
    Q_PROPERTY(QString seriesId READ seriesId NOTIFY detailsChanged)
    Q_PROPERTY(QString seasonId READ seasonId NOTIFY detailsChanged)
    Q_PROPERTY(int seasonNumber READ seasonNumber NOTIFY detailsChanged)
    Q_PROPERTY(int episodeNumberEnd READ episodeNumberEnd NOTIFY detailsChanged)
    Q_PROPERTY(int episodeNumber READ episodeNumber NOTIFY detailsChanged)
    Q_PROPERTY(QUrl backdropUrl READ backdropUrl NOTIFY detailsChanged)
    Q_PROPERTY(QUrl logoUrl READ logoUrl NOTIFY detailsChanged)
    Q_PROPERTY(QUrl posterUrl READ posterUrl NOTIFY detailsChanged)
    // Entries: name, role, type, imageUrl (empty without an image)
    Q_PROPERTY(QVariantList people READ people NOTIFY detailsChanged)
    // Entries: name, startTicks, time (h:mm:ss)
    Q_PROPERTY(QVariantList chapters READ chapters NOTIFY detailsChanged)
    // Entries: id, name, container, size, bitrate, video, audio, subtitles (lists of stream maps: codec, language, title, width, height, channels, bitrate, isDefault, isForced, isExternal)
    Q_PROPERTY(QVariantList mediaSources READ mediaSources NOTIFY detailsChanged)
    Q_PROPERTY(Kinefin::ItemRowModel *similar READ similar CONSTANT)
    // What hangs below the item: the seasons and next episode of a series, the episodes of a season, the items of a collection
    Q_PROPERTY(Kinefin::ItemChildrenModel *below READ below CONSTANT)

  public:
    enum Status { Idle, Loading, Ready, NotFound, Failed };
    Q_ENUM(Status)

    // The listing channels of details models count up from here
    static constexpr int FirstChannel = 65536;
    static constexpr int SimilarLimit = 12;
    // A model uses its own channel for the item and the next one for what is below it
    static constexpr int ChannelsPerModel = 2;
    static constexpr int BelowChannelOffset = 1;

    // `channel` is the model's own listing channel (the controller hands them out); the model starts loading at once
    ItemDetailsModel(JellyfinClient *client, const QString &itemId, int channel,
                     QObject *parent = nullptr);

    ~ItemDetailsModel() override;

    Q_INVOKABLE void reload();
    Q_INVOKABLE void retrySimilar();
    Q_INVOKABLE bool toggleFavorite();
    Q_INVOKABLE bool togglePlayed();

    QString itemId() const { return m_itemId; }
    int channel() const { return m_channel; }
    Status status() const { return m_status; }
    Status similarStatus() const { return m_similarStatus; }
    QString name() const { return m_details.name; }
    QString type() const { return m_details.type; }
    QString overview() const { return m_details.overview; }
    QString tagline() const { return m_details.tagline; }
    int year() const { return m_details.year; }
    int runtimeMinutes() const { return m_details.runtimeMinutes; }
    qreal communityRating() const { return m_details.communityRating; }
    QString officialRating() const { return m_details.officialRating; }
    QStringList genres() const { return m_details.genres; }
    QStringList directors() const { return m_details.directors; }
    QStringList writers() const { return m_details.writers; }
    bool played() const { return m_details.played; }
    bool favorite() const { return m_details.favorite; }
    bool favoriteBusy() const { return m_favoriteBusy; }
    bool favoriteError() const { return m_favoriteError; }
    bool playedBusy() const { return m_playedBusy; }
    bool playedError() const { return m_playedError; }
    qreal playedPercentage() const { return m_details.playedPercentage; }
    int unplayedCount() const { return m_details.unplayedCount; }
    bool canResume() const { return m_details.resumePositionTicks > 0 && playable(); }
    qint64 resumePositionTicks() const { return m_details.resumePositionTicks; }
    bool playable() const;
    QString seriesName() const { return m_details.seriesName; }
    QString seriesId() const { return m_details.seriesId; }
    QString seasonId() const { return m_details.seasonId; }
    int seasonNumber() const { return m_details.seasonNumber; }
    int episodeNumber() const { return m_details.episodeNumber; }
    int episodeNumberEnd() const { return m_details.episodeNumberEnd; }
    QUrl backdropUrl() const;
    QUrl logoUrl() const;
    QUrl posterUrl() const;
    QVariantList people() const;
    QVariantList chapters() const;
    QVariantList mediaSources() const;
    ItemRowModel *similar() { return &m_similar; }
    ItemChildrenModel *below() { return &m_below; }

  Q_SIGNALS:
    void statusChanged();
    void similarStatusChanged();
    void detailsChanged();
    void favoriteWriteStateChanged();
    void playedWriteStateChanged();

  private:
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onSimilarReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void requestSimilar();
    void clear();
    void setStatus(Status status);
    void setSimilarStatus(Status status);
    bool hasSimilar() const;

    QPointer<JellyfinClient> m_client;
    QString m_itemId;
    int m_channel;
    Status m_status = Idle;
    Status m_similarStatus = Idle;
    ItemDetails m_details;
    bool m_favoriteBusy = false;
    bool m_favoriteError = false;
    bool m_playedBusy = false;
    bool m_playedError = false;
    ItemRowModel m_similar;
    ItemChildrenModel m_below;
};
} // namespace Kinefin
