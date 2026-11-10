// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagecache/ImageNetworkAccess.h"

#include "api/JellyfinClient.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>

Q_LOGGING_CATEGORY(imageCacheLog, "kinefin.imagecache")

namespace Kinefin {

namespace {

constexpr int kTransferTimeoutMs = 15000;

// A reply that fails as soon as the event loop runs, with no socket behind it
class RefusedReply : public QNetworkReply {
  public:
    RefusedReply(QNetworkAccessManager::Operation operation, const QNetworkRequest &request,
                 QObject *parent)
        : QNetworkReply(parent)
    {
        setOperation(operation);
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
        setError(QNetworkReply::ProtocolFailure, QStringLiteral("Image request refused"));
        setFinished(true);
        // Queued so the caller has connected to the signals by the time they fire
        QMetaObject::invokeMethod(
            this,
            [this] {
                Q_EMIT errorOccurred(QNetworkReply::ProtocolFailure);
                Q_EMIT finished();
            },
            Qt::QueuedConnection);
    }

    void abort() override {}

  protected:
    qint64 readData(char *, qint64) override { return -1; }
};

void removeHeader(QNetworkRequest &request, const QByteArray &name)
{
    const QList<QByteArray> names = request.rawHeaderList();
    for (const QByteArray &existing : names) {
        // A null value erases the header
        if (existing.compare(name, Qt::CaseInsensitive) == 0)
            request.setRawHeader(existing, QByteArray());
    }
}

} // namespace

ImageNetworkAccess::ImageNetworkAccess(std::shared_ptr<ImageOrigin> origin,
                                       const QString &cacheDirectory, QObject *parent)
    : QNetworkAccessManager(parent), m_origin(std::move(origin))
{
    // No cookie jar is ever created: requests opt out of cookies below and the lazily created default jar stays unused
    if (cacheDirectory.isEmpty())
        return;
    const QFile::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QDir().mkpath(cacheDirectory) || !QFile::setPermissions(cacheDirectory, ownerOnly)) {
        qCWarning(imageCacheLog) << "Image disk cache unavailable, running without it";
        return;
    }
    auto *cache = new QNetworkDiskCache(this);
    cache->setCacheDirectory(cacheDirectory);
    cache->setMaximumCacheSize(kImageCacheBytes);
    setCache(cache);
}

QNetworkReply *ImageNetworkAccess::createRequest(Operation operation,
                                                 const QNetworkRequest &request,
                                                 QIODevice *outgoingData)
{
    const QUrl base = m_origin ? m_origin->get() : QUrl();
    if (operation != GetOperation || !JellyfinClient::isServerImageUrl(base, request.url())) {
        // Without a server (signed out, or the session just ended) an image still in flight is expected to fail: only a request that does not belong to the active server is worth a warning
        if (base.isEmpty())
            qCDebug(imageCacheLog) << "Image request with no active server, refused";
        else
            qCWarning(imageCacheLog) << "Refused image request to host" << request.url().host();
        return new RefusedReply(operation, request, this);
    }
    QNetworkRequest safe(request);
    safe.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                      QNetworkRequest::ManualRedirectPolicy);
    safe.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    safe.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    // Image URLs always carry the image tag, so a cached copy never goes stale
    safe.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    safe.setTransferTimeout(kTransferTimeoutMs);
    removeHeader(safe, "Authorization");
    removeHeader(safe, "Cookie");
    QNetworkReply *reply = QNetworkAccessManager::createRequest(operation, safe, outgoingData);
    const qint64 limit = m_maxImageBytes;
    // The manual redirect policy only hands the 3xx to the caller, and QML's image loader follows it with a new request: end the reply instead. The declared length is checked here too, before any body byte is read
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, limit] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if ((status >= 300 && status < 400)
            || reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit)
            reply->abort();
    });
    // A body without a declared length (or a lying one) is cut once it passes the limit
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply, limit](qint64 received, qint64 total) {
                if (received > limit || total > limit)
                    reply->abort();
            });
    return reply;
}

} // namespace Kinefin
