// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings/MpvConf.h"

#include <QObject>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <memory>

class KConfig;

namespace Kinefin {
struct DeviceProfileOptions;
struct SettingDefinition;
class SettingsSectionModel;

// The client settings of one profile, stored in a KConfig file whose groups are upstream's sections. Only values that differ from the default are written, and nothing is written until the first change. Every read and write goes through `SettingDefinition`: an unknown key, a hidden key, a value of the wrong type or outside a closed list is refused on write and replaced by the default on read
class ClientSettings final : public QObject {
    Q_OBJECT
    // The stored `main.ignoreSSLErrors` or the `--ignore-certificate-errors` flag of this run: invalid TLS certificates of the server are accepted
    Q_PROPERTY(bool certificateErrorsIgnored READ certificateErrorsIgnored NOTIFY
                   certificateErrorsIgnoredChanged)
    Q_PROPERTY(bool certificateErrorsIgnoredByCommandLine READ certificateErrorsIgnoredByCommandLine
                   CONSTANT)

  public:
    static constexpr int kFormatVersion = 1;
    static constexpr int kMinimumWindowSize = 200;
    static constexpr int kMaximumWindowSize = 32000;

    // `path` is the config file. A file written by a newer format version is renamed to `<path>.broken` (replacing an older one) and the settings start from the defaults; if the rename fails nothing is ever written to that file
    explicit ClientSettings(const QString &path, QObject *parent = nullptr);
    ~ClientSettings() override;

    // `<profile data dir>/kinefin.conf`; empty for an invalid profile id
    static QString pathForProfile(const QString &profileId);

    [[nodiscard]] QString path() const { return m_path; }
    // `<profile data dir>/mpv.conf`, next to the settings file: options for mpv in its own format, applied under the managed settings
    [[nodiscard]] QString mpvConfPath() const;
    // libmpv's verdict on entries (the app installs it: this library does not link mpv); without one only the parser decides
    using MpvConfValidator = std::function<QList<MpvConfProblem>(const QList<MpvConfEntry> &)>;
    void setMpvConfValidator(MpvConfValidator validator)
    {
        m_mpvConfValidator = std::move(validator);
    }
    // Reads mpv.conf again: the entries that apply and what is wrong with the other lines, in line order. `exists` tells whether the file is there; one that is not a regular file or is larger than MaxMpvConfBytes is not read
    [[nodiscard]] MpvConf readMpvConf(bool *exists = nullptr) const;
    // For the Settings page: `path`, `exists`, `applied` (the lines that apply) and `problems`, each with its `line`, `name` and `kind` (syntax, section, notAllowed, tooLong, unknownOption or badValue)
    Q_INVOKABLE QVariantMap mpvConfReport() const;
    // The stored value, or the default when absent or invalid. Invalid QVariant for an unknown key
    Q_INVOKABLE QVariant value(const QString &section, const QString &key) const;
    // Hidden keys can be read by the program but not changed through here; they are only hand-edited
    Q_INVOKABLE bool setValue(const QString &section, const QString &key, const QVariant &value);
    Q_INVOKABLE bool isDefault(const QString &section, const QString &key) const;
    // The visible settings of one section, or only those of the group with this heading, owned by this object
    Q_INVOKABLE QObject *sectionModel(const QString &section, const QString &group = {});
    // The group headings of a section in table order; a section that is one card has one empty heading
    Q_INVOKABLE QStringList sectionGroups(const QString &section) const;

    // The window geometry of the last session (`state.geometry`): a hidden key with its own accessors because the settings page never shows it. Empty when nothing usable is stored
    Q_INVOKABLE QRect windowGeometry() const;
    Q_INVOKABLE void setWindowGeometry(const QRect &geometry);

    [[nodiscard]] DeviceProfileOptions deviceProfileOptions() const;

    [[nodiscard]] bool certificateErrorsIgnored() const;
    [[nodiscard]] bool certificateErrorsIgnoredByCommandLine() const
    {
        return m_certificateErrorsIgnoredByCommandLine;
    }
    // Set once at startup, before the UI loads; the flag is never written to the stored setting
    void setCertificateErrorsIgnoredByCommandLine(bool ignored);

  Q_SIGNALS:
    void valueChanged(const QString &section, const QString &key);
    void certificateErrorsIgnoredChanged();
    // A change is kept in memory but could not be written (read-only file or directory, newer format that could not be moved aside)
    void saveFailed();

  private:
    bool store(const SettingDefinition &definition, const QVariant &checked,
               bool reportFailure = true);
    QVariant storedValue(const QString &section, const QString &key) const;

    QString m_path;
    MpvConfValidator m_mpvConfValidator;
    std::unique_ptr<KConfig>
        m_config; // null while the file is a newer format that cannot be moved aside
    QHash<QString, QVariant> m_memory; // values of a session that cannot write
    QHash<QString, SettingsSectionModel *> m_models;
    // Keys already reported as invalid, so one bad hand edit logs once
    mutable QSet<QString> m_warned;
    bool m_certificateErrorsIgnoredByCommandLine = false;
};
} // namespace Kinefin
