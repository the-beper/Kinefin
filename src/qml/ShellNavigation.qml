// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

// The sections of the shell (Home, Movies, Shows, Favorites, Playlists and Downloads) as actions, with their Ctrl+N shortcuts and the keys that cycle through them; a view that shows the sections reads them from here and binds no shortcut of its own, since two shortcuts on one sequence fire neither
Item {
    id: root

    // The shell window: its functions open the pages, its connection holds the session
    required property var shell

    readonly property ConnectionController connection: shell.connection
    readonly property bool signedIn: connection.state === ConnectionController.Authenticated
    readonly property bool offline: connection.state === ConnectionController.Offline

    // The sections in their visual order, which is also the order of the Ctrl+N shortcuts and of the slide of the tab switch: `group` is "home" or "content" (Movies and Shows, which need a library of the server) for the first block and "personal" for what follows the divider
    readonly property var entries: [
        { section: homeAction.section, group: "home", action: homeAction },
        { section: moviesAction.section, group: "content", action: moviesAction },
        { section: tvShowsAction.section, group: "content", action: tvShowsAction },
        { section: favoritesTabAction.section, group: "personal", action: favoritesTabAction },
        { section: playlistsTabAction.section, group: "personal", action: playlistsTabAction },
        { section: downloadsAction.section, group: "personal", action: downloadsAction }
    ]
    // All the actions of `entries`, shown or not; the Ctrl+N shortcuts follow the position among the visible ones
    readonly property var actions: entries.map(entry => entry.action)
    // More than one section to choose from: a lone one (Downloads while offline) is no navigation
    readonly property bool navigable: actions.filter(action => action.visible).length > 1
    // The first personal section shown, where the divider and the caption of the group stand; empty when none shows
    readonly property string firstPersonalSection: entries.find(entry => entry.group === "personal" && entry.action.visible)?.section ?? ""
    // The divider stands only between two blocks that both show a section
    readonly property bool groupsDivided: firstPersonalSection !== "" && entries.some(entry => entry.group !== "personal" && entry.action.visible)

    function tabShortcut(action) {
        let position = 0;
        for (const candidate of actions) {
            if (candidate.visible)
                ++position;
            if (candidate === action)
                return candidate.visible ? "Ctrl+" + position : "";
        }
        return "";
    }

    // Move between the root tabs in their visible order, wrapping (a library type may be absent). Detail pages belong to their root tab; using the shortcut returns to the selected tab, just like clicking its sidebar entry
    function switchRelativeTab(step) {
        if (!signedIn)
            return;
        const tabs = actions.filter(action => action.visible);
        if (tabs.length < 2)
            return;
        const current = Math.max(0, tabs.findIndex(action => action.section === shell.rootTab));
        tabs[(current + step + tabs.length) % tabs.length].trigger();
    }

    Kirigami.Action {
        id: homeAction
        objectName: "homeTabAction"

        property string section: "home"
        text: i18nc("@title:tab", "Home")
        icon.name: "go-home"
        visible: root.signedIn
        enabled: root.signedIn
        shortcut: root.tabShortcut(homeAction)
        onTriggered: shell.goHome()
    }

    // Ctrl+2 and onwards follow the position of the visible tabs: a missing Movies tab moves Shows to Ctrl+2
    Kirigami.Action {
        id: moviesAction
        objectName: "moviesTabAction"

        property string section: "movies"
        text: i18nc("@title:tab", "Movies")
        icon.name: "video-symbolic"
        visible: root.connection.movieLibraries.length > 0
        enabled: root.signedIn
        shortcut: root.tabShortcut(moviesAction)
        onTriggered: shell.openTab("movies")
    }
    Kirigami.Action {
        id: tvShowsAction
        objectName: "tvShowsTabAction"

        property string section: "tvshows"
        text: i18nc("@title:tab", "Shows")
        icon.name: "video-television"
        visible: root.connection.showLibraries.length > 0
        enabled: root.signedIn
        shortcut: root.tabShortcut(tvShowsAction)
        onTriggered: shell.openTab("tvshows")
    }
    Kirigami.Action {
        id: favoritesTabAction
        objectName: "favoritesTabAction"

        property string section: "favorites"
        text: i18nc("@title:tab", "Favorites")
        icon.name: "favorite"
        visible: root.signedIn
        enabled: root.signedIn
        shortcut: root.tabShortcut(favoritesTabAction)
        onTriggered: shell.openFavorites()
    }
    Kirigami.Action {
        id: playlistsTabAction
        objectName: "playlistsTabAction"

        property string section: "playlists"
        text: i18nc("@title:tab", "Playlists")
        icon.name: "view-media-playlist"
        visible: root.signedIn
        enabled: root.signedIn
        shortcut: root.tabShortcut(playlistsTabAction)
        onTriggered: shell.openPlaylists()
    }
    Kirigami.Action {
        id: downloadsAction
        objectName: "downloadsTabAction"

        property string section: "downloads"
        text: i18nc("@title:tab", "Downloads")
        icon.name: "folder-download-symbolic"
        visible: root.signedIn || root.offline
        enabled: root.signedIn || root.offline
        shortcut: root.tabShortcut(downloadsAction)
        onTriggered: shell.openDownloads()
    }

    // PageUp/PageDown are the KDE tab navigation conventions. They work even when a detail page is on top; absent Movies/TV types are skipped
    Shortcut {
        objectName: "previousTabShortcut"
        sequences: ["Ctrl+PgUp", "Ctrl+[", "Ctrl+Shift+Tab"]
        context: Qt.WindowShortcut
        enabled: root.signedIn
        onActivated: root.switchRelativeTab(-1)
    }
    Shortcut {
        objectName: "nextTabShortcut"
        sequences: ["Ctrl+PgDown", "Ctrl+]", "Ctrl+Tab"]
        context: Qt.WindowShortcut
        enabled: root.signedIn
        onActivated: root.switchRelativeTab(1)
    }
}
