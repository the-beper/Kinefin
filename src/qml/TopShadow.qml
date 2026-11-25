// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

// Under a toolbar: a thin shadow on the top edge of the content once it has scrolled, so it looks as if it passes under the bar
Rectangle {
    // Nothing is under the edge while the content is at its start
    required property bool atStart

    anchors {
        left: parent.left
        right: parent.right
        top: parent.top
    }
    height: Kirigami.Units.smallSpacing * 2
    opacity: atStart ? 0 : 1
    gradient: Gradient {
        GradientStop {
            position: 0
            color: Qt.rgba(0, 0, 0, 0.3)
        }
        GradientStop {
            position: 1
            color: Qt.rgba(0, 0, 0, 0)
        }
    }

    Behavior on opacity {
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
        }
    }
}
