// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings/ProfilePaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

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
    // The directory is private even if a temporary save file is briefly created before its final permissions are set
    if (QFileInfo(path).permissions() != ownerOnly && !QFile::setPermissions(path, ownerOnly)) {
        if (error)
            *error = QStringLiteral("Could not secure a directory");
        return false;
    }
    return true;
}

bool writePrivateFile(const QString &path, const QByteArray &data, QString *error)
{
    if (!ensurePrivateDirectory(QFileInfo(path).absolutePath(), error))
        return false;
    QSaveFile output(path);
    // Private before the first byte: the commit is a rename of this file
    if (!output.open(QIODevice::WriteOnly)
        || !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (error)
            *error = QStringLiteral("Could not write a private file");
        return false;
    }
    if (output.write(data) != data.size() || !output.commit()) {
        if (error)
            *error = QStringLiteral("Could not finalize a private file");
        return false;
    }
    return true;
}

ProfileLock::~ProfileLock()
{
    // Closing the descriptor releases the lock
    ::close(m_descriptor);
}

std::unique_ptr<ProfileLock> lockProfile(const QString &profileId, QString *error)
{
    const QString directory = profileDataDirectory(profileId);
    if (directory.isEmpty()) {
        if (error)
            *error = QStringLiteral("Invalid profile id");
        return nullptr;
    }
    if (!ensurePrivateDirectory(directory, error))
        return nullptr;
    const QByteArray path = QFile::encodeName(directory + QStringLiteral("/kinefin.lock"));
    const int descriptor = ::open(path.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        if (error)
            *error = QStringLiteral("The profile lock cannot be taken");
        return nullptr;
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const bool busy = errno == EWOULDBLOCK;
        ::close(descriptor);
        if (error) {
            *error = busy ? QStringLiteral("The profile is in use by another Kinefin process")
                          : QStringLiteral("The profile lock cannot be taken");
        }
        return nullptr;
    }
    return std::unique_ptr<ProfileLock>(new ProfileLock(descriptor));
}

bool profileInUse(const QString &profileId)
{
    const QString directory = profileDataDirectory(profileId);
    if (directory.isEmpty() || !QFileInfo(directory).isDir())
        return false;
    QString error;
    // Taking the lock and letting it go at once: it is free unless another process holds it
    return lockProfile(profileId, &error) == nullptr
           && error.contains(QLatin1String("in use by another"));
}
} // namespace Kinefin
