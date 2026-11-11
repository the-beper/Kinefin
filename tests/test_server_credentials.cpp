// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include "settings/ServerCredentials.h"

using Kinefin::ServerCredential;

class TestServerCredentials : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    static ServerCredential entry(const QString &serverId)
    {
        ServerCredential entry;
        entry.serverId = serverId;
        entry.url = QStringLiteral("http://192.168.1.251:8096");
        entry.userId = QStringLiteral("9c7d46e083324909");
        entry.deviceId = QStringLiteral("kinefin-fiddle-") + serverId;
        entry.deviceName = QStringLiteral("Kinefin test");
        entry.username = QStringLiteral("kinefin-live");
        entry.imageTag = QStringLiteral("tag-1");
        entry.askPassword = serverId == QLatin1String("b");
        entry.remembered = serverId != QLatin1String("b");
        return entry;
    }

    void init()
    {
        m_home = std::make_unique<QTemporaryDir>();
        QVERIFY(m_home->isValid());
        qputenv("XDG_DATA_HOME", m_home->path().toUtf8());
    }

    // The whole 0600 contract: the file never had wider permissions, the directory is owner-only, and the saved JSON carries exactly the declared fields
    void saveIsPrivateAndRoundTrips()
    {
        QString error;
        const QList<ServerCredential> credentials{entry(QStringLiteral("a")),
                                                  entry(QStringLiteral("b"))};
        QVERIFY2(Kinefin::saveServerCredentials(QStringLiteral("default"), credentials, &error),
                 qPrintable(error));

        const QString path = Kinefin::serverCredentialsPath(QStringLiteral("default"));
        QVERIFY(path.endsWith(QStringLiteral("profiles/default/servers.json")));
        QFileInfo info(path);
        QVERIFY(info.exists());
        // Qt reports a POSIX owner-only file with both the Owner and the User flags set (the same mode bits); the contract is what the group/other bits say
        const QFileDevice::Permissions leaked = QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                                | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                                | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QCOMPARE(info.permissions() & leaked, 0);
        const QString folder = info.absolutePath();
        QCOMPARE(folder, m_home->path() + QStringLiteral("/kinefin/profiles/default"));
        QCOMPARE(QFileInfo(folder).permissions() & leaked, 0);

        const QList<ServerCredential> loaded =
            Kinefin::loadServerCredentials(QStringLiteral("default"), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(loaded.size(), 2);
        QVERIFY(loaded == credentials);

        // The declared schema only: no other key can sneak in unnoticed
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root.size(), 1);
        QVERIFY(root.value(QStringLiteral("servers")).isArray());
        for (const auto &value : root.value(QStringLiteral("servers")).toArray()) {
            const QJsonObject entryObject = value.toObject();
            QCOMPARE(entryObject.size(), 9);
            QVERIFY(!entryObject.contains(QStringLiteral("token")));
            QVERIFY(!entryObject.contains(QStringLiteral("password")));
        }
    }

    void anUnoverwrittenStoreStaysPrivate()
    {
        QString error;
        QList<ServerCredential> credentials{entry(QStringLiteral("a"))};
        QVERIFY(Kinefin::saveServerCredentials(QStringLiteral("default"), credentials, &error));
        const QString path = Kinefin::serverCredentialsPath(QStringLiteral("default"));
        // An externally widened file is re-secured by the next save: the rename always lands 0600 again (validate the overwrite path itself)
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ReadGroup);
        QVERIFY(Kinefin::saveServerCredentials(QStringLiteral("default"), credentials, &error));
        const QFileDevice::Permissions leaked = QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                                | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                                | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QCOMPARE(QFileInfo(path).permissions() & leaked, 0);
    }

    void malformedFileRestoresNothing()
    {
        const QString path = Kinefin::serverCredentialsPath(QStringLiteral("default"));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"servers\": \"not an array\"}");
        file.close();
        QString error;
        const QList<ServerCredential> loaded =
            Kinefin::loadServerCredentials(QStringLiteral("default"), &error);
        QVERIFY(loaded.isEmpty());
        QVERIFY(!error.isEmpty());
    }

    // An older snapshot kept the token in this file: it is ignored on load and reported, never returned
    void aLegacyTokenKeyIsIgnoredAndReported()
    {
        const QString path = Kinefin::serverCredentialsPath(QStringLiteral("default"));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"servers\":[{\"serverId\":\"a\",\"url\":\"http://x:8096\","
                   "\"token\":\"legacy-secret\"}]}");
        file.close();
        QString error;
        bool legacy = false;
        const QList<ServerCredential> loaded =
            Kinefin::loadServerCredentials(QStringLiteral("default"), &error, &legacy);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(legacy);
        QCOMPARE(loaded.size(), 1);
        QVERIFY(Kinefin::saveServerCredentials(QStringLiteral("default"), loaded));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("legacy-secret"));
    }

    // A file written before several users per server were kept has no `remembered` key: its token is stored, so it reads as true
    void aRecordWithoutRememberedReadsAsRemembered()
    {
        const QString path = Kinefin::serverCredentialsPath(QStringLiteral("default"));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"servers\":[{\"serverId\":\"a\",\"url\":\"http://x:8096\","
                   "\"userId\":\"u1\",\"imageTag\":\"not a tag!\"}]}");
        file.close();
        QString error;
        const QList<ServerCredential> loaded =
            Kinefin::loadServerCredentials(QStringLiteral("default"), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(loaded.size(), 1);
        QVERIFY(loaded.first().remembered);
        QVERIFY(!loaded.first().askPassword);
        QVERIFY(loaded.first().imageTag.isEmpty()); // not an id: dropped, it ends up in a URL
    }

    // The record of the old one-per-server format owns the legacy wallet key; that survives a save until the owner is migrated or forgotten, and a record with the key never owns it
    void aRecordWithoutRememberedOwnsTheLegacyKeyUntilCleared()
    {
        const QString profile = QStringLiteral("default");
        const QString path = Kinefin::serverCredentialsPath(profile);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"servers\":[{\"serverId\":\"a\",\"url\":\"http://x:8096\","
                   "\"userId\":\"u1\"},{\"serverId\":\"a\",\"url\":\"http://x:8096\","
                   "\"userId\":\"u2\",\"remembered\":false}]}");
        file.close();
        QList<ServerCredential> loaded = Kinefin::loadServerCredentials(profile);
        QCOMPARE(loaded.size(), 2);
        QVERIFY(loaded.at(0).ownsLegacyKey);
        QVERIFY(!loaded.at(1).ownsLegacyKey);

        // Saved as it came: still the owner after a restart
        QVERIFY(Kinefin::saveServerCredentials(profile, loaded));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonArray saved = QJsonDocument::fromJson(file.readAll())
                                     .object()
                                     .value(QStringLiteral("servers"))
                                     .toArray();
        file.close();
        QVERIFY(!saved.at(0).toObject().contains(QStringLiteral("remembered")));
        QVERIFY(saved.at(1).toObject().contains(QStringLiteral("remembered")));
        QVERIFY(Kinefin::loadServerCredentials(profile) == loaded);

        // Once migrated the key is written, and the record owns nothing
        loaded[0].ownsLegacyKey = false;
        QVERIFY(Kinefin::saveServerCredentials(profile, loaded));
        const QList<ServerCredential> migrated = Kinefin::loadServerCredentials(profile);
        QVERIFY(!migrated.at(0).ownsLegacyKey);
        QVERIFY(migrated.at(0).remembered);
    }

    // Both ids end up in wallet keys: a `/` must never alias another key
    void onlyJellyfinIdsAreValidIds()
    {
        ServerCredential good = entry(QStringLiteral("a"));
        QVERIFY(Kinefin::hasValidIds(good));
        for (const QString &bad :
             {QString(), QStringLiteral("x/y"), QStringLiteral("a b"), QStringLiteral("..")}) {
            ServerCredential user = good;
            user.userId = bad;
            QVERIFY2(!Kinefin::hasValidIds(user), qPrintable(bad));
            ServerCredential server = good;
            server.serverId = bad;
            QVERIFY2(!Kinefin::hasValidIds(server), qPrintable(bad));
        }
    }

    void missingFileIsNotAnError()
    {
        QString error;
        QVERIFY(Kinefin::loadServerCredentials(QStringLiteral("default"), &error).isEmpty());
        QVERIFY(error.isEmpty());
    }

    void profilesDoNotLeakIntoEachOther()
    {
        QString error;
        QVERIFY(Kinefin::saveServerCredentials(QStringLiteral("default"),
                                               {entry(QStringLiteral("a"))}, &error));
        QVERIFY(Kinefin::saveServerCredentials(QStringLiteral("second"),
                                               {entry(QStringLiteral("b"))}, &error));
        QCOMPARE(Kinefin::loadServerCredentials(QStringLiteral("default"), &error).first().serverId,
                 QStringLiteral("a"));
        QCOMPARE(Kinefin::loadServerCredentials(QStringLiteral("second"), &error).first().serverId,
                 QStringLiteral("b"));
    }

    // The profile id is a path component: nothing that could leave the profiles directory is accepted, and nothing is written for it
    void invalidProfileIdsNeverTouchTheDisk()
    {
        const QStringList invalid{QString(),
                                  QStringLiteral(".."),
                                  QStringLiteral("."),
                                  QStringLiteral("../x"),
                                  QStringLiteral("a/b"),
                                  QStringLiteral(".hidden"),
                                  QStringLiteral("-flag"),
                                  QStringLiteral("default\n"),
                                  QStringLiteral("a b"),
                                  QString(65, QLatin1Char('a'))};
        for (const QString &id : invalid) {
            QVERIFY2(!Kinefin::validProfileId(id), qPrintable(id));
            QVERIFY2(Kinefin::serverCredentialsPath(id).isEmpty(), qPrintable(id));
            QString error;
            QVERIFY(!Kinefin::saveServerCredentials(id, {entry(QStringLiteral("a"))}, &error));
            QVERIFY(!error.isEmpty());
            error.clear();
            QVERIFY(Kinefin::loadServerCredentials(id, &error).isEmpty());
            QVERIFY(!error.isEmpty());
        }
        QVERIFY(!QFileInfo::exists(m_home->path() + QStringLiteral("/kinefin")));
        QVERIFY(Kinefin::validProfileId(QStringLiteral("default")));
        QVERIFY(Kinefin::validProfileId(QStringLiteral("profile_2.alt-1")));
    }

  private:
    std::unique_ptr<QTemporaryDir> m_home;
};

QTEST_APPLESS_MAIN(TestServerCredentials)
#include "test_server_credentials.moc"
