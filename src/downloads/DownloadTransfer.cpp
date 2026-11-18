// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadTransfer.h"

#include "settings/ProfilePaths.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSslError>
#include <QStorageInfo>
#include <QThread>

#include <cmath>
#include <memory>

namespace Kinefin {
namespace {
constexpr int MaxSidecarBytes = 4096;
constexpr int MaxAuthorizationBytes = 4096;
constexpr qint64 MinFreeBytes = 1024 * 1024;
constexpr int ProgressIntervalMs = 250;
constexpr int SidecarIntervalMs = 2000;

bool validRequest(const DownloadTransfer::Request &request)
{
    const QUrl url = request.url;
    if (!url.isValid() || url.isRelative()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        || url.host().isEmpty() || !url.userName().isEmpty() || !url.password().isEmpty()
        || url.hasFragment())
        return false;
    if (request.authorization.isEmpty() || request.authorization.size() > MaxAuthorizationBytes)
        return false;
    if (request.authorization.contains('\r') || request.authorization.contains('\n'))
        return false;
    if (request.targetPath.isEmpty() || request.partialPath.isEmpty()
        || request.sidecarPath.isEmpty() || request.resumeKey.isEmpty())
        return false;
    if (QFileInfo(request.targetPath).absolutePath()
        != QFileInfo(request.partialPath).absolutePath())
        return false;
    if (request.targetPath == request.partialPath || request.targetPath == request.sidecarPath
        || request.partialPath == request.sidecarPath)
        return false;
    return true;
}

// Content-Range: bytes <start>-<end>/<total|*>
bool parseContentRange(const QByteArray &header, qint64 *start, qint64 *end, qint64 *total)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\Abytes (\\d{1,18})-(\\d{1,18})/(\\d{1,18}|\\*)\\z"));
    const QRegularExpressionMatch match = pattern.match(QString::fromLatin1(header.trimmed()));
    if (!match.hasMatch())
        return false;
    bool okStart = false;
    bool okEnd = false;
    *start = match.captured(1).toLongLong(&okStart);
    *end = match.captured(2).toLongLong(&okEnd);
    if (!okStart || !okEnd || *end < *start)
        return false;
    *total = -1;
    if (match.captured(3) != QLatin1String("*")) {
        bool okTotal = false;
        *total = match.captured(3).toLongLong(&okTotal);
        if (!okTotal)
            return false;
    }
    return true;
}

// A header value is only put on the wire when it cannot split the request
bool validHeaderValue(const QString &value)
{
    return !value.isEmpty() && !value.contains(QChar(u'\r')) && !value.contains(QChar(u'\n'));
}
} // namespace

// The worker owns the QNetworkAccessManager, the file and the sidecar; it is created on the manager's thread and moved to its own before begin()
class DownloadTransfer::Worker final : public QObject {
    Q_OBJECT

  public:
    explicit Worker(DownloadTransfer::Request request) : m_request(std::move(request)) {}

  public Q_SLOTS:
    void begin()
    {
        if (m_done)
            return;
        if (!validRequest(m_request)) {
            fail(Failure::Io, 0);
            return;
        }
        if (!ensurePrivateDirectory(QFileInfo(m_request.targetPath).absolutePath())) {
            fail(Failure::Io, 0);
            return;
        }
        m_sidecarClock.start();
        if (!prepareFinalFile() || !restorePartial())
            return;
        if (m_request.expectedSize >= 0 && m_written == m_request.expectedSize) {
            completePartial();
            return;
        }
        if (m_request.expectedSize >= 0) {
            const QStorageInfo storage(QFileInfo(m_request.targetPath).absolutePath());
            const qint64 needed = m_request.expectedSize - m_written + MinFreeBytes;
            if (storage.isValid() && storage.bytesAvailable() >= 0
                && storage.bytesAvailable() < needed) {
                fail(Failure::NoSpace, 0);
                return;
            }
        }
        m_network = std::make_unique<QNetworkAccessManager>();
        QObject::connect(m_network.get(), &QNetworkAccessManager::sslErrors, this,
                         [this](QNetworkReply *reply, const QList<QSslError> &errors) {
                             if (m_request.ignoreCertificateErrors)
                                 reply->ignoreSslErrors(errors);
                         });
        m_progressClock.start();
        sendRequest();
    }

    void cancel()
    {
        if (m_done)
            return;
        m_cancelled = true;
        if (m_reply) {
            m_reply->abort();
            return;
        }
        fail(Failure::Cancelled, 0);
    }

  Q_SIGNALS:
    void progress(qint64 written, qint64 total);
    void finished(qint64 size, const QString &etag);
    void failed(Kinefin::DownloadTransfer::Failure failure, int httpStatus);
    void done();

  private:
    // A final file is only trusted when the expected size matches: a queued record never resumes on top of a stale one
    bool prepareFinalFile()
    {
        const QFileInfo finalInfo(m_request.targetPath);
        if (!finalInfo.exists() && !finalInfo.isSymLink())
            return true;
        if (finalInfo.isFile() && !finalInfo.isSymLink() && m_request.expectedSize >= 0
            && finalInfo.size() == m_request.expectedSize) {
            QFile::remove(m_request.partialPath);
            QFile::remove(m_request.sidecarPath);
            finish(finalInfo.size());
            return false;
        }
        if (!QFile::remove(m_request.targetPath)) {
            fail(Failure::Io, 0);
            return false;
        }
        return true;
    }

    bool readSidecar(qint64 *size, qint64 *written, QString *etag)
    {
        QFile file(m_request.sidecarPath);
        if (!file.exists() || file.size() <= 0 || file.size() > MaxSidecarBytes
            || !file.open(QIODevice::ReadOnly))
            return false;
        const QJsonDocument document = QJsonDocument::fromJson(file.read(MaxSidecarBytes));
        if (!document.isObject())
            return false;
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("version")).toInt() != 1
            || object.value(QStringLiteral("key")).toString() != m_request.resumeKey)
            return false;
        const QJsonValue storedSize = object.value(QStringLiteral("size"));
        const QJsonValue storedWritten = object.value(QStringLiteral("written"));
        if (!storedSize.isDouble() || !storedWritten.isDouble())
            return false;
        if (storedSize.toDouble() != std::floor(storedSize.toDouble())
            || storedWritten.toDouble() != std::floor(storedWritten.toDouble()))
            return false;
        *size = qint64(storedSize.toDouble());
        *written = qint64(storedWritten.toDouble());
        *etag = object.value(QStringLiteral("etag")).toString();
        if (*size < -1 || *written < 0)
            return false;
        if (*size >= 0 && *written > *size)
            return false;
        if (m_request.expectedSize >= 0 && *size >= 0 && *size != m_request.expectedSize)
            return false;
        // The sidecar's ETag is the item's MediaSources[].ETag, or the response's when the server sent one and the item had none
        const QString wanted = m_request.etag.isEmpty() ? *etag : m_request.etag;
        if (!wanted.isEmpty() && !etag->isEmpty() && wanted != *etag)
            return false;
        if (m_request.etag.isEmpty() && !etag->isEmpty())
            m_request.etag = *etag;
        if (m_request.expectedSize < 0 && *size >= 0)
            m_request.expectedSize = *size;
        return true;
    }

    bool truncatePartial(qint64 written)
    {
        QFile file(m_request.partialPath);
        return file.open(QIODevice::ReadWrite) && file.resize(written);
    }

    void discardPartial()
    {
        if (m_file.isOpen())
            m_file.close();
        QFile::remove(m_request.partialPath);
        QFile::remove(m_request.sidecarPath);
        m_written = 0;
        m_sidecarSize = m_request.expectedSize;
    }

    // A sidecar that does not match this item, source or size is discarded with its partial; an intact one resumes at its own byte count
    bool restorePartial()
    {
        m_sidecarSize = m_request.expectedSize;
        qint64 size = -1;
        qint64 written = 0;
        QString etag;
        const bool sidecar = readSidecar(&size, &written, &etag);
        m_sidecarSize = size;
        if (!sidecar) {
            discardPartial();
            return true;
        }
        const QFileInfo part(m_request.partialPath);
        if (!part.exists()) {
            m_written = 0;
            writeSidecar();
            return true;
        }
        if (!part.isFile() || part.isSymLink() || part.size() < written) {
            discardPartial();
            writeSidecar();
            return true;
        }
        if (part.size() > written && !truncatePartial(written)) {
            fail(Failure::Io, 0);
            return false;
        }
        m_written = written;
        if (m_request.expectedSize >= 0 && m_written > m_request.expectedSize) {
            discardPartial();
            writeSidecar();
        }
        return true;
    }

    bool openPartial()
    {
        if (m_file.isOpen())
            return true;
        m_file.setFileName(m_request.partialPath);
        if (!m_file.open(QIODevice::ReadWrite))
            return false;
        return m_file.seek(m_written);
    }

    void writeSidecar()
    {
        const QJsonObject object{{QStringLiteral("version"), 1},
                                 {QStringLiteral("key"), m_request.resumeKey},
                                 {QStringLiteral("etag"), m_request.etag},
                                 {QStringLiteral("size"), m_sidecarSize},
                                 {QStringLiteral("written"), m_written}};
        if (writePrivateFile(m_request.sidecarPath,
                             QJsonDocument(object).toJson(QJsonDocument::Compact)))
            m_sidecarClock.restart();
    }

    void sendRequest()
    {
        QNetworkRequest request(m_request.url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
        request.setTransferTimeout(30'000);
        request.setRawHeader("Accept", "*/*");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setRawHeader("Authorization", m_request.authorization);
        if (m_written > 0) {
            request.setRawHeader("Range", QByteArrayLiteral("bytes=")
                                              + QByteArray::number(m_written)
                                              + QByteArrayLiteral("-"));
            if (validHeaderValue(m_request.etag))
                request.setRawHeader("If-Range", m_request.etag.toUtf8());
        }
        m_metaReady = false;
        m_reply = m_network->get(request);
        QObject::connect(m_reply, &QNetworkReply::readyRead, this, [this] {
            if (m_done)
                return;
            if (!m_metaReady && !prepareReply())
                return;
            writeAvailable();
        });
        QObject::connect(m_reply, &QNetworkReply::finished, this, [this] { replyFinished(); });
    }

    bool prepareReply()
    {
        const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 0)
            return false;
        const QByteArray contentLengthHeader = m_reply->rawHeader("Content-Length");
        qint64 contentLength = -1;
        if (!contentLengthHeader.isEmpty()) {
            bool ok = false;
            contentLength = contentLengthHeader.toLongLong(&ok);
            if (!ok || contentLength < 0) {
                fail(Failure::Protocol, status);
                return false;
            }
        }
        const QString responseEtag = QString::fromLatin1(m_reply->rawHeader("ETag"));
        if (status == 200) {
            // The server ignored the range: truncate and restart from zero
            if (m_written > 0 || m_file.isOpen())
                discardPartial();
            if (contentLength >= 0) {
                m_request.expectedSize = contentLength;
                m_sidecarSize = contentLength;
            }
            if (validHeaderValue(responseEtag))
                m_request.etag = responseEtag;
        } else if (status == 206) {
            qint64 start = -1;
            qint64 end = -1;
            qint64 total = -1;
            if (!parseContentRange(m_reply->rawHeader("Content-Range"), &start, &end, &total)) {
                fail(Failure::Protocol, status);
                return false;
            }
            if (start != m_written || (contentLength >= 0 && end - start + 1 != contentLength)) {
                fail(Failure::Protocol, status);
                return false;
            }
            const bool changed =
                (!m_request.etag.isEmpty() && validHeaderValue(responseEtag)
                 && responseEtag != m_request.etag)
                || (total >= 0 && m_request.expectedSize >= 0 && total != m_request.expectedSize);
            if (changed)
                return restartFromZero(status, total);
            if (total >= 0) {
                m_request.expectedSize = total;
                m_sidecarSize = total;
            }
            if (validHeaderValue(responseEtag))
                m_request.etag = responseEtag;
        } else {
            return restartOrFail(status);
        }
        if (!openPartial()) {
            fail(Failure::Io, status);
            return false;
        }
        m_metaReady = true;
        return true;
    }

    // One automatic restart after a changed size/tag or an unsatisfiable range; a second mismatch is a hard failure
    bool restartFromZero(int status, qint64 total)
    {
        if (m_restarted) {
            fail(Failure::Changed, status);
            return false;
        }
        m_restarted = true;
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
        discardPartial();
        if (total >= 0) {
            m_request.expectedSize = total;
            m_sidecarSize = total;
        } else {
            m_request.expectedSize = -1;
        }
        m_request.etag.clear();
        if (m_file.isOpen())
            m_file.close();
        if (!openPartial()) {
            fail(Failure::Io, status);
            return false;
        }
        m_metaReady = true;
        sendRequest();
        return true;
    }

    bool restartOrFail(int status)
    {
        if (status == 416 && m_request.expectedSize >= 0 && m_written == m_request.expectedSize) {
            completePartial();
            return false;
        }
        if (status != 416 && status != 409 && status != 412) {
            fail(Failure::Http, status);
            return false;
        }
        if (m_restarted) {
            fail(status == 416 ? Failure::Protocol : Failure::Http, status);
            return false;
        }
        return restartFromZero(status, -1);
    }

    void writeAvailable()
    {
        if (!m_reply || m_done)
            return;
        const QByteArray chunk = m_reply->readAll();
        if (chunk.isEmpty())
            return;
        if (m_request.expectedSize >= 0 && m_written + chunk.size() > m_request.expectedSize) {
            fail(Failure::SizeMismatch, 0);
            return;
        }
        if (m_file.write(chunk) != chunk.size()) {
            const QStorageInfo storage(QFileInfo(m_request.targetPath).absolutePath());
            fail(storage.isValid() && storage.bytesAvailable() >= 0
                         && storage.bytesAvailable() < MinFreeBytes
                     ? Failure::NoSpace
                     : Failure::Io,
                 0);
            return;
        }
        m_written += chunk.size();
        if (m_sidecarClock.elapsed() >= SidecarIntervalMs) {
            m_file.flush();
            writeSidecar();
        }
        if (!m_progressClock.isValid() || m_progressClock.elapsed() >= ProgressIntervalMs) {
            m_progressClock.restart();
            Q_EMIT progress(m_written, m_request.expectedSize);
        }
    }

    void replyFinished()
    {
        if (!m_reply || m_done)
            return;
        const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError error = m_reply->error();
        if (m_cancelled) {
            fail(Failure::Cancelled, status);
            return;
        }
        if (status == 0) {
            fail(Failure::Network, 0);
            return;
        }
        if (status != 200 && status != 206) {
            restartOrFail(status);
            return;
        }
        if (error != QNetworkReply::NoError) {
            fail(Failure::Network, status);
            return;
        }
        if (!m_metaReady && !prepareReply())
            return;
        writeAvailable();
        if (m_done)
            return;
        if (m_file.isOpen() && !m_file.flush()) {
            fail(Failure::Io, status);
            return;
        }
        if (m_request.expectedSize >= 0 && m_written != m_request.expectedSize) {
            fail(Failure::SizeMismatch, status);
            return;
        }
        if (m_request.expectedSize < 0 && m_written == 0) {
            fail(Failure::Protocol, status);
            return;
        }
        completePartial();
    }

    void completePartial()
    {
        if (m_done)
            return;
        if (m_file.isOpen()) {
            m_file.flush();
            m_file.close();
        }
        if (!QFile::remove(m_request.targetPath) && QFileInfo::exists(m_request.targetPath)) {
            fail(Failure::Io, 0);
            return;
        }
        if (!QFile::rename(m_request.partialPath, m_request.targetPath)) {
            fail(Failure::Io, 0);
            return;
        }
        QFile::remove(m_request.sidecarPath);
        finish(m_written);
    }

    void finish(qint64 size)
    {
        if (m_done)
            return;
        m_done = true;
        const QString etag = m_request.etag;
        if (m_reply) {
            m_reply->disconnect(this);
            m_reply->deleteLater();
            m_reply = nullptr;
        }
        Q_EMIT finished(size, etag);
        Q_EMIT done();
    }

    void fail(Failure failure, int httpStatus)
    {
        if (m_done)
            return;
        m_done = true;
        if (m_reply) {
            m_reply->disconnect(this);
            m_reply->abort();
            m_reply->deleteLater();
            m_reply = nullptr;
        }
        if (m_file.isOpen()) {
            m_file.flush();
            m_file.close();
        }
        // A cancelled or failed transfer keeps the exact byte count for the next attempt; the manager deletes both on a user cancel
        if (m_written > 0)
            writeSidecar();
        Q_EMIT failed(failure, httpStatus);
        Q_EMIT done();
    }

    DownloadTransfer::Request m_request;
    std::unique_ptr<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QFile m_file;
    qint64 m_written = 0;
    qint64 m_sidecarSize = -1;
    bool m_metaReady = false;
    bool m_done = false;
    bool m_cancelled = false;
    bool m_restarted = false;
    QElapsedTimer m_progressClock;
    QElapsedTimer m_sidecarClock;
};

DownloadTransfer::DownloadTransfer(QObject *parent) : QObject(parent) {}

DownloadTransfer::~DownloadTransfer()
{
    if (!m_thread)
        return;
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, &Worker::cancel, Qt::QueuedConnection);
    m_thread->quit();
    m_thread->wait();
}

bool DownloadTransfer::start(const Request &request)
{
    if (m_thread || !validRequest(request))
        return false;
    m_thread = new QThread(this);
    m_worker = new Worker(request);
    m_worker->moveToThread(m_thread);
    QObject::connect(m_thread, &QThread::started, m_worker, &Worker::begin);
    QObject::connect(m_worker, &Worker::progress, this, &DownloadTransfer::progress);
    QObject::connect(m_worker, &Worker::finished, this, &DownloadTransfer::finished);
    QObject::connect(m_worker, &Worker::failed, this, &DownloadTransfer::failed);
    QObject::connect(m_worker, &Worker::done, m_thread, &QThread::quit);
    QObject::connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    QObject::connect(m_thread, &QThread::finished, this, [this] {
        m_worker = nullptr;
        m_thread->deleteLater();
        m_thread = nullptr;
    });
    m_thread->start();
    return true;
}

void DownloadTransfer::cancel()
{
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, &Worker::cancel, Qt::QueuedConnection);
}

bool DownloadTransfer::running() const
{
    return m_thread && m_thread->isRunning();
}
} // namespace Kinefin

#include "DownloadTransfer.moc"
