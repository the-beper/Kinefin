// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"

#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <vector>

namespace Kinefin {
// The images an item has now, for the Edit Images page: one row per image type the editor handles (the seven usual ones always, so a type with no image can be filled; Box, BoxRear, Menu and Screenshot only when the item has some), each with its images in index order. One model per open page, on a listing channel of its own. A reload keeps the old rows until the answer replaces them, and a reload that leaves the same types updates the rows in place, so a view keeps its items and focus. The model reads again after every image write of its item, whoever made it. Thumbnails are built by the API client's image URL builder only, and only for an image that has a cache tag. Everything shown is server data: render it as plain text
class EditImagesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString itemId READ itemId CONSTANT)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int imageCount READ imageCount NOTIFY imageCountChanged)

  public:
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    // `imageType`: the ImageType name; `multiple`: the type holds several images (a backdrop); `images`: its images in index order, each {imageType, imageIndex (-1 for a type with one image), imageTag, width, height, size, thumbnailUrl (empty without a tag), previousIndex and nextIndex (the imageIndex of the neighbour before and after it, -1 for none)}
    enum Role { ImageTypeRole = Qt::UserRole + 1, MultipleRole, ImagesRole };

    // The listing channels of the Edit Images models count up from here
    static constexpr int FirstChannel = 7168;
    static constexpr int ThumbnailWidth = 512;

    // The image types the editor shows for every item, in the order of its rows, and the others it shows only for an item that has images of them
    static const QStringList &usualTypes();
    static const QStringList &otherTypes();
    // Whether an image of the type is told apart by an index (a backdrop, a screenshot)
    static bool holdsSeveral(const QString &type);

    // `channel` is the model's own listing channel (the controller hands them out); the model starts loading at once
    EditImagesModel(JellyfinClient *client, const QString &itemId, int channel,
                    QObject *parent = nullptr);
    ~EditImagesModel() override;

    Q_INVOKABLE void reload();
    // Whether an image chosen for `type` would take the place of one the item has: a type with one image holds it, a type with several adds one
    Q_INVOKABLE bool replaces(const QString &type) const;

    QString itemId() const { return m_itemId; }
    int channel() const { return m_channel; }
    Status status() const { return m_status; }
    int imageCount() const { return m_imageCount; }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void statusChanged();
    void imageCountChanged();

  private:
    struct Group {
        QString type;
        QVariantList images;
    };

    void setStatus(Status status);
    void onImagesReady(const QString &itemId, const QVariantList &images, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void clear();
    bool sameItem(const QString &itemId) const;
    QVariantMap imageEntry(const QVariantMap &image, int before, int after) const;

    JellyfinClient *m_client = nullptr;
    QString m_itemId;
    int m_channel = 0;
    Status m_status = Idle;
    std::vector<Group> m_groups;
    int m_imageCount = 0;
};
} // namespace Kinefin
