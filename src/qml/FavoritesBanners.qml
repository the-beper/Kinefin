// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// What a scan that did not read everything tells, on the Favorites page and on its Show All pages. One box at a time: a failed library comes first, and its box says the list may be incomplete as well
ColumnLayout {
    id: banners

    required property FavoritesModel favorites
    // The object names of the boxes start with this: the pages of a stack each have their own
    property string namePrefix: "favorites"

    spacing: 0

    MessageBox {
        objectName: banners.namePrefix + "CappedBanner"
        Layout.margins: Kirigami.Units.largeSpacing
        visible: banners.favorites.capped && !banners.favorites.libraryFailed
        type: Kirigami.MessageType.Information
        text: i18np("Showing %1 favorite: the list may be incomplete", "Showing %1 favorites: the list may be incomplete", banners.favorites.count)
    }

    MessageBox {
        objectName: banners.namePrefix + "PartialBanner"
        Layout.margins: Kirigami.Units.largeSpacing
        visible: banners.favorites.libraryFailed
        type: Kirigami.MessageType.Warning
        text: i18n("Some libraries could not be read: the list may be incomplete")
        actions: [
            Kirigami.Action {
                text: i18n("Retry")
                icon.name: "view-refresh"
                onTriggered: banners.favorites.reload()
            }
        ]
    }
}
