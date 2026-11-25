// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/EarlyUiScale.h"
#include "settings/ClientSettings.h"
#include "settings/ProfileIndex.h"
#include "settings/ProfilePaths.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using Kinefin::startupUiScalePercent;

namespace {
void writeFile(const QString &path, const QByteArray &content)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

// Puts the variable back, or unsets it, when it goes out of scope
class EnvironmentGuard {
  public:
    explicit EnvironmentGuard(const char *name)
        : m_name(name), m_wasSet(qEnvironmentVariableIsSet(name)), m_value(qgetenv(name))
    {
    }
    ~EnvironmentGuard()
    {
        if (m_wasSet)
            qputenv(m_name, m_value);
        else
            qunsetenv(m_name);
    }

  private:
    const char *m_name;
    bool m_wasSet;
    QByteArray m_value;
};

QStringList everythingUnder(const QString &path)
{
    QStringList found;
    QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        found << it.next();
    found.sort();
    return found;
}
} // namespace

// The reader runs before QApplication exists, so this test has no application object either
class TestEarlyUiScale : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir m_root;

    QString conf(const QString &profile = QStringLiteral("default")) const
    {
        return m_root.filePath(QStringLiteral("profiles/") + profile
                               + QStringLiteral("/kinefin.conf"));
    }

    void writeIndex(const QString &indexPath, const QString &defaultId,
                    const QList<Kinefin::ProfileEntry> &profiles)
    {
        QJsonArray array;
        for (const Kinefin::ProfileEntry &entry : profiles)
            array.append(QJsonObject{{QStringLiteral("id"), entry.id},
                                     {QStringLiteral("name"), entry.name}});
        writeFile(indexPath,
                  QJsonDocument(QJsonObject{{QStringLiteral("defaultProfile"), defaultId},
                                            {QStringLiteral("profiles"), array}})
                      .toJson());
    }

    // The profile root of the run is m_root, as `--config-dir` gives it
    void writeDefaultIndex()
    {
        writeIndex(m_root.filePath(QStringLiteral("profiles.json")), QStringLiteral("default"),
                   {{QStringLiteral("default"), QStringLiteral("default")}});
    }

    int percent(const QStringList &more = {}) const
    {
        return startupUiScalePercent(QStringList{QStringLiteral("--config-dir"), m_root.path()}
                                     + more);
    }

  private Q_SLOTS:
    void init()
    {
        QVERIFY(!QCoreApplication::instance());
        QDir(m_root.path()).removeRecursively();
        QVERIFY(QDir().mkpath(m_root.path()));
    }

    void cleanup() { Kinefin::setProfileRoot({}); }

    void eachChoiceIsRead()
    {
        writeDefaultIndex();
        for (const int choice : {100, 110, 125, 150, 175, 200}) {
            writeFile(conf(), "[main]\nuiScale=" + QByteArray::number(choice) + "\n");
            QCOMPARE(percent(), choice);
        }
    }

    void aValueOutsideTheChoicesIsTheDefault()
    {
        writeDefaultIndex();
        for (const char *stored : {"130", "0", "-125", "1250", "abc", "", "125.5", "1e2"}) {
            writeFile(conf(), QByteArray("[main]\nuiScale=") + stored + "\n");
            QVERIFY2(percent() == 100, stored);
        }
    }

    void aMissingKeyGroupFileOrIndexIsTheDefault()
    {
        // No index at all
        QCOMPARE(percent(), 100);
        writeDefaultIndex();
        // The profile has no settings file
        QCOMPARE(percent(), 100);
        writeFile(conf(), "[main]\nfullscreen=true\n");
        QCOMPARE(percent(), 100);
        writeFile(conf(), "[video]\nuiScale=150\n");
        QCOMPARE(percent(), 100);
        writeFile(conf(), "");
        QCOMPARE(percent(), 100);
    }

    void aMalformedFileIsTheDefault()
    {
        writeDefaultIndex();
        static const char garbage[] = "\0\xff\xfe garbage\n[[[main\nuiScale\n=150\n";
        writeFile(conf(), QByteArray(garbage, sizeof(garbage) - 1));
        QCOMPARE(percent(), 100);
        QFile::remove(conf());
        // A folder where the file belongs
        QVERIFY(QDir().mkpath(conf()));
        QCOMPARE(percent(), 100);
        QDir(conf()).removeRecursively();
        writeFile(conf(), "[main]\nuiScale=150\n");
        QCOMPARE(percent(), 150);
        // An index that is not valid names no profile
        writeFile(m_root.filePath(QStringLiteral("profiles.json")), "not json");
        QCOMPARE(percent(), 100);
    }

    void aFileOfANewerFormatIsTheDefault()
    {
        writeDefaultIndex();
        const QByteArray newer = QByteArray::number(Kinefin::ClientSettings::kFormatVersion + 1);
        writeFile(conf(), "[General]\nversion=" + newer + "\n[main]\nuiScale=150\n");
        QCOMPARE(percent(), 100);
        const QByteArray current = QByteArray::number(Kinefin::ClientSettings::kFormatVersion);
        writeFile(conf(), "[General]\nversion=" + current + "\n[main]\nuiScale=150\n");
        QCOMPARE(percent(), 150);
    }

    void theProfileOfTheRunDecidesWhichFileIsRead()
    {
        writeIndex(m_root.filePath(QStringLiteral("profiles.json")), QStringLiteral("default"),
                   {{QStringLiteral("default"), QStringLiteral("default")},
                    {QStringLiteral("other"), QStringLiteral("kids")}});
        writeFile(conf(), "[main]\nuiScale=125\n");
        writeFile(conf(QStringLiteral("other")), "[main]\nuiScale=175\n");
        QCOMPARE(percent(), 125);
        QCOMPARE(percent({QStringLiteral("--profile"), QStringLiteral("other")}), 175);
        QCOMPARE(percent({QStringLiteral("--profile=other")}), 175);
        // The name works as well as the id
        QCOMPARE(percent({QStringLiteral("--profile"), QStringLiteral("kids")}), 175);
        // The last one counts, as it does for QCommandLineParser
        QCOMPARE(percent({QStringLiteral("--profile"), QStringLiteral("other"),
                          QStringLiteral("--profile=default")}),
                 125);
        QCOMPARE(percent({QStringLiteral("--profile"), QStringLiteral("nope")}), 100);
        // What follows `--` is not an option
        QCOMPARE(percent({QStringLiteral("--"), QStringLiteral("--profile=other")}), 125);
    }

    void theConfigDirCanBeGivenWithAnEqualsSign()
    {
        writeDefaultIndex();
        writeFile(conf(), "[main]\nuiScale=110\n");
        QCOMPARE(startupUiScalePercent({QStringLiteral("--config-dir=") + m_root.path()}), 110);
    }

    void withoutAConfigDirTheXdgFoldersAreUsed()
    {
        const EnvironmentGuard configGuard("XDG_CONFIG_HOME");
        const EnvironmentGuard dataGuard("XDG_DATA_HOME");
        qputenv("XDG_CONFIG_HOME", m_root.filePath(QStringLiteral("config")).toUtf8());
        qputenv("XDG_DATA_HOME", m_root.filePath(QStringLiteral("data")).toUtf8());
        writeIndex(m_root.filePath(QStringLiteral("config/kinefin/profiles.json")),
                   QStringLiteral("default"),
                   {{QStringLiteral("default"), QStringLiteral("default")}});
        writeFile(m_root.filePath(QStringLiteral("data/kinefin/profiles/default/kinefin.conf")),
                  "[main]\nuiScale=150\n");
        QCOMPARE(startupUiScalePercent({}), 150);
    }

    void itOnlyReadsAndLeavesTheProfileRootAlone()
    {
        const QTemporaryDir other;
        QVERIFY(other.isValid());
        Kinefin::setProfileRoot(other.path());
        // No index, then an index without a settings file: nothing is created either time
        const QStringList before = everythingUnder(m_root.path());
        QCOMPARE(percent(), 100);
        QCOMPARE(everythingUnder(m_root.path()), before);
        writeDefaultIndex();
        const QStringList withIndex = everythingUnder(m_root.path());
        QCOMPARE(percent(), 100);
        QCOMPARE(everythingUnder(m_root.path()), withIndex);
        QCOMPARE(Kinefin::profileRoot(), QDir::cleanPath(other.path()));
        QVERIFY(everythingUnder(other.path()).isEmpty());
    }
};
QTEST_APPLESS_MAIN(TestEarlyUiScale)
#include "test_early_ui_scale.moc"
