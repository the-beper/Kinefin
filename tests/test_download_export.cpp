// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Host-side export names: separators and control characters never reach a path, the layout mirrors the library, the whole file name stays within the byte cap (multibyte text included) and collisions get a suffix
#include "downloads/ExportNames.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Kinefin::DownloadRecord;
using Kinefin::ExportNames;

namespace {
DownloadRecord movie()
{
    DownloadRecord record;
    record.serverId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    record.userId = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    record.itemId = QStringLiteral("00000000000000000000000000000001");
    record.name = QStringLiteral("Point Break");
    record.type = QStringLiteral("Movie");
    record.container = QStringLiteral("mkv");
    record.year = 1991;
    return record;
}
} // namespace

class TestDownloadExport : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void sanitisesComponents()
    {
        QCOMPARE(
            ExportNames::component(QStringLiteral("A/B\\C:D*E?F\"G<H>I|J"), QStringLiteral("x")),
            QStringLiteral("A B C D E F G H I J"));
        QCOMPARE(ExportNames::component(QStringLiteral("  ..name..  "), QStringLiteral("x")),
                 QStringLiteral("name"));
        QCOMPARE(ExportNames::component(QStringLiteral("..."), QStringLiteral("item")),
                 QStringLiteral("item"));
        QVERIFY(ExportNames::component(QString(500, QLatin1Char('a')), QStringLiteral("x"))
                    .toUtf8()
                    .size()
                <= ExportNames::MaxComponentBytes);
    }

    void layoutsMoviesAndEpisodes()
    {
        const DownloadRecord film = movie();
        QCOMPARE(ExportNames::relativePath(film), QStringLiteral("Movies/Point Break (1991).mkv"));
        DownloadRecord episode = film;
        episode.type = QStringLiteral("Episode");
        episode.seriesName = QStringLiteral("Abyss Station");
        episode.name = QStringLiteral("Weightless");
        episode.seasonNumber = 2;
        episode.episodeNumber = 6;
        QCOMPARE(ExportNames::relativePath(episode),
                 QStringLiteral("Series/Season 02/Abyss Station - S02E06 - Weightless.mkv"));
    }

    void numbersSpecialsAsSeasonZero()
    {
        DownloadRecord episode = movie();
        episode.type = QStringLiteral("Episode");
        episode.seriesName = QStringLiteral("Abyss Station");
        episode.name = QStringLiteral("Recap");
        episode.seasonNumber = 0;
        episode.episodeNumber = 3;
        QCOMPARE(ExportNames::relativePath(episode),
                 QStringLiteral("Series/Season 00/Abyss Station - S00E03 - Recap.mkv"));

        // An unknown season stays unknown and the code falls back to the item id
        episode.seasonNumber = -1;
        QCOMPARE(ExportNames::relativePath(episode),
                 QStringLiteral("Series/Season Unknown/Abyss Station - %1 - Recap.mkv")
                     .arg(episode.itemId));
    }

    void capsTheWholeEpisodeFileName()
    {
        DownloadRecord episode = movie();
        episode.type = QStringLiteral("Episode");
        episode.seriesName = QString(300, QLatin1Char('S'));
        episode.name = QString(300, QLatin1Char('E'));
        episode.seasonNumber = 12;
        episode.episodeNumber = 34;
        const QString relative = ExportNames::relativePath(episode);
        const QString fileName = QFileInfo(relative).fileName();
        QVERIFY(fileName.toUtf8().size() <= ExportNames::MaxFileNameBytes);
        QVERIFY(fileName.contains(QStringLiteral(" - S12E34 - ")));
        QVERIFY(fileName.endsWith(QStringLiteral(".mkv")));
        // The collision suffix still fits a 255 byte file name
        QVERIFY(fileName.toUtf8().size() + QByteArray(" (999)").size() <= 255);
    }

    void capsTheWholeMultibyteFileName()
    {
        DownloadRecord episode = movie();
        episode.type = QStringLiteral("Episode");
        // Two and three bytes per character, so every cut can land inside a code point
        episode.seriesName = QStringLiteral("a") + QString(200, QChar(0x00E9));
        episode.name = QStringLiteral("b") + QString(200, QChar(0x20AC));
        episode.seasonNumber = 1;
        episode.episodeNumber = 2;
        const QString fileName = QFileInfo(ExportNames::relativePath(episode)).fileName();
        QVERIFY(fileName.toUtf8().size() <= ExportNames::MaxFileNameBytes);
        QVERIFY(!fileName.contains(QChar::ReplacementCharacter));
        QCOMPARE(QString::fromUtf8(fileName.toUtf8()), fileName);
        QVERIFY(fileName.endsWith(QStringLiteral(".mkv")));
    }

    void capsALongMovieTitleKeepingTheYear()
    {
        DownloadRecord film = movie();
        film.name = QString(500, QLatin1Char('T'));
        const QString fileName = QFileInfo(ExportNames::relativePath(film)).fileName();
        QVERIFY(fileName.toUtf8().size() <= ExportNames::MaxFileNameBytes);
        QVERIFY(fileName.endsWith(QStringLiteral(" (1991).mkv")));
    }

    void cutsComponentsOnACodePoint_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("2 bytes") << QStringLiteral("a") + QString(200, QChar(0x00E9));
        QTest::newRow("3 bytes") << QStringLiteral("a") + QString(200, QChar(0x20AC));
        QTest::newRow("4 bytes") << QStringLiteral("a")
                                        + QString::fromUcs4(U"\U0001F600").repeated(100);
        QTest::newRow("exactly at the cap")
            << QString(ExportNames::MaxComponentBytes / 2, QChar(0x00E9));
    }

    void cutsComponentsOnACodePoint()
    {
        QFETCH(QString, text);
        const QString clean = ExportNames::component(text, QStringLiteral("x"));
        const int bytes = int(clean.toUtf8().size());
        QVERIFY(bytes <= ExportNames::MaxComponentBytes);
        // At most one character (4 bytes) short of the cap
        QVERIFY(bytes > ExportNames::MaxComponentBytes - 4);
        QVERIFY(!clean.contains(QChar::ReplacementCharacter));
        QCOMPARE(QString::fromUtf8(clean.toUtf8()), clean);
    }

    void suffixesCollisions()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const DownloadRecord film = movie();
        const QString first = ExportNames::availablePath(dir.path(), film);
        QVERIFY(QDir().mkpath(QFileInfo(first).absolutePath()));
        QVERIFY(QFile(first).open(QIODevice::WriteOnly));
        const QString second = ExportNames::availablePath(dir.path(), film);
        QCOMPARE(second, dir.path() + QStringLiteral("/Movies/Point Break (1991) (2).mkv"));
    }
};

QTEST_GUILESS_MAIN(TestDownloadExport)
#include "test_download_export.moc"
