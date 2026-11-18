// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import org.kde.kirigami as Kirigami

// The one message box of the app (never Kirigami.InlineMessage directly): visible by default, full width, icon vertically centred. Texts never end with a period. The Kirigami box (with an action bar of its own) is built the first time this one shows and kept: a page holds several hidden boxes, and each would cost its creation on every push
Item {
    id: box

    property string text
    property int type: Kirigami.MessageType.Information
    property list<T.Action> actions

    // Whether the box showed when it was created: Kirigami fades the content in only for a box that shows later
    property bool shownAtCreation: false

    Layout.fillWidth: true
    implicitWidth: loader.item ? loader.item.implicitWidth : 0
    implicitHeight: loader.item ? loader.item.implicitHeight : 0

    Component.onCompleted: {
        shownAtCreation = visible;
        loader.active = visible;
    }
    onVisibleChanged: {
        if (visible)
            loader.active = true;
    }

    Loader {
        id: loader

        width: box.width
        active: false
        sourceComponent: Kirigami.InlineMessage {
            visible: box.shownAtCreation
            showCloseButton: false
            type: box.type
            text: box.text
            actions: box.actions

            // Kirigami pins the icon to the top of multi-line boxes through the "multi-line" state of its icon; switching that state off leaves its "single-line" fallback (vertical centre). Relies on Kirigami 6.31 internals: the icon is the only child of the content item with exactly the states "multi-line" and "single-line" (the label has a "multi-line" state of its own that must stay). Without a match the icon simply stays where Kirigami puts it
            Component.onCompleted: {
                for (const item of contentItem?.children ?? []) {
                    // Server text is plain text: the Kirigami label would otherwise read it as HTML
                    if (item.text !== undefined && item.textFormat !== undefined)
                        item.textFormat = Text.PlainText;
                    if (item.states?.length === 2 && item.states[0].name === "multi-line" && item.states[1].name === "single-line")
                        item.states[0].when = false;
                }
            }
        }
        // From now on the Kirigami box follows this one: built on a later show, it turns visible only here, after its creation, so it fades in
        onLoaded: item.visible = Qt.binding(() => box.visible)
    }
}
