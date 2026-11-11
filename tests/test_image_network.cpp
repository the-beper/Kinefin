// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FakeJellyfin.h"
#include "imagecache/ImageNetworkAccess.h"
#include "imagecache/ImageOrigin.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

QUrl imageUrl(const QUrl &base, const QString &path = {})
{
    const QString route =
        path.isEmpty() ? QStringLiteral("/Items/%1/Images/Primary").arg(kPlayItem) : path;
    return QUrl(base.toString() + route + QStringLiteral("?maxWidth=360&tag=abc123"));
}

} // namespace

class TestImageNetwork : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void foreignHostPortPathAndPostAreRefused()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QUrl base = server.url(QStringLiteral("/jellyfin"));
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        origin->set(base);
        Kinefin::ImageNetworkAccess network(origin);

        QUrl otherPort = imageUrl(base);
        otherPort.setPort(base.port() + 1);
        QUrl otherHost = imageUrl(base);
        otherHost.setHost(QStringLiteral("localhost"));
        QUrl withUserInfo = imageUrl(base);
        withUserInfo.setUserInfo(QStringLiteral("user:pass"));
        const QList<QUrl> refused = {
            otherPort,
            otherHost,
            withUserInfo,
            imageUrl(base, QStringLiteral("/Users/Me")),
            imageUrl(base, QStringLiteral("/Items/%1/PlaybackInfo").arg(kPlayItem)),
            imageUrl(base, QStringLiteral("/Items/%1/Images/Primary/0").arg(kPlayItem)),
        };
        for (const QUrl &url : refused) {
            std::unique_ptr<QNetworkReply> reply(network.get(QNetworkRequest(url)));
            QSignalSpy finished(reply.get(), &QNetworkReply::finished);
            QTRY_COMPARE(finished.size(), 1);
            QVERIFY2(reply->error() == QNetworkReply::ProtocolFailure, qPrintable(url.toString()));
            QVERIFY(!reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid());
        }
        std::unique_ptr<QNetworkReply> post(
            network.post(QNetworkRequest(imageUrl(base)), QByteArray("x")));
        QSignalSpy postFinished(post.get(), &QNetworkReply::finished);
        QTRY_COMPARE(postFinished.size(), 1);
        QCOMPARE(post->error(), QNetworkReply::ProtocolFailure);

        QCOMPARE(server.imageRequests, 0);
        // Not even another route reached the server: nothing went out at all
        QCOMPARE(server.requestCount, 0);
    }

    void redirectIsNotFollowed()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.redirectImages = true;
        const QUrl base = server.url(QStringLiteral("/jellyfin"));
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        origin->set(base);
        Kinefin::ImageNetworkAccess network(origin);

        std::unique_ptr<QNetworkReply> reply(network.get(QNetworkRequest(imageUrl(base))));
        QSignalSpy finished(reply.get(), &QNetworkReply::finished);
        QTRY_COMPARE(finished.size(), 1);
        // The 3xx ends in an error so the QML image loader has nothing to follow
        QVERIFY(reply->error() != QNetworkReply::NoError);
        QTest::qWait(200);
        QCOMPARE(server.imageRequests, 1);
    }

    void oversizedBodyIsAborted()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QUrl base = server.url(QStringLiteral("/jellyfin"));
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        origin->set(base);
        Kinefin::ImageNetworkAccess network(origin);
        server.imageBody = QByteArray(512 * 1024, 'x');
        network.setMaxImageBytes(1000);

        std::unique_ptr<QNetworkReply> reply(network.get(QNetworkRequest(imageUrl(base))));
        QSignalSpy finished(reply.get(), &QNetworkReply::finished);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(reply->error() != QNetworkReply::NoError);
    }

    void noCredentialsOnImageRequests()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QUrl base = server.url(QStringLiteral("/jellyfin"));
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        origin->set(base);
        Kinefin::ImageNetworkAccess network(origin);

        QNetworkRequest request(imageUrl(base));
        request.setRawHeader("Authorization", "MediaBrowser Token=\"secret\"");
        request.setRawHeader("Cookie", "session=secret");
        std::unique_ptr<QNetworkReply> reply(network.get(request));
        QSignalSpy finished(reply.get(), &QNetworkReply::finished);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QCOMPARE(reply->readAll(), kTinyPng);
        QCOMPARE(server.imageRequests, 1);
        QVERIFY(server.lastImageAuth.isEmpty());
        QVERIFY(server.lastImageCookie.isEmpty());
    }

    void taggedImageServedFromDiskCache()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QUrl base = server.url(QStringLiteral("/jellyfin"));
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        origin->set(base);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Kinefin::ImageNetworkAccess network(origin, directory.filePath(QStringLiteral("images")));

        // The fake answers Cache-Control public, max-age; the second GET is expected to be a cache hit, so the fake sees one request (the cache hit itself is unverified: it depends on Qt storing the entry before the reply finishes)
        for (int round = 0; round < 2; ++round) {
            std::unique_ptr<QNetworkReply> reply(network.get(QNetworkRequest(imageUrl(base))));
            QSignalSpy finished(reply.get(), &QNetworkReply::finished);
            QTRY_COMPARE(finished.size(), 1);
            QCOMPARE(reply->error(), QNetworkReply::NoError);
            QCOMPARE(reply->readAll(), kTinyPng);
            if (round == 0) {
                // The entry is written around the end of the reply: wait for it before the second GET
                QTRY_VERIFY(!QDir(directory.filePath(QStringLiteral("images")))
                                 .entryList(QDir::Files | QDir::AllDirs | QDir::NoDotAndDotDot)
                                 .isEmpty());
            }
        }
        QCOMPARE(server.imageRequests, 1);
    }

    void diskCacheIsPrivateAndCapped()
    {
        QCOMPARE(Kinefin::ImageNetworkAccess::kImageCacheBytes, qint64(256) * 1024 * 1024);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cachePath = directory.filePath(QStringLiteral("nested/images"));
        Kinefin::ImageNetworkAccess network(std::make_shared<Kinefin::ImageOrigin>(), cachePath);
        const QFileInfo info(cachePath);
        QVERIFY(info.isDir());
        const QFile::Permissions permissions = info.permissions();
        QVERIFY(permissions.testFlags(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner));
        QVERIFY(!(permissions
                  & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                     | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)));
        auto *cache = qobject_cast<QNetworkDiskCache *>(network.cache());
        QVERIFY(cache != nullptr);
        QCOMPARE(cache->maximumCacheSize(), Kinefin::ImageNetworkAccess::kImageCacheBytes);
    }

    void clearedOriginRefusesEverything()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QUrl base = server.url(QStringLiteral("/jellyfin"));
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        Kinefin::ImageNetworkAccess network(origin);

        // Never set: nothing is allowed
        std::unique_ptr<QNetworkReply> unset(network.get(QNetworkRequest(imageUrl(base))));
        QSignalSpy unsetFinished(unset.get(), &QNetworkReply::finished);
        QTRY_COMPARE(unsetFinished.size(), 1);
        QVERIFY(unset->error() != QNetworkReply::NoError);

        origin->set(base);
        std::unique_ptr<QNetworkReply> allowed(network.get(QNetworkRequest(imageUrl(base))));
        QSignalSpy allowedFinished(allowed.get(), &QNetworkReply::finished);
        QTRY_COMPARE(allowedFinished.size(), 1);
        QCOMPARE(allowed->error(), QNetworkReply::NoError);
        QCOMPARE(server.imageRequests, 1);

        origin->clear();
        std::unique_ptr<QNetworkReply> cleared(network.get(QNetworkRequest(imageUrl(base))));
        QSignalSpy clearedFinished(cleared.get(), &QNetworkReply::finished);
        QTRY_COMPARE(clearedFinished.size(), 1);
        QVERIFY(cleared->error() != QNetworkReply::NoError);
        QCOMPARE(server.imageRequests, 1);
    }

    void userImageIsFetchedAnonymouslyAndForeignOnesAreRefused()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        const QString withAvatar = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1");
        const QString withoutAvatar = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb2");
        server.users = {{withAvatar, "ta", QStringLiteral("Alice"), QStringLiteral("tag1")},
                        {withoutAvatar, "tb", QStringLiteral("Bob"), {}}};
        const QUrl base = server.url(QStringLiteral("/jellyfin"));
        auto origin = std::make_shared<Kinefin::ImageOrigin>();
        origin->set(base);
        Kinefin::ImageNetworkAccess network(origin);
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);

        QNetworkRequest request(client.userImageUrl(withAvatar, QStringLiteral("tag1")));
        request.setRawHeader("Authorization", "MediaBrowser Token=\"secret\"");
        std::unique_ptr<QNetworkReply> reply(network.get(request));
        QSignalSpy finished(reply.get(), &QNetworkReply::finished);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QCOMPARE(reply->readAll(), kTinyPng);
        QCOMPARE(server.userImageRequests, 1);
        QVERIFY(server.lastUserImageAuth.isEmpty());

        // A user without an avatar is a plain 404
        std::unique_ptr<QNetworkReply> missing(
            network.get(QNetworkRequest(client.userImageUrl(withoutAvatar, {}))));
        QSignalSpy missingFinished(missing.get(), &QNetworkReply::finished);
        QTRY_COMPARE(missingFinished.size(), 1);
        QCOMPARE(missing->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);

        // Another host, an extra path segment, a POST and a stray query key never leave the process
        const int served = server.requestCount;
        QUrl otherHost = client.userImageUrl(withAvatar, QStringLiteral("tag1"));
        otherHost.setHost(QStringLiteral("localhost"));
        QUrl extraPath = client.userImageUrl(withAvatar, QStringLiteral("tag1"));
        extraPath.setPath(base.path() + QStringLiteral("/UserImage/x"));
        const QUrl strayKey(client.userImageUrl(withAvatar, QStringLiteral("tag1")).toString()
                            + QStringLiteral("&api_key=secret"));
        for (const QUrl &url : {otherHost, extraPath, strayKey}) {
            std::unique_ptr<QNetworkReply> refused(network.get(QNetworkRequest(url)));
            QSignalSpy refusedFinished(refused.get(), &QNetworkReply::finished);
            QTRY_COMPARE(refusedFinished.size(), 1);
            QCOMPARE(refused->error(), QNetworkReply::ProtocolFailure);
        }
        std::unique_ptr<QNetworkReply> post(
            network.post(QNetworkRequest(client.userImageUrl(withAvatar, QStringLiteral("tag1"))),
                         QByteArray("x")));
        QSignalSpy postFinished(post.get(), &QNetworkReply::finished);
        QTRY_COMPARE(postFinished.size(), 1);
        QCOMPARE(post->error(), QNetworkReply::ProtocolFailure);
        QCOMPARE(server.requestCount, served);
        QCOMPARE(server.userImageRequests, 2);
    }
};

QTEST_GUILESS_MAIN(TestImageNetwork)
#include "test_image_network.moc"
