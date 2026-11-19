// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The download controller of a signed-in session against the fake server: the watched-state outbox, the rule scan, the manual and rule download signals, the per-owner view of the queue and the export early exits
#include "FakeJellyfin.h"
#include "app/DownloadsController.h"
#include "downloads/DownloadRules.h"
#include "downloads/DownloadStore.h"
#include "downloads/ExportNames.h"
#include "downloads/WatchedOutbox.h"
#include "settings/ProfilePaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Kinefin::DownloadRecord;
using Kinefin::DownloadRule;
using Kinefin::DownloadRules;
using Kinefin::DownloadsController;
using Kinefin::DownloadStore;
using Kinefin::WatchedEvent;
using Kinefin::WatchedOutbox;

namespace {
const QString kProfile = QStringLiteral("default");
const QString kOtherUser = QStringLiteral("cccccccccccccccccccccccccccccccc");
const QString kSeries = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
// The id of the fixture movie
const QString kMovie = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");

QString hexId(char fill, int index)
{
    return QStringLiteral("%1").arg(index, 32, 16, QLatin1Char(fill));
}

QByteArray compact(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray envelope(const QJsonArray &items)
{
    return compact(QJsonObject{{QStringLiteral("Items"), items},
                               {QStringLiteral("TotalRecordCount"), int(items.size())}});
}

// A downloadable item of the fixture `name` ("movie_item" or "episode_item"); the fake sends 64 KiB from its Download route, so the record expects that file
QJsonObject downloadable(const QString &name, const QString &id, const QString &title,
                         bool canDownload = true)
{
    QJsonObject object = QJsonDocument::fromJson(fixture(name))
                             .object()
                             .value(QStringLiteral("Items"))
                             .toArray()
                             .first()
                             .toObject();
    object.insert(QStringLiteral("Id"), id);
    object.insert(QStringLiteral("Name"), title);
    object.insert(QStringLiteral("CanDownload"), canDownload);
    QJsonArray sources = object.value(QStringLiteral("MediaSources")).toArray();
    QJsonObject source = sources.first().toObject();
    source.insert(QStringLiteral("Size"), 64 * 1024);
    source.insert(QStringLiteral("ETag"), QStringLiteral("etag-1"));
    sources.replace(0, source);
    object.insert(QStringLiteral("MediaSources"), sources);
    return object;
}

QJsonObject episodeItem(const QString &id, const QString &title, bool canDownload = true)
{
    return downloadable(QStringLiteral("episode_item"), id, title, canDownload);
}

// An entry of the Episodes listing a rule scan reads
QJsonObject listed(const QString &id, int number, const QString &created = {},
                   const QString &locationType = {})
{
    QJsonObject object{
        {QStringLiteral("Id"), id},
        {QStringLiteral("Name"), QStringLiteral("Episode %1").arg(number)},
        {QStringLiteral("Type"), QStringLiteral("Episode")},
        {QStringLiteral("ParentIndexNumber"), 1},
        {QStringLiteral("IndexNumber"), number},
        {QStringLiteral("UserData"), QJsonObject{{QStringLiteral("Played"), false}}}};
    if (!created.isEmpty())
        object.insert(QStringLiteral("DateCreated"), created);
    if (!locationType.isEmpty())
        object.insert(QStringLiteral("LocationType"), locationType);
    return object;
}

QString downloadsDirectory()
{
    return Kinefin::profileDataDirectory(kProfile) + QStringLiteral("/downloads");
}

// The files of the profile are written before the controller reads them
bool prepareDirectory()
{
    return Kinefin::ensurePrivateDirectory(downloadsDirectory());
}

DownloadRecord completed(const QString &serverId, const QString &userId, const QString &itemId,
                         qint64 position = 0)
{
    DownloadRecord record;
    record.serverId = serverId;
    record.userId = userId;
    record.itemId = itemId;
    record.name = QStringLiteral("Item %1").arg(itemId.left(4));
    record.type = QStringLiteral("Movie");
    record.container = QStringLiteral("mkv");
    record.size = 4096;
    record.bytesWritten = 4096;
    record.state = QStringLiteral("completed");
    record.positionTicks = position;
    record.addedAt = 1;
    record.updatedAt = 1;
    return record;
}

bool seedRecords(const QList<DownloadRecord> &records)
{
    if (!prepareDirectory())
        return false;
    DownloadStore store;
    store.setStoragePath(downloadsDirectory() + QStringLiteral("/index.json"));
    for (const DownloadRecord &record : records) {
        if (!store.upsert(record))
            return false;
    }
    store.flush();
    return true;
}

WatchedEvent outboxEvent(const QString &serverId, const QString &userId, const QString &itemId,
                         qint64 createdAt, bool conflict = false)
{
    WatchedEvent result;
    result.serverId = serverId;
    result.userId = userId;
    result.itemId = itemId;
    result.eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    result.createdAt = createdAt;
    result.conflict = conflict;
    return result;
}

bool seedOutbox(const QList<WatchedEvent> &events)
{
    if (!prepareDirectory())
        return false;
    WatchedOutbox outbox;
    outbox.setStoragePath(downloadsDirectory() + QStringLiteral("/outbox.json"));
    for (const WatchedEvent &entry : events) {
        if (!outbox.append(entry))
            return false;
    }
    outbox.flush();
    return true;
}

bool seedRule(const DownloadRule &rule)
{
    if (!prepareDirectory())
        return false;
    DownloadRules rules;
    rules.setStoragePath(downloadsDirectory() + QStringLiteral("/rules.json"));
    const bool ok = rules.upsert(rule);
    rules.flush();
    return ok;
}

std::optional<DownloadRule> storedRule(const DownloadRule &rule)
{
    DownloadRules rules;
    rules.setStoragePath(downloadsDirectory() + QStringLiteral("/rules.json"));
    return rules.find(rule.serverId, rule.userId, rule.seriesId);
}

// The ids a request log asked for through `ids=`
QStringList askedIds(const QList<QByteArray> &routes)
{
    QStringList ids;
    for (const QByteArray &route : routes)
        ids.append(FakeJellyfin::capturedListingQuery(route).queryItemValue(QStringLiteral("ids"),
                                                                            QUrl::FullyDecoded));
    return ids;
}

QByteArray contents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A fake server, a signed-in client and the controller on top of them; the files a test seeds go in between signIn() and open()
struct Rig {
    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    std::unique_ptr<DownloadsController> downloads;

    bool signIn() { return server.listen() && login(client, server); }

    // Online, with rule downloads allowed on a metered network, so a test does not depend on the host's connection
    void open()
    {
        downloads = std::make_unique<DownloadsController>(&client, kProfile);
        downloads->setUnmeteredOnly(false);
        downloads->setOnline(true);
    }

    QString serverId() const { return client.serverId(); }
    QString userId() const { return client.userId(); }

    void serve(const QJsonObject &item)
    {
        server.itemsById.insert(item.value(QStringLiteral("Id")).toString().toLower(),
                                compact(item));
    }

    DownloadRule rule(const QString &kind, const QString &cursorDate = {}) const
    {
        DownloadRule result;
        result.serverId = serverId();
        result.userId = userId();
        result.seriesId = kSeries;
        result.seriesName = QStringLiteral("Abyss Station");
        result.kind = kind;
        result.count = 3;
        result.cursorDate = cursorDate;
        return result;
    }
};
} // namespace

class TestDownloadsController : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void init()
    {
        m_root = std::make_unique<QTemporaryDir>();
        QVERIFY(m_root->isValid());
        Kinefin::setProfileRoot(m_root->path());
    }

    void cleanup()
    {
        Kinefin::setProfileRoot({});
        m_root.reset();
    }

    // A conflict waits for the user and must not hold the events behind it: the pending one is sent and the conflicted one is read once
    void aConflictDoesNotHoldThePendingEventBehindIt()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        const QString conflicted = hexId('a', 1);
        const QString pending = hexId('b', 2);
        QVERIFY(seedOutbox({outboxEvent(rig.serverId(), rig.userId(), conflicted, 1000),
                            outboxEvent(rig.serverId(), rig.userId(), pending, 1000)}));
        rig.serve(QJsonObject{
            {QStringLiteral("Id"), conflicted},
            {QStringLiteral("Name"), QStringLiteral("Conflicted")},
            {QStringLiteral("UserData"), QJsonObject{{QStringLiteral("Played"), false},
                                                     {QStringLiteral("LastPlayedDate"),
                                                      QStringLiteral("2099-01-01T00:00:00Z")}}}});
        rig.serve(QJsonObject{
            {QStringLiteral("Id"), pending},
            {QStringLiteral("Name"), QStringLiteral("Pending")},
            {QStringLiteral("UserData"), QJsonObject{{QStringLiteral("Played"), false}}}});
        rig.open();
        QCOMPARE(rig.downloads->pendingSyncCount(), 2);

        rig.downloads->reconcileOutbox();
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->pendingSyncCount(), 1, 10000);

        QCOMPARE(rig.server.playedWrites.size(), 1);
        QVERIFY(rig.server.playedWrites.constFirst().route.contains(pending.toLatin1()));
        const QStringList asked = askedIds(rig.server.itemsRoutes);
        QCOMPARE(asked.count(conflicted), 1);
        // The read before the write and the read that verifies it
        QCOMPARE(asked.count(pending), 2);
        const QVariantList conflicts = rig.downloads->outboxConflicts();
        QCOMPARE(conflicts.size(), 1);
        QCOMPARE(conflicts.first().toMap().value(QStringLiteral("itemId")).toString(), conflicted);
    }

    // The scan asks for DateCreated, skips Virtual episodes and keeps the cursor before an episode it could not take
    void aRefusedEpisodeKeepsTheCursorBeforeIt()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        const QString first = hexId('e', 1);
        const QString virtualEpisode = hexId('e', 2);
        const QString refused = hexId('e', 3);
        const QString last = hexId('e', 4);
        const DownloadRule rule =
            rig.rule(QStringLiteral("newEpisodes"), QStringLiteral("2020-01-01T00:00:00.000Z"));
        QVERIFY(seedRule(rule));
        rig.serve(episodeItem(refused, QStringLiteral("Refused")));
        rig.server.listingRawBody.insert(
            QStringLiteral("Episodes"),
            envelope(
                QJsonArray{listed(first, 1, QStringLiteral("2021-01-01T00:00:00.0000000Z")),
                           listed(virtualEpisode, 2, QStringLiteral("2021-02-01T00:00:00.0000000Z"),
                                  QStringLiteral("Virtual")),
                           listed(refused, 3, QStringLiteral("2021-03-01T00:00:00.0000000Z")),
                           listed(last, 4, QStringLiteral("2021-04-01T00:00:00.0000000Z"))}));
        // Items replies stay held: the manual fetch of the third episode is still in flight when the scan starts, so the scan cannot take it
        rig.server.heldFamilies.insert(QStringLiteral("Items"));
        rig.open();
        QSignalSpy revision(rig.downloads.get(), &DownloadsController::rulesRevisionChanged);
        QVERIFY(rig.downloads->downloadItem(refused));

        rig.downloads->scanRules();
        QTRY_COMPARE_WITH_TIMEOUT(revision.count(), 1, 10000);

        const QList<QByteArray> episodeRoutes =
            rig.server.listingRoutes.value(QStringLiteral("Episodes"));
        QCOMPARE(episodeRoutes.size(), 1);
        QCOMPARE(FakeJellyfin::capturedListingQuery(episodeRoutes.first())
                     .queryItemValue(QStringLiteral("fields")),
                 QStringLiteral("DateCreated"));
        const std::optional<DownloadRule> stored = storedRule(rule);
        QVERIFY(stored);
        QCOMPARE(stored->cursorDate, QStringLiteral("2021-01-01T00:00:00.000Z"));
        QCOMPARE(stored->cursorId, first);
        // The Virtual episode was never asked for; the third one only by the manual download
        QTRY_COMPARE(rig.server.itemsRoutes.size(), 3);
        QStringList asked = askedIds(rig.server.itemsRoutes);
        asked.sort();
        QCOMPARE(asked, (QStringList{first, refused, last}));
    }

    // Without a refusal the cursor moves to the newest episode handled
    void theCursorMovesToTheNewestHandledEpisode()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        const DownloadRule rule =
            rig.rule(QStringLiteral("newEpisodes"), QStringLiteral("2020-01-01T00:00:00.000Z"));
        QVERIFY(seedRule(rule));
        const QString old = hexId('e', 1);
        const QString virtualEpisode = hexId('e', 2);
        const QString newest = hexId('e', 3);
        rig.server.listingRawBody.insert(
            QStringLiteral("Episodes"),
            envelope(
                QJsonArray{listed(old, 1, QStringLiteral("2019-01-01T00:00:00.0000000Z")),
                           listed(newest, 2, QStringLiteral("2021-04-01T00:00:00.0000000Z")),
                           listed(virtualEpisode, 3, QStringLiteral("2021-05-01T00:00:00.0000000Z"),
                                  QStringLiteral("Virtual"))}));
        rig.server.heldFamilies.insert(QStringLiteral("Items"));
        rig.open();
        QSignalSpy revision(rig.downloads.get(), &DownloadsController::rulesRevisionChanged);

        rig.downloads->scanRules();
        QTRY_COMPARE_WITH_TIMEOUT(revision.count(), 1, 10000);

        const std::optional<DownloadRule> stored = storedRule(rule);
        QVERIFY(stored);
        QCOMPARE(stored->cursorDate, QStringLiteral("2021-04-01T00:00:00.000Z"));
        QCOMPARE(stored->cursorId, newest);
        QTRY_COMPARE(rig.server.itemsRoutes.size(), 1);
        QCOMPARE(askedIds(rig.server.itemsRoutes), QStringList{newest});
    }

    // The item fetch of a rule download fails after the scan moved the cursor (the server answers with other items than the one asked for, which is no download of it): the cursor goes back before that episode, so the next scan offers it again
    void aRuleDownloadThatFailsLaterRewindsTheCursor()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        const DownloadRule rule =
            rig.rule(QStringLiteral("newEpisodes"), QStringLiteral("2020-01-01T00:00:00.000Z"));
        QVERIFY(seedRule(rule));
        const QString missing = hexId('e', 1);
        rig.server.listingRawBody.insert(
            QStringLiteral("Episodes"),
            envelope(
                QJsonArray{listed(missing, 1, QStringLiteral("2021-04-01T00:00:00.0000000Z"))}));
        rig.open();

        rig.downloads->scanRules();
        QTRY_COMPARE(rig.server.itemsRoutes.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(storedRule(rule)->cursorDate,
                                  QStringLiteral("2021-03-31T23:59:59.999Z"), 10000);
        QVERIFY(storedRule(rule)->cursorId.isEmpty());
    }

    // A download the user did not ask for stays silent, whether it is queued or refused; a manual one reports both
    void ruleDownloadsAreSilentAndManualOnesAreNot()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        const QString unsupported = hexId('e', 1);
        const QString missing = hexId('e', 2);
        const QString accepted = hexId('e', 3);
        const QString manual = hexId('d', 4);
        const QString manualMissing = hexId('d', 5);
        QVERIFY(seedRule(rig.rule(QStringLiteral("nextN"))));
        rig.serve(episodeItem(unsupported, QStringLiteral("Unsupported"), false));
        rig.serve(episodeItem(accepted, QStringLiteral("Accepted")));
        rig.serve(episodeItem(manual, QStringLiteral("Manual")));
        rig.server.listingRawBody.insert(
            QStringLiteral("Episodes"),
            envelope(QJsonArray{listed(unsupported, 1), listed(missing, 2), listed(accepted, 3)}));
        rig.open();
        QSignalSpy enqueued(rig.downloads.get(), &DownloadsController::downloadEnqueued);
        QSignalSpy failed(rig.downloads.get(), &DownloadsController::downloadFailed);

        rig.downloads->scanRules();
        // The refused ones come first in the scan order, so their replies are in before the accepted download finishes
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(accepted),
                                  QStringLiteral("completed"), 10000);
        QCOMPARE(rig.server.itemsRoutes.size(), 3);
        QCOMPARE(enqueued.count(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(rig.downloads->downloadState(unsupported).isEmpty());
        QVERIFY(rig.downloads->downloadState(missing).isEmpty());
        const std::optional<DownloadRecord> record =
            rig.downloads->store()->find(rig.serverId(), rig.userId(), accepted);
        QVERIFY(record);
        QCOMPARE(record->ruleId, kSeries);

        QVERIFY(rig.downloads->downloadItem(manual));
        QTRY_COMPARE(enqueued.count(), 1);
        QCOMPARE(enqueued.first().first().toString(), manual);
        QVERIFY(rig.downloads->downloadItem(manualMissing));
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(0).toString(), manualMissing);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("notfound"));
        QCOMPARE(enqueued.count(), 1);
    }

    // Two owners with the same item id: a local playback changes the record of the owner it names
    void localProgressTouchesOnlyTheGivenOwner()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        const QString item = hexId('a', 7);
        QVERIFY(seedRecords({completed(rig.serverId(), rig.userId(), item, 5),
                             completed(rig.serverId(), kOtherUser, item, 7)}));
        rig.open();
        const auto find = [&](const QString &user) {
            return rig.downloads->store()->find(rig.serverId(), user, item);
        };

        rig.downloads->updateLocalProgress(rig.serverId(), kOtherUser, item, 123, false);
        QCOMPARE(find(kOtherUser)->positionTicks, 123);
        QVERIFY(!find(kOtherUser)->played);
        QCOMPARE(find(rig.userId())->positionTicks, 5);

        rig.downloads->updateLocalProgress(rig.serverId(), kOtherUser, item, 600000000, true);
        QCOMPARE(find(kOtherUser)->positionTicks, 0);
        QVERIFY(find(kOtherUser)->played);
        QCOMPARE(find(rig.userId())->positionTicks, 5);
        QVERIFY(!find(rig.userId())->played);
        // The watched-state event belongs to the other owner: nothing is pending for the signed-in one
        QCOMPARE(rig.downloads->pendingSyncCount(), 0);

        rig.downloads->updateLocalProgress(rig.serverId(), rig.userId(), item, 600000000, true);
        QCOMPARE(find(rig.userId())->positionTicks, 0);
        QVERIFY(find(rig.userId())->played);
        QCOMPARE(rig.downloads->pendingSyncCount(), 1);
    }

    // The page lists the records, the conflicts and the pending count of the current owner only, and follows the owner Offline shows
    void theModelAndTheOutboxFollowTheOwner()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        const QString own1 = hexId('a', 1);
        const QString own2 = hexId('a', 2);
        const QString foreign = hexId('b', 3);
        QVERIFY(seedRecords({completed(rig.serverId(), rig.userId(), own1),
                             completed(rig.serverId(), rig.userId(), own2),
                             completed(rig.serverId(), kOtherUser, foreign)}));
        QVERIFY(seedOutbox({outboxEvent(rig.serverId(), rig.userId(), own1, 1000),
                            outboxEvent(rig.serverId(), rig.userId(), own2, 1000, true),
                            outboxEvent(rig.serverId(), kOtherUser, foreign, 1000, true),
                            outboxEvent(rig.serverId(), kOtherUser, hexId('b', 4), 1000)}));
        rig.open();
        Kinefin::DownloadsModel *model = rig.downloads->model();

        QCOMPARE(model->count(), 2);
        QCOMPARE(rig.downloads->ownerUserId(), rig.userId());
        QCOMPARE(rig.downloads->pendingSyncCount(), 2);
        QVariantList conflicts = rig.downloads->outboxConflicts();
        QCOMPARE(conflicts.size(), 1);
        QCOMPARE(conflicts.first().toMap().value(QStringLiteral("itemId")).toString(), own2);

        QSignalSpy owner(rig.downloads.get(), &DownloadsController::ownerChanged);
        QSignalSpy outbox(rig.downloads.get(), &DownloadsController::outboxChanged);
        rig.downloads->setOfflineOwner(rig.serverId(), kOtherUser);
        QCOMPARE(owner.count(), 1);
        QVERIFY(outbox.count() >= 1);
        QCOMPARE(rig.downloads->ownerUserId(), kOtherUser);
        QCOMPARE(model->count(), 1);
        QCOMPARE(rig.downloads->pendingSyncCount(), 2);
        conflicts = rig.downloads->outboxConflicts();
        QCOMPARE(conflicts.size(), 1);
        QCOMPARE(conflicts.first().toMap().value(QStringLiteral("itemId")).toString(), foreign);

        rig.downloads->setOfflineOwner({}, {});
        QCOMPARE(owner.count(), 2);
        QCOMPARE(rig.downloads->ownerUserId(), rig.userId());
        QCOMPARE(model->count(), 2);
    }

    // The early exits answer exportFinished(false) at once
    void exportEarlyExitsReportFailure()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        rig.serve(downloadable(QStringLiteral("movie_item"), kMovie, QStringLiteral("Arcturus")));
        rig.open();
        QVERIFY(rig.downloads->downloadItem(kMovie));
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(kMovie), QStringLiteral("completed"),
                                  10000);
        QSignalSpy finished(rig.downloads.get(), &DownloadsController::exportFinished);
        QTemporaryDir host;
        QVERIFY(host.isValid());

        const auto exportTo = [&](const QUrl &folder) {
            const qsizetype before = finished.count();
            rig.downloads->exportDownload(rig.serverId(), rig.userId(), kMovie, folder);
            return finished.count() == before + 1 && finished.last().at(0).toString() == kMovie
                   && !finished.last().at(1).toBool();
        };
        QVERIFY(exportTo(QUrl()));
        QVERIFY(exportTo(QUrl(QStringLiteral("https://example.com/folder"))));
        QVERIFY(exportTo(QUrl::fromLocalFile(QStringLiteral("relative/folder"))));
        QVERIFY(exportTo(QUrl::fromLocalFile(host.filePath(QStringLiteral("missing")))));

        // The folder exists but the sub-folder of the layout cannot be created below it
        QVERIFY(writeFile(host.filePath(QStringLiteral("Movies")), "not a folder"));
        QVERIFY(exportTo(QUrl::fromLocalFile(host.path())));
        QVERIFY(QFileInfo(host.filePath(QStringLiteral("Movies"))).isFile());
        QCOMPARE(rig.downloads->downloadState(kMovie), QStringLiteral("completed"));

        // A failed early exit leaves the export slot free
        QVERIFY(QFile::remove(host.filePath(QStringLiteral("Movies"))));
        rig.downloads->exportDownload(rig.serverId(), rig.userId(), kMovie,
                                      QUrl::fromLocalFile(host.path()));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, 10000);
        QVERIFY(finished.last().at(1).toBool());
        QCOMPARE(rig.downloads->downloadState(kMovie), QStringLiteral("exported"));
    }

    // A Kodi sidecar that is already in the folder is never overwritten: the export takes the next free name
    void anExistingNfoIsLeftUntouched()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        rig.serve(downloadable(QStringLiteral("movie_item"), kMovie, QStringLiteral("Arcturus")));
        rig.open();
        rig.downloads->setKodiSidecars(true);
        QVERIFY(rig.downloads->downloadItem(kMovie));
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(kMovie), QStringLiteral("completed"),
                                  10000);
        const std::optional<DownloadRecord> record =
            rig.downloads->store()->find(rig.serverId(), rig.userId(), kMovie);
        QVERIFY(record);
        const QFileInfo relative(Kinefin::ExportNames::relativePath(*record));
        QTemporaryDir host;
        QVERIFY(host.isValid());
        const QString directory = QDir(host.path()).filePath(relative.path());
        QVERIFY(QDir().mkpath(directory));
        const QString taken =
            directory + QLatin1Char('/') + relative.completeBaseName() + QStringLiteral(".nfo");
        QVERIFY(writeFile(taken, "KEEP"));

        QSignalSpy finished(rig.downloads.get(), &DownloadsController::exportFinished);
        rig.downloads->exportDownload(rig.serverId(), rig.userId(), kMovie,
                                      QUrl::fromLocalFile(host.path()));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(finished.first().at(1).toBool());

        QCOMPARE(contents(taken), QByteArray("KEEP"));
        const QString second =
            directory + QLatin1Char('/') + relative.completeBaseName() + QStringLiteral(" (2)");
        QVERIFY(QFileInfo::exists(second + QLatin1Char('.') + relative.suffix()));
        QVERIFY(QFileInfo::exists(second + QStringLiteral(".nfo")));
        QVERIFY(!QFileInfo::exists(directory + QLatin1Char('/') + relative.completeBaseName()
                                   + QLatin1Char('.') + relative.suffix()));
    }

  private:
    static bool writeFile(const QString &path, const QByteArray &data)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    }

    std::unique_ptr<QTemporaryDir> m_root;
};

QTEST_GUILESS_MAIN(TestDownloadsController)
#include "test_downloads_controller.moc"
