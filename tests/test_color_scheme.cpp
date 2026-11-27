// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The color scheme setting: the application palette follows `main.colorScheme`, and KColorSchemeManager leaves no file of its own behind
#include "app/ColorSchemeSetting.h"
#include "settings/ClientSettings.h"

#include <QDirIterator>
#include <QFile>
#include <QGuiApplication>
#include <QPalette>
#include <QTemporaryDir>
#include <QtTest>
#include <KColorSchemeManager>

namespace {
QString g_configHome;

int windowLightness()
{
    return QGuiApplication::palette().color(QPalette::Window).lightness();
}

// The files under `root`, hidden ones too, that mention the manager's [UiSettings] group
QStringList filesWithUiSettings(const QString &root)
{
    QStringList found;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (file.open(QIODevice::ReadOnly) && file.readAll().contains("UiSettings"))
            found << file.fileName();
    }
    return found;
}
} // namespace

class TestColorScheme : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void initTestCase() { QVERIFY(!g_configHome.isEmpty()); }

    // The manager is one per application, a child of it: once created, every test ends back on Auto
    void cleanup()
    {
        if (auto *manager = qApp->findChild<KColorSchemeManager *>())
            manager->activateSchemeId({});
    }

    // Runs first: Auto at the start does not even create the manager, so the palette stays the one the platform set up
    void autoAtTheStartCreatesNoManager()
    {
        QVERIFY(!qApp->findChild<KColorSchemeManager *>());
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QPalette before = QGuiApplication::palette();
        Kinefin::ClientSettings settings(dir.filePath(QStringLiteral("kinefin.conf")));
        Kinefin::followColorScheme(settings);
        QVERIFY(!qApp->findChild<KColorSchemeManager *>());
        QCOMPARE(QGuiApplication::palette(), before);
    }

    // The container has no /usr/share/color-schemes: both schemes must come from the library itself
    void bothSchemesAreOffered()
    {
        auto *manager = KColorSchemeManager::instance();
        QCOMPARE(qApp->findChild<KColorSchemeManager *>(), manager);
        QVERIFY2(manager->indexForSchemeId(QStringLiteral("BreezeDark")).isValid(),
                 "BreezeDark is not offered by KColorSchemeManager");
        QVERIFY2(manager->indexForSchemeId(QStringLiteral("BreezeLight")).isValid(),
                 "BreezeLight is not offered by KColorSchemeManager");
        // The resets between the tests write nothing; pickingASchemeWritesNoConfigFile turns it back on to check the code under test
        manager->setAutosaveChanges(false);
    }

    void valuesMapToSchemeIds()
    {
        QCOMPARE(Kinefin::colorSchemeId(QStringLiteral("light")), QStringLiteral("BreezeLight"));
        QCOMPARE(Kinefin::colorSchemeId(QStringLiteral("dark")), QStringLiteral("BreezeDark"));
        QVERIFY(Kinefin::colorSchemeId(QStringLiteral("auto")).isEmpty());
        QVERIFY(Kinefin::colorSchemeId(QStringLiteral("neon")).isEmpty());
        QVERIFY(Kinefin::colorSchemeId({}).isEmpty());
    }

    void theStoredValueIsAppliedAtOnce()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("kinefin.conf"));
        {
            Kinefin::ClientSettings seed(path);
            QVERIFY(seed.setValue(QStringLiteral("main"), QStringLiteral("colorScheme"),
                                  QStringLiteral("dark")));
        }
        Kinefin::ClientSettings settings(path);
        Kinefin::followColorScheme(settings);
        QCOMPARE(KColorSchemeManager::instance()->activeSchemeId(), QStringLiteral("BreezeDark"));
        QVERIFY2(windowLightness() < 128, "the application palette is not dark");
    }

    void aChangeAtRuntimeIsFollowed()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Kinefin::ClientSettings settings(dir.filePath(QStringLiteral("kinefin.conf")));
        Kinefin::followColorScheme(settings);
        auto *manager = KColorSchemeManager::instance();
        QVERIFY(manager->activeSchemeId().isEmpty());

        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("colorScheme"),
                                  QStringLiteral("dark")));
        QCOMPARE(manager->activeSchemeId(), QStringLiteral("BreezeDark"));
        QVERIFY2(windowLightness() < 128, "dark did not give a dark palette");

        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("colorScheme"),
                                  QStringLiteral("light")));
        QCOMPARE(manager->activeSchemeId(), QStringLiteral("BreezeLight"));
        QVERIFY2(windowLightness() > 128, "light did not give a light palette");

        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("colorScheme"),
                                  QStringLiteral("auto")));
        QVERIFY(manager->activeSchemeId().isEmpty());
    }

    void otherSettingsDoNotTouchTheScheme()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Kinefin::ClientSettings settings(dir.filePath(QStringLiteral("kinefin.conf")));
        Kinefin::followColorScheme(settings);
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("colorScheme"),
                                  QStringLiteral("dark")));
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("uiScale"), 125));
        QCOMPARE(KColorSchemeManager::instance()->activeSchemeId(), QStringLiteral("BreezeDark"));
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("colorScheme"),
                                  QStringLiteral("auto")));
    }

    // The choice stays in kinefin.conf: the code turns the manager's autosave off before it activates a scheme, at the start and on a change
    void pickingASchemeWritesNoConfigFile()
    {
        auto *manager = KColorSchemeManager::instance();
        manager->setAutosaveChanges(true);
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("kinefin.conf"));
        {
            Kinefin::ClientSettings seed(path);
            QVERIFY(seed.setValue(QStringLiteral("main"), QStringLiteral("colorScheme"),
                                  QStringLiteral("dark")));
        }
        Kinefin::ClientSettings settings(path);
        Kinefin::followColorScheme(settings);
        QCOMPARE(manager->activeSchemeId(), QStringLiteral("BreezeDark"));
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("colorScheme"),
                                  QStringLiteral("light")));
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("colorScheme"),
                                  QStringLiteral("auto")));
        QVERIFY2(filesWithUiSettings(g_configHome).isEmpty(),
                 qPrintable(filesWithUiSettings(g_configHome).join(QLatin1Char(' '))));
        // The check can see such a file: with its autosave on, the manager writes one there
        manager->setAutosaveChanges(true);
        manager->activateSchemeId(QStringLiteral("BreezeDark"));
        manager->setAutosaveChanges(false);
        QVERIFY(!filesWithUiSettings(g_configHome).isEmpty());
    }
};

int main(int argc, char **argv)
{
    // Set before the application exists: the manager reads and would write the config of the application under this directory
    const QTemporaryDir configHome;
    if (!configHome.isValid())
        return 1;
    g_configHome = configHome.path();
    qputenv("XDG_CONFIG_HOME", g_configHome.toUtf8());
    QGuiApplication app(argc, argv);
    TestColorScheme test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_color_scheme.moc"
