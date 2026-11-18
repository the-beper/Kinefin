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
    if (clean.toUtf8().size() > MaxComponentBytes) {
        QByteArray bytes = clean.toUtf8().left(MaxComponentBytes);
        while (!bytes.isEmpty() && (bytes.back() & 0xC0) == 0x80)
            bytes.chop(1);
        clean = QString::fromUtf8(bytes).trimmed();
        while (clean.endsWith(QLatin1Char('.')) || clean.endsWith(QLatin1Char(' ')))
            clean.chop(1);
    }
    return clean.isEmpty() ? fallback : clean;
}

QString ExportNames::relativePath(const DownloadRecord &record)
{
    const QString container = component(record.container, QStringLiteral("bin"));
    const QString fallback = record.itemId;
    if (record.type == QLatin1String("Episode")) {
        const QString series = component(record.seriesName, QStringLiteral("Series"));
        const QString episode = component(record.name, fallback);
        const QString code = record.seasonNumber > 0 && record.episodeNumber > 0
                                 ? QStringLiteral("S%1E%2").arg(twoDigits(record.seasonNumber),
                                                                twoDigits(record.episodeNumber))
                                 : record.itemId;
        return QStringLiteral("Series/Season %1/%2 - %3 - %4.%5")
            .arg(record.seasonNumber > 0 ? twoDigits(record.seasonNumber)
                                         : QStringLiteral("Unknown"),
                 series, code, episode, container);
    }
    QString title = component(record.name, fallback);
    if (record.year > 0)
        title += QStringLiteral(" (%1)").arg(record.year);
    return QStringLiteral("Movies/%1.%2").arg(title, container);
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
