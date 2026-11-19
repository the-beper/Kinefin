// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ExportNames.h"

#include <QDir>
#include <QFileInfo>

namespace Kinefin {
namespace {
QString twoDigits(int value)
{
    return QStringLiteral("%1").arg(qMax(0, value), 2, 10, QLatin1Char('0'));
}

int utf8Size(const QString &text)
{
    return int(text.toUtf8().size());
}

// At most `maxBytes` of UTF-8, never cutting inside a code point, without trailing dots and spaces
QString clipBytes(const QString &text, int maxBytes)
{
    const QByteArray full = text.toUtf8();
    if (full.size() <= maxBytes)
        return text;
    int cut = maxBytes;
    while (cut > 0 && (full.at(cut) & 0xC0) == 0x80)
        --cut;
    QString clipped = QString::fromUtf8(full.left(cut));
    while (clipped.endsWith(QLatin1Char('.')) || clipped.endsWith(QLatin1Char(' ')))
        clipped.chop(1);
    return clipped;
}
} // namespace

QString ExportNames::component(const QString &text, const QString &fallback)
{
    QString clean;
    clean.reserve(text.size());
    for (const QChar character : text) {
        const char16_t value = character.unicode();
        const bool separator = value == u'/' || value == u'\\' || value == u':' || value == u'*'
                               || value == u'?' || value == u'"' || value == u'<' || value == u'>'
                               || value == u'|';
        if (separator || value < 0x20 || (value >= 0x7f && value <= 0x9f) || value == 0x2028
            || value == 0x2029)
            clean.append(QLatin1Char(' '));
        else
            clean.append(character);
    }
    clean = clean.simplified();
    while (clean.endsWith(QLatin1Char('.')) || clean.endsWith(QLatin1Char(' ')))
        clean.chop(1);
    while (clean.startsWith(QLatin1Char('.')) || clean.startsWith(QLatin1Char(' ')))
        clean.remove(0, 1);
    clean = clipBytes(clean, MaxComponentBytes);
    return clean.isEmpty() ? fallback : clean;
}

QString ExportNames::relativePath(const DownloadRecord &record)
{
    QString container = component(record.container, QStringLiteral("bin"));
    if (utf8Size(container) > 16)
        container = QStringLiteral("bin");
    const QString suffix = QLatin1Char('.') + container;
    const QString fallback = record.itemId;
    if (record.type == QLatin1String("Episode")) {
        QString series = component(record.seriesName, QStringLiteral("Series"));
        QString episode = component(record.name, fallback);
        // Season 0 (Specials) is a season; an unknown one is -1
        const bool numbered = record.seasonNumber >= 0 && record.episodeNumber >= 0;
        QString code = record.itemId;
        if (numbered) {
            code = QStringLiteral("S%1E%2").arg(twoDigits(record.seasonNumber),
                                                twoDigits(record.episodeNumber));
        }
        const QString middle = QStringLiteral(" - ") + code + QStringLiteral(" - ");
        // The two names share what is left of the file name budget, half each unless one is shorter
        const int budget = MaxFileNameBytes - utf8Size(suffix) - utf8Size(middle);
        if (utf8Size(series) + utf8Size(episode) > budget) {
            const int episodeMax = qMin(utf8Size(episode), budget / 2);
            const int seriesMax = qMin(utf8Size(series), budget - episodeMax);
            series = clipBytes(series, seriesMax);
            episode = clipBytes(episode, qMin(utf8Size(episode), budget - utf8Size(series)));
        }
        const QString season =
            record.seasonNumber >= 0 ? twoDigits(record.seasonNumber) : QStringLiteral("Unknown");
        return QStringLiteral("Series/Season %1/%2%3%4")
            .arg(season, series, middle, episode + suffix);
    }
    const QString year = record.year > 0 ? QStringLiteral(" (%1)").arg(record.year) : QString();
    const QString title = clipBytes(component(record.name, fallback),
                                    MaxFileNameBytes - utf8Size(suffix) - utf8Size(year));
    return QStringLiteral("Movies/%1%2%3").arg(title, year, suffix);
}

QString ExportNames::availablePath(const QString &root, const DownloadRecord &record)
{
    const QString relative = relativePath(record);
    const QFileInfo info(relative);
    // The directory stays relative to the export root, never resolved against the working directory
    const QString directory = info.path() == QLatin1String(".") ? QString() : info.path();
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    for (int attempt = 1; attempt < 1000; ++attempt) {
        const QString name = attempt == 1 ? base : QStringLiteral("%1 (%2)").arg(base).arg(attempt);
        const QString fileName = name + QLatin1Char('.') + suffix;
        const QString relativeName =
            directory.isEmpty() ? fileName : directory + QLatin1Char('/') + fileName;
        const QString absolute = QDir(root).filePath(relativeName);
        if (!QFileInfo::exists(absolute))
            return absolute;
    }
    return QDir(root).filePath(relative);
}
} // namespace Kinefin
