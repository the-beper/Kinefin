// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProfileIndex.h"

#include "settings/ProfilePaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

Q_LOGGING_CATEGORY(profileIndexLog, "kinefin.profiles")

namespace Kinefin {
namespace {
bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

std::optional<ProfileIndexData> parseIndex(const QByteArray &bytes, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(error, QStringLiteral("The profile index is not valid JSON"));
        return std::nullopt;
    }
    ProfileIndexData data;
    data.defaultId = document.object().value(QStringLiteral("defaultProfile")).toString();
    QSet<QString> seen;
    for (const QJsonValue &value : document.object().value(QStringLiteral("profiles")).toArray()) {
        const QJsonObject object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (!validProfileId(id) || seen.contains(id)) {
            fail(error, QStringLiteral("The profile index lists an invalid or duplicate profile"));
            return std::nullopt;
        }
        seen.insert(id);
        data.profiles.append({id, object.value(QStringLiteral("name")).toString(id)});
    }
    if (!seen.contains(data.defaultId)) {
        fail(error, QStringLiteral("The profile index names no listed default profile"));
        return std::nullopt;
    }
    return data;
}

bool writeIndex(const ProfileIndexData &data, QString *error)
{
    const QString path = profileIndexPath();
    if (!ensurePrivateDirectory(QFileInfo(path).absolutePath(), error))
        return false;
    QJsonArray profiles;
    for (const ProfileEntry &entry : data.profiles) {
        profiles.append(
            QJsonObject{{QStringLiteral("id"), entry.id}, {QStringLiteral("name"), entry.name}});
    }
    const QJsonObject index{{QStringLiteral("defaultProfile"), data.defaultId},
                            {QStringLiteral("profiles"), profiles}};
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)
        || !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return fail(error, QStringLiteral("Could not write profile index"));
    const QByteArray bytes = QJsonDocument(index).toJson(QJsonDocument::Indented);
    if (output.write(bytes) != bytes.size() || !output.commit())
        return fail(error, QStringLiteral("Could not finalize profile index"));
    return true;
}

// One lock for every read-modify-write of the file
class IndexLock {
  public:
    explicit IndexLock(QString *error) : m_lock(profileIndexPath() + QStringLiteral(".lock"))
    {
        QString directoryError;
        if (!ensurePrivateDirectory(QFileInfo(profileIndexPath()).absolutePath(), &directoryError))
            fail(error, directoryError);
        else if (!m_lock.tryLock(5000))
            fail(error, QStringLiteral("The profile index is locked by another process"));
        else
            m_ok = true;
    }
    explicit operator bool() const { return m_ok; }

  private:
    QLockFile m_lock;
    bool m_ok = false;
};

std::optional<ProfileIndexData> readLocked(QString *error)
{
    QFile file(profileIndexPath());
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("The profile index is missing or unreadable"));
        return std::nullopt;
    }
    return parseIndex(file.readAll(), error);
}
} // namespace

QString profileIndexPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/kinefin/profiles.json");
}

bool ensureDefaultProfileIndex(QString *error)
{
    IndexLock lock(error);
    if (!lock)
        return false;
    const QString path = profileIndexPath();
    if (QFileInfo::exists(path)) {
        QString readError;
        if (readLocked(&readError))
            return true;
        // An index that cannot even be opened is not an invalid one: it stays where it is
        if (!QFile(path).open(QIODevice::ReadOnly))
            return fail(error, readError);
        const QString broken = path + QStringLiteral(".broken");
        QFile::remove(broken);
        if (!QFile::rename(path, broken))
            return fail(error, QStringLiteral("Could not move the invalid profile index aside"));
        qCWarning(profileIndexLog) << "the profile index was invalid; moved aside to" << broken;
    }
    return writeIndex(
        {QStringLiteral("default"), {{QStringLiteral("default"), QStringLiteral("Default")}}},
        error);
}

std::optional<ProfileIndexData> loadProfileIndex(QString *error)
{
    return readLocked(error);
}

bool addProfile(const QString &name, QString *error)
{
    if (!validProfileName(name))
        return fail(error,
                    QStringLiteral("A profile name has 1-64 letters, digits or underscores"));
    IndexLock lock(error);
    if (!lock)
        return false;
    auto data = readLocked(error);
    if (!data)
        return false;
    for (const ProfileEntry &entry : std::as_const(data->profiles)) {
        if (entry.id.compare(name, Qt::CaseInsensitive) == 0
            || entry.name.compare(name, Qt::CaseInsensitive) == 0)
            return fail(error, QStringLiteral("A profile with this name already exists"));
    }
    data->profiles.append({name, name});
    return writeIndex(*data, error);
}

bool setDefaultProfile(const QString &id, QString *error)
{
    IndexLock lock(error);
    if (!lock)
        return false;
    auto data = readLocked(error);
    if (!data)
        return false;
    const bool listed = std::any_of(data->profiles.cbegin(), data->profiles.cend(),
                                    [&](const ProfileEntry &entry) { return entry.id == id; });
    if (!listed)
        return fail(error, QStringLiteral("No such profile"));
    data->defaultId = id;
    return writeIndex(*data, error);
}

bool removeProfileEntry(const QString &id, QString *error)
{
    IndexLock lock(error);
    if (!lock)
        return false;
    auto data = readLocked(error);
    if (!data)
        return false;
    if (id == data->defaultId)
        return fail(error, QStringLiteral("The default profile cannot be deleted"));
    const qsizetype removed =
        data->profiles.removeIf([&](const ProfileEntry &entry) { return entry.id == id; });
    if (removed == 0)
        return fail(error, QStringLiteral("No such profile"));
    return writeIndex(*data, error);
}
} // namespace Kinefin
