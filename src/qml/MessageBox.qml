// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The one message box of the app (never Kirigami.InlineMessage directly): visible by default, full width, icon vertically centred. Texts never end with a period
Kirigami.InlineMessage {
    Layout.fillWidth: true
    visible: true
    showCloseButton: false

    // Kirigami pins the icon to the top of multi-line boxes through the "multi-line" state of its icon; switching that state off leaves its "single-line" fallback (vertical centre). Relies on Kirigami 6.31 internals: the icon is the only child of the content item with exactly the states "multi-line" and "single-line" (the label has a "multi-line" state of its own that must stay). Without a match the icon simply stays where Kirigami puts it
    Component.onCompleted: {
        for (const item of contentItem?.children ?? []) {
            if (item.states?.length === 2 && item.states[0].name === "multi-line" && item.states[1].name === "single-line")
                item.states[0].when = false;
        }
    }
}
