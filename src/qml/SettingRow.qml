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
            text: row.label
            description: row.key === "newBadge" ? i18n("Items added within the last 7 days are marked New until they are played") : ""
            checked: row.value
            onToggled: row.model.value = checked
        }
    }

    Component {
        id: choiceDelegate
        FormCard.FormComboBoxDelegate {
            text: row.label
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
            label: row.label
            text: row.value
            onEditingFinished: row.model.value = text
        }
    }

    Component {
        id: multilineDelegate
        FormCard.FormTextAreaDelegate {
            label: row.label
            text: row.value
            onEditingFinished: row.model.value = text
        }
    }
}
