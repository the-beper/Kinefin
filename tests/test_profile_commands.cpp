// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MemoryTokenStore.h"
#include "app/ProfileCommands.h"
#include "settings/ProfileIndex.h"
#include "settings/ProfilePaths.h"
#include "settings/ServerCredentialSync.h"
#include "settings/ServerCredentials.h"

#include <QCommandLineParser>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

#include <unistd.h>

using namespace Kinefin;

namespace {
struct Output {
    QString out;
    QString err;
};

void touch(const QString &path)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
}
} // namespace

class TestProfileCommands : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir m_home;

    QStringList ids() const
    {
        QStringList result;
        const auto data = loadProfileIndex();
        for (const ProfileEntry &entry : data->profiles)
            result << entry.id;
        return result;
    }

    std::optional<int> run(const QStringList &arguments, MemoryTokenStore &store, Output &output)
    {
        QCommandLineParser parser;
        addProfileOptions(parser);
        parser.parse(QStringList{QStringLiteral("kinefin")} + arguments);
        QTextStream out(&output.out);
        QTextStream err(&output.err);
        return runProfileCommand(parser, store, out, err);
    }

  private Q_SLOTS:
    void init()
    {
        QDir(m_home.path()).removeRecursively();
        QDir().mkpath(m_home.path());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + QStringLiteral("/data")).toUtf8());
        qputenv("XDG_CACHE_HOME", (m_home.path() + QStringLiteral("/cache")).toUtf8());
        QVERIFY(ensureDefaultProfileIndex());
    }

    void pathsOfTwoProfilesNeverOverlap()
    {
        const QString a = QStringLiteral("alpha");
        const QString b = QStringLiteral("beta");
        QVERIFY(profileDataDirectory(a) != profileDataDirectory(b));
        QVERIFY(profileCacheDirectory(a) != profileCacheDirectory(b));
        QVERIFY(profileCacheDirectory(a) != profileDataDirectory(a));
        QVERIFY(profileLogDirectory(a).startsWith(profileDataDirectory(a) + QLatin1Char('/')));
        QVERIFY(profileDataDirectory(QStringLiteral("../x")).isEmpty());
        QVERIFY(profileCacheDirectory(QString()).isEmpty());
        QVERIFY(profileLogDirectory(QStringLiteral("a/b")).isEmpty());
    }

    void profileNamesAreRestricted()
    {
        for (const QString &name :
             {QStringLiteral("work"), QStringLiteral("Kids_2"), QString(64, QLatin1Char('a'))})
            QVERIFY2(validProfileName(name), qPrintable(name));
        for (const QString &name :
             {QString(), QStringLiteral("a b"), QStringLiteral("../x"), QStringLiteral("a.b"),
              QStringLiteral("a-b"), QString(65, QLatin1Char('a')), QStringLiteral("é")})
            QVERIFY2(!validProfileName(name), qPrintable(name));
    }

    void anInvalidIndexIsMovedAsideAndRebuilt()
    {
        QFile file(profileIndexPath());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{not json");
        file.close();
        QVERIFY(ensureDefaultProfileIndex());
        QVERIFY(QFile::exists(profileIndexPath() + QStringLiteral(".broken")));
        QCOMPARE(ids(), QStringList{QStringLiteral("default")});
        // Valid JSON that does not name a listed default is just as invalid.
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(R"({"defaultProfile":"ghost","profiles":[{"id":"default","name":"Default"}]})");
        file.close();
        QVERIFY(ensureDefaultProfileIndex());
        QCOMPARE(loadProfileIndex()->defaultId, QStringLiteral("default"));
        // A valid index is never touched.
        QVERIFY(addProfile(QStringLiteral("kept")));
        QVERIFY(ensureDefaultProfileIndex());
        QVERIFY(ids().contains(QStringLiteral("kept")));
    }

    void createListAndSetDefault()
    {
        MemoryTokenStore store;
        Output output;
        QCOMPARE(run({QStringLiteral("--create-profile"), QStringLiteral("work")}, store, output),
                 0);
        QCOMPARE(run({QStringLiteral("--create-profile"), QStringLiteral("work")}, store, output),
                 1); // duplicate
        QCOMPARE(run({QStringLiteral("--create-profile"), QStringLiteral("no way")}, store, output),
                 1);
        QCOMPARE(ids(), (QStringList{QStringLiteral("default"), QStringLiteral("work")}));
        output = {};
        QCOMPARE(run({QStringLiteral("--list-profiles")}, store, output), 0);
        QVERIFY(output.out.contains(QStringLiteral("* default\tDefault")));
        QVERIFY(output.out.contains(QStringLiteral("  work\twork")));
        QCOMPARE(
            run({QStringLiteral("--set-default-profile"), QStringLiteral("work")}, store, output),
            0);
        QCOMPARE(loadProfileIndex()->defaultId, QStringLiteral("work"));
        QCOMPARE(
            run({QStringLiteral("--set-default-profile"), QStringLiteral("ghost")}, store, output),
            1);
        QCOMPARE(loadProfileIndex()->defaultId, QStringLiteral("work"));
    }

    void commandsExcludeEachOtherAndProfile()
    {
        MemoryTokenStore store;
        Output output;
        QCOMPARE(run({QStringLiteral("--list-profiles"), QStringLiteral("--create-profile"),
                      QStringLiteral("x")},
                     store, output),
                 2);
        QCOMPARE(run({QStringLiteral("--list-profiles"), QStringLiteral("--profile"),
                      QStringLiteral("default")},
                     store, output),
                 2);
        QVERIFY(!run({QStringLiteral("--profile"), QStringLiteral("default")}, store,
                     output)); // not a command
        QVERIFY(!run({}, store, output));
        QCOMPARE(ids(), QStringList{QStringLiteral("default")});
    }

    void theStartupProfileIsResolvedByIdThenName()
    {
        QVERIFY(addProfile(QStringLiteral("work")));
        QCommandLineParser parser;
        addProfileOptions(parser);
        QString error;
        parser.parse({QStringLiteral("kinefin")});
        QCOMPARE(startupProfileId(parser, &error), QStringLiteral("default"));
        QCommandLineParser named;
        addProfileOptions(named);
        named.parse(
            {QStringLiteral("kinefin"), QStringLiteral("--profile"), QStringLiteral("work")});
        QCOMPARE(startupProfileId(named, &error), QStringLiteral("work"));
        QCommandLineParser unknown;
        addProfileOptions(unknown);
        unknown.parse(
            {QStringLiteral("kinefin"), QStringLiteral("--profile"), QStringLiteral("ghost")});
        QVERIFY(startupProfileId(unknown, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("ghost")));
    }

    void deleteRefusesTheDefaultAndAProfileInUse()
    {
        MemoryTokenStore store;
        Output output;
        QVERIFY(addProfile(QStringLiteral("busy")));
        QCOMPARE(
            run({QStringLiteral("--delete-profile"), QStringLiteral("default")}, store, output), 1);
        const auto lock = lockProfile(QStringLiteral("busy"));
        QVERIFY(lock);
        QVERIFY(profileInUse(QStringLiteral("busy")));
        QVERIFY(!lockProfile(QStringLiteral("busy"))); // a second holder is refused
        QCOMPARE(run({QStringLiteral("--delete-profile"), QStringLiteral("busy")}, store, output),
                 1);
        QVERIFY(ids().contains(QStringLiteral("busy")));
        QVERIFY(QFileInfo::exists(profileDataDirectory(QStringLiteral("busy"))));
    }

    void deleteRemovesTheFilesAndTheWalletEntries()
    {
        MemoryTokenStore store;
        Output output;
        QVERIFY(addProfile(QStringLiteral("old")));
        QVERIFY(addProfile(QStringLiteral("other")));
        const QString id = QStringLiteral("old");
        QVERIFY(saveServerCredentials(
            id, {{QStringLiteral("srv1"), QStringLiteral("https://a.example"), QStringLiteral("u"),
                  QStringLiteral("d"), QStringLiteral("n"), QStringLiteral("name")},
                 {QStringLiteral("srv2"), QStringLiteral("https://b.example"), QStringLiteral("u"),
                  QStringLiteral("d"), QStringLiteral("n"), QStringLiteral("name")}}));
        store.tokens.insert(serverTokenKey(id, QStringLiteral("srv1")), QStringLiteral("t1"));
        store.tokens.insert(serverTokenKey(id, QStringLiteral("srv2")), QStringLiteral("t2"));
        store.tokens.insert(serverTokenKey(QStringLiteral("other"), QStringLiteral("srv1")),
                            QStringLiteral("keep"));
        touch(profileDataDirectory(id) + QStringLiteral("/downloads/film.mkv"));
        touch(profileDataDirectory(id) + QStringLiteral("/logs/kinefin.log"));
        touch(profileCacheDirectory(id) + QStringLiteral("/images/a.png"));

        QCOMPARE(run({QStringLiteral("--delete-profile"), id}, store, output), 0);
        QVERIFY(!ids().contains(id));
        QVERIFY(!QFileInfo::exists(profileDataDirectory(id)));
        QVERIFY(!QFileInfo::exists(profileCacheDirectory(id)));
        QCOMPARE(store.tokens.size(), 1); // only the other profile's token is left
        QVERIFY(ids().contains(QStringLiteral("other")));
    }

    void aWalletFailureStillDeletesTheFiles()
    {
        MemoryTokenStore store;
        store.failRemoves = true;
        Output output;
        QVERIFY(addProfile(QStringLiteral("old")));
        QVERIFY(saveServerCredentials(
            QStringLiteral("old"),
            {{QStringLiteral("srv1"), QStringLiteral("https://a.example"), QStringLiteral("u"),
              QStringLiteral("d"), QStringLiteral("n"), QStringLiteral("name")}}));
        QCOMPARE(run({QStringLiteral("--delete-profile"), QStringLiteral("old")}, store, output),
                 1);
        QVERIFY(output.err.contains(QStringLiteral("wallet")));
        QVERIFY(!ids().contains(QStringLiteral("old")));
        QVERIFY(!QFileInfo::exists(profileDataDirectory(QStringLiteral("old"))));
    }

    void deleteNeverFollowsSymbolicLinks()
    {
        MemoryTokenStore store;
        Output output;
        QVERIFY(addProfile(QStringLiteral("linked")));
        QTemporaryDir outside;
        touch(outside.filePath(QStringLiteral("precious.txt")));
        const QString data = profileDataDirectory(QStringLiteral("linked"));
        touch(data + QStringLiteral("/inside.txt"));
        QVERIFY(QFile::link(outside.path(), data + QStringLiteral("/escape")));
        QCOMPARE(run({QStringLiteral("--delete-profile"), QStringLiteral("linked")}, store, output),
                 0);
        QVERIFY(QFile::exists(outside.filePath(QStringLiteral("precious.txt"))));
        QVERIFY(!QFileInfo::exists(data));
        // A profile directory that is itself a link is refused.
        QVERIFY(addProfile(QStringLiteral("whole")));
        const QString whole = profileDataDirectory(QStringLiteral("whole"));
        QVERIFY(QDir().mkpath(QFileInfo(whole).absolutePath()));
        QVERIFY(QFile::link(outside.path(), whole));
        QCOMPARE(run({QStringLiteral("--delete-profile"), QStringLiteral("whole")}, store, output),
                 1);
        QVERIFY(QFile::exists(outside.filePath(QStringLiteral("precious.txt"))));
        QVERIFY(ids().contains(QStringLiteral("whole")));
    }

    void anUnreadableRecordOfServersFailsTheDeleteButStillRemovesTheFiles()
    {
        MemoryTokenStore store;
        Output output;
        QVERIFY(addProfile(QStringLiteral("odd")));
        touch(profileDataDirectory(QStringLiteral("odd")) + QStringLiteral("/servers.json"));
        QFile file(serverCredentialsPath(QStringLiteral("odd")));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{broken");
        file.close();
        QCOMPARE(run({QStringLiteral("--delete-profile"), QStringLiteral("odd")}, store, output),
                 1);
        QVERIFY(!QFileInfo::exists(profileDataDirectory(QStringLiteral("odd"))));
    }

    void namesAreUniqueIgnoringCaseAndResolvableByName()
    {
        QVERIFY(!addProfile(QStringLiteral("Default")));
        QVERIFY(!addProfile(QStringLiteral("DEFAULT")));
        QVERIFY(addProfile(QStringLiteral("Work")));
        QVERIFY(!addProfile(QStringLiteral("work")));
        // The display name of the first profile is "Default": --profile finds it by name too.
        QCommandLineParser parser;
        addProfileOptions(parser);
        parser.parse(
            {QStringLiteral("kinefin"), QStringLiteral("--profile"), QStringLiteral("Default")});
        QString error;
        QCOMPARE(startupProfileId(parser, &error), QStringLiteral("default"));
    }

    void anUnopenableIndexIsNotMovedAside()
    {
        if (geteuid() == 0)
            QSKIP("root ignores file permissions");
        QVERIFY(QFile::setPermissions(profileIndexPath(), QFileDevice::WriteOwner));
        QString error;
        QVERIFY(!ensureDefaultProfileIndex(&error));
        QVERIFY(!QFile::exists(profileIndexPath() + QStringLiteral(".broken")));
        QVERIFY(QFile::setPermissions(profileIndexPath(),
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    }

    void manyAddsKeepEveryProfile()
    {
        for (int i = 0; i < 20; ++i)
            QVERIFY(addProfile(QStringLiteral("p%1").arg(i)));
        QCOMPARE(ids().size(), 21);
    }
};
QTEST_GUILESS_MAIN(TestProfileCommands)
#include "test_profile_commands.moc"
