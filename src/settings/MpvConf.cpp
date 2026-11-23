// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings/MpvConf.h"

#include <QRegularExpression>
#include <QStringList>

#include <utility>

namespace Kinefin {
namespace {
// `glsl-shaders-append` is the option `glsl-shaders` with the list operation `append`
std::pair<QString, QString> splitListSuffix(const QString &name)
{
    static const QStringList operations{QStringLiteral("append"), QStringLiteral("add"),
                                        QStringLiteral("set"),    QStringLiteral("pre"),
                                        QStringLiteral("del"),    QStringLiteral("clr"),
                                        QStringLiteral("toggle")};
    for (const QString &operation : operations) {
        const QString suffix = QLatin1Char('-') + operation;
        if (name.endsWith(suffix) && name.size() > suffix.size())
            return {name.chopped(suffix.size()), operation};
    }
    return {name, QString()};
}
} // namespace

bool mpvOptionAllowed(const QString &name)
{
    static const QStringList refused{QStringLiteral("script"),
                                     QStringLiteral("scripts"),
                                     QStringLiteral("include"),
                                     QStringLiteral("input-conf"),
                                     QStringLiteral("config"),
                                     QStringLiteral("config-dir"),
                                     QStringLiteral("input-ipc-server"),
                                     QStringLiteral("input-ipc-client"),
                                     QStringLiteral("ytdl"),
                                     QStringLiteral("ytdl-format"),
                                     QStringLiteral("ytdl-raw-options"),
                                     QStringLiteral("load-scripts")};
    const QString base = splitListSuffix(name).first;
    return !name.startsWith(QLatin1String("http-")) && !refused.contains(base)
           && !base.endsWith(QLatin1String("-lavf-o"));
}

MpvConf parseMpvConf(const QString &text)
{
    static const QRegularExpression namePattern(QStringLiteral("^[a-z][a-z0-9-]*$"));
    MpvConf result;
    bool inSection = false;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const int number = i + 1;
        if (number > MaxMpvConfLines) {
            result.problems.append({number, QString(), MpvConfProblem::TooLong});
            break;
        }
        const QString entry = lines.at(i).trimmed();
        if (entry.isEmpty() || entry.startsWith(QLatin1Char('#')))
            continue;
        if (entry.startsWith(QLatin1Char('['))) {
            if (!inSection)
                result.problems.append(
                    {number, entry.left(MaxMpvOptionNameLength), MpvConfProblem::Section});
            inSection = true;
            continue;
        }
        if (inSection)
            continue;
        QString name;
        QString value;
        const qsizetype pos = entry.indexOf(QLatin1Char('='));
        if (pos < 0) {
            // A `#` after the name starts a comment, as in mpv
            name = entry
                       .left(entry.indexOf(QLatin1Char('#')) >= 0 ? entry.indexOf(QLatin1Char('#'))
                                                                  : entry.size())
                       .trimmed();
            value = QStringLiteral("yes");
            if (name.startsWith(QLatin1String("no-"))) {
                name = name.mid(3);
                value = QStringLiteral("no");
            }
        } else {
            name = entry.left(pos).trimmed();
            value = entry.mid(pos + 1).trimmed();
            if (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))) {
                // A quoted value ends at its closing quote; only a comment may follow
                const qsizetype end = value.indexOf(value.at(0), 1);
                const QString rest = end > 0 ? value.mid(end + 1).trimmed() : QStringLiteral("x");
                if (end < 0 || (!rest.isEmpty() && !rest.startsWith(QLatin1Char('#')))) {
                    result.problems.append(
                        {number, name.left(MaxMpvOptionNameLength), MpvConfProblem::Syntax});
                    continue;
                }
                value = value.mid(1, end - 1);
            } else if (const qsizetype comment = value.indexOf(QLatin1Char('#')); comment >= 0) {
                // Unquoted, a `#` ends the value: a colour needs its quotes, `"#FF0000"`
                value = value.left(comment).trimmed();
            }
        }
        if (name.size() > MaxMpvOptionNameLength || value.size() > MaxMpvOptionValueLength) {
            result.problems.append(
                {number, name.left(MaxMpvOptionNameLength), MpvConfProblem::TooLong});
            continue;
        }
        if (!namePattern.match(name).hasMatch()) {
            result.problems.append(
                {number, name.left(MaxMpvOptionNameLength), MpvConfProblem::Syntax});
            continue;
        }
        if (!mpvOptionAllowed(name)) {
            result.problems.append({number, name, MpvConfProblem::NotAllowed});
            continue;
        }
        const auto [option, operation] = splitListSuffix(name);
        result.entries.append({number, option, value, operation});
    }
    return result;
}
} // namespace Kinefin
