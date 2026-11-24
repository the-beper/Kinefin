// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The row a page puts under the window header, in the style of the Movies and Shows tabs (it replaces Kirigami's title row, so the page sets `globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None`): at its start the way back of a page that sits above another and the page's name, then the page's actions, labelled in the Wide header and icons with tooltips below it, what does not fit in the overflow menu. With nothing to show it takes no room
Item {
    id: bar

    // The page's actions (Kirigami.Action): the page keeps them in its own `actions`, the row shows them
    property alias actions: toolbar.actions
    // A page above another: the button takes the shell's back, as Esc does
    property bool backShown: false
    // The page's name; server text for a playlist or a library, so plain text only. Empty for none
    property string title: ""
    // A fade from the row's background over the top of the content under it, for content that starts right at the row (the page sets the row's `z` above its content). The details page uses it instead of a TopFade inside its list, which raced the seasons row's asynchronous cards
    property bool fadeBelow: false
    // Names for the tests, as the pages had them before the row was shared
    property alias toolbarObjectName: toolbar.objectName
    property alias titleObjectName: titleLabel.objectName

    readonly property bool wide: Window.window ? Window.window.wideHeader : true
    // Its own content decides, never an ancestor's visibility: a page sliding out must keep its height
    readonly property bool shown: backShown || title.length > 0 || toolbar.actions.length > 0

    visible: shown
    implicitHeight: shown ? row.implicitHeight : 0
    implicitWidth: row.implicitWidth + Kirigami.Units.largeSpacing * 2

    Rectangle {
        objectName: "pageRowFade"
        visible: bar.fadeBelow
        anchors {
            left: parent.left
            right: parent.right
            top: parent.bottom
        }
        // A smooth quadratic falloff, (1 - t)², from the row's colour at the edge (no seam) to nothing: no band where it turns, and mostly see-through in its lower half
        height: Kirigami.Units.gridUnit * 2
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 1.00)
            }
            GradientStop {
                position: 0.1
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.81)
            }
            GradientStop {
                position: 0.2
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.64)
            }
            GradientStop {
                position: 0.3
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.49)
            }
            GradientStop {
                position: 0.4
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.36)
            }
            GradientStop {
                position: 0.5
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.25)
            }
            GradientStop {
                position: 0.6
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.16)
            }
            GradientStop {
                position: 0.7
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.09)
            }
            GradientStop {
                position: 0.8
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.04)
            }
            GradientStop {
                position: 0.9
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.01)
            }
            GradientStop {
                position: 1.0
                color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.00)
            }
        }
    }

    RowLayout {
        id: row

        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
            leftMargin: Kirigami.Units.largeSpacing
            rightMargin: Kirigami.Units.largeSpacing
        }
        spacing: Kirigami.Units.mediumSpacing

        PageBackButton {
            visible: bar.backShown
        }

        PageTitle {
            id: titleLabel
            text: bar.title
        }

        Kirigami.ActionToolBar {
            id: toolbar

            Layout.fillWidth: true
            display: bar.wide ? Controls.AbstractButton.TextBesideIcon : Controls.AbstractButton.IconOnly
        }
    }
}
