// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.coreaddons as Core

// Compact information overlay, following the Keyboard Shortcuts dialog pattern
Kirigami.Dialog {
    id: dialog
    objectName: "aboutDialogContent"

    // The owner of the update checker sets them and runs the check
    property bool updateChecksEnabled: false
    property bool checking: false
    // A development build's time, shown in place of the version when set
    property string displayVersion: ""
    // What the last manual check found: "", "checking", "upToDate", "failed" or "available" (with updateVersion)
    property string updateStatus: ""
    property string updateVersion: ""
    signal checkForUpdatesRequested()
    signal updateDetailsRequested()

    // A result of an earlier visit is stale; a check that still runs keeps its status
    onAboutToShow: {
        if (!checking)
            updateStatus = "";
    }

    // The AboutData singleton is registered by code, so no qmltypes describe it
    // qmllint disable unresolved-type missing-property
    readonly property var aboutData: Core.AboutData
    // qmllint enable unresolved-type missing-property

    // Overridable for the tests; an entry shows only for an http or https address
    property string homepage: aboutData.homepage
    property string bugAddress: aboutData.bugAddress

    // The room around the header icon and the close button, the same on every side, and the room around the content
    readonly property real margin: Kirigami.Units.largeSpacing
    readonly property real contentMargin: Kirigami.Units.largeSpacing * 2
    readonly property real headerSize: Kirigami.Units.iconSizes.medium

    function webUrl(address: string): string {
        return /^https?:\/\//i.test(address) ? address : "";
    }

    // A link is reachable by Tab, opens with Enter or Space, is read as a link, and is underlined and followed by the external-link icon, so the colour is not the only cue
    component Entry: ColumnLayout {
        id: entry

        property string title
        property string url

        visible: url.length > 0
        spacing: 0
        Layout.fillWidth: true
        // A layout takes its maximum from its children otherwise: an entry whose only child is the link would stay 0 wide
        Layout.maximumWidth: Number.POSITIVE_INFINITY

        // Left-aligned (LinkButton centres its text) and only as wide as its text, so the empty room at its right does not open the link
        Kirigami.UrlButton {
            url: entry.url
            text: entry.title
            font.underline: true
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignLeft
            Accessible.role: Accessible.Link
            Layout.maximumWidth: entry.width
        }
    }

    title: i18n("About Kinefin")
    standardButtons: Kirigami.Dialog.NoButton
    footer: null
    preferredWidth: Kirigami.Units.gridUnit * 27
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4

    header: Controls.Control {
        // Every side set on its own: the desktop style ignores `padding`; the header spans the dialog border (1 px at the top and the sides)
        topPadding: dialog.margin + 1
        bottomPadding: dialog.margin
        leftPadding: dialog.margin + 1
        rightPadding: dialog.margin + 1
        contentItem: RowLayout {
            spacing: Kirigami.Units.largeSpacing
            Kirigami.Icon {
                source: "help-about"
                Layout.preferredWidth: dialog.headerSize
                Layout.preferredHeight: dialog.headerSize
            }
            Kirigami.Heading { level: 3; text: dialog.title; Layout.fillWidth: true }
            Controls.ToolButton {
                text: i18n("Close")
                icon.name: "window-close"
                icon.width: Kirigami.Units.iconSizes.smallMedium
                icon.height: Kirigami.Units.iconSizes.smallMedium
                display: Controls.AbstractButton.IconOnly
                Layout.preferredWidth: dialog.headerSize
                Layout.preferredHeight: dialog.headerSize
                Controls.ToolTip.text: text
                Controls.ToolTip.visible: hovered || visualFocus
                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: dialog.close()
            }
        }
    }

    // The dialog sizes itself from an Item's implicitHeight and scrolls the content when the window is too short
    Item {
        implicitHeight: column.implicitHeight + dialog.contentMargin * 2

        ColumnLayout {
            id: column
            objectName: "aboutDialogColumn"
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                leftMargin: dialog.contentMargin
                rightMargin: dialog.contentMargin
                topMargin: dialog.contentMargin
            }
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                spacing: Kirigami.Units.largeSpacing * 2
                Layout.fillWidth: true
                Kirigami.Icon {
                    source: "io.github.the_beper.Kinefin"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.huge
                    Layout.preferredHeight: Kirigami.Units.iconSizes.huge
                }
                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true
                    Kirigami.Heading {
                        level: 2
                        text: i18n("Kinefin %1", dialog.displayVersion || dialog.aboutData.version)
                        textFormat: Text.PlainText
                    }
                    Controls.Label {
                        text: dialog.aboutData.shortDescription
                        textFormat: Text.PlainText
                        opacity: 0.7
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Controls.Label {
                        text: dialog.aboutData.copyrightStatement
                        textFormat: Text.PlainText
                        opacity: 0.7
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing * 2
                Layout.bottomMargin: Kirigami.Units.smallSpacing * 2
            }
            Entry {
                objectName: "aboutHomepageEntry"
                title: i18n("Homepage")
                url: dialog.webUrl(dialog.homepage)
            }
            Entry {
                objectName: "aboutBugEntry"
                title: i18n("Report a bug")
                url: dialog.webUrl(dialog.bugAddress)
            }
            Entry {
                objectName: "aboutLicenseEntry"
                title: i18n("License: GPL v3")
                url: "https://www.gnu.org/licenses/gpl-3.0.html"
            }
            Controls.Button {
                objectName: "aboutCheckUpdatesButton"
                Layout.alignment: Qt.AlignLeft
                Layout.topMargin: Kirigami.Units.smallSpacing * 2
                visible: dialog.updateChecksEnabled
                enabled: !dialog.checking
                icon.name: "view-refresh-symbolic"
                text: i18nc("@action:button", "Check for Updates")
                onClicked: dialog.checkForUpdatesRequested()
            }
            Controls.Label {
                objectName: "aboutUpdateStatus"
                Layout.fillWidth: true
                visible: dialog.updateChecksEnabled && text.length > 0
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: {
                    switch (dialog.updateStatus) {
                    case "checking":
                        return i18n("Checking for updates…");
                    case "upToDate":
                        return i18n("Kinefin is up to date");
                    case "failed":
                        return i18n("Could not check for updates. Check the internet connection");
                    case "available":
                        return i18n("Kinefin %1 is available", dialog.updateVersion);
                    default:
                        return "";
                    }
                }
            }
            Controls.Button {
                objectName: "aboutViewUpdateButton"
                Layout.alignment: Qt.AlignLeft
                visible: dialog.updateChecksEnabled && dialog.updateStatus === "available"
                icon.name: "download"
                text: i18nc("@action:button", "View Update")
                onClicked: dialog.updateDetailsRequested()
            }
            Controls.Label {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing * 2
                text: i18n("Built with Qt and KDE Frameworks")
                opacity: 0.7
                wrapMode: Text.Wrap
            }
        }
    }
}
