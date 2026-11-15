// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The per-series store on its own: what reaches the disk, what a damaged or foreign file turns into, the caps, and the track matching the player relies on
#include "player/SeriesPreferences.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Kinefin::pickTrack;
using Kinefin::SeriesPreferences;
using Kinefin::TrackChoice;
using Kinefin::TrackFact;

namespace {
const QString kServer = QStringLiteral("0123456789abcdef0123456789abcdef");
const QString kOtherServer = QStringLiteral("fedcba9876543210fedcba9876543210");
const QString kUser = QStringLiteral("aaaabbbbccccddddeeeeffff00001111");
const QString kOtherUser = QStringLiteral("1111000ffffeeeeddddccccbbbbaaaa0");
const QString kSeries = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");

// The id of the n-th of many series
QString seriesId(int number)
{
    return QStringLiteral("%1").arg(number, 32, 16, QLatin1Char('0'));
}

QString ownerPath(const QTemporaryDir &dir, const QString &server = kServer,
                  const QString &user = kUser)
{
    return dir.filePath(server + QLatin1Char('_') + user + QStringLiteral(".json"));
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QJsonObject readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                          : QJsonObject();
}

// A store of the test owner over `dir`, its writes at once
struct Store {
    explicit Store(const QTemporaryDir &dir)
    {
        store.setSaveDelay(std::chrono::milliseconds(0));
        store.setDirectory(dir.path());
        store.setOwner(kServer, kUser);
    }
    SeriesPreferences store;
};
} // namespace

class TestSeriesPreferences : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void choicesComeBackFromTheFileAsIdsCodesAndNumbers()
    {
        QTemporaryDir dir;
        {
            Store owner(dir);
            owner.store.setAudio(kSeries, TrackChoice{QStringLiteral("jpn"), false, 1});
            owner.store.setSubtitle(kSeries, false, TrackChoice{QStringLiteral("en-us"), true, 0});
            owner.store.setRate(kSeries, 1.25);
            owner.store.setAspect(kSeries, QStringLiteral("zoom"));
            QVERIFY(owner.store.contains(kSeries.toUpper()));
        }
        // Written on destruction, private to the user
        const QString path = ownerPath(dir);
        QVERIFY(QFile::exists(path));
        QCOMPARE(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::Permissions());
        const QJsonObject root = readFile(path);
        QCOMPARE(root.keys(), QStringList({QStringLiteral("entries"), QStringLiteral("version")}));
        const QJsonObject entry =
            root.value(QStringLiteral("entries")).toObject().value(kSeries).toObject();
        QCOMPARE(entry.keys(), QStringList({QStringLiteral("aspect"), QStringLiteral("audio"),
                                            QStringLiteral("rate"), QStringLiteral("seq"),
                                            QStringLiteral("subtitle")}));
        Store again(dir);
        const Kinefin::SeriesChoice choice = again.store.choice(kSeries);
        QCOMPARE(choice.audio, std::optional(TrackChoice{QStringLiteral("jpn"), false, 1}));
        QVERIFY(!choice.subtitleOff);
        QCOMPARE(choice.subtitle, std::optional(TrackChoice{QStringLiteral("en-us"), true, 0}));
        QCOMPARE(choice.rate, std::optional(1.25));
        QCOMPARE(choice.aspect, QStringLiteral("zoom"));
    }

    void anEmptyChoiceRemovesItsFieldAndTheLastOneTheFile()
    {
        QTemporaryDir dir;
        Store owner(dir);
        QSignalSpy changed(&owner.store, &SeriesPreferences::changed);
        owner.store.setRate(kSeries, 2.0);
        owner.store.setSubtitle(kSeries, true, std::nullopt);
        QVERIFY(owner.store.choice(kSeries).subtitleOff);
        owner.store.flush();
        QTRY_VERIFY(QFile::exists(ownerPath(dir)));
        owner.store.setRate(kSeries, std::nullopt);
        QVERIFY(!owner.store.choice(kSeries).rate);
        QVERIFY(owner.store.contains(kSeries));
        owner.store.setSubtitle(kSeries, false, std::nullopt);
        QVERIFY(!owner.store.contains(kSeries));
        // Removing what is not there writes nothing
        const int before = changed.size();
        owner.store.setAspect(kSeries, QString());
        QCOMPARE(changed.size(), before);
        owner.store.flush();
        QTRY_VERIFY(!QFile::exists(ownerPath(dir)));
    }

    void invalidValuesAreNeverKept()
    {
        QTemporaryDir dir;
        Store owner(dir);
        owner.store.setRate(kSeries, 9.0);
        owner.store.setRate(kSeries, std::numeric_limits<double>::quiet_NaN());
        owner.store.setAspect(kSeries, QStringLiteral("custom"));
        owner.store.setAudio(kSeries, TrackChoice{QStringLiteral("<b>"), false, -1});
        owner.store.setAudio(QStringLiteral("not an id"),
                             TrackChoice{QStringLiteral("eng"), false, 0});
        QVERIFY(!owner.store.contains(kSeries));
        // A code mpv gives in capitals is kept in lower case
        owner.store.setAudio(
            kSeries, TrackChoice{Kinefin::trackLanguage(QStringLiteral("EN-GB")), false, 0});
        QCOMPARE(owner.store.choice(kSeries).audio->lang, QStringLiteral("en-gb"));
    }

    void aWronglyTypedFieldIsDroppedAlone()
    {
        QTemporaryDir dir;
        const QJsonObject entries{
            {kSeries,
             QJsonObject{{QStringLiteral("audio"), QStringLiteral("eng")},
                         {QStringLiteral("subtitle"), QJsonObject{{QStringLiteral("off"), true}}},
                         {QStringLiteral("rate"), 1.5},
                         {QStringLiteral("aspect"), QStringLiteral("custom")},
                         {QStringLiteral("seq"), QStringLiteral("seven")}}},
            {seriesId(1),
             QJsonObject{{QStringLiteral("audio"),
                          QJsonObject{{QStringLiteral("lang"), QStringLiteral("<img src=x>")},
                                      {QStringLiteral("ordinal"), 2}}}}},
            {QStringLiteral("../../escape"), QJsonObject{{QStringLiteral("rate"), 2.0}}},
            {seriesId(2), QStringLiteral("not an entry")}};
        QVERIFY(writeFile(ownerPath(dir),
                          QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                    {QStringLiteral("entries"), entries}})
                              .toJson()));
        Store owner(dir);
        const Kinefin::SeriesChoice first = owner.store.choice(kSeries);
        QVERIFY(!first.audio);
        QVERIFY(first.subtitleOff);
        QCOMPARE(first.rate, std::optional(1.5));
        QVERIFY(first.aspect.isEmpty());
        // A language that is no code goes; the position stays
        QCOMPARE(owner.store.choice(seriesId(1)).audio,
                 std::optional(TrackChoice{QString(), false, 2}));
        QVERIFY(!owner.store.contains(seriesId(2)));
        QVERIFY(!owner.store.contains(QStringLiteral("../../escape")));
    }

    void aLargeFileIsDeletedUnreadAndANewerOneKeptAside()
    {
        QTemporaryDir dir;
        QVERIFY(writeFile(ownerPath(dir), QByteArray(SeriesPreferences::MaxFileBytes + 1, ' ')));
        {
            Store owner(dir);
            QVERIFY(!QFile::exists(ownerPath(dir)));
        }
        QVERIFY(writeFile(ownerPath(dir), R"({"version":2,"entries":{}})"));
        Store owner(dir);
        QVERIFY(!QFile::exists(ownerPath(dir)));
        QVERIFY(QFile::exists(ownerPath(dir) + QStringLiteral(".broken")));
        // Another owner's file is not read for this one
        QVERIFY(writeFile(
            ownerPath(dir, kServer, kOtherUser),
            R"({"version":1,"entries":{"aa11bb22cc33dd44ee55ff6677889900":{"rate":2}}})"));
        QVERIFY(!owner.store.contains(kSeries));
        owner.store.setOwner(kServer, kOtherUser);
        QVERIFY(owner.store.contains(kSeries));
    }

    void theLeastRecentSeriesGoesFirst()
    {
        QTemporaryDir dir;
        Store owner(dir);
        for (int i = 0; i < SeriesPreferences::MaxSeries; ++i)
            owner.store.setRate(seriesId(i), 1.5);
        // The first one chosen again is the most recent now
        owner.store.setRate(seriesId(0), 2.0);
        owner.store.setRate(kSeries, 1.5);
        QVERIFY(owner.store.contains(seriesId(0)));
        QVERIFY(!owner.store.contains(seriesId(1)));
        QVERIFY(owner.store.contains(kSeries));
        // Full entries for every series pass the file's cap: the oldest stay out of the file
        for (int i = 0; i < SeriesPreferences::MaxSeries; ++i) {
            owner.store.setAudio(seriesId(i), TrackChoice{QStringLiteral("en-us"), true, 12});
            owner.store.setSubtitle(seriesId(i), false,
                                    TrackChoice{QStringLiteral("pt-br"), true, 3});
            owner.store.setAspect(seriesId(i), QStringLiteral("force_16_9_if_4_3"));
        }
        owner.store.flush();
        QTRY_VERIFY(QFile::exists(ownerPath(dir)));
        QVERIFY(QFileInfo(ownerPath(dir)).size() <= SeriesPreferences::MaxFileBytes);
        const QJsonObject kept =
            readFile(ownerPath(dir)).value(QStringLiteral("entries")).toObject();
        QVERIFY(kept.contains(seriesId(SeriesPreferences::MaxSeries - 1)));
        QVERIFY(!kept.contains(kSeries));
    }

    void forgettingAUserOrAServerDeletesTheirFiles()
    {
        QTemporaryDir dir;
        for (const auto &[server, user] :
             {std::pair{kServer, kUser}, std::pair{kServer, kOtherUser},
              std::pair{kOtherServer, kUser}})
            QVERIFY(writeFile(ownerPath(dir, server, user), R"({"version":1,"entries":{}})"));
        Store owner(dir);
        owner.store.setRate(kSeries, 1.5);
        QSignalSpy changed(&owner.store, &SeriesPreferences::changed);
        owner.store.forgetUser(kServer, kOtherUser);
        QVERIFY(!QFile::exists(ownerPath(dir, kServer, kOtherUser)));
        QVERIFY(owner.store.contains(kSeries));
        QCOMPARE(changed.size(), 0);
        owner.store.forgetServer(kServer);
        QVERIFY(!QFile::exists(ownerPath(dir)));
        QVERIFY(QFile::exists(ownerPath(dir, kOtherServer, kUser)));
        // The owner's choices go from memory too, and no pending write brings the file back
        QVERIFY(!owner.store.contains(kSeries));
        QCOMPARE(changed.size(), 1);
        QTest::qWait(50);
        owner.store.flush();
        QVERIFY(!QFile::exists(ownerPath(dir)));
    }

    void aLinkInTheFilesPlaceIsNeverFollowed()
    {
        QTemporaryDir dir;
        const QString target = dir.filePath(QStringLiteral("elsewhere.json"));
        QVERIFY(QFile::link(target, ownerPath(dir)));
        Store owner(dir);
        QVERIFY(!QFileInfo(ownerPath(dir)).isSymLink());
        QVERIFY(QFile::link(target, ownerPath(dir)));
        owner.store.setRate(kSeries, 1.5);
        owner.store.flush();
        QTRY_VERIFY(QFileInfo(ownerPath(dir)).isFile());
        QVERIFY(!QFileInfo(ownerPath(dir)).isSymLink());
        QVERIFY(!QFile::exists(target));
    }

    void largeOrderNumbersFromAFileStillOrderTheChoices()
    {
        QTemporaryDir dir;
        QJsonObject entries;
        for (int i = 0; i < SeriesPreferences::MaxSeries; ++i)
            entries.insert(seriesId(i), QJsonObject{{QStringLiteral("rate"), 1.5},
                                                    {QStringLiteral("seq"), 1e15 - 1000 + i}});
        QVERIFY(writeFile(ownerPath(dir),
                          QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                    {QStringLiteral("entries"), entries}})
                              .toJson(QJsonDocument::Compact)));
        Store owner(dir);
        // A new choice is the most recent, and the oldest of the file makes room for it
        owner.store.setRate(kSeries, 2.0);
        QVERIFY(owner.store.contains(kSeries));
        QVERIFY(!owner.store.contains(seriesId(0)));
        owner.store.setRate(seriesId(500 + 1), 2.0);
        QVERIFY(owner.store.contains(kSeries));
        QVERIFY(!owner.store.contains(seriesId(1)));
    }

    void aPendingWriteGoesToItsOwnersFile()
    {
        QTemporaryDir dir;
        SeriesPreferences store;
        store.setDirectory(dir.path());
        store.setOwner(kServer, kUser);
        // Held by the second of debounce, then the owner changes
        store.setRate(kSeries, 1.5);
        store.setOwner(kServer, kOtherUser);
        QVERIFY(QFile::exists(ownerPath(dir)));
        QVERIFY(!QFile::exists(ownerPath(dir, kServer, kOtherUser)));
        QVERIFY(!store.contains(kSeries));
        store.setOwner(kServer, kUser);
        QCOMPARE(store.choice(kSeries).rate, std::optional(1.5));
    }

    void withoutAFolderTheChoicesLiveInMemory()
    {
        SeriesPreferences store;
        store.setOwner(kServer, kUser);
        store.setRate(kSeries, 1.5);
        QCOMPARE(store.choice(kSeries).rate, std::optional(1.5));
        // Without a valid owner nothing is kept
        store.setOwner(QString(), QString());
        store.setRate(kSeries, 1.5);
        QVERIFY(!store.contains(kSeries));
    }

    void tracksMatchByLanguageThenPosition()
    {
        const QList<TrackFact> tracks{{3, QStringLiteral("eng"), false},
                                      {4, QStringLiteral("jpn"), false},
                                      {5, QStringLiteral("eng"), true},
                                      {6, QStringLiteral("qaa"), false},
                                      {7, QString(), false}};
        // The language with the forced flag, `en` being `eng`
        QCOMPARE(pickTrack(tracks, {QStringLiteral("en"), true, 0}), 5);
        QCOMPARE(pickTrack(tracks, {QStringLiteral("eng"), false, 2}), 3);
        // Then the language alone, whatever the position was
        QCOMPARE(pickTrack(tracks, {QStringLiteral("ja"), true, 0}), 4);
        QCOMPARE(pickTrack(tracks, {QStringLiteral("en-gb"), false, 4}), 3);
        // A code QLocale does not know is compared as it is
        QCOMPARE(pickTrack(tracks, {QStringLiteral("qaa"), false, -1}), 6);
        // Bibliographic codes are the same languages (`fre` is `fra`)
        QCOMPARE(pickTrack({{8, QStringLiteral("fra"), false}}, {QStringLiteral("fre"), false, 3}),
                 8);
        QCOMPARE(pickTrack({{9, QStringLiteral("deu"), false}}, {QStringLiteral("ger"), false, 3}),
                 9);
        // A language the file lacks picks nothing, not the same position
        QCOMPARE(pickTrack(tracks, {QStringLiteral("fre"), false, 1}), 0);
        // Without a language, the same position
        QCOMPARE(pickTrack(tracks, {QString(), false, 4}), 7);
        QCOMPARE(pickTrack(tracks, {QString(), false, 5}), 0);
        QCOMPARE(pickTrack({}, {QStringLiteral("eng"), false, 0}), 0);
    }
};

QTEST_GUILESS_MAIN(TestSeriesPreferences)
#include "test_series_preferences.moc"
