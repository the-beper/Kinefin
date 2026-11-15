// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

// Under a toolbar: the content fades into the background as it scrolls up beneath it, instead of looking cut by the edge
Rectangle {
    // Nothing is under the edge while the content is at its start
    required property bool atStart

    anchors {
        left: parent.left
        right: parent.right
        top: parent.top
    }
    height: Kirigami.Units.gridUnit
    opacity: atStart ? 0 : 1
    gradient: Gradient {
        GradientStop {
            position: 0
            color: Kirigami.Theme.backgroundColor
        }
        GradientStop {
            position: 1
            color: Qt.alpha(Kirigami.Theme.backgroundColor, 0)
        }
    }

    Behavior on opacity {
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
        }
    }
}
