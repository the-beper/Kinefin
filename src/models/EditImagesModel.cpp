// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "EditImagesModel.h"

#include <QHash>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace Kinefin {
namespace {
const QString itemImagesFamily = QStringLiteral("ItemImages");

QString canonicalId(QString id)
{
    return id.remove(QLatin1Char('-')).toLower();
}
} // namespace

const QStringList &EditImagesModel::usualTypes()
{
    static const QStringList types{QStringLiteral("Primary"), QStringLiteral("Backdrop"),
                                   QStringLiteral("Logo"),    QStringLiteral("Banner"),
                                   QStringLiteral("Thumb"),   QStringLiteral("Art"),
                                   QStringLiteral("Disc")};
    return types;
}

const QStringList &EditImagesModel::otherTypes()
{
    static const QStringList types{QStringLiteral("Box"), QStringLiteral("BoxRear"),
                                   QStringLiteral("Menu"), QStringLiteral("Screenshot")};
    return types;
}

bool EditImagesModel::holdsSeveral(const QString &type)
{
    return type == QLatin1String("Backdrop") || type == QLatin1String("Screenshot");
}

EditImagesModel::EditImagesModel(JellyfinClient *client, const QString &itemId, int channel,
                                 const QStringList &types, QObject *parent)
    : QAbstractListModel(parent), m_client(client), m_itemId(itemId), m_channel(channel),
      m_types(types)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::itemImagesReady, this, &EditImagesModel::onImagesReady);
        connect(m_client, &JellyfinClient::listingFailed, this, &EditImagesModel::onListingFailed);
        // Every write of this item's images, from this page or another, changes the list
        connect(m_client, &JellyfinClient::itemImageUpdated, this,
                [this](const QString &itemId, const QString &, int) {
                    if (sameItem(itemId))
                        reload();
                });
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
    reload();
}

EditImagesModel::~EditImagesModel()
{
    // The page is gone: its request has nobody to answer, and the client forgets the channel
    if (m_client)
        m_client->releaseListingChannel(m_channel);
}

void EditImagesModel::reload()
{
    setStatus(Loading);
    if (!m_client || !m_client->fetchItemImages(m_itemId, m_channel))
        setStatus(Failed);
}

int EditImagesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_groups.size());
}

QVariant EditImagesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= int(m_groups.size()))
        return {};
    const Group &group = m_groups.at(size_t(index.row()));
    switch (role) {
    case ImageTypeRole:
        return group.type;
    case MultipleRole:
        return holdsSeveral(group.type);
    case ImagesRole:
        return group.images;
    default:
        return {};
    }
}

QHash<int, QByteArray> EditImagesModel::roleNames() const
{
    return {{ImageTypeRole, "imageType"}, {MultipleRole, "multiple"}, {ImagesRole, "images"}};
}

bool EditImagesModel::replaces(const QString &type) const
{
    if (holdsSeveral(type))
        return false;
    return std::ranges::any_of(m_groups, [&type](const Group &group) {
        return group.type == type && !group.images.isEmpty();
    });
}

void EditImagesModel::setStatus(Status status)
{
    if (m_status == status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

bool EditImagesModel::sameItem(const QString &itemId) const
{
    return canonicalId(itemId) == canonicalId(m_itemId);
}

// One image as the page sees it. The thumbnail comes from the client's image URL builder alone, and only for an image with a cache tag: a URL without one could show a stale copy of an image that was replaced
QVariantMap EditImagesModel::imageEntry(const QVariantMap &image, int before, int after) const
{
    const QString type = image.value(QStringLiteral("imageType")).toString();
    const int index = image.value(QStringLiteral("imageIndex")).toInt();
    const QString tag = image.value(QStringLiteral("imageTag")).toString();
    QUrl thumbnail;
    if (m_client && !tag.isEmpty())
        thumbnail = m_client->imageUrl(m_itemId, type, ThumbnailWidth, tag, 90, QString(), index);
    QVariantMap entry = image;
    entry.insert(QStringLiteral("thumbnailUrl"), thumbnail);
    entry.insert(QStringLiteral("previousIndex"), before);
    entry.insert(QStringLiteral("nextIndex"), after);
    return entry;
}

void EditImagesModel::onImagesReady(const QString &itemId, const QVariantList &images, int channel)
{
    if (channel != m_channel || !sameItem(itemId))
        return;
    QHash<QString, QList<QVariantMap>> byType;
    for (const QVariant &value : images) {
        const QVariantMap image = value.toMap();
        byType[image.value(QStringLiteral("imageType")).toString()].append(image);
    }
    QStringList types = m_types;
    if (types.isEmpty()) {
        types = usualTypes();
        for (const QString &type : otherTypes()) {
            if (byType.contains(type))
                types.append(type);
        }
    }
    std::vector<Group> groups;
    int count = 0;
    for (const QString &type : std::as_const(types)) {
        QList<QVariantMap> list = byType.value(type);
        std::stable_sort(list.begin(), list.end(), [](const QVariantMap &a, const QVariantMap &b) {
            return a.value(QStringLiteral("imageIndex")).toInt()
                   < b.value(QStringLiteral("imageIndex")).toInt();
        });
        Group group{type, {}};
        for (qsizetype i = 0; i < list.size(); ++i) {
            const int before =
                i > 0 ? list.at(i - 1).value(QStringLiteral("imageIndex")).toInt() : -1;
            const int after = i + 1 < list.size()
                                  ? list.at(i + 1).value(QStringLiteral("imageIndex")).toInt()
                                  : -1;
            group.images.append(imageEntry(list.at(i), before, after));
        }
        count += int(group.images.size());
        groups.push_back(std::move(group));
    }

    const bool sameRows =
        groups.size() == m_groups.size()
        && std::equal(groups.begin(), groups.end(), m_groups.begin(),
                      [](const Group &a, const Group &b) { return a.type == b.type; });
    if (sameRows) {
        // In place, so the view keeps its rows and the focus inside them
        for (size_t row = 0; row < groups.size(); ++row) {
            if (groups.at(row).images == m_groups.at(row).images)
                continue;
            m_groups[row].images = std::move(groups[row].images);
            Q_EMIT dataChanged(index(int(row)), index(int(row)), {ImagesRole});
        }
    } else {
        beginResetModel();
        m_groups = std::move(groups);
        endResetModel();
    }
    if (count != m_imageCount) {
        m_imageCount = count;
        Q_EMIT imageCountChanged();
    }
    setStatus(Ready);
}

void EditImagesModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    // The old rows stay on screen: the page says the list could not be read
    if (channel == m_channel && family == itemImagesFamily)
        setStatus(Failed);
}

void EditImagesModel::clear()
{
    if (!m_groups.empty()) {
        beginResetModel();
        m_groups.clear();
        endResetModel();
    }
    if (m_imageCount != 0) {
        m_imageCount = 0;
        Q_EMIT imageCountChanged();
    }
    setStatus(Idle);
}
} // namespace Kinefin
