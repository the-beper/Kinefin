// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import org.kde.kirigamiaddons.formcard as FormCard

// One row of a SettingsSectionModel, shown with the form delegate of its kind. Changes go back through the model (`model.value`), which refuses what the setting definition does not allow
Loader {
    id: row

    required property var model
    required property string key
    required property string label
    required property string kind
    required property var options
    required property var value
    required property real minimum
    required property real maximum
    required property bool disabled
    // The heading of the group the row sits under: a short label ("Color" under Border) is read with it
    property string groupTitle: ""
    // --ignore-certificate-errors forces ignoreSSLErrors on for the run: its switch shows on and cannot be turned off
    property bool certificateErrorsForced: false
    readonly property bool forced: key === "ignoreSSLErrors" && certificateErrorsForced
    readonly property string accessibleLabel: groupTitle.length > 0 ? i18nc("@info:accessible a settings group heading and a setting under it", "%1: %2", groupTitle, label) : label

    Layout.fillWidth: true
    sourceComponent: {
        switch (kind) {
        case "switch":
            return switchDelegate;
        case "choice":
            return choiceDelegate;
        case "number":
            return numberDelegate;
        case "multiline":
            return multilineDelegate;
        default:
            return textDelegate;
        }
    }

    Component {
        id: switchDelegate
        FormCard.FormSwitchDelegate {
            Accessible.name: row.accessibleLabel
            text: row.label
            description: {
                switch (row.key) {
                case "newBadge":
                    return i18n("Items added within the last 7 days are marked New until they are played");
                case "ignoreSSLErrors":
                    return row.forced ? i18n("On for this run: Kinefin was started with --ignore-certificate-errors")
                                      : i18n("Accepts invalid server certificates. Unsafe on networks you do not trust");
                case "kodiSidecars":
                    return i18n("Writes a .nfo and a poster next to each exported file");
                case "night_mode":
                    return i18n("Quiet dialogue louder and loud scenes quieter. Audio passthrough is off while it is on");
                case "exportOnCompletion":
                    return i18n("Disabled until a persistent folder is granted on this system");
                }
                return "";
            }
            checked: row.forced || row.value
            enabled: !row.forced && !row.disabled
            onToggled: row.model.value = checked
        }
    }

    Component {
        id: choiceDelegate
        FormCard.FormComboBoxDelegate {
            enabled: !row.disabled
            Accessible.name: row.accessibleLabel
            text: row.label
            description: row.key === "uiScale" ? i18n("Takes effect the next time Kinefin starts") : ""
            model: row.options
            textRole: "text"
            valueRole: "value"
            currentIndex: row.options.findIndex(option => option.value === row.value)
            onActivated: row.model.value = currentValue
        }
    }

    Component {
        id: numberDelegate
        FormCard.FormSpinBoxDelegate {
            enabled: !row.disabled
            Accessible.name: row.accessibleLabel
            // Only a decimal setting (the playback speed) has a range; it is edited in hundredths because a spin box counts integers
            readonly property bool fractional: row.maximum > row.minimum
            readonly property int divisor: fractional ? 100 : 1

            DoubleValidator {
                id: decimalValidator
                bottom: row.minimum
                top: row.maximum
                decimals: 2
                notation: DoubleValidator.StandardNotation
            }
            IntValidator {
                id: wholeValidator
                bottom: -2147483647
                top: 2147483647
            }

            label: row.label
            from: fractional ? Math.round(row.minimum * divisor) : -2147483647
            to: fractional ? Math.round(row.maximum * divisor) : 2147483647
            stepSize: fractional ? 5 : 1
            value: Math.round(row.value * divisor)
            // The default validator takes the range as whole numbers, which would refuse "1.5"
            validator: fractional ? decimalValidator : wholeValidator
            textFromValue: (number, locale) => Number(number / divisor).toLocaleString(locale, "f", fractional ? 2 : 0)
            // Number.fromLocaleString throws on text that is not a number: keep the current value then
            valueFromText: (text, locale) => {
                try {
                    return Math.round(Number.fromLocaleString(locale, text) * divisor);
                } catch (error) {
                    return value;
                }
            }
            onValueModified: row.model.value = value / divisor
        }
    }

    Component {
        id: textDelegate
        FormCard.FormTextFieldDelegate {
            enabled: !row.disabled
            Accessible.name: row.accessibleLabel
            label: row.label
            text: row.value
            onEditingFinished: row.model.value = text
        }
    }

    Component {
        id: multilineDelegate
        FormCard.FormTextAreaDelegate {
            enabled: !row.disabled
            Accessible.name: row.accessibleLabel
            label: row.label
            text: row.value
            onEditingFinished: row.model.value = text
        }
    }
}
