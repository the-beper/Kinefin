// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard

FormCard.FormCardPage {
    id: page
    objectName: "settingsSectionPage"

    required property ClientSettings settings
    required property string section

    FormCard.FormCard {
        Layout.topMargin: Kirigami.Units.gridUnit

        Repeater {
            model: page.settings.sectionModel(page.section)
            delegate: SettingRow {}
        }
    }
}
