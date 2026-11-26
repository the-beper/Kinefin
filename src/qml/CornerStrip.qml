// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts

// The empty strip a page with no bar of its own (PageToolbar) leaves above its content, where the navigation button of a Compact window floats; it takes no room in any other window
Item {
    Layout.fillWidth: true
    Layout.preferredHeight: PageMetrics.barHeight
    visible: PageMetrics.cornerNavigation
}
