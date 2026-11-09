// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/DeviceProfile.h"
#include "settings/ClientSettings.h"
#include "settings/SettingDefinitions.h"

#include <QAbstractItemModel>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <limits>

#include <unistd.h>

using Kinefin::ClientSettings;

namespace {
QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}
} // namespace

class TestClientSettings : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir m_dir;
    QString path() const { return m_dir.filePath(QStringLiteral("kinefin.conf")); }

  private Q_SLOTS:
    void init()
    {
        QFile::remove(path());
        QFile::remove(path() + QStringLiteral(".broken"));
    }

    void defaultsAreServedAndNothingIsWritten()
    {
        {
            ClientSettings settings(path());
            for (const auto &definition : Kinefin::settingDefinitions()) {
                QCOMPARE(settings.value(definition.section, definition.key),
                         definition.defaultValue);
                QVERIFY(settings.isDefault(definition.section, definition.key));
            }
            QVERIFY(!settings.value(QStringLiteral("video"), QStringLiteral("nope")).isValid());
        }
        QVERIFY(!QFile::exists(path()));
    }

    void roundTripOfEveryType()
    {
        const QString multiline = QStringLiteral(" profile=gpu-hq\n# comment $x [a] =\nsub=\\n ");
        {
            ClientSettings settings(path());
            QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("fullscreen"), true));
            QVERIFY(settings.setValue(QStringLiteral("video"), QStringLiteral("cache"), 150));
            QVERIFY(settings.setValue(QStringLiteral("video"),
                                      QStringLiteral("default_playback_speed"), 1.5));
            QVERIFY(settings.setValue(QStringLiteral("audio"), QStringLiteral("channels"),
                                      QStringLiteral("7.1,5.1,2.0")));
            QVERIFY(settings.setValue(QStringLiteral("other"), QStringLiteral("other_conf"),
                                      multiline));
            QVERIFY(settings.setValue(QStringLiteral("audio"), QStringLiteral("passthrough.dts-hd"),
                                      true));
        }
        ClientSettings reopened(path());
        QCOMPARE(reopened.value(QStringLiteral("main"), QStringLiteral("fullscreen")), true);
        QCOMPARE(reopened.value(QStringLiteral("video"), QStringLiteral("cache")), 150);
        QCOMPARE(reopened.value(QStringLiteral("video"), QStringLiteral("default_playback_speed")),
                 1.5);
        QCOMPARE(reopened.value(QStringLiteral("audio"), QStringLiteral("channels")),
                 QStringLiteral("7.1,5.1,2.0"));
        QCOMPARE(reopened.value(QStringLiteral("other"), QStringLiteral("other_conf")), multiline);
        QCOMPARE(reopened.value(QStringLiteral("audio"), QStringLiteral("passthrough.dts-hd")),
                 true);
    }

    void aValueEqualToTheDefaultIsNotStored()
    {
        ClientSettings settings(path());
        QVERIFY(settings.setValue(QStringLiteral("video"), QStringLiteral("cache"), 500));
        QVERIFY(readAll(path()).contains("cache=500"));
        QVERIFY(settings.setValue(QStringLiteral("video"), QStringLiteral("cache"), 75));
        QVERIFY(!readAll(path()).contains("cache"));
        QVERIFY(settings.isDefault(QStringLiteral("video"), QStringLiteral("cache")));
    }

    void invalidWritesAreRefused()
    {
        ClientSettings settings(path());
        QSignalSpy changed(&settings, &ClientSettings::valueChanged);
        QVERIFY(!settings.setValue(QStringLiteral("video"), QStringLiteral("nope"), 1));
        QVERIFY(!settings.setValue(QStringLiteral("video"), QStringLiteral("cache"),
                                   999)); // outside the list
        QVERIFY(!settings.setValue(QStringLiteral("video"), QStringLiteral("cache"),
                                   QStringLiteral("75"))); // wrong type
        QVERIFY(!settings.setValue(QStringLiteral("main"), QStringLiteral("fullscreen"), 1));
        QVERIFY(!settings.setValue(QStringLiteral("video"),
                                   QStringLiteral("default_playback_speed"), 9.0));
        QVERIFY(!settings.setValue(QStringLiteral("video"), QStringLiteral("hardwareDecoding"),
                                   QStringLiteral("osx_compat")));
        QVERIFY(!settings.setValue(QStringLiteral("main"), QStringLiteral("logLevel"),
                                   QStringLiteral("info"))); // hidden
        QCOMPARE(changed.size(), 0);
        QVERIFY(!QFile::exists(path()));
        // Numbers that are not finite or do not fit are refused, not converted.
        QVERIFY(!settings.setValue(QStringLiteral("video"), QStringLiteral("cache"), 1e20));
        QVERIFY(!settings.setValue(QStringLiteral("video"), QStringLiteral("cache"), std::nan("")));
        QVERIFY(!settings.setValue(QStringLiteral("video"),
                                   QStringLiteral("default_playback_speed"), std::nan("")));
        QVERIFY(!settings.setValue(QStringLiteral("video"),
                                   QStringLiteral("default_playback_speed"),
                                   std::numeric_limits<double>::infinity()));
        // Line breaks only where the setting is multiline.
        QVERIFY(!settings.setValue(QStringLiteral("subtitles"), QStringLiteral("font"),
                                   QStringLiteral("a\nb")));
        QVERIFY(settings.setValue(QStringLiteral("other"), QStringLiteral("other_conf"),
                                  QStringLiteral("a\nb")));
        // A number from QML arrives as a double.
        QVERIFY(settings.setValue(QStringLiteral("video"), QStringLiteral("cache"), 150.0));
        QCOMPARE(settings.value(QStringLiteral("video"), QStringLiteral("cache")), 150);
    }

    void aHandEditedInvalidValueFallsBackToTheDefault()
    {
        writeFile(path(), "[video]\ncache=999\nhardwareDecoding=osx_compat\ndeinterlace=maybe\n\n["
                          "main]\nlogLevel=info\ndisablemouse=true\n");
        ClientSettings settings(path());
        QCOMPARE(settings.value(QStringLiteral("video"), QStringLiteral("cache")), 75);
        QCOMPARE(settings.value(QStringLiteral("video"), QStringLiteral("hardwareDecoding")),
                 QStringLiteral("copy"));
        QCOMPARE(settings.value(QStringLiteral("video"), QStringLiteral("deinterlace")), false);
        // Hidden values are read from the file even though the UI cannot set them.
        QCOMPARE(settings.value(QStringLiteral("main"), QStringLiteral("logLevel")),
                 QStringLiteral("info"));
        QCOMPARE(settings.value(QStringLiteral("main"), QStringLiteral("disablemouse")), true);
    }

    void valueChangedFiresOnlyOnARealChange()
    {
        ClientSettings settings(path());
        QSignalSpy changed(&settings, &ClientSettings::valueChanged);
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("fullscreen"),
                                  false)); // already false
        QCOMPARE(changed.size(), 0);
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("fullscreen"), true));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.at(0).at(0).toString(), QStringLiteral("main"));
        QCOMPARE(changed.at(0).at(1).toString(), QStringLiteral("fullscreen"));
    }

    void unknownKeysSurviveAWrite()
    {
        writeFile(path(), "[video]\nfuture_key=7\n");
        {
            ClientSettings settings(path());
            QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("fullscreen"), true));
        }
        QVERIFY(readAll(path()).contains("future_key=7"));
    }

    void aNewerFileIsMovedAsideAndReplacedByDefaults()
    {
        const QByteArray newer = "[General]\nversion=2\n\n[main]\nfullscreen=true\n";
        writeFile(path(), newer);
        writeFile(path() + QStringLiteral(".broken"), "older broken file");
        ClientSettings settings(path());
        QCOMPARE(readAll(path() + QStringLiteral(".broken")), newer);
        QVERIFY(!QFile::exists(path()));
        QCOMPARE(settings.value(QStringLiteral("main"), QStringLiteral("fullscreen")), false);
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("fullscreen"), true));
        QCOMPARE(readAll(path() + QStringLiteral(".broken")), newer); // untouched by later writes
    }

    void aNewerFileThatCannotBeMovedIsNeverOverwritten()
    {
        // A directory where the staging file must go makes the rename fail on every platform.
        const QByteArray newer = "[General]\nversion=2\n\n[main]\nfullscreen=true\n";
        writeFile(path(), newer);
        QVERIFY(QDir().mkpath(path() + QStringLiteral(".broken.new/keep")));
        {
            ClientSettings settings(path());
            QSignalSpy failed(&settings, &ClientSettings::saveFailed);
            QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("alwaysOnTop"), true));
            QCOMPARE(failed.size(), 1);
            // The change lives in memory for this session.
            QCOMPARE(settings.value(QStringLiteral("main"), QStringLiteral("alwaysOnTop")), true);
        }
        QCOMPARE(readAll(path()), newer);
        QDir(path() + QStringLiteral(".broken.new")).removeRecursively();
    }

    void withoutAPathTheSettingsLiveInMemory()
    {
        ClientSettings settings{QString()};
        QSignalSpy failed(&settings, &ClientSettings::saveFailed);
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("fullscreen"), true));
        QCOMPARE(failed.size(), 1);
        QCOMPARE(settings.value(QStringLiteral("main"), QStringLiteral("fullscreen")), true);
    }

    void anUnwritableFileKeepsTheChangeInMemory()
    {
        if (geteuid() == 0)
            QSKIP("root ignores file permissions");
        ClientSettings settings(path());
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("alwaysOnTop"), true));
        QVERIFY(
            QFile::setPermissions(m_dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QSignalSpy failed(&settings, &ClientSettings::saveFailed);
        QVERIFY(settings.setValue(QStringLiteral("main"), QStringLiteral("fullscreen"), true));
        QCOMPARE(failed.size(), 1);
        QCOMPARE(settings.value(QStringLiteral("main"), QStringLiteral("fullscreen")), true);
        QVERIFY(QFile::setPermissions(m_dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                        | QFileDevice::ExeOwner));
    }

    void profilesHaveTheirOwnFile()
    {
        qputenv("XDG_DATA_HOME", m_dir.filePath(QStringLiteral("data")).toUtf8());
        QStandardPaths::setTestModeEnabled(false);
        const QString first = ClientSettings::pathForProfile(QStringLiteral("one"));
        const QString second = ClientSettings::pathForProfile(QStringLiteral("two"));
        QVERIFY(!first.isEmpty());
        QVERIFY(first != second);
        QVERIFY(first.endsWith(QStringLiteral("/profiles/one/kinefin.conf")));
        QVERIFY(ClientSettings::pathForProfile(QStringLiteral("../x")).isEmpty());
    }

    void theSectionModelFollowsTheSettings()
    {
        ClientSettings settings(path());
        auto *model =
            qobject_cast<QAbstractItemModel *>(settings.sectionModel(QStringLiteral("video")));
        QVERIFY(model);
        QVERIFY(model->rowCount() > 0);
        QHash<QString, int> rows;
        const auto roles = model->roleNames();
        const int keyRole = roles.key("key");
        const int valueRole = roles.key("value");
        for (int row = 0; row < model->rowCount(); ++row)
            rows.insert(model->index(row, 0).data(keyRole).toString(), row);
        QVERIFY(rows.contains(QStringLiteral("cache")));
        QVERIFY(!rows.contains(QStringLiteral("aspect"))); // hidden
        QVERIFY(!rows.contains(QStringLiteral("audio_delay.normal")));

        QSignalSpy changed(model, &QAbstractItemModel::dataChanged);
        QVERIFY(settings.setValue(QStringLiteral("video"), QStringLiteral("cache"), 500));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.at(0).at(0).value<QModelIndex>().row(),
                 rows.value(QStringLiteral("cache")));
        QCOMPARE(model->index(rows.value(QStringLiteral("cache")), 0).data(valueRole), 500);
        QVERIFY(model->setData(model->index(rows.value(QStringLiteral("deinterlace")), 0), true,
                               valueRole));
        QCOMPARE(settings.value(QStringLiteral("video"), QStringLiteral("deinterlace")), true);
        QVERIFY(
            !model->setData(model->index(rows.value(QStringLiteral("cache")), 0), 3, valueRole));
        // The same model object serves every request of a section.
        QCOMPARE(settings.sectionModel(QStringLiteral("video")), model);
    }

    void theDeviceProfileOptionsStartAtTheirDefaults()
    {
        ClientSettings settings(path());
        const Kinefin::DeviceProfileOptions defaults;
        Kinefin::DeviceProfileOptions fromSettings = settings.deviceProfileOptions();
        QCOMPARE(fromSettings.channels, defaults.channels);
        QCOMPARE(fromSettings.preferTranscodeToH265, defaults.preferTranscodeToH265);
        QCOMPARE(fromSettings.allowTranscodeToHevc, defaults.allowTranscodeToHevc);
        QCOMPARE(fromSettings.forceTranscodeHevc, defaults.forceTranscodeHevc);
        QCOMPARE(fromSettings.forceTranscode4k, defaults.forceTranscode4k);
        QCOMPARE(fromSettings.forceTranscodeAv1, defaults.forceTranscodeAv1);
        QCOMPARE(fromSettings.forceTranscodeHdr, defaults.forceTranscodeHdr);
        QCOMPARE(fromSettings.forceTranscodeHi10p, defaults.forceTranscodeHi10p);
        QCOMPARE(fromSettings.forceTranscodeDovi, defaults.forceTranscodeDovi);
        QCOMPARE(fromSettings.alwaysForceTranscode, defaults.alwaysForceTranscode);
        QVERIFY(settings.setValue(QStringLiteral("video"), QStringLiteral("force_transcode_dovi"),
                                  false));
        QVERIFY(settings.setValue(QStringLiteral("audio"), QStringLiteral("channels"),
                                  QStringLiteral("auto")));
        fromSettings = settings.deviceProfileOptions();
        QVERIFY(!fromSettings.forceTranscodeDovi);
        QCOMPARE(fromSettings.channels, QStringLiteral("auto"));
    }

    void windowGeometryRoundTripsAndStaysOutOfTheSettingsPage()
    {
        {
            ClientSettings settings(path());
            QVERIFY(settings.windowGeometry().isEmpty());
            settings.setWindowGeometry(QRect(10, -20, 1280, 720));
            QCOMPARE(settings.windowGeometry(), QRect(10, -20, 1280, 720));
            // Hidden: the generic setter refuses it.
            QVERIFY(!settings.setValue(QStringLiteral("state"), QStringLiteral("geometry"),
                                       QStringLiteral("0,0,800,600")));
        }
        ClientSettings reopened(path());
        QCOMPARE(reopened.windowGeometry(), QRect(10, -20, 1280, 720));
        auto *model =
            qobject_cast<QAbstractItemModel *>(reopened.sectionModel(QStringLiteral("state")));
        QCOMPARE(model->rowCount(), 0);
    }

    void aNonsenseGeometryIsIgnoredOnRead()
    {
        const QStringList bad = {QStringLiteral("1,2,3"),
                                 QStringLiteral("a,b,c,d"),
                                 QStringLiteral("0,0,10,10"),
                                 QStringLiteral("0,0,99999,600"),
                                 QStringLiteral("999999,0,800,600"),
                                 QStringLiteral("0,0,800,-600"),
                                 QStringLiteral("2147483647,0,800,600"),
                                 QStringLiteral("-2147483648,0,800,600"),
                                 QStringLiteral("0,0,2147483647,600")};
        for (const QString &text : bad) {
            writeFile(path(), "[General]\nversion=1\n\n[state]\ngeometry=" + text.toUtf8() + "\n");
            ClientSettings settings(path());
            QVERIFY2(settings.windowGeometry().isEmpty(), qPrintable(text));
        }
    }

    void anEmptyGeometryIsNotStored()
    {
        ClientSettings settings(path());
        settings.setWindowGeometry(QRect());
        QVERIFY(settings.windowGeometry().isEmpty());
        QVERIFY(!QFile::exists(path()));
    }
};
QTEST_GUILESS_MAIN(TestClientSettings)
#include "test_client_settings.moc"
