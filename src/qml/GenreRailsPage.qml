// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "genreRailsPage"

    required property ConnectionController connection

    readonly property var rails: connection.genreRails

    title: i18n("Genres")

    // The library page that opened this one: its grid shows the genre chosen with Show All
    function libraryPageBelow() {
        const stack = page.Window.window.pageStack;
        for (let i = 0; i < stack.depth; ++i) {
            if (stack.get(i)?.objectName === "libraryPage")
                return stack.get(i);
        }
        return null;
    }

    GenreRailsView {
        connection: page.connection
        ownerPage: page
        accessibleName: page.title
        onShowAllRequested: genreId => {
            const libraryPage = page.libraryPageBelow();
            if (!libraryPage)
                return;
            libraryPage.showGenre(genreId);
            page.Window.window.pageStack.pop(libraryPage);
        }
    }
}
