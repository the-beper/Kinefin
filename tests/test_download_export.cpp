// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Host-side export names: separators and control characters never reach a path, the layout mirrors the library and collisions get a suffix
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
