// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ItemDetailsModel.h"

#include <QJsonArray>
#include <QTime>
#include <QVariantMap>

namespace Kinefin {
namespace {
constexpr int kBackdropWidth = 1920;
constexpr int kLogoWidth = 600;
constexpr int kPosterWidth = 400;
constexpr int kPersonWidth = 240;
constexpr qint64 kTicksPerSecond = 10'000'000;

QString clock(qint64 ticks)
{
    const qint64 seconds = ticks / kTicksPerSecond;
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600)
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QVariantMap streamMap(const StreamSummary &stream)
{
    return {{QStringLiteral("codec"), stream.codec},
            {QStringLiteral("language"), stream.language},
            {QStringLiteral("title"), stream.title},
            {QStringLiteral("width"), stream.width},
            {QStringLiteral("height"), stream.height},
            {QStringLiteral("channels"), stream.channels},
            {QStringLiteral("bitrate"), stream.bitrate},
            {QStringLiteral("isDefault"), stream.isDefault},
            {QStringLiteral("isForced"), stream.isForced},
            {QStringLiteral("isExternal"), stream.isExternal}};
}
} // namespace

ItemDetailsModel::ItemDetailsModel(JellyfinClient *client, const QString &itemId, int channel,
                                   QObject *parent)
    : QObject(parent), m_client(client), m_itemId(itemId), m_channel(channel),
      m_similar(client, false, this)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::itemsReady, this, &ItemDetailsModel::onItemsReady);
        connect(m_client, &JellyfinClient::similarItemsReady, this,
                &ItemDetailsModel::onSimilarReady);
        connect(m_client, &JellyfinClient::listingFailed, this, &ItemDetailsModel::onListingFailed);
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
    reload();
}

ItemDetailsModel::~ItemDetailsModel()
{
    // The page is gone: its requests have nobody to answer, and the client forgets the channel
    if (m_client)
        m_client->releaseListingChannel(m_channel);
}

bool ItemDetailsModel::playable() const
{
    return m_details.type == QLatin1String("Movie") || m_details.type == QLatin1String("Episode")
           || m_details.type == QLatin1String("Video");
}

bool ItemDetailsModel::hasSimilar() const
{
    // The server answers an empty envelope for these: no request
    return m_details.type != QLatin1String("Episode") && m_details.type != QLatin1String("Season");
}

void ItemDetailsModel::reload()
{
    ItemQuery query;
    query.itemIds = {m_itemId};
    query.fields = {QStringLiteral("Overview"),     QStringLiteral("Genres"),
                    QStringLiteral("People"),       QStringLiteral("Chapters"),
                    QStringLiteral("MediaSources"), QStringLiteral("Taglines"),
                    QStringLiteral("OriginalTitle")};
    setStatus(Loading);
    if (!m_client || !m_client->fetchItems(query, m_channel))
        setStatus(Failed);
}

void ItemDetailsModel::retrySimilar()
{
    if (m_status == Ready && m_similarStatus == Failed)
        requestSimilar();
}

void ItemDetailsModel::requestSimilar()
{
    if (!hasSimilar()) {
        m_similar.setItems({});
        setSimilarStatus(Idle);
        return;
    }
    // The old similar items stay until the answer replaces them
    setSimilarStatus(Loading);
    if (!m_client || !m_client->fetchSimilarItems(m_itemId, SimilarLimit, m_channel))
        setSimilarStatus(Failed);
}

void ItemDetailsModel::onItemsReady(const QJsonArray &items, int, int channel)
{
    if (channel != m_channel || m_status != Loading)
        return;
    if (items.isEmpty()) {
        // The item is gone (or never was visible to this user)
        m_details = {};
        m_similar.setItems({});
        Q_EMIT detailsChanged();
        setSimilarStatus(Idle);
        setStatus(NotFound);
        return;
    }
    const ItemDetails parsed = parseItemDetails(items.first().toObject());
    // Ids compare without case and without dashes: the same GUID is written both ways
    const auto bare = [](QString id) { return id.remove(QLatin1Char('-')).toLower(); };
    if (!parsed.valid || bare(parsed.id) != bare(m_itemId)) {
        setStatus(Failed);
        return;
    }
    m_details = parsed;
    Q_EMIT detailsChanged();
    setStatus(Ready);
    requestSimilar();
}

void ItemDetailsModel::onSimilarReady(const QJsonArray &items, int, int channel)
{
    if (channel != m_channel || m_similarStatus != Loading)
        return;
    m_similar.setItems(items);
    setSimilarStatus(Ready);
}

void ItemDetailsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (channel != m_channel)
        return;
    if (family == QLatin1String("Items") && m_status == Loading)
        setStatus(Failed);
    else if (family == QLatin1String("Similar") && m_similarStatus == Loading)
        setSimilarStatus(Failed);
}

void ItemDetailsModel::clear()
{
    m_details = {};
    m_similar.setItems({});
    Q_EMIT detailsChanged();
    setSimilarStatus(Idle);
    if (m_status != NotFound)
        setStatus(Idle);
}

void ItemDetailsModel::setStatus(Status status)
{
    if (status == m_status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void ItemDetailsModel::setSimilarStatus(Status status)
{
    if (status == m_similarStatus)
        return;
    m_similarStatus = status;
    Q_EMIT similarStatusChanged();
}

QUrl ItemDetailsModel::backdropUrl() const
{
    return m_client && !m_details.backdropTag.isEmpty()
               ? m_client->imageUrl(m_details.backdropItemId, QStringLiteral("Backdrop"),
                                    kBackdropWidth, m_details.backdropTag)
               : QUrl();
}

QUrl ItemDetailsModel::logoUrl() const
{
    return m_client && !m_details.logoTag.isEmpty()
               ? m_client->imageUrl(m_details.logoItemId, QStringLiteral("Logo"), kLogoWidth,
                                    m_details.logoTag)
               : QUrl();
}

QUrl ItemDetailsModel::posterUrl() const
{
    return m_client && !m_details.primaryTag.isEmpty() && !m_details.id.isEmpty()
               ? m_client->imageUrl(m_details.id, QStringLiteral("Primary"), kPosterWidth,
                                    m_details.primaryTag)
               : QUrl();
}

QVariantList ItemDetailsModel::people() const
{
    QVariantList result;
    for (const PersonSummary &person : m_details.people) {
        QUrl image;
        if (m_client && !person.id.isEmpty() && !person.primaryTag.isEmpty())
            image = m_client->imageUrl(person.id, QStringLiteral("Primary"), kPersonWidth,
                                       person.primaryTag);
        result.append(QVariantMap{{QStringLiteral("name"), person.name},
                                  {QStringLiteral("role"), person.role},
                                  {QStringLiteral("type"), person.type},
                                  {QStringLiteral("imageUrl"), image}});
    }
    return result;
}

QVariantList ItemDetailsModel::chapters() const
{
    QVariantList result;
    for (const ChapterSummary &chapter : m_details.chapters) {
        result.append(QVariantMap{{QStringLiteral("name"), chapter.name},
                                  {QStringLiteral("startTicks"), chapter.startTicks},
                                  {QStringLiteral("time"), clock(chapter.startTicks)}});
    }
    return result;
}

QVariantList ItemDetailsModel::mediaSources() const
{
    QVariantList result;
    for (const SourceSummary &source : m_details.sources) {
        QVariantList video;
        QVariantList audio;
        QVariantList subtitles;
        for (const StreamSummary &stream : source.streams) {
            if (stream.type == QLatin1String("Video"))
                video.append(streamMap(stream));
            else if (stream.type == QLatin1String("Audio"))
                audio.append(streamMap(stream));
            else if (stream.type == QLatin1String("Subtitle"))
                subtitles.append(streamMap(stream));
        }
        result.append(QVariantMap{{QStringLiteral("id"), source.id},
                                  {QStringLiteral("name"), source.name},
                                  {QStringLiteral("container"), source.container},
                                  {QStringLiteral("size"), source.size},
                                  {QStringLiteral("bitrate"), source.bitrate},
                                  {QStringLiteral("video"), video},
                                  {QStringLiteral("audio"), audio},
                                  {QStringLiteral("subtitles"), subtitles}});
    }
    return result;
}
} // namespace Kinefin
