// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "genreRailsPage"

    // The darker View background on every page: a page without a list (a plain flickable) would get the lighter Window one, and the sections would not match
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    required property ConnectionController connection

    readonly property var rails: connection.genreRails

    title: i18n("Genres")
    // The row of the page carries the way back and the name, as on the Movies and Shows tabs
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    header: PageToolbar {
        backShown: true
        title: page.title
    }

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
            // The row slides back to the library, as Back does
            page.Window.window.popTo(libraryPage, true);
        }
    }
}
