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
constexpr qint64 MaxByteCount = qint64(1) << 50;
constexpr qint64 MaxReserveBytes = qint64(1) << 61;
constexpr int ProgressIntervalMs = 250;
constexpr int SpaceIntervalMs = 250;
// A fast link writes far more than the reserve in SpaceIntervalMs, so the bytes written since the last check bound it too
constexpr qint64 SpaceCheckBytes = qint64(8) * 1024 * 1024;
// An HTTP validator longer than this would not fit the sidecar, which would then make every resume start over
constexpr int MaxValidatorBytes = 256;
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
    if (request.reserveBytes < 0 || request.reserveBytes > MaxReserveBytes)
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

// A JSON number is range-checked as a double before it is cast: a value such as 1e300 from a damaged file is invalid, not undefined behaviour
bool readCount(const QJsonValue &value, qint64 minimum, qint64 *out)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number != std::floor(number) || number < double(minimum)
        || number > double(MaxByteCount))
        return false;
    *out = qint64(number);
    return true;
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
        m_storage.setPath(QFileInfo(m_request.targetPath).absolutePath());
        if (m_request.expectedSize >= 0
            && lacksSpace(m_request.expectedSize - m_written + m_request.reserveBytes)) {
            fail(Failure::NoSpace, 0);
            return;
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

    bool readSidecar(qint64 *size, qint64 *written, QString *etag, QString *validator)
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
        if (!readCount(object.value(QStringLiteral("size")), -1, size)
            || !readCount(object.value(QStringLiteral("written")), 0, written))
            return false;
        *etag = object.value(QStringLiteral("etag")).toString();
        *validator = object.value(QStringLiteral("validator")).toString();
        if (!validator->isEmpty() && !validHeaderValue(*validator))
            return false;
        if (*size >= 0 && *written > *size)
            return false;
        if (m_request.expectedSize >= 0 && *size >= 0 && *size != m_request.expectedSize)
            return false;
        // The sidecar's etag is the item's, compared with the request's only when both exist; the validator is an HTTP one and never compared with it
        if (!m_request.etag.isEmpty() && !etag->isEmpty() && m_request.etag != *etag)
            return false;
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
        m_spaceCheckedAt = m_written;
        m_validator.clear();
        m_sidecarSize = m_request.expectedSize;
    }

    // A sidecar that does not match this item, source or size is discarded with its partial; an intact one resumes at its own byte count
    bool restorePartial()
    {
        m_sidecarSize = m_request.expectedSize;
        qint64 size = -1;
        qint64 written = 0;
        QString etag;
        QString validator;
        const bool sidecar = readSidecar(&size, &written, &etag, &validator);
        m_sidecarSize = size;
        if (!sidecar) {
            discardPartial();
            return true;
        }
        m_validator = validator;
        const QFileInfo part(m_request.partialPath);
        if (!part.exists()) {
            m_written = 0;
            m_spaceCheckedAt = m_written;
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
        m_spaceCheckedAt = m_written;
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
        const QJsonObject object{
            {QStringLiteral("version"), 1},           {QStringLiteral("key"), m_request.resumeKey},
            {QStringLiteral("etag"), m_request.etag}, {QStringLiteral("validator"), m_validator},
            {QStringLiteral("size"), m_sidecarSize},  {QStringLiteral("written"), m_written}};
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
            // A weak validator never matches in If-Range, so it would only turn the resume into a full download
            if (validHeaderValue(m_validator) && !m_validator.startsWith(QLatin1String("W/")))
                request.setRawHeader("If-Range", m_validator.toUtf8());
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

    // The response's validator for a later If-Range: the ETag, else Last-Modified. 12.4 sends no ETag on the Download route, so Last-Modified is the one in practice
    QString responseValidator() const
    {
        const QString etag = QString::fromLatin1(m_reply->rawHeader("ETag"));
        if (validHeaderValue(etag) && etag.size() <= MaxValidatorBytes)
            return etag;
        const QString modified = QString::fromLatin1(m_reply->rawHeader("Last-Modified"));
        return validHeaderValue(modified) && modified.size() <= MaxValidatorBytes ? modified
                                                                                  : QString();
    }

    // True only when the current reply is ready to have its body written; a restart or a failure leaves the caller with nothing more to do
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
        const QString validator = responseValidator();
        if (status == 200) {
            // The server ignored the range: truncate and restart from zero
            if (m_written > 0 || m_file.isOpen())
                discardPartial();
            if (contentLength >= 0) {
                m_request.expectedSize = contentLength;
                m_sidecarSize = contentLength;
            }
            m_validator = validator;
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
            // Only the validator an earlier response gave is compared with this one
            const bool changed =
                (!m_validator.isEmpty() && !validator.isEmpty() && validator != m_validator)
                || (total >= 0 && m_request.expectedSize >= 0 && total != m_request.expectedSize);
            if (changed) {
                restartFromZero(status, total);
                return false;
            }
            if (total >= 0) {
                m_request.expectedSize = total;
                m_sidecarSize = total;
            }
            if (!validator.isEmpty())
                m_validator = validator;
        } else {
            restartOrFail(status);
            return false;
        }
        if (!openPartial()) {
            fail(Failure::Io, status);
            return false;
        }
        m_metaReady = true;
        return true;
    }

    // One automatic restart after a changed size/tag or an unsatisfiable range; a second mismatch is a hard failure
    void restartFromZero(int status, qint64 total)
    {
        if (m_restarted) {
            fail(Failure::Changed, status);
            return;
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
        m_validator.clear();
        if (m_file.isOpen())
            m_file.close();
        if (!openPartial()) {
            fail(Failure::Io, status);
            return;
        }
        sendRequest();
    }

    void restartOrFail(int status)
    {
        if (status == 416 && m_request.expectedSize >= 0 && m_written == m_request.expectedSize) {
            completePartial();
            return;
        }
        if (status != 416 && status != 409 && status != 412) {
            fail(Failure::Http, status);
            return;
        }
        if (m_restarted) {
            fail(status == 416 ? Failure::Protocol : Failure::Http, status);
            return;
        }
        restartFromZero(status, -1);
    }

    bool lacksSpace(qint64 needed)
    {
        m_storage.refresh();
        return m_storage.isValid() && m_storage.bytesAvailable() >= 0
               && m_storage.bytesAvailable() < needed;
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
        // A server that sends no size streams until the disk is full, so the reserve is checked on a timer for the whole transfer
        if (!m_spaceClock.isValid() || m_spaceClock.elapsed() >= SpaceIntervalMs
            || m_written + chunk.size() - m_spaceCheckedAt >= SpaceCheckBytes) {
            m_spaceClock.restart();
            m_spaceCheckedAt = m_written + chunk.size();
            if (lacksSpace(m_request.reserveBytes + chunk.size())) {
                fail(Failure::NoSpace, 0);
                return;
            }
        }
        if (m_file.write(chunk) != chunk.size()) {
            fail(lacksSpace(m_request.reserveBytes) ? Failure::NoSpace : Failure::Io, 0);
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
    // The HTTP validator (ETag, else Last-Modified) an earlier response of this download gave; the only one sent as If-Range
    QString m_validator;
    QStorageInfo m_storage;
    bool m_metaReady = false;
    bool m_done = false;
    bool m_cancelled = false;
    bool m_restarted = false;
    QElapsedTimer m_progressClock;
    QElapsedTimer m_sidecarClock;
    QElapsedTimer m_spaceClock;
    // m_written at the last free-space check
    qint64 m_spaceCheckedAt = 0;
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
