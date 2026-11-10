// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProfileIdentity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include "api/JellyfinClient.h"
#include "settings/ProfileIndex.h"
#include "settings/ServerCredentials.h"

namespace Kinefin {
namespace {
const QString deviceIdKey = QStringLiteral("deviceId");

QString fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return {};
}
} // namespace

QString defaultProfileId(QString *error)
{
    QFile file(profileIndexPath());
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Profile index is missing or unreadable"));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QString id = document.object().value(QStringLiteral("defaultProfile")).toString();
    if (!document.isObject() || !validProfileId(id))
        return fail(error, QStringLiteral("Profile index names no valid default profile"));
    return id;
}

QString profileDeviceId(const QString &profileId, QString *error)
{
    const QString directory = profileDataDirectory(profileId);
    if (directory.isEmpty())
        return fail(error, QStringLiteral("Invalid profile id"));
    const QString path = directory + QStringLiteral("/profile.json");
    if (QFileInfo::exists(path)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return fail(error, QStringLiteral("Could not open the profile file"));
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        const QString id = document.object().value(deviceIdKey).toString();
        // The API client's own rule for header fields: a bad id must not reach it
        if (!document.isObject() || !isValidHeaderField(id))
            return fail(error, QStringLiteral("Malformed profile file"));
        return id;
    }
    if (!ensurePrivateDirectory(directory))
        return fail(error, QStringLiteral("Could not create the profile directory"));
    const QString id =
        QStringLiteral("kinefin-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)
        || !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return fail(error, QStringLiteral("Could not write the profile file"));
    const QByteArray data = QJsonDocument(QJsonObject{{deviceIdKey, id}}).toJson();
    if (output.write(data) != data.size() || !output.commit())
        return fail(error, QStringLiteral("Could not finalize the profile file"));
    return id;
}
} // namespace Kinefin
