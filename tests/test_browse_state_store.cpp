// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The browse state store: per server and user round trips, the private file, the debounced write, the caps and what is dropped or kept aside on load
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

ViewState state(const QString &sort, bool descending, const QString &filter,
                const QStringList &genreIds = {})
{
    return ViewState{sort, descending, filter, genreIds};
}

QJsonObject goodView(qint64 seq = 1)
{
    return QJsonObject{{QStringLiteral("sort"), QStringLiteral("Name")},
                       {QStringLiteral("descending"), false},
                       {QStringLiteral("filter"), QStringLiteral("All")},
                       {QStringLiteral("genreIds"), QJsonArray()},
                       {QStringLiteral("seq"), seq}};
}

const QJsonValue kAbsent(QJsonValue::Undefined);

// A copy of the good view with one member replaced (or removed when `value` is `kAbsent`)
QJsonObject viewWith(const QString &key, const QJsonValue &value)
{
    QJsonObject view = goodView();
    if (value.isUndefined())
        view.remove(key);
    else
        view.insert(key, value);
    return view;
}

// A file with one server and one user holding the given selection and views
QByteArray fileFor(const QJsonObject &selection, const QJsonObject &views, int version = 1)
{
    const QJsonObject user{{QStringLiteral("selection"), selection},
                           {QStringLiteral("views"), views}};
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

    void sizeCapEvicts()
    {
        Fixture f;
        QStringList genres;
        for (int i = 0; i < BrowseStateStore::MaxGenreIds; ++i)
            genres.append(QStringLiteral("%1").arg(i, 32, 16, QLatin1Char('a')));
        const ViewState big = state(QStringLiteral("Name"), false, QStringLiteral("All"), genres);
        constexpr int count = 150;
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
        int kept = 0;
        for (int i = 0; i < count; ++i)
            kept += store->view(kServer, kUser, libraryScope(i)).has_value();
        QVERIFY(kept > 0 && kept < count);
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
        const QByteArray bytes = fileFor({{kMovies, kLibrary}}, {}, 2);
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
        const QByteArray bytes = fileFor({{kMovies, kLibrary}}, {}, 2);
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
        const QJsonObject views{
            {QStringLiteral("movies"), viewWith(QStringLiteral("seq"), qint64(1) << 60)}};
        f.write(fileFor({}, views));
        auto store = f.open();
        QVERIFY(!store->view(kServer, kUser, QStringLiteral("movies")));
    }

    void badEntriesDropped()
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
            {QStringLiteral("movies"), goodView()},
            {QStringLiteral("tvshows"),
             viewWith(QStringLiteral("filter"), QStringLiteral("Resumable"))},
            {QStringLiteral("movies:bad id"), goodView()},
            {QStringLiteral("radio"), goodView()},
            {QStringLiteral("boxsets:") + kLibrary, goodView()},
            {QStringLiteral("movies:badsort"),
             viewWith(QStringLiteral("sort"), QStringLiteral("Date Added"))},
            {QStringLiteral("movies:nofilter"), viewWith(QStringLiteral("filter"), kAbsent)},
            {QStringLiteral("movies:badgenre"),
             viewWith(QStringLiteral("genreIds"), QJsonArray{kGenre, QStringLiteral("a/b")})},
            {QStringLiteral("movies:nodescending"),
             viewWith(QStringLiteral("descending"), kAbsent)},
            {QStringLiteral("movies:negseq"), viewWith(QStringLiteral("seq"), -1)},
            {QStringLiteral("movies:notobject"), 5},
        };
        QJsonObject root = QJsonDocument::fromJson(fileFor(selection, views)).object();
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
