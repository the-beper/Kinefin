// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import org.kde.coreaddons as Core
import org.kde.kirigami as Kirigami

// Reads the application's KAboutData (set in main.cpp).
Kirigami.AboutPage {
    objectName: "aboutPage"
    aboutData: Core.AboutData
}
