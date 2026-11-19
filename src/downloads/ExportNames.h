// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "downloads/DownloadStore.h"

#include <QString>

namespace Kinefin {
// The host-side name of one download: server text is sanitised (separators, control characters, leading and trailing dots/spaces, byte length), and the layout mirrors the library: Movies/<Title> (<Year>).<ext>, Series/Season XX/<Series> - SxxEyy - <Episode>.<ext>
struct ExportNames {
    static constexpr int MaxComponentBytes = 120;
    // The whole file name, extension included; file systems stop at 255 bytes and `availablePath` may still add ` (999)`
    static constexpr int MaxFileNameBytes = 240;
    // A single path component, never empty: unusable text becomes `fallback`
    static QString component(const QString &text, const QString &fallback);
    // The file path below `root` for the record, without resolving collisions
    static QString relativePath(const DownloadRecord &record);
    // `relativePath` with ` (2)`, ` (3)`... before the extension when the file already exists below `root`
    static QString availablePath(const QString &root, const DownloadRecord &record);
};
} // namespace Kinefin
