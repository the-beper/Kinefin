// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/ProfileCommands.h"

#include "settings/ProfileIndex.h"
#include "settings/ProfilePaths.h"
#include "settings/ServerCredentialSync.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

#include <memory>

namespace Kinefin {
namespace {
const QString kProfile = QStringLiteral("profile");
const QString kList = QStringLiteral("list-profiles");
const QString kCreate = QStringLiteral("create-profile");
const QString kSetDefault = QStringLiteral("set-default-profile");
const QString kDelete = QStringLiteral("delete-profile");

int failed(QTextStream &err, const QString &message)
{
    err << "kinefin: " << message << Qt::endl;
    return 1;
}
} // namespace

QString resolveProfile(const ProfileIndexData &data, const QString &idOrName)
{
    for (const ProfileEntry &entry : data.profiles) {
        if (entry.id == idOrName)
            return entry.id;
    }
    for (const ProfileEntry &entry : data.profiles) {
        if (entry.name == idOrName)
            return entry.id;
    }
    return {};
}

void addProfileOptions(QCommandLineParser &parser)
{
    const QString argument = QStringLiteral("name");
    parser.addOption(
        {kProfile, QStringLiteral("Start with this profile (its id or name)."), argument});
    parser.addOption({kList, QStringLiteral("List the profiles and exit.")});
    parser.addOption({kCreate, QStringLiteral("Create a profile and exit."), argument});
    parser.addOption(
        {kSetDefault, QStringLiteral("Make a profile the default and exit."), argument});
    parser.addOption(
        {kDelete, QStringLiteral("Delete a profile with its data and exit."), argument});
}

std::optional<int> runProfileCommand(const QCommandLineParser &parser, TokenStore &store,
                                     QTextStream &out, QTextStream &err)
{
    const QStringList commands{kList, kCreate, kSetDefault, kDelete};
    QStringList given;
    for (const QString &command : commands) {
        if (parser.isSet(command))
            given << command;
    }
    if (given.isEmpty())
        return std::nullopt;
    if (given.size() > 1 || parser.isSet(kProfile)) {
        err << "kinefin: use one profile command at a time, and not together with --profile"
            << Qt::endl;
        return 2;
    }
    const QString command = given.first();
    if (command == kList)
        return listProfilesCommand(out, err);
    if (command == kCreate)
        return createProfileCommand(parser.value(kCreate), out, err);
    if (command == kSetDefault)
        return setDefaultProfileCommand(parser.value(kSetDefault), out, err);
    return deleteProfileCommand(parser.value(kDelete), store, out, err);
}

QString startupProfileId(const QCommandLineParser &parser, QString *error)
{
    const auto data = loadProfileIndex(error);
    if (!data)
        return {};
    if (!parser.isSet(kProfile))
        return data->defaultId;
    const QString id = resolveProfile(*data, parser.value(kProfile));
    if (id.isEmpty() && error)
        *error = QStringLiteral("No such profile: ") + parser.value(kProfile);
    return id;
}

int listProfilesCommand(QTextStream &out, QTextStream &err)
{
    QString error;
    const auto data = loadProfileIndex(&error);
    if (!data)
        return failed(err, error);
    for (const ProfileEntry &entry : data->profiles) {
        out << (entry.id == data->defaultId ? '*' : ' ') << ' ' << entry.id << '\t' << entry.name
            << '\n';
    }
    out.flush();
    return 0;
}

int createProfileCommand(const QString &name, QTextStream &out, QTextStream &err)
{
    QString error;
    if (!addProfile(name, &error))
        return failed(err, error);
    out << "Created profile " << name << Qt::endl;
    return 0;
}

int setDefaultProfileCommand(const QString &name, QTextStream &out, QTextStream &err)
{
    QString error;
    const auto data = loadProfileIndex(&error);
    if (!data)
        return failed(err, error);
    const QString id = resolveProfile(*data, name);
    if (id.isEmpty())
        return failed(err, QStringLiteral("No such profile: ") + name);
    if (!setDefaultProfile(id, &error))
        return failed(err, error);
    out << "Default profile is now " << id << Qt::endl;
    return 0;
}

int deleteProfileCommand(const QString &name, TokenStore &store, QTextStream &out, QTextStream &err)
{
    QString error;
    const auto data = loadProfileIndex(&error);
    if (!data)
        return failed(err, error);
    const QString id = resolveProfile(*data, name);
    if (id.isEmpty())
        return failed(err, QStringLiteral("No such profile: ") + name);
    if (id == data->defaultId)
        return failed(err, QStringLiteral("The default profile cannot be deleted"));
    for (const QString &directory : {profileDataDirectory(id), profileCacheDirectory(id)}) {
        if (QFileInfo(directory).isSymLink())
            return failed(
                err, QStringLiteral("A profile directory is a symbolic link; delete it by hand"));
    }

    // Held until the files are gone: nobody can start the profile (or recreate its name and start that) while it is being deleted
    const auto lock = lockProfile(id, &error);
    if (!lock)
        return failed(err, error);
    // The index entry goes first: a failure there leaves the profile whole. The wallet entries are found through servers.json, so they go before the files do
    if (!removeProfileEntry(id, &error))
        return failed(err, error);
    // The callback may arrive after the timeout, when this function is gone: it owns its state
    struct Wait {
        bool ok = false;
        QEventLoop loop;
    };
    const auto wait = std::make_shared<Wait>();
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &wait->loop, &QEventLoop::quit);
    forgetProfileTokens(id, store, [wait](bool ok) {
        wait->ok = ok;
        wait->loop.quit();
    });
    timeout.start(30000);
    wait->loop.exec();
    const bool walletOk = wait->ok;

    bool filesOk = true;
    for (const QString &directory : {profileDataDirectory(id), profileCacheDirectory(id)}) {
        if (QFileInfo::exists(directory))
            filesOk = QDir(directory).removeRecursively() && filesOk;
    }
    if (!filesOk)
        return failed(err, QStringLiteral("Some files of the profile could not be removed"));
    out << "Deleted profile " << id << Qt::endl;
    if (!walletOk) {
        // The files are gone, so the entries can no longer be found later: say so, and fail
        return failed(
            err, QStringLiteral("the session tokens of this profile could not be removed from the "
                                "wallet; remove the \"")
                     + id
                     + QStringLiteral("/…\" entries in the Kinefin folder of KWallet by hand"));
    }
    return 0;
}
} // namespace Kinefin
