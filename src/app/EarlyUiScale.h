// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStringList>

namespace Kinefin {
// The interface scale in percent that the profile of this run stores (`main.uiScale`), for the one place that cannot wait for QApplication: Qt reads QT_SCALE_FACTOR only while it is constructed. `arguments` are the command-line arguments after the program name; `--config-dir` and `--profile` pick the settings file the way the rest of the start does. Read-only, and needs no QCoreApplication. A missing or unreadable file or key, a value outside the setting's choices or a file of a newer format gives the default
int startupUiScalePercent(const QStringList &arguments);
} // namespace Kinefin
