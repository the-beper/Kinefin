// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// One range download: 200 and 206, an ignored range, a resumed sidecar, a changed size, 416, a redirect, a damaged 206 and cancellation
#include "FakeJellyfin.h"
#include "downloads/DownloadTransfer.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Kinefin::DownloadTransfer;

namespace {
const QString kId = QStringLiteral("0123456789abcdef0123456789abcdef");
const QByteArray kAuth = QByteArrayLiteral(
    "MediaBrowser Client=\"Kinefin\", Token=\"f2f4b7c1ee8a4d0bb2a5f9d1f9e0e2ab\"");

DownloadTransfer::Request requestFor(const FakeJellyfin &server, const QString &directory,
                                     qint64 expectedSize)
{
    DownloadTransfer::Request request;
    request.url = server.url(QStringLiteral("/Items/%1/Download").arg(kId));
    request.authorization = kAuth;
    request.targetPath = directory + QStringLiteral("/media.mkv");
    request.partialPath = request.targetPath + QStringLiteral(".part");
    request.sidecarPath = request.partialPath + QStringLiteral(".kinefin-part.json");
    request.resumeKey = kId + QStringLiteral("/item");
    request.expectedSize = expectedSize;
    request.etag = QStringLiteral("\"etag-1\"");
    return request;
}

bool writeBytes(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

bool writeSidecar(const QString &path, const QString &key, const QString &etag, qint64 size,
                  qint64 written)
{
    const QJsonObject object{{QStringLiteral("version"), 1},
                             {QStringLiteral("key"), key},
                             {QStringLiteral("etag"), etag},
                             {QStringLiteral("size"), size},
                             {QStringLiteral("written"), written}};
    return writeBytes(path, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QByteArray contents(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
} // namespace

class TestDownloadTransfer : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void initTestCase() { qRegisterMetaType<Kinefin::DownloadTransfer::Failure>(); }

    void downloadsTheWholeFile()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(requestFor(server, dir.path(), 64 * 1024)));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(finished.constFirst().at(0).toLongLong(), 64 * 1024);
        QCOMPARE(finished.constFirst().at(1).toString(), QStringLiteral("\"etag-1\""));
        QCOMPARE(contents(dir.filePath(QStringLiteral("media.mkv"))),
                 server.downloadBodyFor(kId.toLatin1()));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("media.mkv.part"))));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("media.mkv.part.kinefin-part.json"))));
        QCOMPARE(server.downloadRequests, 1);
        QCOMPARE(server.lastDownloadAuth, kAuth);
        QVERIFY(server.lastDownloadRange.isEmpty());
    }

    void resumesFromTheSidecar()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadTransfer::Request request = requestFor(server, dir.path(), 64 * 1024);
        const QByteArray body = server.downloadBodyFor(kId.toLatin1());
        QVERIFY(writeBytes(request.partialPath, body.left(1000)));
        QVERIFY(
            writeSidecar(request.sidecarPath, request.resumeKey, request.etag, body.size(), 1000));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(server.lastDownloadRange, QByteArrayLiteral("bytes=1000-"));
        QCOMPARE(server.lastDownloadIfRange, QByteArrayLiteral("\"etag-1\""));
        QCOMPARE(contents(request.targetPath), body);
        QVERIFY(!QFile::exists(request.sidecarPath));
    }

    void ignoresAnIgnoredRange()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadIgnoreRange = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadTransfer::Request request = requestFor(server, dir.path(), 64 * 1024);
        const QByteArray body = server.downloadBodyFor(kId.toLatin1());
        QVERIFY(writeBytes(request.partialPath, body.left(1000)));
        QVERIFY(
            writeSidecar(request.sidecarPath, request.resumeKey, request.etag, body.size(), 1000));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        QVERIFY(!server.lastDownloadRange.isEmpty());
        QCOMPARE(contents(request.targetPath), body);
    }

    void restartsAfterAChangedSize()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadTransfer::Request request = requestFor(server, dir.path(), 70000);
        const QByteArray body = server.downloadBodyFor(kId.toLatin1());
        QVERIFY(writeBytes(request.partialPath, body.left(1000)));
        QVERIFY(writeSidecar(request.sidecarPath, request.resumeKey, request.etag, 70000, 1000));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(server.downloadRequests, 2);
        QCOMPARE(finished.constFirst().at(0).toLongLong(), body.size());
        QCOMPARE(contents(request.targetPath), body);
    }

    void restartsAfterAnUnsatisfiableRange()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadTransfer::Request request = requestFor(server, dir.path(), -1);
        const QByteArray body = server.downloadBodyFor(kId.toLatin1());
        QVERIFY(writeBytes(request.partialPath, body));
        QVERIFY(
            writeSidecar(request.sidecarPath, request.resumeKey, request.etag, -1, body.size()));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(server.downloadRequests, 2);
        QCOMPARE(contents(request.targetPath), body);
    }

    void discardsASidecarOfAnotherEtag()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadTransfer::Request request = requestFor(server, dir.path(), 64 * 1024);
        const QByteArray body = server.downloadBodyFor(kId.toLatin1());
        QVERIFY(writeBytes(request.partialPath, body.left(1000)));
        QVERIFY(writeSidecar(request.sidecarPath, request.resumeKey, QStringLiteral("\"old\""),
                             body.size(), 1000));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        QVERIFY(server.lastDownloadRange.isEmpty());
        QCOMPARE(contents(request.targetPath), body);
    }

    void rejectsA206WithoutContentRange()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadStatus = 206;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(requestFor(server, dir.path(), 64 * 1024)));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(finished.isEmpty());
        QCOMPARE(failed.constFirst().at(0).value<DownloadTransfer::Failure>(),
                 DownloadTransfer::Failure::Protocol);
        QCOMPARE(failed.constFirst().at(1).toInt(), 206);
    }

    void refusesARedirect()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.redirectDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(requestFor(server, dir.path(), 64 * 1024)));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(finished.isEmpty());
        QCOMPARE(failed.constFirst().at(0).value<DownloadTransfer::Failure>(),
                 DownloadTransfer::Failure::Http);
        QCOMPARE(failed.constFirst().at(1).toInt(), 302);
        QCOMPARE(server.downloadRequests, 1);
    }

    void cancels()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdDownload = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(requestFor(server, dir.path(), 64 * 1024)));
        QTRY_COMPARE_WITH_TIMEOUT(server.downloadRequests, 1, 5000);
        transfer.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(finished.isEmpty());
        QCOMPARE(failed.constFirst().at(0).value<DownloadTransfer::Failure>(),
                 DownloadTransfer::Failure::Cancelled);
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("media.mkv"))));
    }

    void refusesAnIncompleteRequest()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadTransfer transfer;
        DownloadTransfer::Request request;
        QVERIFY(!transfer.start(request));
        request.authorization = kAuth;
        QVERIFY(!transfer.start(request));
    }
};

QTEST_GUILESS_MAIN(TestDownloadTransfer)
#include "test_download_transfer.moc"
