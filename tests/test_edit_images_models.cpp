// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The models behind the Edit Images pages: the images an item has now, and what the providers offer for it
#include "FakeJellyfin.h"

#include "models/EditImagesModel.h"
#include "models/RemoteImagesModel.h"

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QUrlQuery>
#include <QtTest/QtTest>

#include <memory>

using Kinefin::EditImagesModel;
using Kinefin::RemoteImagesModel;

namespace {
struct Session {
    explicit Session(bool administrator = true)
    {
        server.administrator = administrator;
        ok = server.listen() && login(client, server);
    }

    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    bool ok = false;
};

const QString kOtherItem = QStringLiteral("aaaa0000aaaa0000aaaa0000aaaa0001");

QString typeAt(const QAbstractItemModel &model, int row)
{
    return model.data(model.index(row, 0), EditImagesModel::ImageTypeRole).toString();
}

QVariantList imagesAt(const QAbstractItemModel &model, int row)
{
    return model.data(model.index(row, 0), EditImagesModel::ImagesRole).toList();
}

QVariantMap imageOf(const QAbstractItemModel &model, int row, int position)
{
    return imagesAt(model, row).at(position).toMap();
}

QVariant offer(const RemoteImagesModel &model, int row, int role)
{
    return model.data(model.index(row, 0), role);
}

// The Download request line a fake saw for `itemId`, `type` and the full-size `url`
QByteArray downloadRoute(const QString &itemId, const QString &type, const QString &url)
{
    return "/jellyfin/Items/" + itemId.toLatin1() + "/RemoteImages/Download?type=" + type.toLatin1()
           + "&imageUrl=" + QUrl::toPercentEncoding(url);
}
} // namespace

class TestEditImagesModels : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void imagesAreGroupedByType()
    {
        Session s;
        QVERIFY(s.ok);
        EditImagesModel model(&s.client, kPlayItem, 91100);
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(model.status(), EditImagesModel::Loading);
        QTRY_COMPARE(model.status(), EditImagesModel::Ready);
        // The seven usual types always, then the others the item has images of
        QStringList types;
        for (int row = 0; row < model.rowCount(); ++row)
            types.append(typeAt(model, row));
        QCOMPARE(types, (QStringList{QStringLiteral("Primary"), QStringLiteral("Backdrop"),
                                     QStringLiteral("Logo"), QStringLiteral("Banner"),
                                     QStringLiteral("Thumb"), QStringLiteral("Art"),
                                     QStringLiteral("Disc"), QStringLiteral("Box")}));
        QCOMPARE(model.imageCount(), 6);
        QCOMPARE(imagesAt(model, 0).size(), 1);
        QCOMPARE(imagesAt(model, 1).size(), 3);
        QCOMPARE(imagesAt(model, 3).size(), 0);
        QVERIFY(!model.data(model.index(0, 0), EditImagesModel::MultipleRole).toBool());
        QVERIFY(model.data(model.index(1, 0), EditImagesModel::MultipleRole).toBool());
        QCOMPARE(model.rowCount(model.index(0, 0)), 0);

        // A choice replaces the image of a type that holds one, adds to a type with several, and fills an empty one
        QVERIFY(model.replaces(QStringLiteral("Primary")));
        QVERIFY(model.replaces(QStringLiteral("Logo")));
        QVERIFY(!model.replaces(QStringLiteral("Backdrop")));
        QVERIFY(!model.replaces(QStringLiteral("Banner")));
        QVERIFY(!model.replaces(QStringLiteral("NotAType")));

        // The images of a type with several come in index order, each naming its neighbours
        QCOMPARE(imageOf(model, 1, 0).value(QStringLiteral("imageIndex")).toInt(), 0);
        QCOMPARE(imageOf(model, 1, 0).value(QStringLiteral("previousIndex")).toInt(), -1);
        QCOMPARE(imageOf(model, 1, 0).value(QStringLiteral("nextIndex")).toInt(), 1);
        QCOMPARE(imageOf(model, 1, 1).value(QStringLiteral("previousIndex")).toInt(), 0);
        QCOMPARE(imageOf(model, 1, 1).value(QStringLiteral("nextIndex")).toInt(), 2);
        QCOMPARE(imageOf(model, 1, 2).value(QStringLiteral("previousIndex")).toInt(), 1);
        QCOMPARE(imageOf(model, 1, 2).value(QStringLiteral("nextIndex")).toInt(), -1);
        QCOMPARE(imageOf(model, 0, 0).value(QStringLiteral("imageIndex")).toInt(), -1);
        QCOMPARE(imageOf(model, 0, 0).value(QStringLiteral("width")).toInt(), 2000);

        // A thumbnail is a URL the client's builder made, for the image's own index and tag
        const QUrl thumbnail = imageOf(model, 1, 1).value(QStringLiteral("thumbnailUrl")).toUrl();
        QVERIFY(Kinefin::JellyfinClient::isServerImageUrl(s.client.serverUrl(), thumbnail));
        const QUrlQuery query(thumbnail);
        QCOMPARE(query.queryItemValue(QStringLiteral("imageIndex")), QStringLiteral("1"));
        QCOMPARE(query.queryItemValue(QStringLiteral("tag")),
                 QStringLiteral("bbbb1111bbbb1111bbbb1111bbbb1111"));
        QVERIFY(thumbnail.path().endsWith(QStringLiteral("/Images/Backdrop")));
        QCOMPARE(s.server.itemImageRoutes.size(), 1);
    }

    void aWriteOfThisItemReadsTheListAgain()
    {
        Session s;
        QVERIFY(s.ok);
        EditImagesModel model(&s.client, kPlayItem, 91101);
        QTRY_COMPARE(model.status(), EditImagesModel::Ready);
        QCOMPARE(s.server.itemImageRoutes.size(), 1);
        QVERIFY(s.client.deleteImage(kPlayItem, QStringLiteral("Primary"), -1));
        QTRY_COMPARE(s.server.itemImageRoutes.size(), 2);
        QTRY_COMPARE(model.status(), EditImagesModel::Ready);
        // The same item spelled with dashes is the same item; another item is not
        QVERIFY(s.client.deleteImage(QStringLiteral("c7dd3b1a-c7dd-3b1a-9d42-f2ab74f8a1b9"),
                                     QStringLiteral("Logo"), -1));
        QTRY_COMPARE(s.server.itemImageRoutes.size(), 3);
        QVERIFY(s.client.deleteImage(kOtherItem, QStringLiteral("Logo"), -1));
        QTRY_COMPARE(s.server.imageDeletes.size(), 3);
        QTest::qWait(100);
        QCOMPARE(s.server.itemImageRoutes.size(), 3);
    }

    void aReloadUpdatesTheRowsInPlace()
    {
        Session s;
        QVERIFY(s.ok);
        EditImagesModel model(&s.client, kPlayItem, 91102);
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QTRY_COMPARE(model.status(), EditImagesModel::Ready);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        // One backdrop left, the same types: the rows change in place
        const QJsonArray one{
            QJsonObject{{QStringLiteral("ImageType"), QStringLiteral("Backdrop")},
                        {QStringLiteral("ImageIndex"), 0},
                        {QStringLiteral("ImageTag"), QStringLiteral("bbbb0000bbbb0000")}},
            QJsonObject{{QStringLiteral("ImageType"), QStringLiteral("Box")},
                        {QStringLiteral("ImageTag"), QStringLiteral("dddd1111")}}};
        s.server.itemImagesBody = QJsonDocument(one).toJson(QJsonDocument::Compact);
        model.reload();
        QTRY_COMPARE(model.imageCount(), 2);
        QTRY_COMPARE(model.status(), EditImagesModel::Ready);
        QCOMPARE(resets.size(), 0);
        QVERIFY(changes.size() >= 1);
        QCOMPARE(imagesAt(model, 1).size(), 1);
        QCOMPARE(imagesAt(model, 0).size(), 0);
        // A type that was not there joins: the list is built again
        const QJsonArray more{
            QJsonObject{{QStringLiteral("ImageType"), QStringLiteral("Screenshot")},
                        {QStringLiteral("ImageIndex"), 0},
                        {QStringLiteral("ImageTag"), QStringLiteral("ssss")}}};
        s.server.itemImagesBody = QJsonDocument(more).toJson(QJsonDocument::Compact);
        model.reload();
        QTRY_COMPARE(resets.size(), 1);
        QCOMPARE(model.rowCount(), 8);
        QCOMPARE(typeAt(model, 7), QStringLiteral("Screenshot"));
        QCOMPARE(model.imageCount(), 1);
    }

    void imagesWithoutATagHaveNoThumbnail()
    {
        Session s;
        QVERIFY(s.ok);
        const QJsonArray images{
            QJsonObject{{QStringLiteral("ImageType"), QStringLiteral("Primary")}}};
        s.server.itemImagesBody = QJsonDocument(images).toJson(QJsonDocument::Compact);
        EditImagesModel model(&s.client, kPlayItem, 91103);
        QTRY_COMPARE(model.status(), EditImagesModel::Ready);
        QCOMPARE(model.imageCount(), 1);
        QVERIFY(imageOf(model, 0, 0).value(QStringLiteral("thumbnailUrl")).toUrl().isEmpty());
    }

    void aFailedReadKeepsWhatIsShown()
    {
        Session s;
        QVERIFY(s.ok);
        EditImagesModel model(&s.client, kPlayItem, 91104);
        QTRY_COMPARE(model.status(), EditImagesModel::Ready);
        s.server.itemImagesStatus = 500;
        model.reload();
        QTRY_COMPARE(model.status(), EditImagesModel::Failed);
        QCOMPARE(model.imageCount(), 6);
        QCOMPARE(model.rowCount(), 8);
        s.server.itemImagesStatus = 200;
        model.reload();
        QTRY_COMPARE(model.status(), EditImagesModel::Ready);
    }

    void aNonAdministratorGetsNothing()
    {
        Session s(false);
        QVERIFY(s.ok);
        EditImagesModel model(&s.client, kPlayItem, 91105);
        QCOMPARE(model.status(), EditImagesModel::Failed);
        QCOMPARE(model.rowCount(), 0);
        QTest::qWait(50);
        QVERIFY(s.server.itemImageRoutes.isEmpty());
    }

    void theSessionEndingEmptiesTheModel()
    {
        Session s;
        QVERIFY(s.ok);
        EditImagesModel model(&s.client, kPlayItem, 91106);
        QTRY_COMPARE(model.status(), EditImagesModel::Ready);
        s.client.forgetSession();
        QCOMPARE(model.status(), EditImagesModel::Idle);
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.imageCount(), 0);
    }

    void aModelThatGoesAwayLeavesNoRequestToAnswer()
    {
        Session s;
        QVERIFY(s.ok);
        QSignalSpy failures(&s.client, &Kinefin::JellyfinClient::listingFailed);
        QSignalSpy ready(&s.client, &Kinefin::JellyfinClient::itemImagesReady);
        s.server.holdItemImages = true;
        auto model = std::make_unique<EditImagesModel>(&s.client, kPlayItem, 91107);
        QTRY_COMPARE(s.server.itemImageRoutes.size(), 1);
        model.reset();
        s.server.holdItemImages = false;
        s.server.releaseHeld();
        QTest::qWait(150);
        QCOMPARE(ready.size(), 0);
        QCOMPARE(failures.size(), 0);
    }

    void theOnlineListStartsWithTheProvidersAndTheFirstPage()
    {
        Session s;
        QVERIFY(s.ok);
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Primary"), 91110);
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(model.status(), RemoteImagesModel::Loading);
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QTRY_COMPARE(model.providersStatus(), RemoteImagesModel::Ready);
        QCOMPARE(model.rowCount(), 5);
        QCOMPARE(model.imageType(), QStringLiteral("Primary"));
        // The types any provider offers, in the editor's order, and the providers of the current one
        QCOMPARE(
            model.imageTypes(),
            (QStringList{QStringLiteral("Primary"), QStringLiteral("Backdrop"),
                         QStringLiteral("Logo"), QStringLiteral("Banner"), QStringLiteral("Thumb"),
                         QStringLiteral("Art"), QStringLiteral("Disc")}));
        QCOMPARE(model.providerNames(),
                 (QStringList{QStringLiteral("TheMovieDb"), QStringLiteral("Fanart")}));
        QCOMPARE(model.total(), 57);
        QCOMPARE(model.firstShown(), 1);
        QCOMPARE(model.lastShown(), RemoteImagesModel::PageSize);
        QVERIFY(model.hasNext());
        QVERIFY(!model.hasPrevious());
        QCOMPARE(offer(model, 0, RemoteImagesModel::ProviderRole).toString(),
                 QStringLiteral("TheMovieDb"));
        QCOMPARE(offer(model, 0, RemoteImagesModel::WidthRole).toInt(), 2000);
        QCOMPARE(offer(model, 0, RemoteImagesModel::HeightRole).toInt(), 3000);
        QCOMPARE(offer(model, 0, RemoteImagesModel::LanguageRole).toString(), QStringLiteral("en"));
        QCOMPARE(offer(model, 0, RemoteImagesModel::RatingRole).toDouble(), 5.4);
        QCOMPARE(offer(model, 0, RemoteImagesModel::VoteCountRole).toInt(), 12);
        QCOMPARE(offer(model, 0, RemoteImagesModel::RatingTypeRole).toString(),
                 QStringLiteral("Score"));
        // Only a provider's own https preview is a thumbnail
        QCOMPARE(offer(model, 0, RemoteImagesModel::ThumbnailUrlRole).toUrl(),
                 QUrl(QStringLiteral("https://image.tmdb.org/t/p/w154/poster-a.jpg")));
        QVERIFY(offer(model, 1, RemoteImagesModel::ThumbnailUrlRole).toUrl().isEmpty());
        QVERIFY(offer(model, 2, RemoteImagesModel::ThumbnailUrlRole).toUrl().isEmpty());
        QVERIFY(!offer(model, 3, RemoteImagesModel::ThumbnailUrlRole).toUrl().isEmpty());
        // The model offers no way to the full-size URL
        const QHash<int, QByteArray> roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            QVERIFY2(!it.value().toLower().contains("url") || it.value() == "thumbnailUrl",
                     it.value().constData());
        QCOMPARE(s.server.remoteImageRoutes.size(), 1);
        const QByteArray route = s.server.remoteImageRoutes.constFirst();
        QVERIFY(route.contains("type=Primary"));
        QVERIFY(route.contains("startIndex=0"));
        QVERIFY(route.contains("limit=24"));
        QVERIFY(route.contains("includeAllLanguages=false"));
        QVERIFY(!route.contains("providerName"));
        QCOMPARE(s.server.remoteImageProviderRoutes.size(), 1);
    }

    void aChangeOfTypeProviderOrLanguageStartsOver()
    {
        Session s;
        QVERIFY(s.ok);
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Primary"), 91111);
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QTRY_COMPARE(model.providersStatus(), RemoteImagesModel::Ready);
        // The rows of another type are gone at once: no row on screen is an offer of another type
        model.setImageType(QStringLiteral("Art"));
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.status(), RemoteImagesModel::Loading);
        QCOMPARE(model.providerNames(), (QStringList{QStringLiteral("Fanart")}));
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QVERIFY(s.server.remoteImageRoutes.constLast().contains("type=Art"));
        QCOMPARE(model.rowCount(), 5);

        model.setProvider(QStringLiteral("Fanart"));
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QVERIFY(s.server.remoteImageRoutes.constLast().contains("providerName=Fanart"));
        // A provider that does not offer the type is no choice
        const qsizetype requests = s.server.remoteImageRoutes.size();
        model.setProvider(QStringLiteral("TheMovieDb"));
        model.setProvider(QString(Kinefin::MaxRemoteImageNameChars + 1, QLatin1Char('x')));
        QCOMPARE(model.provider(), QStringLiteral("Fanart"));
        QCOMPARE(s.server.remoteImageRoutes.size(), requests);
        // A type that is none is ignored
        model.setImageType(QStringLiteral("NotAType"));
        QCOMPARE(model.imageType(), QStringLiteral("Art"));

        // The provider that does not offer the new type is dropped with it
        model.setImageType(QStringLiteral("Backdrop"));
        QCOMPARE(model.provider(), QString());
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QVERIFY(!s.server.remoteImageRoutes.constLast().contains("providerName"));
        QVERIFY(s.server.remoteImageRoutes.constLast().contains("type=Backdrop"));

        model.setAllLanguages(true);
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QVERIFY(s.server.remoteImageRoutes.constLast().contains("includeAllLanguages=true"));
        QVERIFY(model.allLanguages());
    }

    void pagesMoveByThePageSize()
    {
        Session s;
        QVERIFY(s.ok);
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Primary"), 91112);
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        model.previousPage();
        QCOMPARE(s.server.remoteImageRoutes.size(), 1);
        model.nextPage();
        // One request at a time: the page is asked for once
        model.nextPage();
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QCOMPARE(s.server.remoteImageRoutes.size(), 2);
        QVERIFY(s.server.remoteImageRoutes.constLast().contains("startIndex=24"));
        QCOMPARE(model.firstShown(), 25);
        QCOMPARE(model.lastShown(), 48);
        QVERIFY(model.hasPrevious());
        QVERIFY(model.hasNext());
        model.nextPage();
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QVERIFY(s.server.remoteImageRoutes.constLast().contains("startIndex=48"));
        QCOMPARE(model.firstShown(), 49);
        QCOMPARE(model.lastShown(), 57);
        QVERIFY(!model.hasNext());
        model.nextPage();
        QCOMPARE(s.server.remoteImageRoutes.size(), 3);
        model.previousPage();
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QVERIFY(s.server.remoteImageRoutes.constLast().contains("startIndex=24"));
    }

    void downloadingSendsTheOfferAndSaysWhenItIsDone()
    {
        Session s;
        QVERIFY(s.ok);
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Primary"), 91113);
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QSignalSpy done(&model, &RemoteImagesModel::downloaded);
        QSignalSpy states(&model, &RemoteImagesModel::downloadStateChanged);
        QVERIFY(!model.download(-1));
        QVERIFY(!model.download(model.rowCount()));
        QCOMPARE(model.downloadState(), RemoteImagesModel::NoDownload);
        QVERIFY(model.download(0));
        QCOMPARE(model.downloadState(), RemoteImagesModel::Downloading);
        // One at a time
        QVERIFY(!model.download(1));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(model.downloadState(), RemoteImagesModel::NoDownload);
        QCOMPARE(states.size(), 2);
        QCOMPARE(s.server.remoteImageDownloads.size(), 1);
        QCOMPARE(s.server.remoteImageDownloads.constFirst(),
                 downloadRoute(kPlayItem, QStringLiteral("Primary"),
                               QStringLiteral("https://image.tmdb.org/t/p/original/poster-a.jpg")));
        // The page goes on: another image of the same type can follow
        QVERIFY(model.download(3));
        QTRY_COMPARE(done.size(), 2);
    }

    void aFailedDownloadIsReportedAndCanBeTriedAgain()
    {
        Session s;
        QVERIFY(s.ok);
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Primary"), 91114);
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QSignalSpy done(&model, &RemoteImagesModel::downloaded);
        s.server.remoteImageDownloadStatus = 500;
        QVERIFY(model.download(0));
        QTRY_COMPARE(model.downloadState(), RemoteImagesModel::DownloadFailed);
        QCOMPARE(model.downloadStatus(), 500);
        QCOMPARE(done.size(), 0);
        // The page still lists the offers, and says so only until it is dismissed
        QCOMPARE(model.rowCount(), 5);
        model.dismissDownloadError();
        QCOMPARE(model.downloadState(), RemoteImagesModel::NoDownload);
        s.server.remoteImageDownloadStatus = 204;
        QVERIFY(model.download(0));
        QTRY_COMPARE(done.size(), 1);
    }

    void aDownloadAnotherWriteMadeIsNotOurs()
    {
        Session s;
        QVERIFY(s.ok);
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Primary"), 91115);
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QSignalSpy done(&model, &RemoteImagesModel::downloaded);
        // Writes nobody here asked for do not look like the end of a download
        Q_EMIT s.client.itemImageUpdated(kPlayItem, QStringLiteral("Primary"), -1);
        Q_EMIT s.client.itemImageUpdateFailed(kPlayItem, QStringLiteral("Primary"), -1, 500);
        QCOMPARE(model.downloadState(), RemoteImagesModel::NoDownload);
        s.server.remoteImageDownloadStatus = 500;
        QVERIFY(model.download(0));
        Q_EMIT s.client.itemImageUpdated(kOtherItem, QStringLiteral("Primary"), -1);
        Q_EMIT s.client.itemImageUpdated(kPlayItem, QStringLiteral("Logo"), -1);
        Q_EMIT s.client.itemImageUpdated(kPlayItem, QStringLiteral("Primary"), 0);
        QCOMPARE(model.downloadState(), RemoteImagesModel::Downloading);
        QCOMPARE(done.size(), 0);
        QTRY_COMPARE(model.downloadState(), RemoteImagesModel::DownloadFailed);
    }

    void failedReadsFallBackAndCanBeRetried()
    {
        Session s;
        QVERIFY(s.ok);
        s.server.remoteImagesStatus = 500;
        s.server.remoteImageProvidersStatus = 500;
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Art"), 91116);
        QTRY_COMPARE(model.status(), RemoteImagesModel::Failed);
        QTRY_COMPARE(model.providersStatus(), RemoteImagesModel::Failed);
        QCOMPARE(model.rowCount(), 0);
        // Without the providers the selectors offer the usual types (and the one asked for) and no provider
        QVERIFY(model.imageTypes().contains(QStringLiteral("Primary")));
        QVERIFY(model.imageTypes().contains(QStringLiteral("Art")));
        QVERIFY(!model.imageTypes().contains(QStringLiteral("Box")));
        QVERIFY(model.providerNames().isEmpty());
        QVERIFY(!model.download(0));
        s.server.remoteImagesStatus = 200;
        s.server.remoteImageProvidersStatus = 200;
        model.reload();
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QTRY_COMPARE(model.providersStatus(), RemoteImagesModel::Ready);
        QCOMPARE(model.rowCount(), 5);
    }

    void aTypeThatIsNoneStartsAsPrimary()
    {
        Session s;
        QVERIFY(s.ok);
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Chapter"), 91117);
        QCOMPARE(model.imageType(), QStringLiteral("Primary"));
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        QVERIFY(s.server.remoteImageRoutes.constFirst().contains("type=Primary"));
    }

    void aThumbnailOffAProviderDomainNeverReachesThePage()
    {
        Session s;
        QVERIFY(s.ok);
        s.server.holdRemoteImages = true;
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Primary"), 91118);
        QTRY_COMPARE(s.server.remoteImageRoutes.size(), 1);
        // An answer the client never made (it checks every preview itself): the model checks again
        const auto image = [](quint32 id, const QString &thumbnail) {
            return QVariantMap{{QStringLiteral("offerId"), id},
                               {QStringLiteral("thumbnailUrl"), thumbnail},
                               {QStringLiteral("provider"), QStringLiteral("<b>Hostile</b>")}};
        };
        const QVariantList images{
            image(900001, QStringLiteral("https://image.tmdb.org/t/p/w154/a.jpg")),
            image(900002, QStringLiteral("https://tracker.example.invalid/pixel.png")),
            image(900003, QStringLiteral("file:///etc/passwd")),
            image(900004, QStringLiteral("http://image.tmdb.org/t/p/w154/a.jpg")),
            image(900005, QStringLiteral("https://image.tmdb.org.evil.example/a.jpg")),
            image(900006, QStringLiteral("https://user:pw@image.tmdb.org/a.jpg")),
            image(0, QStringLiteral("https://image.tmdb.org/t/p/w154/b.jpg"))};
        Q_EMIT s.client.remoteImagesReady(kPlayItem, QStringLiteral("Primary"), 0, images, 7, {},
                                          91118);
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        // The offer without an id is dropped, and only the first thumbnail survives
        QCOMPARE(model.rowCount(), 6);
        QVERIFY(!offer(model, 0, RemoteImagesModel::ThumbnailUrlRole).toUrl().isEmpty());
        for (int row = 1; row < model.rowCount(); ++row)
            QVERIFY2(offer(model, row, RemoteImagesModel::ThumbnailUrlRole).toUrl().isEmpty(),
                     qPrintable(QString::number(row)));
        // The provider's name stays a plain string for the page to show as text
        QCOMPARE(offer(model, 0, RemoteImagesModel::ProviderRole).toString(),
                 QStringLiteral("<b>Hostile</b>"));
        // The client never minted these offers: nothing goes to the server for them
        QVERIFY(!model.download(0));
        QCOMPARE(model.downloadState(), RemoteImagesModel::DownloadFailed);
        QCOMPARE(model.downloadStatus(), 0);
        QTest::qWait(100);
        QVERIFY(s.server.remoteImageDownloads.isEmpty());
        s.server.holdRemoteImages = false;
        s.server.releaseHeld();
    }

    void anAnswerForAnotherChannelOrTypeIsIgnored()
    {
        Session s;
        QVERIFY(s.ok);
        s.server.holdRemoteImages = true;
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Primary"), 91119);
        QTRY_COMPARE(s.server.remoteImageRoutes.size(), 1);
        const QVariantList images{QVariantMap{{QStringLiteral("offerId"), 900010u}}};
        Q_EMIT s.client.remoteImagesReady(kPlayItem, QStringLiteral("Primary"), 0, images, 1, {},
                                          91999);
        Q_EMIT s.client.remoteImagesReady(kPlayItem, QStringLiteral("Backdrop"), 0, images, 1, {},
                                          91119);
        Q_EMIT s.client.remoteImagesReady(kPlayItem, QStringLiteral("Primary"), 24, images, 1, {},
                                          91119);
        Q_EMIT s.client.remoteImagesReady(kOtherItem, QStringLiteral("Primary"), 0, images, 1, {},
                                          91119);
        QCOMPARE(model.status(), RemoteImagesModel::Loading);
        QCOMPARE(model.rowCount(), 0);
        Q_EMIT s.client.remoteImagesReady(kPlayItem, QStringLiteral("Primary"), 0, images, 1, {},
                                          91119);
        QCOMPARE(model.status(), RemoteImagesModel::Ready);
        QCOMPARE(model.rowCount(), 1);
        s.server.holdRemoteImages = false;
        s.server.releaseHeld();
    }

    void theSessionEndingEmptiesTheOnlineList()
    {
        Session s;
        QVERIFY(s.ok);
        RemoteImagesModel model(&s.client, kPlayItem, QStringLiteral("Primary"), 91120);
        QTRY_COMPARE(model.status(), RemoteImagesModel::Ready);
        s.client.forgetSession();
        QCOMPARE(model.status(), RemoteImagesModel::Idle);
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.total(), 0);
        QVERIFY(!model.download(0));
    }
};

QTEST_GUILESS_MAIN(TestEditImagesModels)
#include "test_edit_images_models.moc"
