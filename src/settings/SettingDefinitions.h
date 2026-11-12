// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QVariant>
#include <KLazyLocalizedString>

namespace Kinefin {
// One client setting of parity §2: the section and key are upstream's (`audio` / `passthrough.dts-hd`), the default and the closed value list are its own. The table is the only place that lists them; tests/test_settings_parity.cpp compares it with the parity document
struct SettingDefinition {
    enum class Type { Bool, Int, Double, String };
    struct Option {
        QVariant value;
        KLazyLocalizedString label;
    };

    QString section;
    QString key;
    Type type{};
    QVariant defaultValue;
    // Closed list: a stored value outside it falls back to the default. Empty: any value of the type (a double also needs min <= value <= max)
    QList<Option> options;
    double min = 0;
    double max = 0;
    // Hidden by upstream (parity ◇): stored and read, never shown or settable from the UI
    bool hidden = false;
    // A string with line breaks, e.g. `other_conf`
    bool multiline = false;
    KLazyLocalizedString label;
};

[[nodiscard]] const QList<SettingDefinition> &settingDefinitions();
[[nodiscard]] const SettingDefinition *findSetting(const QString &section, const QString &key);
// Converts `value` to the definition's type, or an invalid QVariant when it does not fit its type or its option list
[[nodiscard]] QVariant validatedSettingValue(const SettingDefinition &definition,
                                             const QVariant &value);
} // namespace Kinefin
