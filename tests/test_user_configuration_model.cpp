// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The user preferences editor against the fake server: lossless merge, conflicts, the write and its verification, the loss of the session in every phase, and what is shown for values the editor does not know
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "models/CultureOptionsModel.h"
#include "models/FilterOptionsModel.h"
#include "models/UserConfigurationModel.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <algorithm>

using Kinefin::CultureOptionsModel;
using Kinefin::LibraryOption;
using Kinefin::UserConfigurationModel;
using State = UserConfigurationModel::State;

namespace {
const QString kUser = QStringLiteral("d5ed5d4e2e5447b58e9f2b6f7bd5c96a");
const QString kOtherUser = QStringLiteral("0123456789abcdef0123456789abcdef");
const QString kLibA = QStringLiteral("a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1");
const QString kLibB = QStringLiteral("b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2");
const QString kLibC = QStringLiteral("c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3");
const QString kHidden = QStringLiteral("f9f9f9f9f9f9f9f9f9f9f9f9f9f9f9f9");
const QString kHiddenToo = QStringLiteral("e8e8e8e8e8e8e8e8e8e8e8e8e8e8e8e8");

QString text(const char *value)
{
    return QString::fromLatin1(value);
}

// A configuration with everything the editor owns, plus what it must carry untouched: unknown keys, nulls, nested values, an array it does not edit and an enum from a newer server
QJsonObject richConfiguration()
{
    return QJsonObject{
        {text("AudioLanguagePreference"), QJsonValue::Null},
        {text("SubtitleLanguagePreference"), text("ita")},
        {text("SubtitleMode"), text("Smart")},
        {text("PlayDefaultAudioTrack"), true},
        {text("RememberAudioSelections"), true},
        {text("RememberSubtitleSelections"), false},
        {text("EnableNextEpisodeAutoPlay"), true},
        {text("DisplayMissingEpisodes"), false},
        {text("HidePlayedInLatest"), true},
        {text("GroupedFolders"), QJsonArray{kLibA}},
        {text("DisplayCollectionsView"), false},
        {text("EnableLocalPassword"), false},
        {text("OrderedViews"), QJsonArray{kHidden, kLibA, kLibB, kLibC}},
        {text("MyMediaExcludes"), QJsonValue::Null},
        {text("LatestItemsExcludes"), QJsonArray{kHidden}},
        {text("FutureField"),
         QJsonObject{{text("Nested"), QJsonArray{1, 2, QJsonValue::Null}}, {text("Flag"), true}}},
        {text("NullField"), QJsonValue::Null},
        {text("UnknownEnum"), text("FromTheFuture")}};
}

QByteArray userBody(const QString &id, const QJsonObject &configuration)
{
    return QJsonDocument(
               QJsonObject{{text("Id"), id},
                           {text("Name"), text("bepi")},
                           {text("Policy"), QJsonObject{{text("IsAdministrator"), false}}},
                           {text("FutureUserField"), 1},
                           {text("Configuration"), configuration}})
        .toJson(QJsonDocument::Compact);
}

// The fake server keeps `remote` as the user's configuration: GET /Users/Me serves it, a POST replaces it (unless `storePosts` is off), and a test edits it to play another device
struct Fixture {
    explicit Fixture(const QJsonObject &configuration = richConfiguration()) : remote(configuration)
    {
        ok = server.listen() && login(client, server);
        installHooks();
        model.setLibraries(
            {{kLibA, text("Movies")}, {kLibB, text("Shows")}, {kLibC, text("Anime")}});
    }

    void serveMe()
    {
        server.usersMeRawBody = rawOverride.isEmpty() ? userBody(bodyUserId, remote) : rawOverride;
    }

    void installHooks()
    {
        server.onUsersMe = [this] { serveMe(); };
        server.onUserConfigurationPost = [this](const QByteArray &body) {
            posts.append(body);
            if (storePosts)
                remote = QJsonDocument::fromJson(body).object();
        };
    }

    bool waitFor(State state)
    {
        return QTest::qWaitFor([&] { return model.state() == state; }, 5000);
    }

    bool loadReady()
    {
        model.load();
        return waitFor(State::Ready);
    }

    bool relogin()
    {
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        return client.authenticate(QStringLiteral("fixture-user"),
                                   QStringLiteral("fixture-password"))
               && QTest::qWaitFor([&] { return loggedIn.size() == 1; }, 5000);
    }

    QJsonObject lastPost() const { return QJsonDocument::fromJson(posts.constLast()).object(); }

    QJsonObject remote;
    QString bodyUserId = kUser;
    QByteArray rawOverride;
    bool storePosts = true;
    QList<QByteArray> posts;
    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    UserConfigurationModel model{&client};
    bool ok = false;
};
} // namespace

class TestUserConfigurationModel : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void loadsTheTypedDraftAndTheSidePreviews()
    {
        Fixture f;
        QVERIFY(f.ok);
        QCOMPARE(f.model.state(), State::Idle);
        f.model.load();
        QCOMPARE(f.model.state(), State::Loading);
        QVERIFY(f.waitFor(State::Ready));
        QVERIFY(f.model.loaded());
        QVERIFY(!f.model.dirty());
        QCOMPARE(f.model.audioLanguage(), QString());
        QCOMPARE(f.model.subtitleLanguage(), text("ita"));
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
        QVERIFY(f.model.playDefaultAudioTrack());
        QVERIFY(!f.model.rememberSubtitleSelections());
        QCOMPARE(f.model.orderedViews(), (QStringList{kLibA, kLibB, kLibC}));
        QVERIFY(f.model.myMediaExcludes().isEmpty());
        QVERIFY(f.model.latestItemsExcludes().isEmpty());
        // The hidden id appears in two arrays and is counted once
        QCOMPARE(f.model.unavailableLibraryCount(), 1);
        QTRY_VERIFY(f.model.cultures()->available());
        QTRY_COMPARE(f.model.homeSectionsStatus(), UserConfigurationModel::HomeSectionsReady);
        QCOMPARE(f.model.homeSections(),
                 (QVariantList{int(UserConfigurationModel::Libraries),
                               int(UserConfigurationModel::ContinueWatching)}));
        // Nothing but reads so far
        for (const auto &request : f.server.requestLog)
            QVERIFY2(request.first == "GET" || request.first == "POST", "unexpected method");
        QVERIFY(f.posts.isEmpty());
    }

    void aSaveKeepsEverythingItDoesNotEdit()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        const QJsonObject original = f.remote;
        f.model.setPlayDefaultAudioTrack(false);
        f.model.setSubtitleMode(text("Always"));
        QVERIFY(f.model.dirtyFields().contains(text("SubtitleMode")));
        QVERIFY(f.model.dirtyFields().contains(text("PlayDefaultAudioTrack")));
        QCOMPARE(f.model.dirtyFields().size(), 2);
        QVERIFY(f.model.canSave());
        QSignalSpy saved(&f.model, &UserConfigurationModel::saved);
        QSignalSpy applied(&f.model, &UserConfigurationModel::configurationApplied);
        const int readsBefore = f.server.usersMeRequests;
        f.model.save();
        QCOMPARE(f.model.state(), State::Saving);
        QVERIFY(f.waitFor(State::Ready));

        // One write of the whole object: the two edits and every other value exactly as the server had it
        QCOMPARE(f.posts.size(), 1);
        QJsonObject expected = original;
        expected.insert(text("PlayDefaultAudioTrack"), false);
        expected.insert(text("SubtitleMode"), text("Always"));
        QCOMPARE(f.lastPost(), expected);
        QVERIFY(f.lastPost().value(text("AudioLanguagePreference")).isNull());
        QVERIFY(f.lastPost().contains(text("NullField")));
        QVERIFY(f.lastPost().value(text("MyMediaExcludes")).isNull());
        QCOMPARE(f.lastPost().value(text("OrderedViews")), original.value(text("OrderedViews")));
        // The read before the write and the read that verifies it
        QCOMPARE(f.server.usersMeRequests - readsBefore, 2);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(applied.size(), 1);
        const auto snapshot =
            applied.constFirst().constFirst().value<Kinefin::UserPreferencesSnapshot>();
        QCOMPARE(snapshot.subtitleMode, text("Always"));
        QVERIFY(!snapshot.playDefaultAudioTrack);
        QCOMPARE(snapshot.orderedViews, (QStringList{kHidden, kLibA, kLibB, kLibC}));
        QVERIFY(!f.model.dirty());
        QVERIFY(!f.model.canSave());

        // The request shape: JSON, the token in the header only, no query, and display preferences are never written
        for (const auto &call : f.server.accountCalls) {
            if (call.method != "POST")
                continue;
            QVERIFY(call.route.endsWith("/Users/Configuration"));
            QVERIFY(!call.route.contains('?'));
            QCOMPARE(call.contentType, QByteArray("application/json"));
            QVERIFY(call.auth.contains(kFixtureToken));
        }
        for (const auto &request : f.server.requestLog)
            QVERIFY(!(request.first != "GET" && request.second.contains("DisplayPreferences")));
    }

    void aChangeBackIsNotDirtyAndDiscardResetsTheDraft()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        QSignalSpy changed(&f.model, &UserConfigurationModel::draftChanged);
        f.model.setHidePlayedInLatest(false);
        QCOMPARE(f.model.dirtyFields(), QStringList{text("HidePlayedInLatest")});
        f.model.setHidePlayedInLatest(true);
        QVERIFY(!f.model.dirty());
        QVERIFY(changed.size() >= 2);

        f.model.setEnableNextEpisodeAutoPlay(false);
        f.model.setDisplayMissingEpisodes(true);
        f.model.setMyMediaExcluded(kLibB, true);
        f.model.setLatestExcluded(kLibC, true);
        f.model.moveOrderedView(2, 0);
        QCOMPARE(f.model.orderedViews(), (QStringList{kLibC, kLibA, kLibB}));
        QCOMPARE(f.model.dirtyFields().size(), 5);
        const int readsBefore = f.server.usersMeRequests;
        f.model.discard();
        QCOMPARE(f.model.state(), State::Ready);
        QVERIFY(!f.model.dirty());
        QVERIFY(f.model.enableNextEpisodeAutoPlay());
        QVERIFY(!f.model.displayMissingEpisodes());
        QCOMPARE(f.model.orderedViews(), (QStringList{kLibA, kLibB, kLibC}));
        QVERIFY(f.model.myMediaExcludes().isEmpty());
        // Saving with nothing dirty asks nothing
        f.model.save();
        QCOMPARE(f.model.state(), State::Ready);
        QCOMPARE(f.server.usersMeRequests, readsBefore);
        QVERIFY(f.posts.isEmpty());
    }

    void theControlsRefuseUntilTheLoadIsDone()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.model.setHidePlayedInLatest(false);
        QVERIFY(f.model.hidePlayedInLatest());
        f.model.load();
        f.model.setSubtitleMode(text("Always"));
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
        QVERIFY(!f.model.dirty());
    }

    void aConflictBlocksTheWrite()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.model.setSubtitleMode(text("Always"));
        f.model.setHidePlayedInLatest(false);
        // Another device changed one of the two fields we edit, and one we do not
        f.remote.insert(text("SubtitleMode"), text("None"));
        f.remote.insert(text("RememberAudioSelections"), false);
        QSignalSpy saved(&f.model, &UserConfigurationModel::saved);
        f.model.save();
        QVERIFY(f.waitFor(State::Conflict));
        QVERIFY(f.posts.isEmpty());
        QCOMPARE(f.model.conflictFields(), QStringList{text("SubtitleMode")});
        QCOMPARE(saved.size(), 0);
        // The draft survives and the page can return to editing
        QCOMPARE(f.model.subtitleMode(), text("Always"));
        QVERIFY(!f.model.canSave());
        f.model.keepEditing();
        QCOMPARE(f.model.state(), State::Ready);
        QVERIFY(f.model.conflictFields().isEmpty());
        // The server's values are the baseline now: the draft stays, a field nobody edited follows the server
        QCOMPARE(f.model.subtitleMode(), text("Always"));
        QVERIFY(!f.model.rememberAudioSelections());
        QCOMPARE(f.model.dirtyFields().size(), 2);
        // The second save overwrites knowingly
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.posts.size(), 1);
        QCOMPARE(f.lastPost().value(text("SubtitleMode")), QJsonValue(text("Always")));
        QCOMPARE(f.lastPost().value(text("RememberAudioSelections")), QJsonValue(false));
        QVERIFY(!f.model.dirty());
    }

    void reloadingAfterAConflictTakesTheServerValues()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.model.setSubtitleMode(text("Always"));
        f.remote.insert(text("SubtitleMode"), text("None"));
        f.remote.insert(text("RememberAudioSelections"), false);
        f.model.save();
        QVERIFY(f.waitFor(State::Conflict));
        f.model.load();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.model.subtitleMode(), text("None"));
        QVERIFY(!f.model.rememberAudioSelections());
        QVERIFY(!f.model.dirty());
    }

    void discardingAfterAConflictShowsTheServerValues()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.model.setSubtitleMode(text("Always"));
        f.remote.insert(text("SubtitleMode"), text("None"));
        f.model.save();
        QVERIFY(f.waitFor(State::Conflict));
        f.model.discard();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.model.subtitleMode(), text("None"));
        QVERIFY(!f.model.dirty());
        QVERIFY(f.posts.isEmpty());
    }

    void discardingTheDraftOfAClosedPageReadsNothing()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.model.setSubtitleMode(text("Always"));
        f.remote.insert(text("SubtitleMode"), text("None"));
        f.model.save();
        QVERIFY(f.waitFor(State::Conflict));
        f.model.discardDraft();
        // At once, with no read of the server: the draft is back at the baseline
        QCOMPARE(f.model.state(), State::Ready);
        QVERIFY(!f.model.dirty());
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
    }

    void aServerValueThatAlreadyEqualsTheDraftIsNoConflict()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.model.setSubtitleMode(text("Always"));
        f.remote.insert(text("SubtitleMode"), text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.posts.size(), 1);
    }

    void remoteChangesToOtherFieldsSurviveTheSave()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.model.setPlayDefaultAudioTrack(false);
        f.remote.insert(text("RememberAudioSelections"), false);
        f.remote.insert(text("ServerAddedThis"), QJsonArray{7});
        f.remote.insert(text("HidePlayedInLatest"), false);
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.posts.size(), 1);
        QCOMPARE(f.lastPost().value(text("RememberAudioSelections")), QJsonValue(false));
        QCOMPARE(f.lastPost().value(text("ServerAddedThis")), QJsonValue(QJsonArray{7}));
        QCOMPARE(f.lastPost().value(text("HidePlayedInLatest")), QJsonValue(false));
        QCOMPARE(f.lastPost().value(text("PlayDefaultAudioTrack")), QJsonValue(false));
        // The verified configuration is the new baseline, remote changes included
        QVERIFY(!f.model.rememberAudioSelections());
        QVERIFY(!f.model.hidePlayedInLatest());
        QVERIFY(!f.model.dirty());
    }

    void aWriteRacingTheReadWinsAndSaysSo()
    {
        // Between the fresh read and the write another device changes the field we edit: with no compare-and-swap on the server the write replaces it, and the verification sees our value
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        int reads = 0;
        f.server.onUsersMe = [&] {
            f.serveMe();
            if (++reads == 1)
                f.remote.insert(text("SubtitleMode"), text("None"));
        };
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.posts.size(), 1);
        QCOMPARE(f.lastPost().value(text("SubtitleMode")), QJsonValue(text("Always")));
        QCOMPARE(f.remote.value(text("SubtitleMode")), QJsonValue(text("Always")));
    }

    void aWriteTheServerDidNotKeepIsNotReportedAsSaved()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.storePosts = false;
        QSignalSpy saved(&f.model, &UserConfigurationModel::saved);
        QSignalSpy applied(&f.model, &UserConfigurationModel::configurationApplied);
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Failed));
        QCOMPARE(f.model.failure(), UserConfigurationModel::NotVerified);
        QCOMPARE(saved.size(), 0);
        QCOMPARE(applied.size(), 0);
        QVERIFY(f.model.dirty());
        // The way out is a reload
        f.model.retry();
        QVERIFY(f.waitFor(State::Ready));
        QVERIFY(!f.model.dirty());
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
    }

    void aFailedVerificationReadIsNotASuccess()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.server.onUsersMe = [&] {
            f.serveMe();
            if (!f.posts.isEmpty())
                f.server.usersMeStatus = 500;
        };
        QSignalSpy saved(&f.model, &UserConfigurationModel::saved);
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Failed));
        QCOMPARE(f.model.failure(), UserConfigurationModel::NotVerified);
        QCOMPARE(saved.size(), 0);
        f.server.usersMeStatus = 0;
        f.installHooks();
        f.model.retry();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.model.subtitleMode(), text("Always"));
    }

    void aFailedWriteKeepsTheDraftAndRetryNeverConflictsWithItself()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        // The server applies the write and answers 500: the reply was lost, the data was not
        f.server.userConfigurationStatus = 500;
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Failed));
        QCOMPARE(f.model.failure(), UserConfigurationModel::SaveFailed);
        QCOMPARE(f.model.subtitleMode(), text("Always"));
        QVERIFY(f.model.dirty());
        f.server.userConfigurationStatus = 204;
        f.model.retry();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.posts.size(), 2);
        QVERIFY(!f.model.dirty());
        QCOMPARE(f.model.subtitleMode(), text("Always"));
    }

    void aFailedWriteCanBeDiscarded()
    {
        // The write was sent, so its outcome is unknown: discarding reloads from the server (which did not apply it here)
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.storePosts = false;
        f.server.userConfigurationStatus = 500;
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Failed));
        f.model.discard();
        QCOMPARE(f.model.state(), State::Loading);
        QVERIFY(f.waitFor(State::Ready));
        QVERIFY(!f.model.dirty());
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
    }

    void discardingAnUnknownWriteReloadsAndLaterEditsDoNotConflict()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        // The server applies the write and answers 500: the stale baseline must not survive a discard
        f.server.userConfigurationStatus = 500;
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Failed));
        QCOMPARE(f.model.failure(), UserConfigurationModel::SaveFailed);
        f.model.discard();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.model.subtitleMode(), text("Always"));
        QVERIFY(!f.model.dirty());
        f.server.userConfigurationStatus = 204;
        f.model.setSubtitleMode(text("None"));
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.posts.size(), 2);
        QCOMPARE(f.lastPost().value(text("SubtitleMode")), QJsonValue(text("None")));
        QVERIFY(!f.model.dirty());
    }

    void aWriteRefusedBeforeBeingSentDiscardsToTheBaseline()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.server.usersMeStatus = 500;
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Failed));
        QCOMPARE(f.model.failure(), UserConfigurationModel::SaveFailed);
        QVERIFY(f.posts.isEmpty());
        f.model.discard();
        QCOMPARE(f.model.state(), State::Ready);
        QVERIFY(!f.model.dirty());
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
    }

    void twoSavesAreNotQueued()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        f.model.save();
        f.model.load();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.posts.size(), 1);
    }

    void aRefreshKeepsTheValuesOnScreenAndTakesTheServers()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.loadReady());
        // Another device changed a value meanwhile
        f.remote[text("SubtitleMode")] = text("Always");
        QList<State> seen;
        QObject::connect(&f.model, &UserConfigurationModel::stateChanged, &f.model,
                         [&] { seen << f.model.state(); });
        f.model.refresh();
        // The values stay, editable, while the server is read
        QCOMPARE(f.model.state(), State::Ready);
        QVERIFY(f.model.loaded());
        QVERIFY(f.model.editable());
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
        QTRY_COMPARE(f.model.subtitleMode(), text("Always"));
        QCOMPARE(f.model.state(), State::Ready);
        QVERIFY(!f.model.dirty());
        // Never Loading on the way
        QVERIFY(!seen.contains(State::Loading));
        QVERIFY(f.posts.isEmpty());
    }

    void aRefreshNeverOverwritesADraft()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.loadReady());
        f.model.setSubtitleMode(text("OnlyForced"));
        QVERIFY(f.model.dirty());
        f.remote[text("SubtitleMode")] = text("Always");
        QSignalSpy answered(&f.client, &Kinefin::JellyfinClient::currentUserConfigurationReady);
        f.model.refresh();
        QTRY_COMPARE(answered.size(), 1);
        QCOMPARE(f.model.subtitleMode(), text("OnlyForced"));
        QVERIFY(f.model.dirty());
        QCOMPARE(f.model.state(), State::Ready);
        // The save still reads the server first and sees the other device's change
        f.model.save();
        QVERIFY(f.waitFor(State::Conflict));
    }

    void aFailedOrUnusableRefreshChangesNothing()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.loadReady());
        QSignalSpy answered(&f.client, &Kinefin::JellyfinClient::currentUserConfigurationReady);
        QSignalSpy failed(&f.client, &Kinefin::JellyfinClient::currentUserConfigurationFailed);
        for (const int status : {500, 0}) {
            f.server.usersMeStatus = status;
            f.rawOverride =
                status == 0 ? QByteArrayLiteral("{\"Id\":\"someone else\"}") : QByteArray();
            const qsizetype before = answered.size() + failed.size();
            f.model.refresh();
            QTRY_COMPARE(answered.size() + failed.size(), before + 1);
            QCOMPARE(f.model.state(), State::Ready);
            QCOMPARE(f.model.failure(), UserConfigurationModel::NoFailure);
            QCOMPARE(f.model.subtitleMode(), text("Smart"));
            QVERIFY(f.model.editable());
        }
        // Nothing is left waiting: the next refresh reads again
        f.server.usersMeStatus = 0;
        f.rawOverride.clear();
        f.remote[text("SubtitleMode")] = text("Always");
        f.model.refresh();
        QTRY_COMPARE(f.model.subtitleMode(), text("Always"));
    }

    void aRefreshReadsTheConfigurationOnlyAndSignalsOnlyAChange()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.loadReady());
        QTRY_VERIFY(f.model.cultures()->available());
        QTRY_COMPARE(f.model.homeSectionsStatus(), UserConfigurationModel::HomeSectionsReady);
        const qsizetype requests = f.server.requestLog.size();
        QSignalSpy answered(&f.client, &Kinefin::JellyfinClient::currentUserConfigurationReady);
        QSignalSpy states(&f.model, &UserConfigurationModel::stateChanged);
        f.model.refresh();
        QTRY_COMPARE(answered.size(), 1);
        // One read of the configuration, nothing else; the same values change nothing on screen
        QCOMPARE(f.server.requestLog.size(), requests + 1);
        QVERIFY(f.server.requestLog.constLast().second.contains("Users/Me"));
        QCOMPARE(states.size(), 0);
    }

    void aSaveDuringARefreshWritesOnce()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.loadReady());
        f.model.refresh();
        // The user saves before the refresh answers: the save's own read supersedes it
        f.model.setSubtitleMode(text("OnlyForced"));
        f.model.save();
        QTRY_COMPARE(f.posts.size(), 1);
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.model.failure(), UserConfigurationModel::NoFailure);
        QCOMPARE(f.lastPost().value(text("SubtitleMode")).toString(), text("OnlyForced"));
        QVERIFY(!f.model.dirty());
    }

    void aRefreshWithoutValuesLoads()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.model.refresh();
        QCOMPARE(f.model.state(), State::Loading);
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
    }

    void aDraftDroppedDuringARefreshTakesTheServersValues()
    {
        Fixture f;
        QVERIFY(f.ok);
        QVERIFY(f.loadReady());
        f.model.setSubtitleMode(text("OnlyForced"));
        f.remote[text("SubtitleMode")] = text("Always");
        f.model.refresh();
        // The page goes before the answer: the draft is dropped, the refresh goes on
        f.model.discardDraft();
        QVERIFY(!f.model.dirty());
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
        QTRY_COMPARE(f.model.subtitleMode(), text("Always"));
        QVERIFY(!f.model.dirty());
    }

    void aLoadFailureCanBeRetried()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.usersMeStatus = 500;
        f.model.load();
        QVERIFY(f.waitFor(State::Failed));
        QCOMPARE(f.model.failure(), UserConfigurationModel::LoadFailed);
        QVERIFY(!f.model.loaded());
        QVERIFY(!f.model.editable());
        f.model.save();
        QCOMPARE(f.model.state(), State::Failed);
        f.server.usersMeStatus = 0;
        f.model.retry();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.model.failure(), UserConfigurationModel::NoFailure);
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
    }

    void noSessionIsALoadFailure()
    {
        Kinefin::JellyfinClient client;
        UserConfigurationModel model(&client);
        model.load();
        QCOMPARE(model.state(), State::Failed);
        QCOMPARE(model.failure(), UserConfigurationModel::LoadFailed);
    }

    void anUnreadableOrForeignProfileIsRefused()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.rawOverride = QByteArrayLiteral("{\"Id\":\"d5ed5d4e2e5447b58e9f2b6f7bd5c96a\","
                                          "\"Configuration\":\"nope\"}");
        f.model.load();
        QVERIFY(f.waitFor(State::Failed));
        QCOMPARE(f.model.failure(), UserConfigurationModel::InvalidProfile);
        QVERIFY(!f.model.loaded());

        f.rawOverride.clear();
        f.bodyUserId = kOtherUser;
        f.model.retry();
        QVERIFY(f.waitFor(State::Failed));
        QCOMPARE(f.model.failure(), UserConfigurationModel::InvalidProfile);
        QCOMPARE(f.model.subtitleMode(), text("Default"));

        f.bodyUserId = kUser;
        f.model.retry();
        QVERIFY(f.waitFor(State::Ready));
        QVERIFY(f.posts.isEmpty());
    }

    void aProfileThatSwitchesUserBeforeTheWriteIsNeverWritten()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.model.setSubtitleMode(text("Always"));
        f.bodyUserId = kOtherUser;
        f.model.save();
        QVERIFY(f.waitFor(State::Failed));
        QCOMPARE(f.model.failure(), UserConfigurationModel::InvalidProfile);
        QVERIFY(f.posts.isEmpty());
    }

    void theSessionEndingMidLoadLeavesNothingBehind()
    {
        Fixture f;
        QVERIFY(f.ok);
        const quint64 before = f.model.generation();
        f.server.onUsersMe = [&] {
            f.serveMe();
            f.client.forgetSession();
        };
        f.model.load();
        QVERIFY(f.waitFor(State::Idle));
        QVERIFY(f.model.generation() > before);
        QTest::qWait(300);
        QCOMPARE(f.model.state(), State::Idle);
        QVERIFY(!f.model.loaded());
        QVERIFY(!f.model.cultures()->available());
        QCOMPARE(f.model.homeSectionsStatus(), UserConfigurationModel::HomeSectionsIdle);

        // The next session starts clean
        f.installHooks();
        QVERIFY(f.relogin());
        QVERIFY(f.loadReady());
        QVERIFY(!f.model.dirty());
    }

    void theSessionEndingMidSaveWritesNothing()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        QSignalSpy saved(&f.model, &UserConfigurationModel::saved);
        QSignalSpy applied(&f.model, &UserConfigurationModel::configurationApplied);
        f.model.setSubtitleMode(text("Always"));
        f.model.setOrderedViews({kLibC, kLibB, kLibA});
        f.model.save();
        QCOMPARE(f.model.state(), State::Saving);
        f.client.forgetSession();
        QCOMPARE(f.model.state(), State::Idle);
        QVERIFY(!f.model.dirty());
        QVERIFY(f.model.libraries().isEmpty());
        QTest::qWait(300);
        QVERIFY(f.posts.isEmpty());
        QCOMPARE(saved.size(), 0);
        QCOMPARE(applied.size(), 0);

        // A new session does not inherit the draft, a retry or a success
        QVERIFY(f.relogin());
        QVERIFY(f.loadReady());
        QVERIFY(!f.model.dirty());
        QCOMPARE(f.model.subtitleMode(), text("Smart"));
        QTest::qWait(100);
        QCOMPARE(saved.size(), 0);
        QVERIFY(f.posts.isEmpty());
    }

    void theSessionEndingMidWriteReportsNoSuccessToTheNextSession()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        QSignalSpy saved(&f.model, &UserConfigurationModel::saved);
        QSignalSpy applied(&f.model, &UserConfigurationModel::configurationApplied);
        // The write reaches the server; the session is gone before the answer
        f.server.onUserConfigurationPost = [&](const QByteArray &body) {
            f.posts.append(body);
            f.remote = QJsonDocument::fromJson(body).object();
            f.client.forgetSession();
        };
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Idle));
        const int reads = f.server.usersMeRequests;
        QTest::qWait(300);
        QCOMPARE(f.posts.size(), 1);
        // No verification read, no success, no retry
        QCOMPARE(f.server.usersMeRequests, reads);
        QCOMPARE(saved.size(), 0);
        QCOMPARE(applied.size(), 0);
        QVERIFY(!f.model.loaded());

        f.installHooks();
        QVERIFY(f.relogin());
        QVERIFY(f.loadReady());
        QVERIFY(!f.model.dirty());
        QCOMPARE(saved.size(), 0);
        QCOMPARE(f.posts.size(), 1);
    }

    void clearingDropsEverythingTiedToTheSession()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        QTRY_VERIFY(f.model.cultures()->available());
        QTRY_COMPARE(f.model.homeSectionsStatus(), UserConfigurationModel::HomeSectionsReady);
        f.model.setSubtitleLanguage(text("eng"));
        QVERIFY(f.model.dirty());
        const quint64 before = f.model.generation();
        f.client.forgetSession();
        QCOMPARE(f.model.state(), State::Idle);
        QVERIFY(f.model.generation() > before);
        QVERIFY(!f.model.loaded());
        QVERIFY(!f.model.dirty());
        QVERIFY(f.model.dirtyFields().isEmpty());
        QCOMPARE(f.model.subtitleLanguage(), QString());
        QVERIFY(f.model.orderedViews().isEmpty());
        QVERIFY(f.model.libraries().isEmpty());
        QCOMPARE(f.model.unavailableLibraryCount(), 0);
        QCOMPARE(f.model.cultures()->rowCount(), 0);
        QVERIFY(f.model.homeSections().isEmpty());
        QVERIFY(f.model.snapshot().orderedViews.isEmpty());
    }

    void anUnknownLanguageAndEnumStayUntilChanged()
    {
        QJsonObject configuration = richConfiguration();
        configuration.insert(text("SubtitleLanguagePreference"), text("xyz"));
        configuration.insert(text("AudioLanguagePreference"), text("<b>fre</b>"));
        configuration.insert(text("SubtitleMode"), text("Weird"));
        Fixture f(configuration);
        QVERIFY(f.ok && f.loadReady());
        QTRY_VERIFY(f.model.cultures()->available());
        QCOMPARE(f.model.subtitleLanguage(), text("xyz"));
        QCOMPARE(f.model.audioLanguage(), text("<b>fre</b>"));
        QCOMPARE(f.model.subtitleMode(), text("Weird"));
        QCOMPARE(f.model.cultures()->indexOfCode(text("xyz")), -1);

        // A value the catalog or the enum lacks cannot be picked; the stored one can always be picked back
        f.model.setSubtitleLanguage(text("qqq"));
        f.model.setSubtitleMode(text("Nope"));
        QVERIFY(!f.model.dirty());
        f.model.setSubtitleLanguage(text("eng"));
        f.model.setSubtitleMode(text("Always"));
        QCOMPARE(f.model.dirtyFields().size(), 2);
        f.model.setSubtitleLanguage(text("xyz"));
        f.model.setSubtitleMode(text("Weird"));
        QVERIFY(!f.model.dirty());

        // Saving something else writes the odd values back as they were
        f.model.setHidePlayedInLatest(false);
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.lastPost().value(text("SubtitleLanguagePreference")), QJsonValue(text("xyz")));
        QCOMPARE(f.lastPost().value(text("AudioLanguagePreference")),
                 QJsonValue(text("<b>fre</b>")));
        QCOMPARE(f.lastPost().value(text("SubtitleMode")), QJsonValue(text("Weird")));
    }

    void aKnownLanguageIsWrittenAndAutomaticIsEmpty()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        QTRY_VERIFY(f.model.cultures()->available());
        f.model.setAudioLanguage(text("eng"));
        f.model.setSubtitleLanguage(QString());
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.lastPost().value(text("AudioLanguagePreference")), QJsonValue(text("eng")));
        QCOMPARE(f.lastPost().value(text("SubtitleLanguagePreference")), QJsonValue(QString()));
        QCOMPARE(f.model.audioLanguage(), text("eng"));
        QCOMPARE(f.model.subtitleLanguage(), QString());
    }

    void withoutACatalogOnlyStoredAndAutomaticValuesAreOffered()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.culturesStatus = 500;
        QVERIFY(f.loadReady());
        QTest::qWait(200);
        QVERIFY(!f.model.cultures()->available());
        f.model.setAudioLanguage(text("eng"));
        QVERIFY(!f.model.dirty());
        f.model.setSubtitleLanguage(QString());
        QCOMPARE(f.model.dirtyFields(), QStringList{text("SubtitleLanguagePreference")});
        f.model.setSubtitleLanguage(text("ita"));
        QVERIFY(!f.model.dirty());
    }

    void aShapeTheEditorCannotRepresentIsReadOnlyAndKept()
    {
        QJsonObject configuration = richConfiguration();
        configuration.insert(text("PlayDefaultAudioTrack"), text("yes"));
        configuration.insert(text("OrderedViews"), text("not an array"));
        Fixture f(configuration);
        QVERIFY(f.ok && f.loadReady());
        QVERIFY(f.model.malformedFields().contains(text("PlayDefaultAudioTrack")));
        QVERIFY(f.model.malformedFields().contains(text("OrderedViews")));
        f.model.setPlayDefaultAudioTrack(false);
        f.model.setOrderedViews({kLibC, kLibA, kLibB});
        f.model.moveOrderedView(0, 1);
        QVERIFY(!f.model.dirty());
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.lastPost().value(text("PlayDefaultAudioTrack")), QJsonValue(text("yes")));
        QCOMPARE(f.lastPost().value(text("OrderedViews")), QJsonValue(text("not an array")));
    }

    void hiddenIdsStayInTheirSlotsOfOrderedViews()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        QCOMPARE(f.model.orderedViews(), (QStringList{kLibA, kLibB, kLibC}));
        f.model.moveOrderedView(2, 0);
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        // The hidden id keeps slot 0; only the visible slots are refilled
        QCOMPARE(f.lastPost().value(text("OrderedViews")).toArray(),
                 (QJsonArray{kHidden, kLibC, kLibA, kLibB}));
        QCOMPARE(f.model.orderedViews(), (QStringList{kLibC, kLibA, kLibB}));
        // The other arrays were not touched
        QCOMPARE(f.lastPost().value(text("LatestItemsExcludes")).toArray(), (QJsonArray{kHidden}));
        QVERIFY(f.lastPost().value(text("MyMediaExcludes")).isNull());
    }

    void aVisibleLibraryMissingFromTheArrayIsAddedOnceAtTheEnd()
    {
        QJsonObject configuration = richConfiguration();
        configuration.insert(text("OrderedViews"), QJsonArray{kHidden, kLibA, kHiddenToo, kLibA});
        Fixture f(configuration);
        QVERIFY(f.ok && f.loadReady());
        QCOMPARE(f.model.orderedViews(), (QStringList{kLibA, kLibB, kLibC}));
        f.model.setOrderedViews({kLibC, kLibA});
        QCOMPARE(f.model.orderedViews(), (QStringList{kLibC, kLibA, kLibB}));
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        // Two visible slots (the repeated A counts twice) take C and A, the hidden ids stay, B comes last and nothing is duplicated
        QCOMPARE(f.lastPost().value(text("OrderedViews")).toArray(),
                 (QJsonArray{kHidden, kLibC, kHiddenToo, kLibA, kLibB}));
    }

    void exclusionsEditOnlyTheVisibleIds()
    {
        QJsonObject configuration = richConfiguration();
        configuration.insert(text("MyMediaExcludes"), QJsonArray{kHiddenToo, kLibA});
        Fixture f(configuration);
        QVERIFY(f.ok && f.loadReady());
        QCOMPARE(f.model.myMediaExcludes(), QStringList{kLibA});
        f.model.setMyMediaExcluded(kLibB, true);
        f.model.setMyMediaExcluded(kLibA, false);
        f.model.setMyMediaExcluded(kHidden, true);
        f.model.setLatestExcluded(QStringLiteral("not-a-library"), true);
        QCOMPARE(f.model.myMediaExcludes(), QStringList{kLibB});
        QVERIFY(f.model.latestItemsExcludes().isEmpty());
        QCOMPARE(f.model.dirtyFields(), QStringList{text("MyMediaExcludes")});
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.lastPost().value(text("MyMediaExcludes")).toArray(),
                 (QJsonArray{kHiddenToo, kLibB}));
        QCOMPARE(f.lastPost().value(text("LatestItemsExcludes")).toArray(), (QJsonArray{kHidden}));
        QCOMPARE(f.model.unavailableLibraryCount(), 2);
    }

    void aNullExclusionListBecomesAnArrayOnlyWhenEdited()
    {
        Fixture f;
        QVERIFY(f.ok && f.loadReady());
        f.model.setMyMediaExcluded(kLibC, true);
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.lastPost().value(text("MyMediaExcludes")).toArray(), (QJsonArray{kLibC}));
    }

    void theLibrariesMayArriveAfterTheLoad()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.model.setLibraries({});
        QVERIFY(f.loadReady());
        QVERIFY(f.model.orderedViews().isEmpty());
        QCOMPARE(f.model.unavailableLibraryCount(), 4);
        f.model.setLibraries({{kLibB, text("Shows")},
                              {kLibA, text("Movies")},
                              {QStringLiteral("../bad"), text("Bad")},
                              {kLibA, text("Again")},
                              {kLibC, QString()},
                              {kHiddenToo, QString(300, QLatin1Char('n'))}});
        // Invalid ids, repeats and empty names are dropped; the stored order wins over the server order of the visible ones
        QCOMPARE(f.model.libraries().size(), 3);
        QCOMPARE(f.model.libraries().constFirst().toMap().value(text("id")).toString(), kLibB);
        QCOMPARE(f.model.libraries().constLast().toMap().value(text("name")).toString().size(),
                 int(Kinefin::FilterOptionsModel::MaxNameLength));
        QCOMPARE(f.model.orderedViews(), (QStringList{kLibA, kLibB, kHiddenToo}));
        QVERIFY(!f.model.dirty());
        // An edit made now survives the next change of the list, among the ids still visible
        f.model.moveOrderedView(0, 2);
        QCOMPARE(f.model.orderedViews(), (QStringList{kLibB, kHiddenToo, kLibA}));
        f.model.setLibraries({{kLibA, text("Movies")}, {kLibB, text("Shows")}});
        QCOMPARE(f.model.orderedViews(), (QStringList{kLibB, kLibA}));
        QVERIFY(f.model.dirty());
    }

    void theHomeSectionsPreviewIsReadOnlyAndConservative()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.displayPreferencesRawBody = QByteArrayLiteral(
            "{\"CustomPrefs\":{\"homesection0\":\"latestmedia\",\"homesection1\":\"<b>x</b>\","
            "\"homesection2\":\"none\",\"homesection3\":\"nextup\",\"homesection4\":\"\","
            "\"homesection5\":7,\"homesection6\":\"librarybuttons\",\"homesection11\":\"resume\"}"
            "}");
        QVERIFY(f.loadReady());
        QTRY_COMPARE(f.model.homeSectionsStatus(), UserConfigurationModel::HomeSectionsReady);
        QCOMPARE(f.model.homeSections(), (QVariantList{int(UserConfigurationModel::RecentlyAdded),
                                                       int(UserConfigurationModel::Unsupported),
                                                       int(UserConfigurationModel::NextUp),
                                                       int(UserConfigurationModel::Libraries)}));
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        for (const auto &request : f.server.requestLog)
            QVERIFY(!(request.first != "GET" && request.second.contains("DisplayPreferences")));
    }

    void aMissingPreviewDoesNotStopTheEditor()
    {
        Fixture f;
        QVERIFY(f.ok);
        f.server.displayPreferencesStatus = 404;
        QVERIFY(f.loadReady());
        QTRY_COMPARE(f.model.homeSectionsStatus(), UserConfigurationModel::HomeSectionsUnavailable);
        QVERIFY(f.model.homeSections().isEmpty());
        f.model.setSubtitleMode(text("Always"));
        f.model.save();
        QVERIFY(f.waitFor(State::Ready));
        QCOMPARE(f.posts.size(), 1);
    }

    void theCultureCatalogIsBoundedAndPlainText()
    {
        CultureOptionsModel cultures;
        QSignalSpy available(&cultures, &CultureOptionsModel::availableChanged);
        QJsonArray entries;
        for (int i = 0; i < 1200; ++i)
            entries.append(
                QJsonObject{{text("ThreeLetterISOLanguageName"), QStringLiteral("l%1").arg(i)},
                            {text("DisplayName"), QStringLiteral("Language %1").arg(i)}});
        cultures.setCultures(entries);
        QCOMPARE(cultures.rowCount(), int(CultureOptionsModel::MaxOptions));
        QCOMPARE(available.size(), 1);

        entries = QJsonArray{
            QJsonObject{{text("ThreeLetterISOLanguageName"), text("fra")},
                        {text("ThreeLetterISOLanguageNames"), QJsonArray{text("fra"), text("fre")}},
                        {text("DisplayName"), text("<b>French</b> & more")}},
            QJsonObject{{text("ThreeLetterISOLanguageName"), text("FRA")},
                        {text("DisplayName"), text("Duplicate")}},
            QJsonObject{{text("ThreeLetterISOLanguageName"), text("../x")},
                        {text("DisplayName"), text("Bad code")}},
            QJsonObject{{text("ThreeLetterISOLanguageName"), QString(40, QLatin1Char('a'))},
                        {text("DisplayName"), text("Too long")}},
            QJsonObject{{text("Name"), text("NoCode")}},
            QJsonObject{{text("ThreeLetterISOLanguageName"), text("deu")},
                        {text("Name"), text("German")}},
            QJsonValue(5).toObject()};
        cultures.setCultures(entries);
        QCOMPARE(cultures.rowCount(), 2);
        QCOMPARE(cultures.indexOfCode(text("FRE")), 0);
        QCOMPARE(cultures.nameOf(text("fra")), text("<b>French</b> & more"));
        QCOMPARE(cultures.nameOf(text("deu")), text("German"));
        QVERIFY(!cultures.contains(text("../x")));
        cultures.setCultures({});
        QVERIFY(!cultures.available());
        QCOMPARE(available.size(), 2);
    }
};

QTEST_MAIN(TestUserConfigurationModel)
#include "test_user_configuration_model.moc"
