// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringView>

#include <optional>

namespace Kinefin {
// Ordered by severity; `Disable` is above every message level so a file level of `Disable` writes nothing.
enum class LogLevel { Debug, Info, Warn, Error, Fatal, Disable };

// Exactly "debug", "info", "warn", "error", "fatal" or "disable" (lowercase); anything else gives nullopt.
std::optional<LogLevel> parseLogLevel(QStringView name);

// Masks the value of `Token="..."`, `api_key=` and `ApiKey=` (key names case-insensitive) with `***`.
QString redactSecrets(const QString &line);

// Rotates `kinefin.log` (ten files kept) in `directory` and mirrors the Qt log lines at or above `fileLevel` into a fresh one; every message is still forwarded unfiltered to the previous handler. On failure returns false with the reason in `error`, and nothing is installed.
bool installProfileLog(const QString &directory, LogLevel fileLevel, QString *error = nullptr);

// Restores the previous message handler and closes the log file; does nothing if none is installed.
void removeProfileLog();
} // namespace Kinefin
