// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// The image types of Jellyfin as the Edit Images pages present them: the name of each, the shape of its usual picture and the width of a tile of the online grid
QtObject {
    function label(type) {
        switch (type) {
        case "Primary":
            return i18nc("@label image type", "Primary");
        case "Backdrop":
            return i18nc("@label image type", "Backdrop");
        case "Logo":
            return i18nc("@label image type", "Logo");
        case "Banner":
            return i18nc("@label image type", "Banner");
        case "Thumb":
            return i18nc("@label image type", "Thumbnail");
        case "Art":
            return i18nc("@label image type", "Art");
        case "Disc":
            return i18nc("@label image type", "Disc");
        case "Box":
            return i18nc("@label image type", "Box");
        case "BoxRear":
            return i18nc("@label image type", "Box (rear)");
        case "Menu":
            return i18nc("@label image type", "Menu");
        case "Screenshot":
            return i18nc("@label image type", "Screenshot");
        }
        return type;
    }

    // The width over the height of the usual picture of the type
    function aspect(type) {
        switch (type) {
        case "Primary":
        case "Box":
        case "BoxRear":
            return 2 / 3;
        case "Banner":
            return 5;
        case "Logo":
            return 2.5;
        case "Disc":
            return 1;
        }
        return 16 / 9;
    }

    // The width of a tile of the online grid, in grid units
    function tileUnits(type) {
        switch (type) {
        case "Primary":
        case "Box":
        case "BoxRear":
        case "Disc":
            return 10;
        case "Banner":
            return 24;
        case "Logo":
            return 14;
        }
        return 16;
    }
}
