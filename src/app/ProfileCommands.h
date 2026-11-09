// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <optional>

class QCommandLineParser;
class QTextStream;

namespace Kinefin {
class TokenStore;

// `--profile`, `--list-profiles`, `--create-profile`, `--set-default-profile` and `--delete-profile`.
void addProfileOptions(QCommandLineParser &parser);

// Runs the administrative command of the command line, if any, and returns its exit code (0 done, 1 failed, 2 a usage error such as two commands at once or a command together with `--profile`); nullopt when the command line has none. Meant to run before the single-instance service, so it works while the app is running.
std::optional<int> runProfileCommand(const QCommandLineParser &parser, TokenStore &store,
                                     QTextStream &out, QTextStream &err);

// The profile the app starts with: `--profile` (the id, else the name) or the index's default. Empty with `error` set when the index is unusable or names no such profile.
QString startupProfileId(const QCommandLineParser &parser, QString *error);

int listProfilesCommand(QTextStream &out, QTextStream &err);
int createProfileCommand(const QString &name, QTextStream &out, QTextStream &err);
int setDefaultProfileCommand(const QString &name, QTextStream &out, QTextStream &err);
// Refuses the default profile and one that is in use. Removes, in this order: the index entry, the wallet entries of the profile's servers, then its data and cache directories (downloads included). A wallet that cannot be reached is reported but does not keep the files.
int deleteProfileCommand(const QString &name, TokenStore &store, QTextStream &out,
                         QTextStream &err);
} // namespace Kinefin
