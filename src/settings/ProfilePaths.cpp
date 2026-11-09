// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings/ProfilePaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QStandardPaths>

namespace Kinefin {
bool validProfileId(const QString &profileId)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[A-Za-z0-9_][A-Za-z0-9._-]{0,63}\\z"));
    return pattern.match(profileId).hasMatch();
}

bool validProfileName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_]{1,64}\\z"));
    return pattern.match(name).hasMatch();
}

QString profileDataDirectory(const QString &profileId)
{
    if (!validProfileId(profileId))
        return {};
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/kinefin/profiles/") + profileId;
}

QString profileCacheDirectory(const QString &profileId)
{
    if (!validProfileId(profileId))
        return {};
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
           + QStringLiteral("/kinefin/profiles/") + profileId;
}

QString profileLogDirectory(const QString &profileId)
{
    const QString directory = profileDataDirectory(profileId);
    return directory.isEmpty() ? QString() : directory + QStringLiteral("/logs");
}

bool ensurePrivateDirectory(const QString &path, QString *error)
{
    constexpr auto ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QDir().mkpath(path)) {
        if (error)
            *error = QStringLiteral("Could not create a directory");
        return false;
    }
    // The directory is private even if a temporary save file is briefly created before its final permissions are set.
    if (QFileInfo(path).permissions() != ownerOnly && !QFile::setPermissions(path, ownerOnly)) {
        if (error)
            *error = QStringLiteral("Could not secure a directory");
        return false;
    }
    return true;
}

std::unique_ptr<QLockFile> lockProfile(const QString &profileId, QString *error)
{
    const QString directory = profileDataDirectory(profileId);
    if (directory.isEmpty()) {
        if (error)
            *error = QStringLiteral("Invalid profile id");
        return nullptr;
    }
    if (!ensurePrivateDirectory(directory, error))
        return nullptr;
    auto lock = std::make_unique<QLockFile>(directory + QStringLiteral("/kinefin.lock"));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        if (error) {
            *error = lock->error() == QLockFile::LockFailedError
                         ? QStringLiteral("The profile is in use by another Kinefin process")
                         : QStringLiteral("The profile lock cannot be taken");
        }
        return nullptr;
    }
    return lock;
}

bool profileInUse(const QString &profileId)
{
    const QString directory = profileDataDirectory(profileId);
    if (directory.isEmpty() || !QFileInfo(directory).isDir())
        return false;
    QLockFile lock(directory + QStringLiteral("/kinefin.lock"));
    lock.setStaleLockTime(0);
    return !lock.tryLock(0);
}
} // namespace Kinefin
