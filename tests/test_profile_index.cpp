// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include "settings/ProfileIndex.h"
#include "settings/ProfilePaths.h"

class TestProfileIndex : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void cleanup() { Kinefin::setProfileRoot({}); }

    void bootstrapIsIdempotentAndPrivate()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
        QString error;
        QVERIFY2(Kinefin::ensureDefaultProfileIndex(&error), qPrintable(error));
        const QString path = Kinefin::profileIndexPath();
        QVERIFY2(path.startsWith(home.path()), qPrintable(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray first = file.readAll();
        QVERIFY(QJsonDocument::fromJson(first)
                    .object()
                    .value(QStringLiteral("defaultProfile"))
                    .toString()
                == QStringLiteral("default"));
        QVERIFY(!(file.permissions() & QFileDevice::ReadGroup));
        QVERIFY(!(file.permissions() & QFileDevice::ReadOther));
        file.close();
        QVERIFY(Kinefin::ensureDefaultProfileIndex(&error));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), first);
    }

    void configRootReplacesTheXdgLocations()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        Kinefin::setProfileRoot(root.path());
        QCOMPARE(Kinefin::profileIndexPath(), root.path() + QStringLiteral("/profiles.json"));
        QCOMPARE(Kinefin::profileDataDirectory(QStringLiteral("default")),
                 root.path() + QStringLiteral("/profiles/default"));
        QCOMPARE(Kinefin::profileCacheDirectory(QStringLiteral("default")),
                 root.path() + QStringLiteral("/cache/default"));
        QString error;
        QVERIFY2(Kinefin::ensureDefaultProfileIndex(&error), qPrintable(error));
        QVERIFY(QFileInfo::exists(root.path() + QStringLiteral("/profiles.json")));
    }
};
QTEST_APPLESS_MAIN(TestProfileIndex)
#include "test_profile_index.moc"
