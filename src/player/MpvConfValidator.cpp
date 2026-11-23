// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/MpvConfValidator.h"

#include <QStringList>

#include <mpv/client.h>

#include <clocale>

namespace Kinefin {
QList<MpvConfProblem> validateMpvConf(const QList<MpvConfEntry> &entries)
{
    QList<MpvConfProblem> problems;
    if (entries.isEmpty())
        return problems;
    // mpv_create() refuses a numeric locale other than C: the player's own start makes it so for the process too
    std::setlocale(LC_NUMERIC, "C");
    mpv_handle *handle = mpv_create();
    if (!handle)
        return problems;
    mpv_set_option_string(handle, "config", "no");
    mpv_set_option_string(handle, "terminal", "no");
    mpv_set_option_string(handle, "msg-level", "all=no");
    // Options whose value mpv acts on as soon as it is set (a file it opens) are not tried: the parser's verdict stands
    static const QStringList notTried{QStringLiteral("log-file"), QStringLiteral("dump-stats"),
                                      QStringLiteral("stream-record")};
    for (const MpvConfEntry &entry : entries) {
        if (notTried.contains(entry.name))
            continue;
        const int status = mpv_set_option_string(handle, entry.name.toUtf8().constData(),
                                                 entry.value.toUtf8().constData());
        if (status >= 0)
            continue;
        problems.append({entry.line, entry.name,
                         status == MPV_ERROR_OPTION_NOT_FOUND ? MpvConfProblem::UnknownOption
                                                              : MpvConfProblem::BadValue});
    }
    mpv_destroy(handle);
    return problems;
}
} // namespace Kinefin
