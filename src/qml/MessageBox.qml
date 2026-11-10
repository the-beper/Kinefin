// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The one message box of the app (never Kirigami.InlineMessage directly): visible by default, full width. Texts never end with a period
Kirigami.InlineMessage {
    Layout.fillWidth: true
    visible: true
    showCloseButton: false
}
