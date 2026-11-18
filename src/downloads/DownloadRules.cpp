// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadRules.h"

#include "api/MediaSourceSelection.h"
#include "settings/ProfilePaths.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace Kinefin {
namespace {
const QStringList kKinds{QStringLiteral("nextN"), QStringLiteral("season"),
                         QStringLiteral("newEpisodes")};

bool validRule(const DownloadRule &rule)
{
    return isValidJellyfinId(rule.serverId) && isValidJellyfinId(rule.userId)
           && isValidJellyfinId(rule.seriesId) && kKinds.contains(rule.kind)
           && rule.seasonNumber >= -1 && rule.seasonNumber <= 10000 && rule.count >= 1
           && rule.count <= 1000;
}

QJsonObject ruleObject(const DownloadRule &rule)
{
    return {{QStringLiteral("serverId"), rule.serverId},
            {QStringLiteral("userId"), rule.userId},
            {QStringLiteral("seriesId"), rule.seriesId},
            {QStringLiteral("seriesName"), rule.seriesName},
            {QStringLiteral("kind"), rule.kind},
            {QStringLiteral("seasonNumber"), rule.seasonNumber},
            {QStringLiteral("count"), rule.count},
            {QStringLiteral("enabled"), rule.enabled},
            {QStringLiteral("cursorDate"), rule.cursorDate},
            {QStringLiteral("cursorId"), rule.cursorId}};
}

std::optional<DownloadRule> readRule(const QJsonObject &object)
{
    DownloadRule rule;
    const auto text = [&object](QLatin1String key, QString *out) {
        const QJsonValue value = object.value(key);
        if (!value.isString())
            return false;
        *out = value.toString();
        return true;
    };
    if (!text(QLatin1String("seriesName"), &rule.seriesName)) {
        // A rule written before the tracked list existed has no name: the id stands for it
        rule.seriesName.clear();
    }
    if (!text(QLatin1String("serverId"), &rule.serverId)
        || !text(QLatin1String("userId"), &rule.userId)
        || !text(QLatin1String("seriesId"), &rule.seriesId)
        || !text(QLatin1String("kind"), &rule.kind))
        return std::nullopt;
    const QJsonValue season = object.value(QLatin1String("seasonNumber"));
    const QJsonValue count = object.value(QLatin1String("count"));
    rule.seasonNumber = season.isDouble() ? season.toInt() : -1;
    rule.count = count.isDouble() ? count.toInt() : 3;
    rule.enabled = object.value(QLatin1String("enabled")).toBool(true);
    if (!text(QLatin1String("cursorDate"), &rule.cursorDate)
        || !text(QLatin1String("cursorId"), &rule.cursorId))
        return std::nullopt;
    if (!validRule(rule))
        return std::nullopt;
    return rule;
}
} // namespace

QString DownloadRule::key() const
{
    return serverId.toLower() + QLatin1Char('/') + userId.toLower() + QLatin1Char('/')
           + seriesId.toLower();
}

DownloadRules::DownloadRules(QObject *parent) : QObject(parent) {}

DownloadRules::~DownloadRules()
{
    flush();
}

void DownloadRules::setStoragePath(const QString &path)
{
    flush();
    m_path = path;
    m_rules.clear();
    m_dirty = false;
    if (!m_path.isEmpty())
        load();
}

void DownloadRules::load()
{
    QFile file(m_path);
    if (!file.exists())
        return;
    const QFileInfo info(m_path);
    const auto setAside = [this, &info] {
        m_rules.clear();
        const QString broken = m_path + QStringLiteral(".broken");
        QFile::remove(broken);
        QFile::rename(info.absoluteFilePath(), broken);
    };
    if (info.isSymLink() || !info.isFile() || info.size() > 512 * 1024) {
        setAside();
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        setAside();
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()
        || document.object().value(QStringLiteral("version")).toInt() != FileVersion) {
        setAside();
        return;
    }
    for (const QJsonValue &entry : document.object().value(QStringLiteral("rules")).toArray()) {
        if (!entry.isObject())
            continue;
        const std::optional<DownloadRule> rule = readRule(entry.toObject());
        if (!rule)
            continue;
        bool replaced = false;
        for (DownloadRule &existing : m_rules) {
            if (existing.key() == rule->key()) {
                existing = *rule;
                replaced = true;
                break;
            }
        }
        if (!replaced && m_rules.size() < MaxRules)
            m_rules.append(*rule);
    }
}

std::optional<DownloadRule> DownloadRules::find(const QString &serverId, const QString &userId,
                                                const QString &seriesId) const
{
    const QString key = serverId.toLower() + QLatin1Char('/') + userId.toLower() + QLatin1Char('/')
                        + seriesId.toLower();
    for (const DownloadRule &rule : m_rules) {
        if (rule.key() == key)
            return rule;
    }
    return std::nullopt;
}

bool DownloadRules::upsert(const DownloadRule &rule)
{
    if (!validRule(rule))
        return false;
    bool replaced = false;
    for (DownloadRule &existing : m_rules) {
        if (existing.key() == rule.key()) {
            existing = rule;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        if (m_rules.size() >= MaxRules)
            m_rules.removeFirst();
        m_rules.append(rule);
    }
    m_dirty = true;
    const bool written = write();
    Q_EMIT changed();
    return written;
}

bool DownloadRules::remove(const QString &serverId, const QString &userId, const QString &seriesId)
{
    const QString key = serverId.toLower() + QLatin1Char('/') + userId.toLower() + QLatin1Char('/')
                        + seriesId.toLower();
    for (int row = 0; row < m_rules.size(); ++row) {
        if (m_rules.at(row).key() != key)
            continue;
        m_rules.removeAt(row);
        m_dirty = true;
        const bool written = write();
        Q_EMIT changed();
        return written;
    }
    return true;
}

void DownloadRules::flush()
{
    if (m_dirty)
        write();
}

bool DownloadRules::write()
{
    if (m_path.isEmpty()) {
        m_dirty = false;
        return true;
    }
    QJsonArray rules;
    for (const DownloadRule &rule : std::as_const(m_rules))
        rules.append(ruleObject(rule));
    const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("version"), FileVersion},
                                                      {QStringLiteral("rules"), rules}})
                                .toJson(QJsonDocument::Compact);
    QString error;
    if (!writePrivateFile(m_path, data, &error)) {
        if (!m_saveFailing)
            Q_EMIT saveFailed();
        m_saveFailing = true;
        return false;
    }
    m_saveFailing = false;
    m_dirty = false;
    return true;
}
} // namespace Kinefin
