// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"

#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

#include <vector>

namespace Kinefin {
// The images the metadata providers offer for one item, one page at a time, and the download of the one the user picks, for the online image page. One model per open page, on a listing channel of its own (the providers and the images are two families on it). The list holds only what the client validated: a thumbnail is a URL the client accepted for a provider's domain (checked again here), and the full-size URL of an offer never leaves the client, which keeps it under an offer id of its own that the model holds and QML never sees. A change of the type, the provider or the language starts again from the first page and empties the rows at once, so a row on screen is always an offer of the type shown. Everything shown is server data: render it as plain text
class RemoteImagesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString itemId READ itemId CONSTANT)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(Status providersStatus READ providersStatus NOTIFY providersChanged)
    Q_PROPERTY(QString imageType READ imageType WRITE setImageType NOTIFY imageTypeChanged)
    Q_PROPERTY(QString provider READ provider WRITE setProvider NOTIFY providerChanged)
    Q_PROPERTY(bool allLanguages READ allLanguages WRITE setAllLanguages NOTIFY allLanguagesChanged)
    Q_PROPERTY(QStringList imageTypes READ imageTypes NOTIFY providersChanged)
    Q_PROPERTY(QStringList providerNames READ providerNames NOTIFY providersChanged)
    Q_PROPERTY(int total READ total NOTIFY pageChanged)
    Q_PROPERTY(int firstShown READ firstShown NOTIFY pageChanged)
    Q_PROPERTY(int lastShown READ lastShown NOTIFY pageChanged)
    Q_PROPERTY(bool hasPrevious READ hasPrevious NOTIFY pageChanged)
    Q_PROPERTY(bool hasNext READ hasNext NOTIFY pageChanged)
    Q_PROPERTY(DownloadState downloadState READ downloadState NOTIFY downloadStateChanged)
    Q_PROPERTY(int downloadStatus READ downloadStatus NOTIFY downloadStateChanged)

  public:
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    enum DownloadState { NoDownload, Downloading, DownloadFailed };
    Q_ENUM(DownloadState)
    // `provider`: the provider's name; `thumbnailUrl`: empty unless the client accepted the provider's preview; `imageWidth` and `imageHeight`: in pixels (named so because a delegate has a width and a height); `rating`: a score from 0 to 10 or, with `ratingType` "Likes", a count of likes; `language`: empty when the image has none
    enum Role {
        ProviderRole = Qt::UserRole + 1,
        ThumbnailUrlRole,
        WidthRole,
        HeightRole,
        LanguageRole,
        RatingRole,
        VoteCountRole,
        RatingTypeRole
    };

    // The images of one page: the page is the same size whatever the width of the grid
    static constexpr int PageSize = 24;

    // `channel` is the model's own listing channel (the controller hands them out); `imageType` is the type to look for first (an unknown one is Primary). The model asks for the providers and the first page at once
    RemoteImagesModel(JellyfinClient *client, const QString &itemId, const QString &imageType,
                      int channel, QObject *parent = nullptr);
    ~RemoteImagesModel() override;

    // Asks again for the page shown (the first after a change of the type, the provider or the language); the providers too when they are not known
    Q_INVOKABLE void reload();
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    // Has the server download the image of a row as the item's image of the current type; false, with no request, for a row that is not shown or while another download runs. The outcome is downloadState: Downloading, then `downloaded` or DownloadFailed
    Q_INVOKABLE bool download(int row);
    // Forgets a failed download, so the page stops saying so
    Q_INVOKABLE void dismissDownloadError();

    QString itemId() const { return m_itemId; }
    int channel() const { return m_channel; }
    Status status() const { return m_status; }
    Status providersStatus() const { return m_providersStatus; }
    QString imageType() const { return m_type; }
    void setImageType(const QString &type);
    QString provider() const { return m_provider; }
    void setProvider(const QString &provider);
    bool allLanguages() const { return m_allLanguages; }
    void setAllLanguages(bool all);
    // The types a provider can offer, in the editor's order, with the current one always in; the usual ones while the providers are not known
    QStringList imageTypes() const;
    // The providers that offer the current type, in the server's order; empty while they are not known
    QStringList providerNames() const;
    int total() const { return m_total; }
    // The 1-based range of the images of the page against the total (both 0 for none)
    int firstShown() const;
    int lastShown() const;
    bool hasPrevious() const { return m_startIndex > 0; }
    bool hasNext() const { return m_startIndex + PageSize < m_total; }
    DownloadState downloadState() const { return m_downloadState; }
    // The HTTP status of a failed download, 0 for a transport error or a request that was not sent
    int downloadStatus() const { return m_downloadStatus; }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void statusChanged();
    void providersChanged();
    void imageTypeChanged();
    void providerChanged();
    void allLanguagesChanged();
    void pageChanged();
    void downloadStateChanged();
    // The server saved the downloaded image as the item's image
    void downloaded();

  private:
    struct Offer {
        quint32 id = 0;
        QString provider;
        QUrl thumbnailUrl;
        int width = 0;
        int height = 0;
        QString language;
        double rating = 0;
        int voteCount = 0;
        QString ratingType;
    };
    struct Provider {
        QString name;
        QStringList types;
    };

    void setStatus(Status status);
    void setDownloadState(DownloadState state, int status = 0);
    void requestProviders();
    void requestPage(int startIndex);
    void clearRows();
    void onProvidersReady(const QString &itemId, const QVariantList &providers, int channel);
    void onImagesReady(const QString &itemId, const QString &type, int startIndex,
                       const QVariantList &images, int total, const QStringList &providers,
                       int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void onImageUpdated(const QString &itemId, const QString &type, int index);
    void onImageUpdateFailed(const QString &itemId, const QString &type, int index, int status);
    void clear();
    bool sameItem(const QString &itemId) const;
    bool providersKnown() const { return m_providersStatus == Ready && !m_providers.empty(); }
    bool offeredByAProvider(const QString &type) const;

    JellyfinClient *m_client = nullptr;
    QString m_itemId;
    int m_channel = 0;
    QString m_type;
    QString m_provider;
    bool m_allLanguages = false;
    Status m_status = Idle;
    Status m_providersStatus = Idle;
    std::vector<Provider> m_providers;
    std::vector<Offer> m_rows;
    int m_startIndex = 0;
    int m_total = 0;
    DownloadState m_downloadState = NoDownload;
    int m_downloadStatus = 0;
    QString m_downloadType;
};
} // namespace Kinefin
