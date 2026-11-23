// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>

namespace Kinefin {
// One option of an mpv.conf-style text: its line (from 1), the option name and its value
struct MpvConfEntry {
    int line = 0;
    QString name;
    QString value;
    // `append`, `add`, `pre`, `set`, `del`, `clr` or `toggle` for a list option written with its suffix (`glsl-shaders-append`): `name` is then the option itself and the player applies it with change-list; empty for a plain value
    QString listOperation;
};

// A line that does not apply, and why; `UnknownOption` and `BadValue` come from libmpv (MpvConfValidator), the others from the parser
struct MpvConfProblem {
    // `File` (line 0): the file is not a regular file or cannot be read; `TooLong` at line 0 is a file over MaxMpvConfBytes
    enum Kind { Syntax, Section, NotAllowed, TooLong, UnknownOption, BadValue, File };
    int line = 0;
    QString name;
    Kind kind = Syntax;
};

struct MpvConf {
    QList<MpvConfEntry> entries;
    QList<MpvConfProblem> problems;
};

constexpr qsizetype MaxMpvConfBytes = 64 * 1024;
constexpr int MaxMpvConfLines = 1000;
constexpr int MaxMpvOptionNameLength = 64;
constexpr int MaxMpvOptionValueLength = 4096;

// The lines of mpv's own format that matter here, for the `mpv.conf` of the profile and for the Manual MPV Configuration setting alike: `option=value`, `option` alone (a flag, `yes`), `no-option` (`no`), `#` comment lines and a `#` that ends a line outside quotes (as mpv has it: `sub-color=\"#FF0000\"` needs its quotes), blank lines, a value in single or double quotes, and the list suffixes (`glsl-shaders-append`). A `[profile]` section is reported and what follows it is left out, since a profile applies only when asked for. A line that breaks a rule is a problem and the others still apply
MpvConf parseMpvConf(const QString &text);

// Whether a file or a setting may set the option. The ones that name scripts, other configuration files, a control socket, an external downloader or raw ffmpeg options (`*-lavf-o`), and `http-*`, are refused, also with a list suffix (`scripts-append`); a built-in `profile` is allowed, the file has no profiles of its own; this is a guard against the obvious, not a sandbox: the file is the user's own and mpv runs the rest as written
bool mpvOptionAllowed(const QString &name);
} // namespace Kinefin
