// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The browse state store: per server and user round trips, the version 1 migration, the private file, the debounced write, the caps and what is dropped or kept aside on load
#include "settings/BrowseStateStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <chrono>
#include <memory>

#include <sys/stat.h>

using Kinefin::BrowseStateStore;
using ViewState = BrowseStateStore::ViewState;

namespace {
const QString kServer = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
const QString kOtherServer = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
const QString kUser = QStringLiteral("cccccccccccccccccccccccccccccccc");
const QString kOtherUser = QStringLiteral("dddddddddddddddddddddddddddddddd");
const QString kLibrary = QStringLiteral("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
const QString kGenre = QStringLiteral("ffffffffffffffffffffffffffffffff");
const QString kMovies = QStringLiteral("movies");
const QString kShows = QStringLiteral("tvshows");

// `filter` is a version 1 filter name: `All` (no flag), `Unplayed`, `Played`, `Resumable` or `Favorites`
ViewState state(const QString &sort, bool descending, const QString &filter,
                const QStringList &genreIds = {})
{
    ViewState result;
    result.sortKey = sort;
    result.descending = descending;
    if (filter != QLatin1String("All"))
        result.statusFlags = {filter == QLatin1String("Favorites")
                                  ? QStringLiteral("IsFavorite")
                                  : QString(QStringLiteral("Is") + filter)};
    result.genreIds = genreIds;
    return result;
}

// Zero padded: a padding that is itself a hex digit would make different indexes the same id
QString hex(int index)
{
    return QStringLiteral("%1").arg(index, 32, 16, QLatin1Char('0'));
}

QString hash(int index)
{
    return QStringLiteral("%1").arg(index, 64, 16, QLatin1Char('0'));
}

QJsonObject goodView(qint64 seq = 1)
{
    return QJsonObject{{QStringLiteral("sortKey"), QStringLiteral("Name")},
                       {QStringLiteral("descending"), false},
                       {QStringLiteral("statusFlags"), QJsonArray()},
                       {QStringLiteral("genreIds"), QJsonArray()},
                       {QStringLiteral("seq"), seq}};
}

// The version 1 form of a view
QJsonObject v1View(qint64 seq = 1)
{
    return QJsonObject{{QStringLiteral("sort"), QStringLiteral("Name")},
                       {QStringLiteral("descending"), false},
                       {QStringLiteral("filter"), QStringLiteral("All")},
                       {QStringLiteral("genreIds"), QJsonArray()},
                       {QStringLiteral("seq"), seq}};
}

const QJsonValue kAbsent(QJsonValue::Undefined);

// A copy of the view with one member replaced (or removed when `value` is `kAbsent`)
QJsonObject viewWith(QJsonObject view, const QString &key, const QJsonValue &value)
{
    if (value.isUndefined())
        view.remove(key);
    else
        view.insert(key, value);
    return view;
}

// A file with one server and one user holding the given selection and views
QByteArray fileFor(const QJsonObject &selection, const QJsonObject &views, int version = 2,
                   const QJsonObject &userExtra = {})
{
    QJsonObject user = userExtra;
    user.insert(QStringLiteral("selection"), selection);
    user.insert(QStringLiteral("views"), views);
    const QJsonObject server{
        {QStringLiteral("users"), QJsonObject{{kUser, user}}},
    };
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), version},
                                     {QStringLiteral("nextSeq"), 100},
                                     {QStringLiteral("servers"), QJsonObject{{kServer, server}}}})
        .toJson(QJsonDocument::Compact);
}

QString libraryScope(int index)
{
    return QStringLiteral("movies:lib%1").arg(index);
}

struct Fixture {
    // A store on the fixture's file; the write waits for `flush()` or the destructor unless a delay is given
    std::unique_ptr<BrowseStateStore>
    open(std::chrono::milliseconds delay = std::chrono::minutes(10)) const
    {
        auto store = std::make_unique<BrowseStateStore>();
        store->setSaveDelay(delay);
        store->setStoragePath(path);
        return store;
    }
    QByteArray stored() const
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    void write(const QByteArray &bytes) const
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }

    QTemporaryDir dir;
    QString path = dir.filePath(QStringLiteral("profile/browse.json"));
};

bool holds(const BrowseStateStore &store, const QString &serverId, const QString &userId,
           const QString &scope, const ViewState &expected)
{
    const std::optional<ViewState> found = store.view(serverId, userId, scope);
    return found && *found == expected;
}
// Every field away from its default
ViewState fullState()
{
    ViewState full;
    full.sortKey = QStringLiteral("CommunityRating");
    full.descending = true;
    full.statusFlags = {QStringLiteral("IsUnplayed"), QStringLiteral("IsFavorite"),
                        QStringLiteral("IsResumable")};
    full.features = {QStringLiteral("HasSubtitles"), QStringLiteral("HasTrailer"),
                     QStringLiteral("HasSpecialFeature"), QStringLiteral("HasThemeSong"),
                     QStringLiteral("HasThemeVideo")};
    full.quality = QStringLiteral("HD");
    full.is4K = true;
    full.is3D = true;
    full.videoTypes = {QStringLiteral("BluRay"), QStringLiteral("Dvd")};
    full.genreIds = {kGenre, kLibrary};
    full.tagHashes = {BrowseStateStore::hashText(QStringLiteral("Cult Classic"))};
    full.ratingHashes = {BrowseStateStore::hashText(QStringLiteral("PG-13")),
                         BrowseStateStore::hashText(QStringLiteral("R"))};
    full.years = {1800, 2024, 2200};
    full.seriesStatus = {QStringLiteral("Continuing"), QStringLiteral("Ended"),
                         QStringLiteral("Unreleased")};
    full.episodeKind = QStringLiteral("Unaired");
    full.viewMode = QStringLiteral("list");
    full.imageType = QStringLiteral("Thumb");
    full.showTitle = false;
    full.showYear = false;
    full.cardLayout = false;
    return full;
}
} // namespace

class TestBrowseStateStore : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void roundTripPerScopeAndUser()
    {
        Fixture f;
        const ViewState a =
            state(QStringLiteral("DateAdded"), true, QStringLiteral("Unplayed"), {kGenre});
        const ViewState b = state(QStringLiteral("Name"), false, QStringLiteral("All"));
        const ViewState c = state(QStringLiteral("Runtime"), true, QStringLiteral("Favorites"));
        {
            auto store = f.open();
            store->setSelection(kServer, kUser, kMovies, kLibrary);
            store->setSelection(kServer, kUser, kShows, QString());
            store->setView(kServer, kUser, QStringLiteral("movies"), a);
            store->setView(kServer, kUser, QStringLiteral("movies:") + kLibrary, b);
            store->setView(kServer, kOtherUser, QStringLiteral("movies"), c);
            store->setView(kOtherServer, kUser, QStringLiteral("boxsets:") + kLibrary, c);
            store->flush();
        }
        auto store = f.open();
        QCOMPARE(store->selection(kServer, kUser, kMovies), kLibrary);
        QCOMPARE(store->selection(kServer, kUser, kShows), QString());
        QCOMPARE(store->selection(kServer, kOtherUser, kMovies), QString());
        QVERIFY(holds(*store, kServer, kUser, QStringLiteral("movies"), a));
        QVERIFY(holds(*store, kServer, kUser, QStringLiteral("movies:") + kLibrary, b));
        QVERIFY(holds(*store, kServer, kOtherUser, QStringLiteral("movies"), c));
        QVERIFY(holds(*store, kOtherServer, kUser, QStringLiteral("boxsets:") + kLibrary, c));
        QVERIFY(!store->view(kServer, kUser, QStringLiteral("tvshows")));
        QVERIFY(!store->view(kOtherServer, kOtherUser, QStringLiteral("movies")));
        // The enums are stored as their key names
        QVERIFY(f.stored().contains("\"DateAdded\""));
    }

    // Named filters: per user and kind, a name unique without regard to case, a limit, and the same file rules as the views
    void savedFiltersRoundTripAndKeepTheirRules()
    {
        Fixture f;
        using Result = BrowseStateStore::SaveResult;
        using Rename = BrowseStateStore::RenameResult;
        ViewState unwatched = state(QStringLiteral("SortName"), false, QStringLiteral("Unplayed"));
        unwatched.is4K = true;
        unwatched.tagHashes = {BrowseStateStore::hashText(QStringLiteral("Dutch"))};
        const ViewState favorite =
            state(QStringLiteral("SortName"), false, QStringLiteral("Favorites"));
        {
            auto store = f.open();
            QCOMPARE(store->saveFilter(kServer, kUser, QStringLiteral("movies"),
                                       QStringLiteral("Unwatched 4K"), unwatched, false),
                     Result::Saved);
            // A name is taken without regard to case, unless it is to be replaced
            QCOMPARE(store->saveFilter(kServer, kUser, QStringLiteral("movies"),
                                       QStringLiteral("unwatched 4k"), favorite, false),
                     Result::Exists);
            QCOMPARE(store->saveFilter(kServer, kUser, QStringLiteral("movies"),
                                       QStringLiteral("unwatched 4k"), favorite, true),
                     Result::Replaced);
            // Another kind and another user have their own
            QCOMPARE(store->saveFilter(kServer, kUser, QStringLiteral("series"),
                                       QStringLiteral("unwatched 4k"), unwatched, false),
                     Result::Saved);
            QCOMPARE(store->saveFilter(kServer, kOtherUser, QStringLiteral("movies"),
                                       QStringLiteral("unwatched 4k"), unwatched, false),
                     Result::Saved);
            store->flush();
        }
        auto store = f.open();
        const auto movies = store->savedFilters(kServer, kUser, QStringLiteral("movies"));
        QCOMPARE(movies.size(), 1);
        QCOMPARE(movies.constFirst().name, QStringLiteral("unwatched 4k"));
        QCOMPARE(movies.constFirst().state.statusFlags, QStringList{QStringLiteral("IsFavorite")});
        QCOMPARE(store->savedFilters(kServer, kUser).size(), 2);
        const auto series = store->savedFilter(kServer, kUser, QStringLiteral("series"),
                                               QStringLiteral("UNWATCHED 4K"));
        QVERIFY(series);
        QCOMPARE(series->tagHashes, unwatched.tagHashes);
        QVERIFY(!store->savedFilter(kServer, kUser, QStringLiteral("episodes"),
                                    QStringLiteral("unwatched 4k")));
        // The tag is a digest in the file, never its text
        QVERIFY(!f.stored().contains("Dutch"));

        // Rename keeps the set, a taken name refuses, a missing one is missing
        QCOMPARE(store->saveFilter(kServer, kUser, QStringLiteral("movies"),
                                   QStringLiteral("Second"), unwatched, false),
                 Result::Saved);
        QCOMPARE(store->renameSavedFilter(kServer, kUser, QStringLiteral("movies"),
                                          QStringLiteral("Second"), QStringLiteral("UNWATCHED 4K")),
                 Rename::Exists);
        QCOMPARE(store->renameSavedFilter(kServer, kUser, QStringLiteral("movies"),
                                          QStringLiteral("Second"), QStringLiteral("Third")),
                 Rename::Renamed);
        // The same name in another case is a rename of the same set
        QCOMPARE(store->renameSavedFilter(kServer, kUser, QStringLiteral("movies"),
                                          QStringLiteral("third"), QStringLiteral("THIRD")),
                 Rename::Renamed);
        QCOMPARE(store->renameSavedFilter(kServer, kUser, QStringLiteral("movies"),
                                          QStringLiteral("none"), QStringLiteral("Other")),
                 Rename::Missing);
        QCOMPARE(store->renameSavedFilter(kServer, kUser, QStringLiteral("movies"),
                                          QStringLiteral("THIRD"), QStringLiteral(" padded")),
                 Rename::Invalid);
        QVERIFY(
            store->savedFilter(kServer, kUser, QStringLiteral("movies"), QStringLiteral("third")));
        QVERIFY(store->removeSavedFilter(kServer, kUser, QStringLiteral("movies"),
                                         QStringLiteral("third")));
        QVERIFY(!store->removeSavedFilter(kServer, kUser, QStringLiteral("movies"),
                                          QStringLiteral("third")));
        QCOMPARE(store->savedFilters(kServer, kUser, QStringLiteral("movies")).size(), 1);
    }

    void savedFilterNamesKindsAndLimitsAreChecked()
    {
        Fixture f;
        using Result = BrowseStateStore::SaveResult;
        auto store = f.open();
        const ViewState view = state(QStringLiteral("SortName"), false, QStringLiteral("Unplayed"));
        const auto save = [&](const QString &kind, const QString &name) {
            return store->saveFilter(kServer, kUser, kind, name, view, false);
        };
        const QString movies = QStringLiteral("movies");
        QCOMPARE(save(movies, QString()), Result::Invalid);
        QCOMPARE(save(movies, QStringLiteral(" padded")), Result::Invalid);
        QCOMPARE(save(movies, QStringLiteral("padded ")), Result::Invalid);
        QCOMPARE(save(movies, QStringLiteral("two\nlines")), Result::Invalid);
        QCOMPARE(save(movies, QString(41, QLatin1Char('a'))), Result::Invalid);
        QCOMPARE(save(movies, QString(40, QLatin1Char('a'))), Result::Saved);
        QCOMPARE(save(QStringLiteral("tvshows"), QStringLiteral("kind")), Result::Invalid);
        QCOMPARE(store->saveFilter(QStringLiteral("not an id"), kUser, movies,
                                   QStringLiteral("server"), view, false),
                 Result::Invalid);
        // A state the store would not accept as a view is refused too, and so are more tags than a query carries
        ViewState bad = view;
        bad.sortKey = QStringLiteral("not a token!");
        QCOMPARE(store->saveFilter(kServer, kUser, movies, QStringLiteral("bad"), bad, false),
                 Result::Invalid);
        ViewState manyTags = view;
        for (int i = 0; i <= BrowseStateStore::MaxSavedHashes; ++i)
            manyTags.tagHashes.append(hash(i + 1));
        QCOMPARE(store->saveFilter(kServer, kUser, movies, QStringLiteral("tags"), manyTags, false),
                 Result::Invalid);
        // Fifty a user, then it is full; a replace of one that exists is still allowed
        for (int i = 1; i < BrowseStateStore::MaxSavedFilters; ++i)
            QCOMPARE(save(movies, QStringLiteral("set %1").arg(i)), Result::Saved);
        QCOMPARE(save(movies, QStringLiteral("one too many")), Result::Full);
        QCOMPARE(store->saveFilter(kServer, kUser, movies, QStringLiteral("set 1"), view, true),
                 Result::Replaced);
        // By kind and then by name, without regard to case
        const auto names = store->savedFilters(kServer, kUser, movies);
        QCOMPARE(names.size(), BrowseStateStore::MaxSavedFilters);
        for (qsizetype i = 1; i < names.size(); ++i)
            QVERIFY(names.at(i - 1).name.compare(names.at(i).name, Qt::CaseInsensitive) < 0);
    }

    void aBadSavedFilterInTheFileIsDroppedAlone()
    {
        Fixture f;
        QJsonObject good = goodView(5);
        good.insert(QStringLiteral("kind"), QStringLiteral("movies"));
        good.insert(QStringLiteral("name"), QStringLiteral("Good"));
        QJsonObject badKind = good;
        badKind.insert(QStringLiteral("kind"), QStringLiteral("music"));
        badKind.insert(QStringLiteral("name"), QStringLiteral("Bad kind"));
        QJsonObject badName = good;
        badName.insert(QStringLiteral("name"), QStringLiteral(" untrimmed"));
        QJsonObject duplicate = good;
        duplicate.insert(QStringLiteral("name"), QStringLiteral("GOOD"));
        QJsonObject badView = good;
        badView.insert(QStringLiteral("name"), QStringLiteral("Bad view"));
        badView.insert(QStringLiteral("sortKey"), QStringLiteral("no way!"));
        QJsonObject badSeq = good;
        badSeq.insert(QStringLiteral("name"), QStringLiteral("Bad seq"));
        badSeq.insert(QStringLiteral("seq"), -3);
        const QJsonObject user{{QStringLiteral("savedFilters"),
                                QJsonArray{good, badKind, badName, duplicate, badView, badSeq}}};
        f.write(
            QJsonDocument(
                QJsonObject{{QStringLiteral("version"), 2},
                            {QStringLiteral("nextSeq"), 100},
                            {QStringLiteral("servers"),
                             QJsonObject{{kServer, QJsonObject{{QStringLiteral("users"),
                                                                QJsonObject{{kUser, user}}}}}}}})
                .toJson(QJsonDocument::Compact));
        auto store = f.open();
        const auto saved = store->savedFilters(kServer, kUser);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.constFirst().name, QStringLiteral("Good"));
        // A new set takes a number past the one read
        QCOMPARE(store->saveFilter(kServer, kUser, QStringLiteral("movies"), QStringLiteral("Next"),
                                   ViewState(), false),
                 BrowseStateStore::SaveResult::Saved);
    }

    void aSetOverTheHashLimitIsNotLoadedAndTheSetsAloneNeverPassTheFileLimit()
    {
        Fixture f;
        // A hand-edited file with a set past the limit of tags
        QJsonObject over = goodView(5);
        over.insert(QStringLiteral("kind"), QStringLiteral("movies"));
        over.insert(QStringLiteral("name"), QStringLiteral("Over"));
        QJsonArray hashes;
        for (int i = 0; i <= BrowseStateStore::MaxSavedHashes; ++i)
            hashes.append(hash(i + 1));
        over.insert(QStringLiteral("tagHashes"), hashes);
        const QJsonObject user{{QStringLiteral("savedFilters"), QJsonArray{over}}};
        f.write(
            QJsonDocument(
                QJsonObject{{QStringLiteral("version"), 3},
                            {QStringLiteral("nextSeq"), 100},
                            {QStringLiteral("servers"),
                             QJsonObject{{kServer, QJsonObject{{QStringLiteral("users"),
                                                                QJsonObject{{kUser, user}}}}}}}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(f.open()->savedFilters(kServer, kUser).isEmpty());

        // Sets as large as they may be: a save that would take the sets past the file limit is refused, and nothing a person named is dropped
        auto store = f.open();
        ViewState big = state(QStringLiteral("SortName"), false, QStringLiteral("Unplayed"));
        for (int i = 0; i < BrowseStateStore::MaxSavedHashes; ++i) {
            big.tagHashes.append(hash(i + 1));
            big.ratingHashes.append(hash(i + 101));
        }
        for (int i = 0; i < BrowseStateStore::MaxGenreIds; ++i)
            big.genreIds.append(hex(i + 1));
        for (int i = 0; i < BrowseStateStore::MaxYears; ++i)
            big.years.append(1900 + i);
        QStringList saved;
        for (int i = 0; i < BrowseStateStore::MaxSavedFilters; ++i) {
            const QString name = QStringLiteral("big %1").arg(i);
            const auto result =
                store->saveFilter(kServer, kUser, QStringLiteral("movies"), name, big, false);
            if (result == BrowseStateStore::SaveResult::Full)
                break;
            QCOMPARE(result, BrowseStateStore::SaveResult::Saved);
            saved << name;
        }
        QVERIFY(!saved.isEmpty());
        store->flush();
        QVERIFY(f.stored().size() <= BrowseStateStore::MaxFileBytes);
        auto reopened = f.open();
        QCOMPARE(reopened->savedFilters(kServer, kUser).size(), saved.size());
        // A refused replace leaves the set it meant to replace
        if (saved.size() < BrowseStateStore::MaxSavedFilters) {
            QCOMPARE(store->saveFilter(kServer, kUser, QStringLiteral("movies"),
                                       QStringLiteral("one more"), big, false),
                     BrowseStateStore::SaveResult::Full);
            QVERIFY(!store->savedFilter(kServer, kUser, QStringLiteral("movies"),
                                        QStringLiteral("one more")));
        }
    }

    void aFileOverTheSizeLimitLosesViewsBeforeTheSetsANameWasGiven()
    {
        Fixture f;
        auto store = f.open();
        ViewState big = state(QStringLiteral("SortName"), false, QStringLiteral("Unplayed"));
        for (int i = 0; i < BrowseStateStore::MaxHashes; ++i) {
            big.tagHashes.append(hash(i + 1));
            big.ratingHashes.append(hash(i + 101));
        }
        for (int i = 0; i < BrowseStateStore::MaxViews; ++i)
            store->setView(kServer, kUser, libraryScope(i + 1), big);
        QVERIFY(store->saveFilter(
                    kServer, kUser, QStringLiteral("movies"), QStringLiteral("Kept"),
                    state(QStringLiteral("SortName"), false, QStringLiteral("Played")), false)
                == BrowseStateStore::SaveResult::Saved);
        store->flush();
        QVERIFY(f.stored().size() <= BrowseStateStore::MaxFileBytes);
        auto reopened = f.open();
        QVERIFY(reopened->savedFilter(kServer, kUser, QStringLiteral("movies"),
                                      QStringLiteral("Kept")));
    }

    void fileIsPrivate()
    {
        Fixture f;
        auto store = f.open();
        store->setView(kServer, kUser, QStringLiteral("movies"),
                       state(QStringLiteral("Name"), false, QStringLiteral("All")));
        store->flush();
        QVERIFY(QFileInfo::exists(f.path));
        const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                            | QFileDevice::ReadOther | QFileDevice::WriteOther
                            | QFileDevice::ExeOther;
        const QFileDevice::Permissions file = QFileInfo(f.path).permissions();
        QVERIFY(file.testFlag(QFileDevice::ReadOwner) && file.testFlag(QFileDevice::WriteOwner));
        QVERIFY(!(file & others));
        QVERIFY(!file.testFlag(QFileDevice::ExeOwner));
        const QFileDevice::Permissions folder =
            QFileInfo(QFileInfo(f.path).absolutePath()).permissions();
        QVERIFY(!(folder & others));
    }

    void debouncedAndFlushedOnDestruction()
    {
        Fixture f;
        {
            // Edits within the delay make one write, after it
            auto store = f.open(std::chrono::milliseconds(50));
            store->setSelection(kServer, kUser, kMovies, kLibrary);
            store->setView(kServer, kUser, QStringLiteral("movies"),
                           state(QStringLiteral("Name"), false, QStringLiteral("All")));
            QVERIFY(!QFileInfo::exists(f.path));
            QTRY_VERIFY(QFileInfo::exists(f.path));
        }
        QFile::remove(f.path);
        auto store = std::make_unique<BrowseStateStore>();
        store->setSaveDelay(std::chrono::minutes(10));
        store->setStoragePath(f.path);
        store->setSelection(kServer, kUser, kShows, kLibrary);
        QVERIFY(!QFileInfo::exists(f.path));
        store.reset();
        QVERIFY(f.stored().contains(kLibrary.toUtf8()));
    }

    void lruKeeps200()
    {
        Fixture f;
        const ViewState a = state(QStringLiteral("Name"), false, QStringLiteral("All"));
        const ViewState changed = state(QStringLiteral("Name"), true, QStringLiteral("All"));
        auto store = f.open();
        for (int i = 0; i <= BrowseStateStore::MaxViews; ++i)
            store->setView(kServer, kUser, libraryScope(i), a);
        // 201 written: the first one is gone
        QVERIFY(!store->view(kServer, kUser, libraryScope(0)));
        QVERIFY(store->view(kServer, kUser, libraryScope(1)));
        QVERIFY(store->view(kServer, kUser, libraryScope(BrowseStateStore::MaxViews)));
        // Writing a view again makes it the newest
        store->setView(kServer, kUser, libraryScope(1), changed);
        store->setView(kServer, kUser, libraryScope(1000), a);
        QVERIFY(holds(*store, kServer, kUser, libraryScope(1), changed));
        QVERIFY(!store->view(kServer, kUser, libraryScope(2)));
        store->flush();
        auto again = f.open();
        int kept = 0;
        for (int i = 0; i <= BrowseStateStore::MaxViews; ++i)
            kept += again->view(kServer, kUser, libraryScope(i)).has_value();
        QCOMPARE(kept, BrowseStateStore::MaxViews - 1);
        QVERIFY(again->view(kServer, kUser, libraryScope(1000)));
        // A file written by hand with more views than the cap is cut on load, oldest first
        QJsonObject views;
        for (int i = 0; i < 250; ++i)
            views.insert(libraryScope(i), goodView(i));
        f.write(fileFor({}, views));
        auto crowded = f.open();
        QVERIFY(!crowded->view(kServer, kUser, libraryScope(49)));
        QVERIFY(crowded->view(kServer, kUser, libraryScope(50)));
        QVERIFY(crowded->view(kServer, kUser, libraryScope(249)));
    }

    void sizeCapEvictsTheOldestViewsFirst()
    {
        Fixture f;
        // The worst case of one view: every selection at its cap, which is far more than MaxViews of them could fit in MaxFileBytes
        ViewState big = state(QStringLiteral("Name"), false, QStringLiteral("All"));
        for (int i = 0; i < BrowseStateStore::MaxGenreIds; ++i)
            big.genreIds.append(hex(i));
        for (int i = 0; i < BrowseStateStore::MaxHashes; ++i) {
            big.tagHashes.append(hash(i));
            big.ratingHashes.append(hash(1000 + i));
        }
        for (int i = 0; i < BrowseStateStore::MaxYears; ++i)
            big.years.append(BrowseStateStore::MinYear + i);
        constexpr int count = BrowseStateStore::MaxViews;
        {
            auto store = f.open();
            for (int i = 0; i < count; ++i)
                store->setView(kServer, kUser, libraryScope(i), big);
            store->flush();
        }
        QVERIFY(QFileInfo(f.path).size() <= BrowseStateStore::MaxFileBytes);
        auto store = f.open();
        QVERIFY(!store->view(kServer, kUser, libraryScope(0)));
        QVERIFY(holds(*store, kServer, kUser, libraryScope(count - 1), big));
        // The byte cap, not MaxViews, is what binds: the views kept are exactly the newest ones, without a gap
        int kept = 0;
        bool seenKept = false;
        for (int i = 0; i < count; ++i) {
            const bool has = store->view(kServer, kUser, libraryScope(i)).has_value();
            kept += has;
            QVERIFY2(has || !seenKept, "a newer view was dropped before an older one");
            seenKept = seenKept || has;
        }
        QVERIFY(kept > 0 && kept < count / 2);
    }

    void oversizedDeletedUnread()
    {
        Fixture f;
        // Valid, with a selection: it would load if it were read
        QJsonObject root = QJsonDocument::fromJson(fileFor({{kMovies, kLibrary}}, {})).object();
        root.insert(QStringLiteral("padding"),
                    QString(int(BrowseStateStore::MaxFileBytes), QLatin1Char('x')));
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        QVERIFY(QFileInfo(f.path).size() > BrowseStateStore::MaxFileBytes);
        auto store = f.open();
        QCOMPARE(store->selection(kServer, kUser, kMovies), QString());
        QVERIFY(!QFileInfo::exists(f.path));
    }

    void symlinkNotFollowed()
    {
        Fixture f;
        // A link to a valid file
        const QString target = f.dir.filePath(QStringLiteral("target.json"));
        QFile real(target);
        QVERIFY(real.open(QIODevice::WriteOnly));
        real.write(fileFor({{kMovies, kLibrary}}, {}));
        real.close();
        QDir().mkpath(QFileInfo(f.path).absolutePath());
        QVERIFY(QFile::link(target, f.path));
        {
            auto store = f.open();
            QCOMPARE(store->selection(kServer, kUser, kMovies), QString());
        }
        QVERIFY(!QFileInfo(f.path).isSymLink());
        QVERIFY(!QFileInfo::exists(f.path));
        QVERIFY(QFileInfo::exists(target));
        // A dangling link is in the way as well
        QVERIFY(QFile::link(f.dir.filePath(QStringLiteral("missing.json")), f.path));
        f.open();
        QVERIFY(!QFileInfo(f.path).isSymLink());
        // A pipe would block a read
        QVERIFY(::mkfifo(QFile::encodeName(f.path).constData(), 0600) == 0);
        f.open();
        QVERIFY(!QFileInfo::exists(f.path));
    }

    void newerVersionRenamedBroken()
    {
        Fixture f;
        const QByteArray bytes = fileFor({{kMovies, kLibrary}}, {}, 4);
        f.write(bytes);
        const QString aside = f.path + QStringLiteral(".broken");
        QFile old(aside);
        QVERIFY(old.open(QIODevice::WriteOnly));
        old.write("older");
        old.close();
        auto store = f.open();
        QCOMPARE(store->selection(kServer, kUser, kMovies), QString());
        QVERIFY(!QFileInfo::exists(f.path));
        QFile kept(aside);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), bytes);
    }

    void aNewerFileThatCannotBeSetAsideIsNotOverwritten()
    {
        Fixture f;
        const QByteArray bytes = fileFor({{kMovies, kLibrary}}, {}, 4);
        f.write(bytes);
        // A directory with a file in it blocks both the removal and the rename of the .broken name
        const QString aside = f.path + QStringLiteral(".broken");
        QVERIFY(QDir().mkpath(aside));
        QFile blocker(aside + QStringLiteral("/keep"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        auto store = f.open();
        store->setSelection(kServer, kUser, kMovies, kLibrary);
        store->flush();
        QFile kept(f.path);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), bytes);
    }

    void aSequenceNumberBeyondTheLimitDropsTheView()
    {
        Fixture f;
        const QJsonObject views{{QStringLiteral("movies"),
                                 viewWith(goodView(), QStringLiteral("seq"), qint64(1) << 60)}};
        f.write(fileFor({}, views));
        auto store = f.open();
        QVERIFY(!store->view(kServer, kUser, QStringLiteral("movies")));
    }

    void badEntriesDroppedInVersion1()
    {
        Fixture f;
        const QJsonObject selection{
            {kMovies, kLibrary},
            // Not a tab type
            {QStringLiteral("music"), kLibrary},
            // Not a string
            {kShows, 12},
        };
        QJsonObject views{
            {QStringLiteral("movies"), v1View()},
            {QStringLiteral("tvshows"),
             viewWith(v1View(), QStringLiteral("filter"), QStringLiteral("Resumable"))},
            {QStringLiteral("movies:bad id"), v1View()},
            {QStringLiteral("radio"), v1View()},
            {QStringLiteral("boxsets:") + kLibrary, v1View()},
            {QStringLiteral("movies:badsort"),
             viewWith(v1View(), QStringLiteral("sort"), QStringLiteral("Date Added"))},
            {QStringLiteral("movies:nofilter"),
             viewWith(v1View(), QStringLiteral("filter"), kAbsent)},
            {QStringLiteral("movies:badgenre"),
             viewWith(v1View(), QStringLiteral("genreIds"),
                      QJsonArray{kGenre, QStringLiteral("a/b")})},
            {QStringLiteral("movies:nodescending"),
             viewWith(v1View(), QStringLiteral("descending"), kAbsent)},
            {QStringLiteral("movies:negseq"), viewWith(v1View(), QStringLiteral("seq"), -1)},
            {QStringLiteral("movies:notobject"), 5},
        };
        QJsonObject root = QJsonDocument::fromJson(fileFor(selection, views, 1)).object();
        // Invalid server and user ids
        QJsonObject servers = root.value(QStringLiteral("servers")).toObject();
        servers.insert(QStringLiteral("bad server"), servers.value(kServer));
        QJsonObject server = servers.value(kServer).toObject();
        QJsonObject users = server.value(QStringLiteral("users")).toObject();
        users.insert(QStringLiteral("bad user"), users.value(kUser));
        server.insert(QStringLiteral("users"), users);
        servers.insert(kServer, server);
        root.insert(QStringLiteral("servers"), servers);
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));

        auto store = f.open();
        QCOMPARE(store->selection(kServer, kUser, kMovies), kLibrary);
        QCOMPARE(store->selection(kServer, kUser, kShows), QString());
        const ViewState good = state(QStringLiteral("Name"), false, QStringLiteral("All"));
        QVERIFY(holds(*store, kServer, kUser, QStringLiteral("movies"), good));
        QVERIFY(holds(*store, kServer, kUser, QStringLiteral("boxsets:") + kLibrary, good));
        // `Resumable` has no meaning over series
        QVERIFY(holds(*store, kServer, kUser, QStringLiteral("tvshows"), good));
        for (const QString &scope :
             {QStringLiteral("movies:bad id"), QStringLiteral("radio"),
              QStringLiteral("movies:badsort"), QStringLiteral("movies:nofilter"),
              QStringLiteral("movies:badgenre"), QStringLiteral("movies:nodescending"),
              QStringLiteral("movies:negseq"), QStringLiteral("movies:notobject")})
            QVERIFY2(!store->view(kServer, kUser, scope), qPrintable(scope));
        QVERIFY(!store->view(QStringLiteral("bad server"), kUser, QStringLiteral("movies")));
        QVERIFY(!store->view(kServer, QStringLiteral("bad user"), QStringLiteral("movies")));
    }

    void malformedTopLevelIsDeleted_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("not json") << QByteArray("not json");
        QTest::newRow("an array") << QByteArray("[]");
        QTest::newRow("no version") << QByteArray(R"({"servers":{}})");
        QTest::newRow("version as text") << QByteArray(R"({"version":"1","servers":{}})");
        QTest::newRow("version zero") << QByteArray(R"({"version":0,"servers":{}})");
        QTest::newRow("servers not an object") << QByteArray(R"({"version":1,"servers":[]})");
    }

    void malformedTopLevelIsDeleted()
    {
        QFETCH(QByteArray, bytes);
        Fixture f;
        f.write(bytes);
        auto store = f.open();
        QVERIFY(!QFileInfo::exists(f.path));
        QVERIFY(!QFileInfo::exists(f.path + QStringLiteral(".broken")));
        QCOMPARE(store->selection(kServer, kUser, kMovies), QString());
    }

    void unknownFieldsIgnored()
    {
        Fixture f;
        QJsonObject view = goodView();
        view.insert(QStringLiteral("view"), QStringLiteral("Poster"));
        view.insert(QStringLiteral("later"), QJsonObject{{QStringLiteral("x"), 1}});
        QJsonObject root = QJsonDocument::fromJson(
                               fileFor({{kMovies, kLibrary}}, {{QStringLiteral("movies"), view}}))
                               .object();
        root.insert(QStringLiteral("extra"), true);
        QJsonObject servers = root.value(QStringLiteral("servers")).toObject();
        QJsonObject server = servers.value(kServer).toObject();
        server.insert(QStringLiteral("note"), QStringLiteral("x"));
        servers.insert(kServer, server);
        root.insert(QStringLiteral("servers"), servers);
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        auto store = f.open();
        QCOMPARE(store->selection(kServer, kUser, kMovies), kLibrary);
        QVERIFY(holds(*store, kServer, kUser, QStringLiteral("movies"),
                      state(QStringLiteral("Name"), false, QStringLiteral("All"))));
        QVERIFY(QFileInfo::exists(f.path));
    }

    void forgetServerDropsAllUsers()
    {
        Fixture f;
        const ViewState a = state(QStringLiteral("Name"), false, QStringLiteral("All"));
        {
            auto store = f.open();
            store->setSelection(kServer, kUser, kMovies, kLibrary);
            store->setView(kServer, kUser, QStringLiteral("movies"), a);
            store->setView(kServer, kOtherUser, QStringLiteral("movies"), a);
            store->setView(kOtherServer, kUser, QStringLiteral("movies"), a);
            store->forgetServer(kServer);
            QVERIFY(!store->view(kServer, kUser, QStringLiteral("movies")));
            QVERIFY(!store->view(kServer, kOtherUser, QStringLiteral("movies")));
            QCOMPARE(store->selection(kServer, kUser, kMovies), QString());
            store->flush();
        }
        auto store = f.open();
        QVERIFY(!store->view(kServer, kUser, QStringLiteral("movies")));
        QVERIFY(!store->view(kServer, kOtherUser, QStringLiteral("movies")));
        QVERIFY(holds(*store, kOtherServer, kUser, QStringLiteral("movies"), a));
        // Nothing left: no file
        store->forgetServer(kOtherServer);
        store->flush();
        QVERIFY(!QFileInfo::exists(f.path));
    }

    void version2RoundTripOfEveryField()
    {
        Fixture f;
        const ViewState full = fullState();
        ViewState other = fullState();
        other.statusFlags.clear();
        other.imageType = QStringLiteral("Banner");
        const QString scope = QStringLiteral("movies/favorites:") + kLibrary;
        {
            auto store = f.open();
            store->setView(kServer, kUser, scope, full);
            store->setView(kServer, kUser, QStringLiteral("tvshows/episodes"), other);
            store->flush();
        }
        QCOMPARE(
            QJsonDocument::fromJson(f.stored()).object().value(QStringLiteral("version")).toInt(),
            3);
        auto store = f.open();
        QVERIFY(holds(*store, kServer, kUser, scope, full));
        QVERIFY(holds(*store, kServer, kUser, QStringLiteral("tvshows/episodes"), other));
        // The defaults are what a view starts from
        const ViewState defaults;
        QCOMPARE(defaults.sortKey, QStringLiteral("SortName"));
        QCOMPARE(defaults.viewMode, QStringLiteral("grid"));
        QCOMPARE(defaults.imageType, QStringLiteral("Primary"));
        QCOMPARE(defaults.episodeKind, QStringLiteral("All"));
        QVERIFY(defaults.showTitle && defaults.showYear && defaults.cardLayout);
    }

    void tagAndRatingTextsNeverInTheFile()
    {
        Fixture f;
        const QString tag = QStringLiteral("Zebra Secret Tag");
        const QString rating = QStringLiteral("TV-MA-Zebra");
        // SHA-256 of "abc", from FIPS 180-2
        QCOMPARE(
            BrowseStateStore::hashText(QStringLiteral("abc")),
            QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        // The exact text is hashed: case and surrounding blanks count
        QVERIFY(BrowseStateStore::hashText(tag) != BrowseStateStore::hashText(tag.toLower()));
        QVERIFY(BrowseStateStore::hashText(tag)
                != BrowseStateStore::hashText(tag + QLatin1Char(' ')));
        ViewState tagged;
        tagged.tagHashes = {BrowseStateStore::hashText(tag)};
        tagged.ratingHashes = {BrowseStateStore::hashText(rating)};
        {
            auto store = f.open();
            store->setView(kServer, kUser, QStringLiteral("movies"), tagged);
            store->flush();
        }
        const QByteArray bytes = f.stored();
        QVERIFY(!bytes.contains("Zebra"));
        QVERIFY(bytes.contains(BrowseStateStore::hashText(tag).toUtf8()));
        QVERIFY(bytes.contains(BrowseStateStore::hashText(rating).toUtf8()));
        auto store = f.open();
        QVERIFY(holds(*store, kServer, kUser, QStringLiteral("movies"), tagged));
    }

    void version1MigratesAndIsWrittenAsVersion2()
    {
        Fixture f;
        const QByteArray v1 = QStringLiteral(R"({
            "version": 1, "nextSeq": 40,
            "servers": {"%1": {"users": {"%2": {
                "selection": {"movies": "%3", "tvshows": ""},
                "views": {
                    "movies": {"sort": "DateAdded", "descending": true, "filter": "Unplayed", "genreIds": ["%4", "%3"], "seq": 7},
                    "tvshows": {"sort": "Name", "descending": false, "filter": "Resumable", "genreIds": [], "seq": 9},
                    "boxsets:%3": {"sort": "Runtime", "descending": false, "filter": "Favorites", "genreIds": [], "seq": 11},
                    "movies:%3": {"sort": "Name", "descending": false, "filter": "Played", "genreIds": [], "seq": 12},
                    "tvshows:%3": {"sort": "Name", "descending": false, "filter": "Mystery", "genreIds": [], "seq": 13}
                }}}}}})")
                                  .arg(kServer, kUser, kLibrary, kGenre)
                                  .toUtf8();
        f.write(v1);
        {
            auto store = f.open();
            QCOMPARE(store->selection(kServer, kUser, kMovies), kLibrary);
            QCOMPARE(store->selection(kServer, kUser, kShows), QString());
            const ViewState movies = state(QStringLiteral("DateAdded"), true,
                                           QStringLiteral("Unplayed"), {kGenre, kLibrary});
            QVERIFY(holds(*store, kServer, kUser, QStringLiteral("movies"), movies));
            // Over series there is no resumable filter
            QVERIFY(holds(*store, kServer, kUser, QStringLiteral("tvshows"),
                          state(QStringLiteral("Name"), false, QStringLiteral("All"))));
            QVERIFY(holds(*store, kServer, kUser, QStringLiteral("boxsets:") + kLibrary,
                          state(QStringLiteral("Runtime"), false, QStringLiteral("Favorites"))));
            QVERIFY(holds(*store, kServer, kUser, QStringLiteral("movies:") + kLibrary,
                          state(QStringLiteral("Name"), false, QStringLiteral("Played"))));
            // A filter name of no known meaning leaves no flag, the rest is kept
            QVERIFY(holds(*store, kServer, kUser, QStringLiteral("tvshows:") + kLibrary,
                          state(QStringLiteral("Name"), false, QStringLiteral("All"))));
            // The new options start at their defaults
            const ViewState read = *store->view(kServer, kUser, QStringLiteral("movies"));
            QCOMPARE(read.viewMode, QStringLiteral("grid"));
            QCOMPARE(read.imageType, QStringLiteral("Primary"));
            QVERIFY(read.showTitle && read.showYear && read.cardLayout);
            QVERIFY(read.features.isEmpty() && read.years.isEmpty() && read.quality.isEmpty());
            // Nothing is written until something changes
            QCOMPARE(f.stored(), v1);
            store->setView(kServer, kUser, QStringLiteral("movies"),
                           state(QStringLiteral("Name"), false, QStringLiteral("All")));
            store->flush();
        }
        const QJsonObject root = QJsonDocument::fromJson(f.stored()).object();
        QCOMPARE(root.value(QStringLiteral("version")).toInt(), 3);
        // The counter went on from where it was: the edited view got 40, the others kept their number
        QCOMPARE(root.value(QStringLiteral("nextSeq")).toInteger(), 41);
        const QJsonObject views = root.value(QStringLiteral("servers"))
                                      .toObject()
                                      .value(kServer)
                                      .toObject()
                                      .value(QStringLiteral("users"))
                                      .toObject()
                                      .value(kUser)
                                      .toObject()
                                      .value(QStringLiteral("views"))
                                      .toObject();
        const auto seqOf = [&views](const QString &scope) {
            return views.value(scope).toObject().value(QStringLiteral("seq")).toInteger();
        };
        QCOMPARE(seqOf(QStringLiteral("movies")), 40);
        QCOMPARE(seqOf(QStringLiteral("tvshows")), 9);
        QCOMPARE(seqOf(QStringLiteral("boxsets:") + kLibrary), 11);
        QVERIFY(views.value(QStringLiteral("boxsets:") + kLibrary)
                    .toObject()
                    .value(QStringLiteral("statusFlags"))
                    .toArray()
                == QJsonArray{QStringLiteral("IsFavorite")});
        auto store = f.open();
        QVERIFY(holds(*store, kServer, kUser, QStringLiteral("boxsets:") + kLibrary,
                      state(QStringLiteral("Runtime"), false, QStringLiteral("Favorites"))));
    }

    void invalidFieldsDropOnlyThatView_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QJsonValue>("value");
        const auto row = [](const char *name, const char *key, const QJsonValue &value) {
            QTest::newRow(name) << QString::fromLatin1(key) << value;
        };
        QJsonArray moreGenres;
        QJsonArray moreHashes;
        for (int i = 0; i <= BrowseStateStore::MaxGenreIds; ++i) {
            moreGenres.append(hex(i));
            moreHashes.append(hash(i));
        }
        QJsonArray tooManyYears;
        for (int i = 0; i <= BrowseStateStore::MaxYears; ++i)
            tooManyYears.append(1900 + i);
        row("sortKey with a blank", "sortKey", QStringLiteral("Date Added"));
        row("sortKey not text", "sortKey", 5);
        row("sortKey empty", "sortKey", QString());
        row("sortKey too long", "sortKey", QString(33, QLatin1Char('a')));
        row("descending as text", "descending", QStringLiteral("yes"));
        row("statusFlags unknown", "statusFlags", QJsonArray{QStringLiteral("IsBogus")});
        row("statusFlags lower case", "statusFlags", QJsonArray{QStringLiteral("isplayed")});
        row("statusFlags not an array", "statusFlags", QStringLiteral("IsPlayed"));
        row("statusFlags with a number", "statusFlags", QJsonArray{5});
        row("statusFlags twice", "statusFlags",
            QJsonArray{QStringLiteral("IsPlayed"), QStringLiteral("IsPlayed")});
        row("features unknown", "features", QJsonArray{QStringLiteral("HasNothing")});
        row("quality unknown", "quality", QStringLiteral("UHD"));
        row("quality not text", "quality", 1);
        row("is4K as text", "is4K", QStringLiteral("true"));
        row("is3D as number", "is3D", 1);
        row("videoTypes unknown", "videoTypes", QJsonArray{QStringLiteral("Vhs")});
        row("genreIds not an id", "genreIds", QJsonArray{QStringLiteral("a/b")});
        row("genreIds too many", "genreIds", moreGenres);
        row("tagHashes too short", "tagHashes", QJsonArray{QStringLiteral("abc")});
        row("tagHashes upper case", "tagHashes", QJsonArray{hash(10).toUpper()});
        row("tagHashes not hex", "tagHashes", QJsonArray{QString(64, QLatin1Char('g'))});
        row("tagHashes too many", "tagHashes", moreHashes);
        row("ratingHashes clear text", "ratingHashes", QJsonArray{QStringLiteral("PG-13")});
        row("ratingHashes too many", "ratingHashes", moreHashes);
        row("years below", "years", QJsonArray{BrowseStateStore::MinYear - 1});
        row("years above", "years", QJsonArray{BrowseStateStore::MaxYear + 1});
        row("years with a fraction", "years", QJsonArray{2000.5});
        row("years as text", "years", QJsonArray{QStringLiteral("2000")});
        row("years huge", "years", QJsonArray{1e300});
        row("years too many", "years", tooManyYears);
        row("seriesStatus unknown", "seriesStatus", QJsonArray{QStringLiteral("Paused")});
        row("episodeKind unknown", "episodeKind", QStringLiteral("Everything"));
        row("viewMode unknown", "viewMode", QStringLiteral("tiles"));
        row("viewMode upper case", "viewMode", QStringLiteral("Grid"));
        row("imageType unknown", "imageType", QStringLiteral("Poster"));
        row("showTitle as number", "showTitle", 1);
        row("showYear as text", "showYear", QStringLiteral("x"));
        row("cardLayout null", "cardLayout", QJsonValue(QJsonValue::Null));
    }

    void invalidFieldsDropOnlyThatView()
    {
        QFETCH(QString, key);
        QFETCH(QJsonValue, value);
        Fixture f;
        const QJsonObject views{{QStringLiteral("movies"), goodView(1)},
                                {QStringLiteral("tvshows"), viewWith(goodView(2), key, value)},
                                {QStringLiteral("movies/collections"), goodView(3)}};
        f.write(fileFor({{kMovies, kLibrary}}, views));
        auto store = f.open();
        QVERIFY(!store->view(kServer, kUser, QStringLiteral("tvshows")));
        QVERIFY(store->view(kServer, kUser, QStringLiteral("movies")));
        QVERIFY(store->view(kServer, kUser, QStringLiteral("movies/collections")));
        QCOMPARE(store->selection(kServer, kUser, kMovies), kLibrary);
    }

    void limitsAreInclusive()
    {
        Fixture f;
        ViewState edge;
        for (int i = 0; i < BrowseStateStore::MaxGenreIds; ++i)
            edge.genreIds.append(hex(i));
        for (int i = 0; i < BrowseStateStore::MaxHashes; ++i) {
            edge.tagHashes.append(hash(i));
            edge.ratingHashes.append(hash(i + 100));
        }
        for (int i = 0; i < BrowseStateStore::MaxYears; ++i)
            edge.years.append(BrowseStateStore::MinYear + i);
        edge.years.last() = BrowseStateStore::MaxYear;
        edge.quality = QStringLiteral("SD");
        {
            auto store = f.open();
            store->setView(kServer, kUser, QStringLiteral("movies"), edge);
            QVERIFY(holds(*store, kServer, kUser, QStringLiteral("movies"), edge));
            // One past a limit is refused, the stored view stays as it was
            ViewState over = edge;
            over.years.append(2000);
            store->setView(kServer, kUser, QStringLiteral("movies"), over);
            over = edge;
            over.tagHashes.append(hash(1000));
            store->setView(kServer, kUser, QStringLiteral("movies"), over);
            over = edge;
            over.genreIds.append(hex(1000));
            store->setView(kServer, kUser, QStringLiteral("movies"), over);
            over = edge;
            over.viewMode = QStringLiteral("tiles");
            store->setView(kServer, kUser, QStringLiteral("movies"), over);
            over = edge;
            over.years.first() = BrowseStateStore::MinYear - 1;
            store->setView(kServer, kUser, QStringLiteral("movies"), over);
            QVERIFY(holds(*store, kServer, kUser, QStringLiteral("movies"), edge));
            store->flush();
        }
        auto store = f.open();
        QVERIFY(holds(*store, kServer, kUser, QStringLiteral("movies"), edge));
    }

    void scopeKeysAreStrict()
    {
        Fixture f;
        const QString id = kLibrary;
        const QStringList valid{QStringLiteral("movies"),
                                QStringLiteral("tvshows"),
                                QStringLiteral("movies:") + id,
                                QStringLiteral("tvshows:") + id,
                                QStringLiteral("boxsets:") + id,
                                QStringLiteral("movies/favorites"),
                                QStringLiteral("movies/favorites:") + id,
                                QStringLiteral("movies/collections"),
                                QStringLiteral("tvshows/favorites"),
                                QStringLiteral("tvshows/favorites:") + id,
                                QStringLiteral("tvshows/episodes"),
                                QStringLiteral("tvshows/episodes:") + id,
                                QStringLiteral("tvshows/network:") + id};
        const QStringList invalid{QStringLiteral("movies/"),
                                  QStringLiteral("movies/favorites:"),
                                  QStringLiteral("movies/favorites:bad id"),
                                  QStringLiteral("movies/collections:") + id,
                                  QStringLiteral("tvshows/network"),
                                  QStringLiteral("tvshows/network:Studio Name"),
                                  QStringLiteral("tvshows/network:"),
                                  QStringLiteral("movies/episodes"),
                                  QStringLiteral("tvshows/favorites:"),
                                  QStringLiteral("tvshows/collections"),
                                  QStringLiteral("boxsets"),
                                  QStringLiteral("boxsets/x"),
                                  QStringLiteral("music")};
        QJsonObject views;
        const ViewState a = state(QStringLiteral("Name"), true, QStringLiteral("Played"));
        {
            auto store = f.open();
            for (const QString &scope : valid)
                store->setView(kServer, kUser, scope, a);
            for (const QString &scope : invalid)
                store->setView(kServer, kUser, scope, a);
            for (const QString &scope : valid)
                QVERIFY2(store->view(kServer, kUser, scope), qPrintable(scope));
            for (const QString &scope : invalid)
                QVERIFY2(!store->view(kServer, kUser, scope), qPrintable(scope));
            store->flush();
        }
        // The same keys in a file by hand
        int seq = 1;
        for (const QString &scope : valid + invalid)
            views.insert(scope, goodView(seq++));
        f.write(fileFor({}, views));
        auto store = f.open();
        for (const QString &scope : valid)
            QVERIFY2(store->view(kServer, kUser, scope), qPrintable(scope));
        for (const QString &scope : invalid)
            QVERIFY2(!store->view(kServer, kUser, scope), qPrintable(scope));
    }

    void resumableIsDroppedOverSeriesOnly()
    {
        Fixture f;
        ViewState resumable;
        resumable.statusFlags = {QStringLiteral("IsResumable"), QStringLiteral("IsFavorite")};
        ViewState favorite;
        favorite.statusFlags = {QStringLiteral("IsFavorite")};
        auto store = f.open();
        for (const QString &scope :
             QStringList{QStringLiteral("tvshows"), QString(QStringLiteral("tvshows:") + kLibrary),
                         QStringLiteral("tvshows/favorites"),
                         QString(QStringLiteral("tvshows/favorites:") + kLibrary),
                         QString(QStringLiteral("tvshows/network:") + kLibrary)}) {
            store->setView(kServer, kUser, scope, resumable);
            QVERIFY2(holds(*store, kServer, kUser, scope, favorite), qPrintable(scope));
        }
        for (const QString &scope : {QStringLiteral("movies"), QStringLiteral("movies/favorites"),
                                     QStringLiteral("tvshows/episodes")}) {
            store->setView(kServer, kUser, scope, resumable);
            QVERIFY2(holds(*store, kServer, kUser, scope, resumable), qPrintable(scope));
        }
    }

    void capAndLruWithTheNewScopeKeys()
    {
        Fixture f;
        const ViewState a = state(QStringLiteral("Name"), false, QStringLiteral("All"));
        const auto scope = [](int i) -> QString {
            switch (i % 4) {
            case 0:
                return QStringLiteral("tvshows/network:") + hex(i);
            case 1:
                return QStringLiteral("tvshows/episodes:") + hex(i);
            case 2:
                return QStringLiteral("movies/favorites:") + hex(i);
            default:
                return QStringLiteral("movies:") + hex(i);
            }
        };
        auto store = f.open();
        for (int i = 0; i <= BrowseStateStore::MaxViews; ++i)
            store->setView(kServer, kUser, scope(i), a);
        QVERIFY(!store->view(kServer, kUser, scope(0)));
        QVERIFY(store->view(kServer, kUser, scope(1)));
        // Writing a view again makes it the newest
        ViewState changed = a;
        changed.viewMode = QStringLiteral("list");
        store->setView(kServer, kUser, scope(1), changed);
        store->setView(kServer, kUser, QStringLiteral("movies/collections"), a);
        QVERIFY(holds(*store, kServer, kUser, scope(1), changed));
        QVERIFY(!store->view(kServer, kUser, scope(2)));
        QVERIFY(store->view(kServer, kUser, scope(3)));
        store->flush();
        auto again = f.open();
        int kept = 0;
        for (int i = 0; i <= BrowseStateStore::MaxViews; ++i)
            kept += again->view(kServer, kUser, scope(i)).has_value();
        QCOMPARE(kept, BrowseStateStore::MaxViews - 1);
        QVERIFY(again->view(kServer, kUser, QStringLiteral("movies/collections")));
        QVERIFY(QFileInfo(f.path).size() <= BrowseStateStore::MaxFileBytes);
    }

    // The Favorites page lost its grouping: a file that still carries the old key loads as before, and the key is gone with the next save
    void theRetiredFavoritesGroupingIsDroppedOnTheNextSave()
    {
        Fixture f;
        f.write(fileFor({{kMovies, kLibrary}}, {{QStringLiteral("movies"), goodView()}}, 2,
                        {{QStringLiteral("favoritesGrouping"), QStringLiteral("Genre")}}));
        {
            auto store = f.open();
            QCOMPARE(store->selection(kServer, kUser, kMovies), kLibrary);
            QVERIFY(store->view(kServer, kUser, QStringLiteral("movies")));
            store->setView(kServer, kUser, QStringLiteral("movies"),
                           state(QStringLiteral("Name"), true, QStringLiteral("All")));
            store->flush();
        }
        QFile file(f.path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(!saved.contains("favoritesGrouping"));
        // What was there stays: the selection and the view just saved
        auto reopened = f.open();
        QCOMPARE(reopened->selection(kServer, kUser, kMovies), kLibrary);
        QVERIFY(holds(*reopened, kServer, kUser, QStringLiteral("movies"),
                      state(QStringLiteral("Name"), true, QStringLiteral("All"))));
    }

    void saveFailedOncePerStreak()
    {
        Fixture f;
        const ViewState a = state(QStringLiteral("Name"), false, QStringLiteral("All"));
        const ViewState b = state(QStringLiteral("Name"), true, QStringLiteral("All"));
        // A regular file where the folder should be
        QFile blocker(f.dir.filePath(QStringLiteral("blocker")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        const QString blocked = f.dir.filePath(QStringLiteral("blocker/browse.json"));
        auto store = std::make_unique<BrowseStateStore>();
        store->setSaveDelay(std::chrono::minutes(10));
        store->setStoragePath(blocked);
        QSignalSpy failures(store.get(), &BrowseStateStore::saveFailed);
        store->setView(kServer, kUser, QStringLiteral("movies"), a);
        store->flush();
        QCOMPARE(failures.size(), 1);
        store->setView(kServer, kUser, QStringLiteral("movies"), b);
        store->flush();
        QCOMPARE(failures.size(), 1);
        // A write that works ends the streak
        store->setStoragePath(f.path);
        store->setView(kServer, kUser, QStringLiteral("movies"), a);
        store->flush();
        QVERIFY(QFileInfo::exists(f.path));
        QCOMPARE(failures.size(), 1);
        store->setStoragePath(blocked);
        store->setView(kServer, kUser, QStringLiteral("movies"), a);
        store->flush();
        QCOMPARE(failures.size(), 2);
    }
};

QTEST_MAIN(TestBrowseStateStore)
#include "test_browse_state_store.moc"
