// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QThread;

namespace Kinefin {
// One resumable range download of an original file, in a worker thread of its own: `media.part` and its sidecar are appended to, the sidecar is validated before a resume, and the final name is only written once the byte count is known. The caller supplies the URL, the Authorization header and the expected size/ETag from its own item reply; this class never builds a URL from server text. A redirect, a 200 that ignores the range (the file restarts from zero once), a 206 whose start or total disagrees, a changed ETag/size, a short or oversized body, and cancellation all have their own outcome; a partial file is kept for the next attempt except after a cancel of a finished transfer
class DownloadTransfer final : public QObject {
    Q_OBJECT

  public:
    enum class Failure { Network, Http, Protocol, SizeMismatch, Changed, Io, NoSpace, Cancelled };
    Q_ENUM(Failure)

    struct Request {
        QUrl url;
        QByteArray authorization;
        QString targetPath;
        QString partialPath;
        QString sidecarPath;
        // itemId: a sidecar of another item is never resumed
        QString resumeKey;
        // -1 when unknown, learned from Content-Length or Content-Range
        qint64 expectedSize = -1;
        QString etag;
        bool ignoreCertificateErrors = false;
    };

    explicit DownloadTransfer(QObject *parent = nullptr);
    ~DownloadTransfer() override;

    // Starts the transfer; false when the request is invalid or one is already running
    bool start(const Request &request);
    // Aborts the reply, keeps the partial file and its sidecar, and reports Failure::Cancelled
    void cancel();
    bool running() const;

  Q_SIGNALS:
    void progress(qint64 written, qint64 total);
    void finished(qint64 size, const QString &etag);
    void failed(Kinefin::DownloadTransfer::Failure failure, int httpStatus);

  private:
    class Worker;
    QPointer<Worker> m_worker;
    QThread *m_thread = nullptr;
};
} // namespace Kinefin

Q_DECLARE_METATYPE(Kinefin::DownloadTransfer::Failure)
