// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include "api/DeviceProfile.h"

using Kinefin::buildDeviceProfile;
using Kinefin::DeviceProfileOptions;

class TestDeviceProfile : public QObject {
    Q_OBJECT
  private:
    static QJsonObject goldenProfile()
    {
        QFile file(
            QStringLiteral(KINEFIN_SOURCE_DIR "/tests/fixtures/device_profile_default.json"));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    static QJsonObject videoTranscodingProfile(const QJsonObject &profile)
    {
        for (const QJsonValue &value :
             profile.value(QStringLiteral("TranscodingProfiles")).toArray()) {
            if (value.toObject().value(QStringLiteral("Type")).toString()
                == QStringLiteral("Video"))
                return value.toObject();
        }
        return {};
    }

    static QJsonArray codecProfiles(const DeviceProfileOptions &options)
    {
        return buildDeviceProfile(options).value(QStringLiteral("CodecProfiles")).toArray();
    }

    // One Equals/NotEquals/... condition as "Property Condition Value", for compact asserts
    static QStringList conditions(const QJsonObject &codecProfile)
    {
        QStringList result;
        for (const QJsonValue &value : codecProfile.value(QStringLiteral("Conditions")).toArray()) {
            const QJsonObject condition = value.toObject();
            result << condition.value(QStringLiteral("Property")).toString() + QLatin1Char(' ')
                          + condition.value(QStringLiteral("Condition")).toString()
                          + QLatin1Char(' ') + condition.value(QStringLiteral("Value")).toString();
        }
        return result;
    }

  private Q_SLOTS:
    void defaultMatchesGolden()
    {
        const QJsonObject golden = goldenProfile();
        QVERIFY(!golden.isEmpty());
        QCOMPARE(buildDeviceProfile({}), golden);
        // ResponseProfiles no longer exist in the 12.4 DeviceProfile schema
        QVERIFY(!buildDeviceProfile({}).contains(QStringLiteral("ResponseProfiles")));
    }

    void videoCodecMatrix()
    {
        const auto codecs = [](bool allow, bool prefer) {
            DeviceProfileOptions options;
            options.allowTranscodeToHevc = allow;
            options.preferTranscodeToH265 = prefer;
            return videoTranscodingProfile(buildDeviceProfile(options))
                .value(QStringLiteral("VideoCodec"))
                .toString();
        };
        QCOMPARE(codecs(false, false), QStringLiteral("h264,mpeg4,mpeg2video"));
        QCOMPARE(codecs(false, true), QStringLiteral("h264,mpeg4,mpeg2video"));
        QCOMPARE(codecs(true, false), QStringLiteral("h264,h265,hevc,mpeg4,mpeg2video"));
        QCOMPARE(codecs(true, true), QStringLiteral("h265,hevc,h264,mpeg4,mpeg2video"));
    }

    void maxAudioChannels()
    {
        const auto channels = [](const QString &setting) {
            DeviceProfileOptions options;
            options.channels = setting;
            return videoTranscodingProfile(buildDeviceProfile(options))
                .value(QStringLiteral("MaxAudioChannels"));
        };
        QCOMPARE(channels(QStringLiteral("2.0")), QJsonValue(QStringLiteral("2")));
        for (const QString &setting :
             {QStringLiteral("auto"), QStringLiteral("5.1,2.0"), QStringLiteral("7.1,5.1,2.0")}) {
            QCOMPARE(channels(setting), QJsonValue(QStringLiteral("6")));
        }
    }

    void codecProfilePerSetting()
    {
        DeviceProfileOptions none;
        none.forceTranscodeDovi = false;
        QVERIFY(codecProfiles(none).isEmpty());

        DeviceProfileOptions dovi;
        QCOMPARE(codecProfiles(dovi).size(), 1);
        QCOMPARE(conditions(codecProfiles(dovi).at(0).toObject()),
                 QStringList{QStringLiteral("VideoRangeType NotEquals DOVI")});

        const auto only = [](auto setter) {
            DeviceProfileOptions options;
            options.forceTranscodeDovi = false;
            setter(options);
            return codecProfiles(options);
        };

        const QJsonArray hdr = only([](auto &o) { o.forceTranscodeHdr = true; });
        QCOMPARE(hdr.size(), 1);
        QCOMPARE(conditions(hdr.at(0).toObject()),
                 QStringList{QStringLiteral("VideoRangeType Equals SDR")});

        const QJsonArray hi10p = only([](auto &o) { o.forceTranscodeHi10p = true; });
        QCOMPARE(hi10p.size(), 1);
        QCOMPARE(conditions(hi10p.at(0).toObject()),
                 QStringList{QStringLiteral("VideoBitDepth LessThanEqual 8")});

        const QJsonArray hevc = only([](auto &o) { o.forceTranscodeHevc = true; });
        QCOMPARE(hevc.size(), 2);
        QCOMPARE(hevc.at(0).toObject().value(QStringLiteral("Codec")).toString(),
                 QStringLiteral("hevc"));
        QCOMPARE(hevc.at(1).toObject().value(QStringLiteral("Codec")).toString(),
                 QStringLiteral("h265"));
        for (const QJsonValue &profile : hevc)
            QCOMPARE(conditions(profile.toObject()), QStringList{QStringLiteral("Width Equals 0")});

        const QJsonArray av1 = only([](auto &o) { o.forceTranscodeAv1 = true; });
        QCOMPARE(av1.size(), 1);
        QCOMPARE(av1.at(0).toObject().value(QStringLiteral("Codec")).toString(),
                 QStringLiteral("av1"));
        QCOMPARE(conditions(av1.at(0).toObject()), QStringList{QStringLiteral("Width Equals 0")});

        const QJsonArray fourK = only([](auto &o) { o.forceTranscode4k = true; });
        QCOMPARE(fourK.size(), 1);
        QVERIFY(!fourK.at(0).toObject().contains(QStringLiteral("Codec")));
        QCOMPARE(conditions(fourK.at(0).toObject()),
                 (QStringList{QStringLiteral("Width LessThanEqual 1920"),
                              QStringLiteral("Height LessThanEqual 1080")}));
    }

    void codecProfileOrderAndRequiredFlag()
    {
        DeviceProfileOptions all;
        all.forceTranscodeHdr = all.forceTranscodeHi10p = all.forceTranscodeHevc = true;
        all.forceTranscodeAv1 = all.forceTranscode4k = true;
        const QJsonArray profiles = codecProfiles(all);
        // Parity §3 order: dovi, hdr, hi10p, hevc, h265, av1, 4k
        QStringList sequence;
        int conditionCount = 0;
        for (const QJsonValue &profile : profiles) {
            const QJsonObject object = profile.toObject();
            sequence << object.value(QStringLiteral("Codec")).toString() + QLatin1Char('|')
                            + conditions(object).join(QLatin1Char(';'));
            for (const QJsonValue &condition :
                 object.value(QStringLiteral("Conditions")).toArray()) {
                ++conditionCount;
                QVERIFY(!condition.toObject().contains(QStringLiteral("IsRequired")));
            }
        }
        QCOMPARE(sequence,
                 (QStringList{
                     QStringLiteral("|VideoRangeType NotEquals DOVI"),
                     QStringLiteral("|VideoRangeType Equals SDR"),
                     QStringLiteral("|VideoBitDepth LessThanEqual 8"),
                     QStringLiteral("hevc|Width Equals 0"), QStringLiteral("h265|Width Equals 0"),
                     QStringLiteral("av1|Width Equals 0"),
                     QStringLiteral("|Width LessThanEqual 1920;Height LessThanEqual 1080")}));
        QCOMPARE(conditionCount, 8);
    }

    void alwaysForceDropsVideoDirectPlay()
    {
        DeviceProfileOptions options;
        options.alwaysForceTranscode = true;
        QStringList types;
        for (const QJsonValue &profile :
             buildDeviceProfile(options).value(QStringLiteral("DirectPlayProfiles")).toArray()) {
            types << profile.toObject().value(QStringLiteral("Type")).toString();
        }
        QCOMPARE(types, (QStringList{QStringLiteral("Audio"), QStringLiteral("Photo")}));
        QCOMPARE(buildDeviceProfile(options).value(QStringLiteral("TranscodingProfiles")),
                 buildDeviceProfile({}).value(QStringLiteral("TranscodingProfiles")));
    }
};
QTEST_APPLESS_MAIN(TestDeviceProfile)
#include "test_device_profile.moc"
