// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RemoteImagesModel.h"

#include "EditImagesModel.h"

#include <QHash>

#include <algorithm>
#include <utility>

namespace Kinefin {
namespace {
const QString providersFamily = QStringLiteral("RemoteImageProviders");
const QString imagesFamily = QStringLiteral("RemoteImages");

QString canonicalId(QString id)
{
    return id.remove(QLatin1Char('-')).toLower();
}

bool knownType(const QString &type)
{
    return EditImagesModel::usualTypes().contains(type)
           || EditImagesModel::otherTypes().contains(type);
}
} // namespace

RemoteImagesModel::RemoteImagesModel(JellyfinClient *client, const QString &itemId,
                                     const QString &imageType, int channel, QObject *parent)
    : QAbstractListModel(parent), m_client(client), m_itemId(itemId), m_channel(channel),
      m_type(knownType(imageType) ? imageType : QStringLiteral("Primary"))
{
    if (m_client) {
        connect(m_client, &JellyfinClient::remoteImageProvidersReady, this,
                &RemoteImagesModel::onProvidersReady);
        connect(m_client, &JellyfinClient::remoteImagesReady, this,
                &RemoteImagesModel::onImagesReady);
        connect(m_client, &JellyfinClient::listingFailed, this,
                &RemoteImagesModel::onListingFailed);
        connect(m_client, &JellyfinClient::itemImageUpdated, this,
                &RemoteImagesModel::onImageUpdated);
        connect(m_client, &JellyfinClient::itemImageUpdateFailed, this,
                &RemoteImagesModel::onImageUpdateFailed);
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
    requestProviders();
    requestPage(0);
}

RemoteImagesModel::~RemoteImagesModel()
{
    // The page is gone: its requests have nobody to answer, and the client forgets the channel
    if (m_client)
        m_client->releaseListingChannel(m_channel);
}

void RemoteImagesModel::reload()
{
    if (m_providersStatus != Ready)
        requestProviders();
    requestPage(m_startIndex);
}

void RemoteImagesModel::nextPage()
{
    if (hasNext() && m_status != Loading)
        requestPage(m_startIndex + PageSize);
}

void RemoteImagesModel::previousPage()
{
    if (hasPrevious() && m_status != Loading)
        requestPage(qMax(0, m_startIndex - PageSize));
}

bool RemoteImagesModel::download(int row)
{
    if (!m_client || m_status != Ready || m_downloadState == Downloading || row < 0
        || row >= int(m_rows.size()))
        return false;
    m_downloadType = m_type;
    if (!m_client->downloadRemoteImage(m_itemId, m_type, m_rows.at(size_t(row)).id)) {
        setDownloadState(DownloadFailed);
        return false;
    }
    setDownloadState(Downloading);
    return true;
}

void RemoteImagesModel::dismissDownloadError()
{
    if (m_downloadState == DownloadFailed)
        setDownloadState(NoDownload);
}

void RemoteImagesModel::setImageType(const QString &type)
{
    if (type == m_type || !knownType(type))
        return;
    m_type = type;
    Q_EMIT imageTypeChanged();
    // A provider that does not offer the new type no longer applies
    if (!m_provider.isEmpty() && providersKnown() && !providerNames().contains(m_provider)) {
        m_provider.clear();
        Q_EMIT providerChanged();
    }
    Q_EMIT providersChanged();
    m_total = 0;
    requestPage(0);
}

void RemoteImagesModel::setProvider(const QString &provider)
{
    if (provider == m_provider || provider.size() > MaxRemoteImageNameChars)
        return;
    if (!provider.isEmpty() && providersKnown() && !providerNames().contains(provider))
        return;
    m_provider = provider;
    Q_EMIT providerChanged();
    m_total = 0;
    requestPage(0);
}

void RemoteImagesModel::setAllLanguages(bool all)
{
    if (all == m_allLanguages)
        return;
    m_allLanguages = all;
    Q_EMIT allLanguagesChanged();
    m_total = 0;
    requestPage(0);
}

bool RemoteImagesModel::offeredByAProvider(const QString &type) const
{
    return std::any_of(m_providers.begin(), m_providers.end(),
                       [&type](const Provider &provider) { return provider.types.contains(type); });
}

QStringList RemoteImagesModel::imageTypes() const
{
    QStringList types;
    for (const QStringList &list : {EditImagesModel::usualTypes(), EditImagesModel::otherTypes()}) {
        for (const QString &type : list) {
            const bool offered = providersKnown() ? offeredByAProvider(type)
                                                  : EditImagesModel::usualTypes().contains(type);
            if (offered || type == m_type)
                types.append(type);
        }
    }
    return types;
}

QStringList RemoteImagesModel::providerNames() const
{
    QStringList names;
    if (!providersKnown())
        return names;
    for (const Provider &provider : m_providers) {
        if (provider.types.contains(m_type) && !names.contains(provider.name))
            names.append(provider.name);
    }
    return names;
}

int RemoteImagesModel::firstShown() const
{
    return m_total > 0 ? m_startIndex + 1 : 0;
}

int RemoteImagesModel::lastShown() const
{
    return m_total > 0 ? qMin(m_startIndex + PageSize, m_total) : 0;
}

int RemoteImagesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant RemoteImagesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= int(m_rows.size()))
        return {};
    const Offer &offer = m_rows.at(size_t(index.row()));
    switch (role) {
    case ProviderRole:
        return offer.provider;
    case ThumbnailUrlRole:
        return offer.thumbnailUrl;
    case WidthRole:
        return offer.width;
    case HeightRole:
        return offer.height;
    case LanguageRole:
        return offer.language;
    case RatingRole:
        return offer.rating;
    case VoteCountRole:
        return offer.voteCount;
    case RatingTypeRole:
        return offer.ratingType;
    default:
        return {};
    }
}

QHash<int, QByteArray> RemoteImagesModel::roleNames() const
{
    // The size roles are not called width and height: a delegate has those properties already
    return {{ProviderRole, "provider"},   {ThumbnailUrlRole, "thumbnailUrl"},
            {WidthRole, "imageWidth"},    {HeightRole, "imageHeight"},
            {LanguageRole, "language"},   {RatingRole, "rating"},
            {VoteCountRole, "voteCount"}, {RatingTypeRole, "ratingType"}};
}

void RemoteImagesModel::setStatus(Status status)
{
    if (m_status == status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void RemoteImagesModel::setDownloadState(DownloadState state, int status)
{
    if (m_downloadState == state && m_downloadStatus == status)
        return;
    m_downloadState = state;
    m_downloadStatus = status;
    Q_EMIT downloadStateChanged();
}

bool RemoteImagesModel::sameItem(const QString &itemId) const
{
    return canonicalId(itemId) == canonicalId(m_itemId);
}

void RemoteImagesModel::requestProviders()
{
    m_providersStatus = Loading;
    Q_EMIT providersChanged();
    if (!m_client || !m_client->fetchRemoteImageProviders(m_itemId, m_channel)) {
        m_providersStatus = Failed;
        Q_EMIT providersChanged();
    }
}

void RemoteImagesModel::requestPage(int startIndex)
{
    m_startIndex = startIndex;
    clearRows();
    setStatus(Loading);
    Q_EMIT pageChanged();
    if (!m_client
        || !m_client->fetchRemoteImages(m_itemId, m_type, startIndex, PageSize, m_provider,
                                        m_allLanguages, m_channel))
        setStatus(Failed);
}

void RemoteImagesModel::clearRows()
{
    if (m_rows.empty())
        return;
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

void RemoteImagesModel::onProvidersReady(const QString &itemId, const QVariantList &providers,
                                         int channel)
{
    if (channel != m_channel || !sameItem(itemId))
        return;
    m_providers.clear();
    for (const QVariant &value : providers) {
        const QVariantMap map = value.toMap();
        const QString name = map.value(QStringLiteral("name")).toString();
        const QStringList types = map.value(QStringLiteral("supportedImages")).toStringList();
        if (!name.isEmpty())
            m_providers.push_back({name, types});
    }
    m_providersStatus = Ready;
    Q_EMIT providersChanged();
}

void RemoteImagesModel::onImagesReady(const QString &itemId, const QString &type, int startIndex,
                                      const QVariantList &images, int total, const QStringList &,
                                      int channel)
{
    if (channel != m_channel || !sameItem(itemId) || type != m_type || startIndex != m_startIndex
        || m_status != Loading)
        return;
    std::vector<Offer> rows;
    rows.reserve(size_t(images.size()));
    for (const QVariant &value : images) {
        const QVariantMap map = value.toMap();
        Offer offer;
        offer.id = map.value(QStringLiteral("offerId")).toUInt();
        offer.provider = map.value(QStringLiteral("provider")).toString();
        // The client has checked the preview already; a page that is handed a URL checks it again
        const QUrl preview(map.value(QStringLiteral("thumbnailUrl")).toString());
        offer.thumbnailUrl = JellyfinClient::isProviderImageUrl(preview) ? preview : QUrl();
        offer.width = map.value(QStringLiteral("width")).toInt();
        offer.height = map.value(QStringLiteral("height")).toInt();
        offer.language = map.value(QStringLiteral("language")).toString();
        offer.rating = map.value(QStringLiteral("rating")).toDouble();
        offer.voteCount = map.value(QStringLiteral("voteCount")).toInt();
        offer.ratingType = map.value(QStringLiteral("ratingType")).toString();
        if (offer.id != 0)
            rows.push_back(std::move(offer));
    }
    m_total = qMax(total, startIndex + int(rows.size()));
    if (!rows.empty()) {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    }
    Q_EMIT pageChanged();
    setStatus(Ready);
}

void RemoteImagesModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (channel != m_channel)
        return;
    if (family == imagesFamily && m_status == Loading) {
        clearRows();
        setStatus(Failed);
    } else if (family == providersFamily && m_providersStatus == Loading) {
        // The selectors offer the usual types and every provider at once
        m_providersStatus = Failed;
        Q_EMIT providersChanged();
    }
}

void RemoteImagesModel::onImageUpdated(const QString &itemId, const QString &type, int index)
{
    if (m_downloadState != Downloading || !sameItem(itemId) || type != m_downloadType
        || index != -1)
        return;
    setDownloadState(NoDownload);
    Q_EMIT downloaded();
}

void RemoteImagesModel::onImageUpdateFailed(const QString &itemId, const QString &type, int index,
                                            int status)
{
    if (m_downloadState != Downloading || !sameItem(itemId) || type != m_downloadType
        || index != -1)
        return;
    setDownloadState(DownloadFailed, status);
}

void RemoteImagesModel::clear()
{
    clearRows();
    m_providers.clear();
    m_providersStatus = Idle;
    m_total = 0;
    m_startIndex = 0;
    setDownloadState(NoDownload);
    setStatus(Idle);
    Q_EMIT providersChanged();
    Q_EMIT pageChanged();
}
} // namespace Kinefin
