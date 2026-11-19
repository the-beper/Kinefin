// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// One range download: 200 and 206, an ignored range, a resumed sidecar (item ETag and HTTP validator apart), a changed size or validator, 416, a redirect, a damaged 206, wrong body sizes, HTTP errors, damaged sidecars, the free-space reserve and cancellation
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

QJsonObject sidecarObject(const QString &key, const QString &etag, const QJsonValue &size,
                          const QJsonValue &written, const QString &validator = {})
{
    return {{QStringLiteral("version"), 1}, {QStringLiteral("key"), key},
            {QStringLiteral("etag"), etag}, {QStringLiteral("validator"), validator},
            {QStringLiteral("size"), size}, {QStringLiteral("written"), written}};
}

// `etag` is the item's, `validator` the HTTP one the sidecar saw in an earlier response
bool writeSidecar(const QString &path, const QString &key, const QString &etag, qint64 size,
                  qint64 written, const QString &validator = {})
{
    const QJsonObject object = sidecarObject(key, etag, double(size), double(written), validator);
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
        QVERIFY(writeSidecar(request.sidecarPath, request.resumeKey, request.etag, body.size(),
                             1000, QStringLiteral("\"etag-1\"")));

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

    // The item's ETag (MediaSources) and the HTTP ETag header are different values: only the HTTP one goes into If-Range
    void resumesWhenTheHttpValidatorDiffersFromTheItemEtag()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadEtag = QByteArrayLiteral("\"http-1\"");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadTransfer::Request request = requestFor(server, dir.path(), 64 * 1024);
        request.etag = QStringLiteral("\"item-hash\"");
        const QByteArray body = server.downloadBodyFor(kId.toLatin1());
        QVERIFY(writeBytes(request.partialPath, body.left(1000)));
        QVERIFY(writeSidecar(request.sidecarPath, request.resumeKey, request.etag, body.size(),
                             1000, QStringLiteral("\"http-1\"")));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(server.downloadRequests, 1);
        QCOMPARE(server.lastDownloadRange, QByteArrayLiteral("bytes=1000-"));
        QCOMPARE(server.lastDownloadIfRange, QByteArrayLiteral("\"http-1\""));
        QCOMPARE(finished.constFirst().at(1).toString(), QStringLiteral("\"item-hash\""));
        QCOMPARE(contents(request.targetPath), body);
    }

    void resumesWithLastModifiedAsTheValidator()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadEtag.clear();
        server.downloadLastModified = QByteArrayLiteral("Wed, 21 Oct 2026 07:28:00 GMT");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadTransfer::Request request = requestFor(server, dir.path(), 64 * 1024);
        const QByteArray body = server.downloadBodyFor(kId.toLatin1());
        QVERIFY(writeBytes(request.partialPath, body.left(1000)));
        QVERIFY(writeSidecar(request.sidecarPath, request.resumeKey, request.etag, body.size(),
                             1000, QStringLiteral("Wed, 21 Oct 2026 07:28:00 GMT")));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(server.downloadRequests, 1);
        QCOMPARE(server.lastDownloadIfRange, QByteArrayLiteral("Wed, 21 Oct 2026 07:28:00 GMT"));
        QCOMPARE(contents(request.targetPath), body);
    }

    void sendsNoIfRangeWithoutAValidator()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadTransfer::Request request = requestFor(server, dir.path(), 64 * 1024);
        const QByteArray body = server.downloadBodyFor(kId.toLatin1());
        QVERIFY(writeBytes(request.partialPath, body.left(1000)));
        // The item's ETag is never an HTTP validator, so a sidecar that saw none resumes on the range alone
        QVERIFY(
            writeSidecar(request.sidecarPath, request.resumeKey, request.etag, body.size(), 1000));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(server.lastDownloadRange, QByteArrayLiteral("bytes=1000-"));
        QVERIFY(server.lastDownloadIfRange.isEmpty());
        QCOMPARE(contents(request.targetPath), body);
    }

    void restartsWhenTheHttpValidatorChanged()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadEtag = QByteArrayLiteral("\"http-2\"");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadTransfer::Request request = requestFor(server, dir.path(), 64 * 1024);
        const QByteArray body = server.downloadBodyFor(kId.toLatin1());
        QVERIFY(writeBytes(request.partialPath, body.left(1000)));
        QVERIFY(writeSidecar(request.sidecarPath, request.resumeKey, request.etag, body.size(),
                             1000, QStringLiteral("\"http-1\"")));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        // The server saw the old validator in If-Range and answered the whole file
        QCOMPARE(server.downloadRequests, 1);
        QCOMPARE(server.lastDownloadIfRange, QByteArrayLiteral("\"http-1\""));
        QCOMPARE(contents(request.targetPath), body);
    }

    // The 206 arrives with its body, so a restart can happen while the reply is already finished
    void restartsAfterAChangedSizeWithASmallBody()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadBody = QByteArray(3000, 's');
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadTransfer::Request request = requestFor(server, dir.path(), 5000);
        QVERIFY(writeBytes(request.partialPath, server.downloadBody.left(1000)));
        QVERIFY(writeSidecar(request.sidecarPath, request.resumeKey, request.etag, 5000, 1000));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(server.downloadRequests, 2);
        QCOMPARE(finished.constFirst().at(0).toLongLong(), 3000);
        QCOMPARE(contents(request.targetPath), server.downloadBody);
    }

    void failsWhenASecondSizeMismatchFollowsTheRestart()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadForce206 = true;
        server.downloadClaimedTotals = {70000, 80000};
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
        QVERIFY(finished.isEmpty());
        QCOMPARE(failed.constFirst().at(0).value<DownloadTransfer::Failure>(),
                 DownloadTransfer::Failure::Changed);
        QCOMPARE(server.downloadRequests, 2);
        QVERIFY(!QFile::exists(request.targetPath));
    }

    void rejectsA206ThatStartsElsewhere()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadStartShift = 7;
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
        QVERIFY(finished.isEmpty());
        QCOMPARE(failed.constFirst().at(0).value<DownloadTransfer::Failure>(),
                 DownloadTransfer::Failure::Protocol);
        QCOMPARE(failed.constFirst().at(1).toInt(), 206);
        QVERIFY(!QFile::exists(request.targetPath));
    }

    void failsOnABodyShorterThanExpected()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadOmitLength = true;
        server.downloadBody = QByteArray(3000, 's');
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(requestFor(server, dir.path(), 64 * 1024)));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(finished.isEmpty());
        QCOMPARE(failed.constFirst().at(0).value<DownloadTransfer::Failure>(),
                 DownloadTransfer::Failure::SizeMismatch);
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("media.mkv"))));
    }

    void failsOnABodyLongerThanExpected()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadOmitLength = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(requestFor(server, dir.path(), 3000)));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(finished.isEmpty());
        QCOMPARE(failed.constFirst().at(0).value<DownloadTransfer::Failure>(),
                 DownloadTransfer::Failure::SizeMismatch);
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("media.mkv"))));
    }

    void reportsHttpErrors_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("forbidden") << 403;
        QTest::newRow("not found") << 404;
        QTest::newRow("server error") << 500;
    }

    void reportsHttpErrors()
    {
        QFETCH(int, status);
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadStatus = status;
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
        QCOMPARE(failed.constFirst().at(1).toInt(), status);
        QCOMPARE(server.downloadRequests, 1);
    }

    void failsWhenARestartIsRefusedAgain()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.downloadStatus = 409;
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
        QVERIFY(finished.isEmpty());
        QCOMPARE(failed.constFirst().at(0).value<DownloadTransfer::Failure>(),
                 DownloadTransfer::Failure::Http);
        QCOMPARE(failed.constFirst().at(1).toInt(), 409);
        QCOMPARE(server.downloadRequests, 2);
    }

    void discardsADamagedSidecar_data()
    {
        QTest::addColumn<QByteArray>("sidecar");
        const auto bytes = [](const QJsonObject &object) {
            return QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        const QString key = kId + QStringLiteral("/item");
        const QString etag = QStringLiteral("\"etag-1\"");
        const QByteArray valid = bytes(sidecarObject(key, etag, 65536, 1000));
        QTest::newRow("not json") << QByteArrayLiteral("not json");
        QTest::newRow("truncated") << valid.left(valid.size() - 12);
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("oversized") << QByteArray(5000, ' ');
        QTest::newRow("another item")
            << bytes(sidecarObject(QStringLiteral("other/item"), etag, 65536, 1000));
        QTest::newRow("written beyond size") << bytes(sidecarObject(key, etag, 65536, 70000));
        QTest::newRow("negative written") << bytes(sidecarObject(key, etag, 65536, -5));
        QTest::newRow("fractional written") << bytes(sidecarObject(key, etag, 65536, 10.5));
        QTest::newRow("huge written") << bytes(sidecarObject(key, etag, 65536, 1e300));
        QTest::newRow("huge size") << bytes(sidecarObject(key, etag, 1e300, 1000));
        QTest::newRow("size is text") << bytes(sidecarObject(key, etag, QStringLiteral("x"), 1000));
        QTest::newRow("validator with a line break")
            << bytes(sidecarObject(key, etag, 65536, 1000, QStringLiteral("a\nb")));
    }

    void discardsADamagedSidecar()
    {
        QFETCH(QByteArray, sidecar);
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadTransfer::Request request = requestFor(server, dir.path(), 64 * 1024);
        const QByteArray body = server.downloadBodyFor(kId.toLatin1());
        QVERIFY(writeBytes(request.partialPath, body.left(1000)));
        QVERIFY(writeBytes(request.sidecarPath, sidecar));

        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(failed.isEmpty());
        // The partial it described is not trusted: the file starts from zero
        QCOMPARE(server.downloadRequests, 1);
        QVERIFY(server.lastDownloadRange.isEmpty());
        QCOMPARE(contents(request.targetPath), body);
    }

    void stopsBelowTheFreeSpaceReserve()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // No size is known up front, so only the check while streaming can catch it
        DownloadTransfer::Request request = requestFor(server, dir.path(), -1);
        request.reserveBytes = qint64(1) << 60;
        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(finished.isEmpty());
        QCOMPARE(failed.constFirst().at(0).value<DownloadTransfer::Failure>(),
                 DownloadTransfer::Failure::NoSpace);
        QCOMPARE(server.downloadRequests, 1);
        QVERIFY(!QFile::exists(request.targetPath));
    }

    void refusesToStartBelowTheReserve()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DownloadTransfer::Request request = requestFor(server, dir.path(), 64 * 1024);
        request.reserveBytes = qint64(1) << 60;
        DownloadTransfer transfer;
        QSignalSpy finished(&transfer, &DownloadTransfer::finished);
        QSignalSpy failed(&transfer, &DownloadTransfer::failed);
        QVERIFY(transfer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 5000);
        QVERIFY(finished.isEmpty());
        QCOMPARE(failed.constFirst().at(0).value<DownloadTransfer::Failure>(),
                 DownloadTransfer::Failure::NoSpace);
        QCOMPARE(server.downloadRequests, 0);
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
