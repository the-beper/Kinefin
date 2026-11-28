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

// The id of the media source of the fixture movie
const QString kSource = QStringLiteral("ms-arcturus-0001");

QJsonObject subtitleStream(int index, const QString &codec, const QString &language,
                           const QString &title, bool external = true, bool text = true)
{
    return {{QStringLiteral("Type"), QStringLiteral("Subtitle")},
            {QStringLiteral("Index"), index},
            {QStringLiteral("Codec"), codec},
            {QStringLiteral("Language"), language},
            {QStringLiteral("DisplayTitle"), title},
            {QStringLiteral("IsExternal"), external},
            {QStringLiteral("IsTextSubtitleStream"), text}};
}

QJsonObject chapterEntry(const QString &name, qint64 ticks)
{
    return {{QStringLiteral("Name"), name}, {QStringLiteral("StartPositionTicks"), ticks}};
}

// `item` with the streams of its first media source (whose id is kSource) and its chapters replaced
QJsonObject withExtras(QJsonObject item, const QJsonArray &streams, const QJsonArray &chapters,
                       const QString &sourceId = kSource)
{
    QJsonArray sources = item.value(QStringLiteral("MediaSources")).toArray();
    QJsonObject source = sources.first().toObject();
    source.insert(QStringLiteral("Id"), sourceId);
    source.insert(QStringLiteral("MediaStreams"), streams);
    sources.replace(0, source);
    item.insert(QStringLiteral("MediaSources"), sources);
    item.insert(QStringLiteral("Chapters"), chapters);
    return item;
}

QJsonObject infoOf(const QString &folder)
{
    return QJsonDocument::fromJson(contents(folder + QStringLiteral("/info.json"))).object();
}

bool infoHas(const QString &folder, const QString &key)
{
    return infoOf(folder).contains(key);
}

// A fake server, a signed-in client and the controller on top of them; the files a test seeds go in between signIn() and open()
struct Rig {
    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    std::unique_ptr<DownloadsController> downloads;

    bool signIn() { return server.listen() && login(client, server); }

    void open()
    {
        downloads = std::make_unique<DownloadsController>(&client, kProfile);
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

// The folder of a completed download, empty when it is not playable
QString itemFolder(Rig &rig, const QString &itemId)
{
    const QString path = rig.downloads->playableDownload(rig.serverId(), rig.userId(), itemId)
                             .value(QStringLiteral("path"))
                             .toString();
    return path.isEmpty() ? QString() : QFileInfo(path).absolutePath();
}
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
        QCOMPARE(FakeJellyfin::capturedListingQuery(episodeRoutes.first())
                     .queryItemValue(QStringLiteral("isMissing")),
                 QStringLiteral("false"));
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
    void aPausedRuleScansNothingAndTakesUpFromNowWhenSwitchedOn()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        DownloadRule rule =
            rig.rule(QStringLiteral("newEpisodes"), QStringLiteral("2020-01-01T00:00:00.000Z"));
        rule.cursorId = hexId('e', 9);
        QVERIFY(seedRule(rule));
        const QString added = hexId('e', 3);
        rig.server.listingRawBody.insert(
            QStringLiteral("Episodes"),
            envelope(QJsonArray{listed(added, 2, QStringLiteral("2021-04-01T00:00:00.0000000Z"))}));
        rig.open();
        QVERIFY(rig.downloads->seriesRules()
                    .constFirst()
                    .toMap()
                    .value(QStringLiteral("enabled"))
                    .toBool());

        QSignalSpy revision(rig.downloads.get(), &DownloadsController::rulesRevisionChanged);
        QVERIFY(rig.downloads->setSeriesRuleEnabled(kSeries, false));
        QCOMPARE(revision.count(), 1);
        QVERIFY(!rig.downloads->seriesRules()
                     .constFirst()
                     .toMap()
                     .value(QStringLiteral("enabled"))
                     .toBool());
        QVERIFY(!rig.downloads->seriesRule(kSeries).value(QStringLiteral("enabled")).toBool());
        // Nothing is asked of the server for a paused rule
        rig.downloads->scanRules();
        QTest::qWait(300);
        QVERIFY(!rig.server.listingRoutes.contains(QStringLiteral("Episodes")));
        // The same state again is no change
        QVERIFY(rig.downloads->setSeriesRuleEnabled(kSeries, false));
        QCOMPARE(revision.count(), 1);

        // Editing a paused rule keeps it paused
        QVERIFY(rig.downloads->setSeriesRule(kSeries, QStringLiteral("Abyss Station"),
                                             QStringLiteral("newEpisodes"), -1, 2));
        QVERIFY(!rig.downloads->seriesRule(kSeries).value(QStringLiteral("enabled")).toBool());
        QVERIFY(!rig.server.listingRoutes.contains(QStringLiteral("Episodes")));

        // Switched on again: the cursor is now, so what was added meanwhile is not downloaded
        const QDateTime before = QDateTime::currentDateTimeUtc().addSecs(-1);
        QVERIFY(rig.downloads->setSeriesRuleEnabled(kSeries, true));
        const std::optional<DownloadRule> stored = storedRule(rule);
        QVERIFY(stored);
        QVERIFY(stored->enabled);
        QVERIFY(stored->cursorId.isEmpty());
        QVERIFY(QDateTime::fromString(stored->cursorDate, Qt::ISODateWithMs) >= before);
    }

    // An episode row of the Download All listing: the fixture episode with its sources, in the watched state asked for
    static QJsonObject plannedRow(int number, bool played, const QString &locationType = {},
                                  bool withSize = true)
    {
        QJsonObject row = episodeItem(hexId('e', number), QStringLiteral("Episode %1").arg(number));
        row.insert(QStringLiteral("UserData"), QJsonObject{{QStringLiteral("Played"), played}});
        if (!locationType.isEmpty())
            row.insert(QStringLiteral("LocationType"), locationType);
        if (!withSize) {
            QJsonArray sources = row.value(QStringLiteral("MediaSources")).toArray();
            QJsonObject source = sources.first().toObject();
            source.remove(QStringLiteral("Size"));
            sources.replace(0, source);
            row.insert(QStringLiteral("MediaSources"), sources);
        }
        return row;
    }

    void downloadAllCountsWhatABatchWouldQueueAndQueuesIt()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        rig.server.listingRawBody.insert(
            QStringLiteral("Items"),
            envelope(QJsonArray{plannedRow(1, false), plannedRow(2, true),
                                plannedRow(3, false, QStringLiteral("Virtual")),
                                plannedRow(4, false, {}, false)}));
        rig.open();
        QSignalSpy changed(rig.downloads.get(), &DownloadsController::downloadAllPlanChanged);
        QVERIFY(rig.downloads->planDownloadAll(kSeries, true));
        QCOMPARE(rig.downloads->downloadAllPlan().value(QStringLiteral("state")).toString(),
                 QStringLiteral("reading"));
        QTRY_COMPARE(rig.downloads->downloadAllPlan().value(QStringLiteral("state")).toString(),
                     QStringLiteral("ready"));
        // A series is read recursively, 25 rows at a time, for the sources and the download permission
        const QUrlQuery query =
            FakeJellyfin::capturedListingQuery(rig.server.itemsRoutes.constFirst());
        QCOMPARE(query.queryItemValue(QStringLiteral("parentId")), kSeries);
        QCOMPARE(query.queryItemValue(QStringLiteral("recursive")), QStringLiteral("true"));
        QCOMPARE(query.queryItemValue(QStringLiteral("includeItemTypes")),
                 QStringLiteral("Episode"));
        QCOMPARE(query.queryItemValue(QStringLiteral("limit")), QStringLiteral("25"));
        // The server's virtual episodes (no file) are asked away, not only skipped afterwards
        QCOMPARE(query.queryItemValue(QStringLiteral("isMissing")), QStringLiteral("false"));
        QVERIFY(query.queryItemValue(QStringLiteral("fields"))
                    .contains(QStringLiteral("MediaSources")));
        // The watched one and the one without a file stay out; the size of the last one is not known
        QVariantMap plan = rig.downloads->downloadAllPlan();
        QCOMPARE(plan.value(QStringLiteral("total")).toInt(), 4);
        QCOMPARE(plan.value(QStringLiteral("count")).toInt(), 2);
        QCOMPARE(plan.value(QStringLiteral("watchedSkipped")).toInt(), 1);
        QVERIFY(!plan.value(QStringLiteral("sizeKnown")).toBool());
        QCOMPARE(plan.value(QStringLiteral("size")).toLongLong(), qint64(64 * 1024));
        // Counting the watched one too is no request
        const int reads = rig.server.itemsRoutes.size();
        rig.downloads->setDownloadAllUnwatchedOnly(false);
        plan = rig.downloads->downloadAllPlan();
        QCOMPARE(plan.value(QStringLiteral("count")).toInt(), 3);
        QCOMPARE(plan.value(QStringLiteral("watchedSkipped")).toInt(), 0);
        QCOMPARE(rig.server.itemsRoutes.size(), reads);
        QVERIFY(changed.size() >= 3);

        // Starting queues them from the rows already read, in order, with no second request each
        QCOMPARE(rig.downloads->startDownloadAll(), 3);
        QCOMPARE(rig.server.itemsRoutes.size(), reads);
        QCOMPARE(rig.downloads->downloadAllPlan().value(QStringLiteral("state")).toString(),
                 QStringLiteral("idle"));
        QVERIFY(!rig.downloads->downloadState(hexId('e', 1)).isEmpty());
        QVERIFY(!rig.downloads->downloadState(hexId('e', 2)).isEmpty());
        QVERIFY(!rig.downloads->downloadState(hexId('e', 4)).isEmpty());
        QVERIFY(rig.downloads->downloadState(hexId('e', 3)).isEmpty());

        // What has a record is left out of the next plan
        QVERIFY(rig.downloads->planDownloadAll(kSeries, true));
        QTRY_COMPARE(rig.downloads->downloadAllPlan().value(QStringLiteral("state")).toString(),
                     QStringLiteral("ready"));
        plan = rig.downloads->downloadAllPlan();
        QCOMPARE(plan.value(QStringLiteral("alreadyDownloaded")).toInt(), 3);
        QCOMPARE(plan.value(QStringLiteral("count")).toInt(), 0);
        QCOMPARE(rig.downloads->startDownloadAll(), 0);
    }

    void downloadAllOfASeasonIsNotRecursiveAndAFailureOrACancelEndsThePlan()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        rig.server.listingRawBody.insert(QStringLiteral("Items"),
                                         envelope(QJsonArray{plannedRow(1, false)}));
        rig.open();
        QVERIFY(rig.downloads->planDownloadAll(kSeries, false));
        QTRY_COMPARE(rig.downloads->downloadAllPlan().value(QStringLiteral("state")).toString(),
                     QStringLiteral("ready"));
        const QUrlQuery query =
            FakeJellyfin::capturedListingQuery(rig.server.itemsRoutes.constFirst());
        QVERIFY(!query.hasQueryItem(QStringLiteral("recursive")));
        QCOMPARE(rig.downloads->downloadAllPlan().value(QStringLiteral("sizeKnown")).toBool(),
                 true);
        rig.downloads->cancelDownloadAll();
        QCOMPARE(rig.downloads->downloadAllPlan().value(QStringLiteral("state")).toString(),
                 QStringLiteral("idle"));
        QCOMPARE(rig.downloads->startDownloadAll(), 0);

        // A bad id or a refusing server ends in a failure that the dialog can name
        QVERIFY(!rig.downloads->planDownloadAll(QStringLiteral("../x"), true));
        rig.server.listingStatus.insert(QStringLiteral("Items"), 500);
        QVERIFY(rig.downloads->planDownloadAll(kSeries, true));
        QTRY_COMPARE(rig.downloads->downloadAllPlan().value(QStringLiteral("state")).toString(),
                     QStringLiteral("failed"));
        QCOMPARE(rig.downloads->startDownloadAll(), 0);
    }

    // A server that claims more rows than it sends must not keep the reading going
    void downloadAllStopsWhenAPageAddsNothing()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        rig.server.listingRawBody.insert(
            QStringLiteral("Items"),
            compact(QJsonObject{{QStringLiteral("Items"), QJsonArray{0, QStringLiteral("x")}},
                                {QStringLiteral("TotalRecordCount"), 100000}}));
        rig.open();
        QVERIFY(rig.downloads->planDownloadAll(kSeries, true));
        QTRY_COMPARE(rig.downloads->downloadAllPlan().value(QStringLiteral("state")).toString(),
                     QStringLiteral("ready"));
        QCOMPARE(rig.downloads->downloadAllPlan().value(QStringLiteral("total")).toInt(), 0);
        QCOMPARE(rig.server.itemsRoutes.size(), 1);
    }

    void downloadAllStopsAtItsLimitAndSaysSo()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        // Pages of the same rows, a series that says it has far more
        QJsonArray page;
        for (int i = 1; i <= DownloadsController::DownloadAllPage; ++i)
            page.append(plannedRow(i, false));
        rig.server.listingRawBody.insert(
            QStringLiteral("Items"),
            compact(QJsonObject{{QStringLiteral("Items"), page},
                                {QStringLiteral("TotalRecordCount"), 5000}}));
        rig.open();
        QVERIFY(rig.downloads->planDownloadAll(kSeries, true));
        QTRY_COMPARE_WITH_TIMEOUT(
            rig.downloads->downloadAllPlan().value(QStringLiteral("state")).toString(),
            QStringLiteral("ready"), 15000);
        const QVariantMap plan = rig.downloads->downloadAllPlan();
        QVERIFY(plan.value(QStringLiteral("truncated")).toBool());
        QCOMPARE(plan.value(QStringLiteral("total")).toInt(), DownloadsController::MaxDownloadAll);
        QCOMPARE(plan.value(QStringLiteral("limit")).toInt(), DownloadsController::MaxDownloadAll);
        QCOMPARE(rig.server.itemsRoutes.size(),
                 DownloadsController::MaxDownloadAll / DownloadsController::DownloadAllPage);
    }

    void aRuleOfAnotherSeriesCannotBeSwitched()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        QVERIFY(seedRule(rig.rule(QStringLiteral("nextN"))));
        rig.open();
        QVERIFY(!rig.downloads->setSeriesRuleEnabled(hexId('a', 5), false));
        QVERIFY(rig.downloads->seriesRule(kSeries).value(QStringLiteral("enabled")).toBool());
    }

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
        // The three item fetches and the chapters read of the finished download
        QTRY_COMPARE(rig.server.itemsRoutes.size(), 4);
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

    // The speed is the engine's average of a running transfer: with none running, and once the last one ends, the total and every row read zero
    void noSpeedWithoutARunningDownload()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        const QString item = hexId('d', 6);
        rig.serve(episodeItem(item, QStringLiteral("Speedless")));
        rig.open();
        QCOMPARE(rig.downloads->bytesPerSecond(), 0);
        QCOMPARE(rig.downloads->property("bytesPerSecond").toLongLong(), 0);

        QVERIFY(rig.downloads->downloadItem(item));
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(item), QStringLiteral("completed"),
                                  10000);
        QCOMPARE(rig.downloads->bytesPerSecond(), 0);
        const Kinefin::DownloadsModel *model = rig.downloads->model();
        QCOMPARE(model->rowCount(), 1);
        const QModelIndex row = model->index(0);
        QCOMPARE(model->data(row, Kinefin::DownloadsModel::BytesPerSecondRole).toLongLong(), 0);
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
        // The page's two sections, each a view of the model: both records are on this device
        QCOMPARE(rig.downloads->completedModel()->rowCount(), 2);
        QCOMPARE(rig.downloads->activeModel()->rowCount(), 0);
        // The disk of the downloads has some room left, read from the nearest folder that exists
        QVERIFY(rig.downloads->freeBytes() >= 0);
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

    // The chapters and the external text subtitles of the item reach info.json and the item folder; embedded, image-based and out-of-allowlist streams do not, and the player's map carries both lists
    void chaptersAndTextSubtitlesAreKept()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        rig.serve(withExtras(
            downloadable(QStringLiteral("movie_item"), kMovie, QStringLiteral("Arcturus")),
            QJsonArray{subtitleStream(3, QStringLiteral("subrip"), QStringLiteral("ita"),
                                      QStringLiteral("Italian SRT")),
                       subtitleStream(4, QStringLiteral("pgssub"), QStringLiteral("eng"),
                                      QStringLiteral("Image"), true, false),
                       subtitleStream(5, QStringLiteral("subrip"), QStringLiteral("eng"),
                                      QStringLiteral("Embedded"), false, true),
                       subtitleStream(6, QStringLiteral("WebVTT"), QStringLiteral("eng"),
                                      QStringLiteral("English\nVTT"))},
            QJsonArray{chapterEntry(QStringLiteral("Intro"), 0),
                       chapterEntry(QStringLiteral("Middle"), 9'000'000'000),
                       chapterEntry(QStringLiteral("Past the runtime cap"), qint64(1) << 41)}));
        rig.open();
        QVERIFY(rig.downloads->downloadItem(kMovie));
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(kMovie), QStringLiteral("completed"),
                                  10000);
        const QString folder = itemFolder(rig, kMovie);
        QVERIFY(!folder.isEmpty());
        // The info file is rewritten once, after the last subtitle file
        QTRY_VERIFY_WITH_TIMEOUT(infoHas(folder, QStringLiteral("subtitles")), 10000);

        QCOMPARE(rig.server.subtitleDownloadRequests, 2);
        QVERIFY(!rig.server.subtitleDownloadAuth.isEmpty());
        QCOMPARE(rig.server.subtitleDownloadAuth, rig.client.streamAuthorization());
        const QByteArray prefix = QByteArrayLiteral("/Videos/") + kMovie.toUtf8() + '/'
                                  + kSource.toUtf8() + QByteArrayLiteral("/Subtitles/");
        QCOMPARE(rig.server.subtitleDownloadRoutes.size(), 2);
        QVERIFY(rig.server.subtitleDownloadRoutes.at(0).endsWith(
            QByteArray(prefix + "3/0/Stream.srt")));
        QVERIFY(rig.server.subtitleDownloadRoutes.at(1).endsWith(
            QByteArray(prefix + "6/0/Stream.vtt")));
        QCOMPARE(contents(folder + QStringLiteral("/subs/3.srt")), rig.server.subtitleDownloadBody);
        QCOMPARE(contents(folder + QStringLiteral("/subs/6.vtt")), rig.server.subtitleDownloadBody);
        QVERIFY(!QFileInfo::exists(folder + QStringLiteral("/subs/4.srt")));
        QVERIFY(!QFileInfo::exists(folder + QStringLiteral("/subs/5.srt")));

        const QJsonObject info = infoOf(folder);
        QCOMPARE(info.value(QStringLiteral("name")).toString(), QStringLiteral("Arcturus"));
        const QJsonArray chapters = info.value(QStringLiteral("chapters")).toArray();
        QCOMPARE(chapters.size(), 2);
        QCOMPARE(chapters.at(1).toObject().value(QStringLiteral("Name")).toString(),
                 QStringLiteral("Middle"));
        QCOMPARE(
            qint64(
                chapters.at(1).toObject().value(QStringLiteral("StartPositionTicks")).toDouble()),
            qint64(9'000'000'000));
        const QJsonArray subtitles = info.value(QStringLiteral("subtitles")).toArray();
        QCOMPARE(subtitles.size(), 2);
        QCOMPARE(subtitles.at(0).toObject().value(QStringLiteral("index")).toInt(), 3);
        QCOMPARE(subtitles.at(0).toObject().value(QStringLiteral("file")).toString(),
                 QStringLiteral("subs/3.srt"));
        QCOMPARE(subtitles.at(0).toObject().value(QStringLiteral("language")).toString(),
                 QStringLiteral("ita"));
        QCOMPARE(subtitles.at(0).toObject().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Italian SRT"));
        QCOMPARE(subtitles.at(1).toObject().value(QStringLiteral("file")).toString(),
                 QStringLiteral("subs/6.vtt"));
        QCOMPARE(subtitles.at(1).toObject().value(QStringLiteral("title")).toString(),
                 QStringLiteral("English VTT"));

        const QVariantMap play =
            rig.downloads->playableDownload(rig.serverId(), rig.userId(), kMovie);
        const QVariantList playChapters = play.value(QStringLiteral("chapters")).toList();
        QCOMPARE(playChapters.size(), 2);
        QCOMPARE(playChapters.at(1).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Middle"));
        QCOMPARE(playChapters.at(1).toMap().value(QStringLiteral("startTicks")).toLongLong(),
                 qint64(9'000'000'000));
        const QVariantList playSubtitles = play.value(QStringLiteral("subtitles")).toList();
        QCOMPARE(playSubtitles.size(), 2);
        const QString first = playSubtitles.at(0).toMap().value(QStringLiteral("path")).toString();
        QVERIFY(QDir::isAbsolutePath(first));
        QCOMPARE(first, QDir(folder).filePath(QStringLiteral("subs/3.srt")));
        QCOMPARE(playSubtitles.at(0).toMap().value(QStringLiteral("language")).toString(),
                 QStringLiteral("ita"));
        QCOMPARE(playSubtitles.at(1).toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("English VTT"));
    }

    // A stream that is not an external text subtitle of an allowed codec with an index in range asks for nothing, and the chapters still land
    void unsuitableSubtitleStreamsAreNotFetched()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        QJsonObject noIndex = subtitleStream(0, QStringLiteral("subrip"), QStringLiteral("eng"),
                                             QStringLiteral("No index"));
        noIndex.remove(QStringLiteral("Index"));
        QJsonObject fraction = subtitleStream(0, QStringLiteral("subrip"), QStringLiteral("eng"),
                                              QStringLiteral("Fraction"));
        fraction.insert(QStringLiteral("Index"), 1.5);
        QJsonObject audio = subtitleStream(1, QStringLiteral("subrip"), QStringLiteral("eng"),
                                           QStringLiteral("Audio"));
        audio.insert(QStringLiteral("Type"), QStringLiteral("Audio"));
        rig.serve(withExtras(
            downloadable(QStringLiteral("movie_item"), kMovie, QStringLiteral("Arcturus")),
            QJsonArray{subtitleStream(2, QStringLiteral("pgssub"), QStringLiteral("eng"),
                                      QStringLiteral("Image"), true, false),
                       subtitleStream(3, QStringLiteral("subrip"), QStringLiteral("eng"),
                                      QStringLiteral("Embedded"), false, true),
                       subtitleStream(4, QStringLiteral("mov_text"), QStringLiteral("eng"),
                                      QStringLiteral("Other codec")),
                       subtitleStream(10000, QStringLiteral("subrip"), QStringLiteral("eng"),
                                      QStringLiteral("Index too large")),
                       subtitleStream(-1, QStringLiteral("subrip"), QStringLiteral("eng"),
                                      QStringLiteral("Negative index")),
                       fraction, noIndex, audio},
            QJsonArray{chapterEntry(QStringLiteral("Intro"), 0)}));
        rig.open();
        QVERIFY(rig.downloads->downloadItem(kMovie));
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(kMovie), QStringLiteral("completed"),
                                  10000);
        const QString folder = itemFolder(rig, kMovie);
        QVERIFY(!folder.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(infoHas(folder, QStringLiteral("chapters")), 10000);

        QCOMPARE(rig.server.subtitleDownloadRequests, 0);
        QVERIFY(!infoHas(folder, QStringLiteral("subtitles")));
        QVERIFY(!QFileInfo::exists(folder + QStringLiteral("/subs")));
    }

    // The media source id is a path segment of the request: an unsafe one asks for no subtitle at all
    void anUnsafeMediaSourceIdFetchesNoSubtitle()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        rig.serve(withExtras(
            downloadable(QStringLiteral("movie_item"), kMovie, QStringLiteral("Arcturus")),
            QJsonArray{subtitleStream(2, QStringLiteral("subrip"), QStringLiteral("eng"),
                                      QStringLiteral("English"))},
            QJsonArray{chapterEntry(QStringLiteral("Intro"), 0)}, QStringLiteral("a/../b")));
        rig.open();
        QVERIFY(rig.downloads->downloadItem(kMovie));
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(kMovie), QStringLiteral("completed"),
                                  10000);
        const QString folder = itemFolder(rig, kMovie);
        QVERIFY(!folder.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(infoHas(folder, QStringLiteral("chapters")), 10000);

        QCOMPARE(rig.server.subtitleDownloadRequests, 0);
        QVERIFY(!infoHas(folder, QStringLiteral("subtitles")));
        QVERIFY(!QFileInfo::exists(folder + QStringLiteral("/subs")));
    }

    void aRefusedSubtitleFileLeavesNothing_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("redirect");
        QTest::addColumn<int>("bodySize");
        QTest::newRow("not found") << 404 << false << -1;
        QTest::newRow("server error") << 500 << false << -1;
        QTest::newRow("redirect") << 200 << true << -1;
        QTest::newRow("empty body") << 200 << false << 0;
        QTest::newRow("too large")
            << 200 << false << int(DownloadsController::MaxSubtitleBytes + 1);
    }

    // A refused, redirected, empty or oversized file is skipped without a retry: no file, no entry, and the rest of the info file is written
    void aRefusedSubtitleFileLeavesNothing()
    {
        QFETCH(int, status);
        QFETCH(bool, redirect);
        QFETCH(int, bodySize);
        Rig rig;
        QVERIFY(rig.signIn());
        rig.server.subtitleDownloadStatus = status;
        rig.server.redirectSubtitleDownload = redirect;
        if (bodySize >= 0)
            rig.server.subtitleDownloadBody = QByteArray(bodySize, 'x');
        rig.serve(withExtras(
            downloadable(QStringLiteral("movie_item"), kMovie, QStringLiteral("Arcturus")),
            QJsonArray{subtitleStream(2, QStringLiteral("subrip"), QStringLiteral("eng"),
                                      QStringLiteral("English"))},
            QJsonArray{chapterEntry(QStringLiteral("Intro"), 0)}));
        rig.open();
        QVERIFY(rig.downloads->downloadItem(kMovie));
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(kMovie), QStringLiteral("completed"),
                                  10000);
        const QString folder = itemFolder(rig, kMovie);
        QVERIFY(!folder.isEmpty());
        // The rewrite comes after the only subtitle attempt
        QTRY_VERIFY_WITH_TIMEOUT(infoHas(folder, QStringLiteral("chapters")), 10000);

        QCOMPARE(rig.server.subtitleDownloadRequests, 1);
        QVERIFY(!QFileInfo::exists(folder + QStringLiteral("/subs/2.srt")));
        QVERIFY(!infoHas(folder, QStringLiteral("subtitles")));
        const QVariantMap play =
            rig.downloads->playableDownload(rig.serverId(), rig.userId(), kMovie);
        QCOMPARE(play.value(QStringLiteral("chapters")).toList().size(), 1);
        QVERIFY(play.value(QStringLiteral("subtitles")).toList().isEmpty());
    }

    // The download is removed while its subtitle is on the wire: nothing is written, the folder is not created again, and the next item's subtitle (queued behind it) still arrives
    void aRemovedDownloadGetsNoSubtitleFile()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        const QString other = hexId('b', 1);
        const QJsonArray streams{subtitleStream(1, QStringLiteral("srt"), QStringLiteral("eng"),
                                                QStringLiteral("English"))};
        const QJsonArray chapters{chapterEntry(QStringLiteral("Intro"), 0)};
        rig.serve(withExtras(
            downloadable(QStringLiteral("movie_item"), kMovie, QStringLiteral("Arcturus")), streams,
            chapters));
        rig.serve(withExtras(
            downloadable(QStringLiteral("movie_item"), other, QStringLiteral("Betelgeuse")),
            streams, chapters));
        rig.server.holdSubtitleDownload = true;
        rig.open();
        QVERIFY(rig.downloads->downloadItem(kMovie));
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(kMovie), QStringLiteral("completed"),
                                  10000);
        QTRY_COMPARE_WITH_TIMEOUT(rig.server.heldSubtitleCount(), qsizetype(1), 10000);
        const QString folder = itemFolder(rig, kMovie);
        QVERIFY(!folder.isEmpty());
        QVERIFY(rig.downloads->downloadItem(other));
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(other), QStringLiteral("completed"),
                                  10000);
        const QString otherFolder = itemFolder(rig, other);
        QVERIFY(!otherFolder.isEmpty());

        QVERIFY(rig.downloads->removeDownload(rig.serverId(), rig.userId(), kMovie));
        QVERIFY(!QFileInfo::exists(folder));
        rig.server.holdSubtitleDownload = false;
        rig.server.releaseSubtitleDownloads();
        // One request is in flight at a time: the other item's file is only fetched after the removed item's reply was handled
        QTRY_VERIFY_WITH_TIMEOUT(infoHas(otherFolder, QStringLiteral("subtitles")), 10000);
        QVERIFY(QFileInfo::exists(otherFolder + QStringLiteral("/subs/1.srt")));
        QVERIFY(!QFileInfo::exists(folder));
        QCOMPARE(rig.server.subtitleDownloadRequests, 2);
    }

    // The info file is read back as untrusted: only canonical subs/<index>.<ext> regular files inside the item folder, at most eight, cleaned text, and an unreadable file gives empty lists
    void playableDownloadValidatesTheInfoFile()
    {
        Rig rig;
        QVERIFY(rig.signIn());
        rig.serve(withExtras(
            downloadable(QStringLiteral("movie_item"), kMovie, QStringLiteral("Arcturus")),
            QJsonArray(), QJsonArray()));
        rig.open();
        QVERIFY(rig.downloads->downloadItem(kMovie));
        QTRY_COMPARE_WITH_TIMEOUT(rig.downloads->downloadState(kMovie), QStringLiteral("completed"),
                                  10000);
        const QString folder = itemFolder(rig, kMovie);
        QVERIFY(!folder.isEmpty());
        const QString subs = folder + QStringLiteral("/subs");
        QVERIFY(QDir().mkpath(subs));
        QVERIFY(writeFile(subs + QStringLiteral("/2.srt"), "x"));
        QVERIFY(writeFile(subs + QStringLiteral("/02.srt"), "x"));
        QVERIFY(writeFile(subs + QStringLiteral("/2.exe"), "x"));
        QVERIFY(writeFile(folder + QStringLiteral("/outside.srt"), "x"));
        QVERIFY(QFile::link(subs + QStringLiteral("/2.srt"), subs + QStringLiteral("/3.srt")));
        const auto entry = [](const QString &file, const QString &language, const QString &title) {
            return QJsonObject{{QStringLiteral("index"), 2},
                               {QStringLiteral("file"), file},
                               {QStringLiteral("language"), language},
                               {QStringLiteral("title"), title}};
        };
        const QJsonObject info{
            {QStringLiteral("chapters"),
             QJsonArray{chapterEntry(QStringLiteral("One\nline"), 5),
                        chapterEntry(QStringLiteral("Past the runtime cap"), qint64(1) << 41),
                        chapterEntry(QStringLiteral("Last"), 7)}},
            {QStringLiteral("subtitles"),
             QJsonArray{entry(QStringLiteral("subs/2.srt"), QStringLiteral("ITA"),
                              QStringLiteral("Line\none") + QChar(0x202E) + QStringLiteral("two")),
                        entry(QStringLiteral("subs/3.srt"), QStringLiteral("ita"), {}),
                        entry(QStringLiteral("subs/02.srt"), QStringLiteral("ita"), {}),
                        entry(QStringLiteral("subs/2.exe"), QStringLiteral("ita"), {}),
                        entry(QStringLiteral("subs/9.srt"), QStringLiteral("ita"), {}),
                        entry(QStringLiteral("../outside.srt"), QStringLiteral("ita"), {}),
                        entry(QStringLiteral("/etc/passwd"), QStringLiteral("ita"), {}),
                        QStringLiteral("subs/2.srt")}}};
        QVERIFY(writeFile(folder + QStringLiteral("/info.json"),
                          QJsonDocument(info).toJson(QJsonDocument::Compact)));

        QVariantMap play = rig.downloads->playableDownload(rig.serverId(), rig.userId(), kMovie);
        QVERIFY(!play.value(QStringLiteral("path")).toString().isEmpty());
        const QVariantList chapters = play.value(QStringLiteral("chapters")).toList();
        QCOMPARE(chapters.size(), 2);
        QCOMPARE(chapters.at(0).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("One line"));
        QCOMPARE(chapters.at(0).toMap().value(QStringLiteral("startTicks")).toLongLong(),
                 qint64(5));
        QCOMPARE(chapters.at(1).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Last"));
        const QVariantList subtitles = play.value(QStringLiteral("subtitles")).toList();
        QCOMPARE(subtitles.size(), 1);
        QCOMPARE(subtitles.at(0).toMap().value(QStringLiteral("path")).toString(),
                 QDir(folder).filePath(QStringLiteral("subs/2.srt")));
        QVERIFY(subtitles.at(0).toMap().value(QStringLiteral("language")).toString().isEmpty());
        QCOMPARE(subtitles.at(0).toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Line one two"));

        // At most eight tracks, whatever the file lists
        QJsonArray many;
        for (int index = 10; index < 20; ++index) {
            QVERIFY(writeFile(subs + QStringLiteral("/%1.srt").arg(index), "x"));
            many.append(entry(QStringLiteral("subs/%1.srt").arg(index), QStringLiteral("eng"),
                              QStringLiteral("Track")));
        }
        QVERIFY(writeFile(folder + QStringLiteral("/info.json"),
                          QJsonDocument(QJsonObject{{QStringLiteral("subtitles"), many}})
                              .toJson(QJsonDocument::Compact)));
        play = rig.downloads->playableDownload(rig.serverId(), rig.userId(), kMovie);
        QCOMPARE(play.value(QStringLiteral("subtitles")).toList().size(),
                 DownloadsController::MaxSubtitleTracks);

        // Malformed and oversized files give empty lists, never a failed call
        QVERIFY(writeFile(folder + QStringLiteral("/info.json"), "{ not json"));
        play = rig.downloads->playableDownload(rig.serverId(), rig.userId(), kMovie);
        QVERIFY(!play.value(QStringLiteral("path")).toString().isEmpty());
        QVERIFY(play.value(QStringLiteral("chapters")).toList().isEmpty());
        QVERIFY(play.value(QStringLiteral("subtitles")).toList().isEmpty());
        QVERIFY(writeFile(
            folder + QStringLiteral("/info.json"),
            QJsonDocument(
                QJsonObject{{QStringLiteral("chapters"),
                             QJsonArray{chapterEntry(QStringLiteral("Late"), 1)}},
                            {QStringLiteral("padding"), QString(600 * 1024, QLatin1Char('x'))}})
                .toJson(QJsonDocument::Compact)));
        play = rig.downloads->playableDownload(rig.serverId(), rig.userId(), kMovie);
        QVERIFY(!play.value(QStringLiteral("path")).toString().isEmpty());
        QVERIFY(play.value(QStringLiteral("chapters")).toList().isEmpty());
        QVERIFY(QFile::remove(folder + QStringLiteral("/info.json")));
        play = rig.downloads->playableDownload(rig.serverId(), rig.userId(), kMovie);
        QVERIFY(!play.value(QStringLiteral("path")).toString().isEmpty());
        QVERIFY(play.value(QStringLiteral("subtitles")).toList().isEmpty());
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
