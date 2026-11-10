// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include "settings/ProfileIdentity.h"
#include "settings/ProfileIndex.h"
#include "settings/ServerCredentials.h"

class TestProfileIdentity : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void init()
    {
        m_home = std::make_unique<QTemporaryDir>();
        QVERIFY(m_home->isValid());
        qputenv("XDG_DATA_HOME", (m_home->path() + QStringLiteral("/data")).toUtf8());
        qputenv("XDG_CONFIG_HOME", (m_home->path() + QStringLiteral("/config")).toUtf8());
    }

    void defaultProfileComesFromTheIndex()
    {
        QString error;
        QVERIFY(Kinefin::defaultProfileId(&error).isEmpty()); // no index yet
        QVERIFY(!error.isEmpty());
        QVERIFY(Kinefin::ensureDefaultProfileIndex(&error));
        error.clear();
        QCOMPARE(Kinefin::defaultProfileId(&error), QStringLiteral("default"));
        QVERIFY(error.isEmpty());
    }

    void anIndexNamingAnUnsafeProfileIsRefused()
    {
        QString error;
        QVERIFY(Kinefin::ensureDefaultProfileIndex(&error));
        QFile file(Kinefin::profileIndexPath());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{\"defaultProfile\":\"../evil\",\"profiles\":[]}");
        file.close();
        QVERIFY(Kinefin::defaultProfileId(&error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void theDeviceIdIsCreatedOnceAndStaysPrivate()
    {
        QString error;
        const QString first = Kinefin::profileDeviceId(QStringLiteral("default"), &error);
        QVERIFY2(!first.isEmpty(), qPrintable(error));
        QVERIFY(first.startsWith(QStringLiteral("kinefin-")));
        QCOMPARE(Kinefin::profileDeviceId(QStringLiteral("default"), &error), first);
        QVERIFY(Kinefin::profileDeviceId(QStringLiteral("other"), &error) != first);

        const QFileDevice::Permissions leaked = QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                                | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                                | QFileDevice::WriteOther | QFileDevice::ExeOther;
        const QString directory = Kinefin::profileDataDirectory(QStringLiteral("default"));
        QCOMPARE(QFileInfo(directory).permissions() & leaked, 0);
        QCOMPARE(QFileInfo(directory + QStringLiteral("/profile.json")).permissions() & leaked, 0);
    }

    // The id binds the access token: a damaged file is an error, never silently replaced
    void aMalformedProfileFileIsNotOverwritten()
    {
        QString error;
        QVERIFY(!Kinefin::profileDeviceId(QStringLiteral("default"), &error).isEmpty());
        const QString path = Kinefin::profileDataDirectory(QStringLiteral("default"))
                             + QStringLiteral("/profile.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("garbage");
        file.close();
        error.clear();
        QVERIFY(Kinefin::profileDeviceId(QStringLiteral("default"), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("garbage"));
    }

    void anInvalidProfileIdIsRefused()
    {
        QString error;
        QVERIFY(Kinefin::profileDeviceId(QStringLiteral("../x"), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(m_home->path() + QStringLiteral("/data")));
    }

  private:
    std::unique_ptr<QTemporaryDir> m_home;
};

QTEST_APPLESS_MAIN(TestProfileIdentity)
#include "test_profile_identity.moc"
