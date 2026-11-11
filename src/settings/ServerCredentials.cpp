// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServerCredentials.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "api/MediaSourceSelection.h"

namespace Kinefin {
namespace {
QFileDevice::Permissions ownerOnlyFile()
{
    return QFileDevice::ReadOwner | QFileDevice::WriteOwner;
}
} // namespace

QString serverCredentialsPath(const QString &profileId)
{
    const QString directory = profileDataDirectory(profileId);
    return directory.isEmpty() ? QString() : directory + QStringLiteral("/servers.json");
}

bool hasValidIds(const ServerCredential &credential)
{
    return isValidJellyfinId(credential.serverId) && isValidJellyfinId(credential.userId);
}

QList<ServerCredential> loadServerCredentials(const QString &profileId, QString *error,
                                              bool *legacyToken)
{
    QList<ServerCredential> result;
    const QString path = serverCredentialsPath(profileId);
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("Invalid profile id");
        return result;
    }
    QFile file(path);
    if (!QFileInfo::exists(path)) {
        return result;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Could not open server credentials");
        return result;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        if (error)
            *error = QStringLiteral("Malformed server credentials document");
        return result;
    }
    const QJsonValue serversValue = document.object().value(QStringLiteral("servers"));
    if (!serversValue.isArray()) {
        if (error)
            *error = QStringLiteral("Malformed server credentials document");
        return result;
    }
    for (const auto &value : serversValue.toArray()) {
        const QJsonObject object = value.toObject();
        ServerCredential entry;
        entry.serverId = object.value(QStringLiteral("serverId")).toString();
        entry.url = object.value(QStringLiteral("url")).toString();
        entry.userId = object.value(QStringLiteral("userId")).toString();
        if (object.contains(QStringLiteral("token")) && legacyToken)
            *legacyToken = true;
        entry.deviceId = object.value(QStringLiteral("deviceId")).toString();
        entry.deviceName = object.value(QStringLiteral("deviceName")).toString();
        entry.username = object.value(QStringLiteral("username")).toString();
        entry.imageTag = object.value(QStringLiteral("imageTag")).toString();
        // The tag ends up in an image URL: anything that is not an id is dropped
        if (!entry.imageTag.isEmpty() && !isValidJellyfinId(entry.imageTag))
            entry.imageTag.clear();
        entry.askPassword = object.value(QStringLiteral("askPassword")).toBool(false);
        entry.remembered = object.value(QStringLiteral("remembered")).toBool(true);
        entry.ownsLegacyKey = !object.contains(QStringLiteral("remembered"));
        if (entry.serverId.isEmpty() || entry.url.isEmpty()) {
            if (error)
                *error = QStringLiteral("Server credential entry is incomplete");
            return result;
        }
        result.append(entry);
    }
    return result;
}

bool saveServerCredentials(const QString &profileId, const QList<ServerCredential> &credentials,
                           QString *error)
{
    const QString path = serverCredentialsPath(profileId);
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("Invalid profile id");
        return false;
    }
    const QString folder = QFileInfo(path).absolutePath();
    if (!ensurePrivateDirectory(folder, error))
        return false;
    QJsonArray array;
    for (const ServerCredential &entry : credentials) {
        QJsonObject object{{QStringLiteral("serverId"), entry.serverId},
                           {QStringLiteral("url"), entry.url},
                           {QStringLiteral("userId"), entry.userId},
                           {QStringLiteral("deviceId"), entry.deviceId},
                           {QStringLiteral("deviceName"), entry.deviceName},
                           {QStringLiteral("username"), entry.username},
                           {QStringLiteral("imageTag"), entry.imageTag},
                           {QStringLiteral("askPassword"), entry.askPassword}};
        if (!entry.ownsLegacyKey)
            object.insert(QStringLiteral("remembered"), entry.remembered);
        array.append(object);
    }
    const QJsonObject root{{QStringLiteral("servers"), array}};
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Could not write server credentials");
        return false;
    }
    // Private before the first byte: the commit is a rename of this file
    if (!output.setPermissions(ownerOnlyFile())) {
        if (error)
            *error = QStringLiteral("Could not secure server credentials");
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (output.write(data) != data.size() || !output.commit()) {
        if (error)
            *error = QStringLiteral("Could not finalize server credentials");
        return false;
    }
    return true;
}
} // namespace Kinefin
