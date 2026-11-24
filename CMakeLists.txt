# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(kinefin VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

# 6.24 is the first Kirigami with Kirigami.StyleHints, which the list rows use
find_package(ECM 6.24.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs6)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Quick Qml QuickControls2 DBus Network WebSockets)
# libcrypto verifies the device authentication of Cast devices (CastDeviceAuth.cpp): Qt has no RSA signature API and checks chains against the process-wide CA list
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
# The QML files are flattened with resource aliases, so there is no extra directory whose qmldir the new behaviour of this policy would generate
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 OLD)
endif()
find_package(KF6 6.24 REQUIRED COMPONENTS Config CoreAddons I18n DBusAddons Wallet)
# The KDE runtime 6.11 ships SDL 3.2; the gamepad input needs nothing but the library
find_package(SDL3 3.2 REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
qt_standard_project_setup(REQUIRES 6.6)
set(KINEFIN_MPVQT_URL "https://download.kde.org/stable/mpvqt/mpvqt-1.2.1.tar.xz"
    CACHE STRING "URL of the pinned mpvqt 1.2.1 tarball (see the Flatpak manifest)")
include(FetchContent)
# The hash of the vendored renderer is part of the patch step, so an edit of third_party/mpvqt reaches an existing build tree too
file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpvqt/mpvrenderer.cpp KINEFIN_MPVQT_RENDERER_CPP)
file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpvqt/mpvrenderer.h KINEFIN_MPVQT_RENDERER_H)
FetchContent_Declare(mpvqt
    URL "${KINEFIN_MPVQT_URL}"
    URL_HASH SHA256=6ff5f56a0edd8248073446e5cc038f47d4b17c46b6af43cf438123d0ac911189
    # The renderer sources of third_party/mpvqt replace the tarball's (the build image has no patch tool); copy_if_different keeps a repeated populate harmless. A new mpvqt version needs them diffed against its own renderer first
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpvqt/mpvrenderer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpvqt/mpvrenderer.h
        <SOURCE_DIR>/src/
    COMMAND ${CMAKE_COMMAND} -E echo "vendored mpvqt renderer ${KINEFIN_MPVQT_RENDERER_CPP} ${KINEFIN_MPVQT_RENDERER_H}"
)
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(mpvqt)
unset(BUILD_SHARED_LIBS)

add_library(kinefin_core STATIC
    src/app/Logging.cpp
    src/app/Logging.h
    src/app/ProfileCommands.cpp
    src/app/ProfileCommands.h
    src/api/JellyfinClient.cpp
    src/api/JellyfinClient.h
    src/api/JellyfinSocketClient.cpp
    src/api/JellyfinSocketClient.h
    src/api/DeviceProfile.cpp
    src/api/DeviceProfile.h
    src/api/MediaSourceSelection.cpp
    src/api/MediaSourceSelection.h
    src/api/PlaybackReport.cpp
    src/api/PlaybackReport.h
    src/api/SyncPlayProtocol.cpp
    src/api/SyncPlayProtocol.h
    src/downloads/DownloadManager.cpp
    src/downloads/DownloadManager.h
    src/downloads/DownloadStore.cpp
    src/downloads/DownloadStore.h
    src/downloads/DownloadTransfer.cpp
    src/downloads/DownloadTransfer.h
    src/downloads/WatchedOutbox.cpp
    src/downloads/WatchedOutbox.h
    src/downloads/ExportNames.cpp
    src/downloads/ExportNames.h
    src/downloads/DownloadRules.cpp
    src/downloads/DownloadRules.h
    src/integration/NetworkMeter.cpp
    src/integration/NetworkMeter.h
    src/models/DownloadsModel.cpp
    src/models/DownloadsModel.h
    src/syncplay/ServerClock.cpp
    src/syncplay/ServerClock.h
    src/syncplay/SyncPlayGroupsModel.cpp
    src/syncplay/SyncPlayGroupsModel.h
    src/syncplay/SyncPlayQueueModel.cpp
    src/syncplay/SyncPlayQueueModel.h
    src/models/CultureOptionsModel.cpp
    src/models/CultureOptionsModel.h
    src/models/FilterOptionsModel.cpp
    src/models/FilterOptionsModel.h
    src/models/FavoritesModel.cpp
    src/models/FavoritesModel.h
    src/models/FavoritesSectionModel.cpp
    src/models/FavoritesSectionModel.h
    src/models/UserConfigurationModel.cpp
    src/models/UserConfigurationModel.h
    src/models/GenreRailsModel.cpp
    src/models/GenreRailsModel.h
    src/models/PlayQueue.cpp
    src/models/PlayQueue.h
    src/models/PlaylistEntriesModel.cpp
    src/models/PlaylistEntriesModel.h
    src/models/PlaylistsController.cpp
    src/models/PlaylistsController.h
    src/models/CardRoles.cpp
    src/models/CardRoles.h
    src/models/HomeRowsModel.cpp
    src/models/HomeRowsModel.h
    src/models/ItemDetails.cpp
    src/models/ItemDetails.h
    src/models/ItemDetailsModel.cpp
    src/models/ItemDetailsModel.h
    src/models/ItemChildrenModel.cpp
    src/models/ItemChildrenModel.h
    src/models/ItemRowModel.cpp
    src/models/ItemRowModel.h
    src/models/ItemSummary.cpp
    src/models/PagedItemsModel.cpp
    src/models/SearchModel.cpp
    src/models/SearchModel.h
    src/models/SeasonEpisodesModel.cpp
    src/models/SeasonEpisodesModel.h
    src/models/PagedItemsModel.h
    src/models/ResumeItemsModel.cpp
    src/models/ResumeItemsModel.h
    src/models/PersonItemsModel.cpp
    src/models/PersonItemsModel.h
    src/models/CollectionsModel.cpp
    src/models/CollectionsModel.h
    src/models/RemoteSessionsModel.cpp
    src/models/RemoteSessionsModel.h
    src/models/ItemSummary.h
    src/models/LibraryItemsModel.cpp
    src/models/LibraryItemsModel.h
    src/models/LibrarySubtabController.cpp
    src/models/LibrarySubtabController.h
    src/models/RecommendationRowsModel.cpp
    src/models/RecommendationRowsModel.h
    src/imagecache/ImageNetworkAccess.cpp
    src/imagecache/ImageNetworkAccess.h
    src/imagecache/ImageOrigin.cpp
    src/imagecache/ImageOrigin.h
    src/player/PlaybackReporter.cpp
    src/player/PlaybackReporter.h
    src/player/PlaybackSession.cpp
    src/integration/LinkRouter.cpp
    src/settings/BrowseStateStore.cpp
    src/settings/BrowseStateStore.h
    src/settings/ClientSettings.cpp
    src/settings/ClientSettings.h
    src/settings/KWalletTokenStore.cpp
    src/settings/KWalletTokenStore.h
    src/settings/ProfileIdentity.cpp
    src/settings/ProfileIdentity.h
    src/settings/ProfileIndex.cpp
    src/settings/ProfileIndex.h
    src/settings/MpvConf.cpp
    src/settings/MpvConf.h
    src/settings/ProfilePaths.cpp
    src/settings/ProfilePaths.h
    src/settings/ServerCredentials.cpp
    src/settings/ServerCredentials.h
    src/settings/ServerCredentialSync.cpp
    src/settings/ServerCredentialSync.h
    src/settings/SettingDefinitions.cpp
    src/settings/SettingDefinitions.h
    src/settings/SettingsSectionModel.cpp
    src/settings/SettingsSectionModel.h
    src/settings/TokenStore.h
)
target_include_directories(kinefin_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Lets the QML module see the core library's types that src/app/ClientSettingsQml.h exposes.
qt_extract_metatypes(kinefin_core)
target_compile_definitions(kinefin_core PRIVATE TRANSLATION_DOMAIN="kinefin")
target_link_libraries(kinefin_core PUBLIC Qt6::Core Qt6::DBus Qt6::Network Qt6::WebSockets KF6::Wallet KF6::ConfigCore KF6::I18n)

add_library(kinefin_player STATIC
    src/player/BitrateFallback.cpp
    src/player/BitrateFallback.h
    src/player/ClipExporter.cpp
    src/player/ClipExporter.h
    src/player/MpvConfValidator.cpp
    src/player/MpvConfValidator.h
    src/player/PlaybackEngine.cpp
    src/player/PlaybackEngine.h
    src/player/GroupPlayback.h
    src/player/PlayerController.cpp
    src/player/PlayerController.h
    src/player/PlayerSettings.cpp
    src/player/PlayerSettings.h
    src/player/SeriesPreferences.cpp
    src/player/SeriesPreferences.h
    src/player/Trickplay.cpp
    src/player/Trickplay.h
    src/syncplay/SyncPlayController.cpp
    src/syncplay/SyncPlayController.h
)
target_compile_definitions(kinefin_player PRIVATE TRANSLATION_DOMAIN="kinefin")
target_include_directories(kinefin_player PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kinefin_player PUBLIC kinefin_core MpvQt::MpvQt Qt6::Gui)
if(mpvqt_BINARY_DIR)
    target_include_directories(kinefin_player PRIVATE ${mpvqt_BINARY_DIR}/src)
endif()

# The desktop around the app: MPRIS and the screensaver inhibition on the session bus, the remote-control socket
add_library(kinefin_integration STATIC
    src/app/InputFilter.cpp
    src/app/InputFilter.h
    src/integration/GamepadInput.cpp
    src/integration/GamepadInput.h
    src/integration/GamepadMapper.cpp
    src/integration/GamepadMapper.h
    src/integration/KRunnerProvider.cpp
    src/integration/KRunnerProvider.h
    src/integration/Mpris.cpp
    src/integration/Mpris.h
    src/integration/MprisArtwork.cpp
    src/integration/MprisArtwork.h
    src/integration/RemoteInputServer.cpp
    src/integration/RemoteInputServer.h
    src/integration/ScreenSaverInhibitor.cpp
    src/integration/ScreenSaverInhibitor.h
    src/integration/SuspendInhibitor.cpp
    src/integration/SuspendInhibitor.h
    src/integration/SpatialNavigation.cpp
    src/integration/SpatialNavigation.h
)
target_compile_definitions(kinefin_integration PRIVATE TRANSLATION_DOMAIN="kinefin")
target_link_libraries(kinefin_integration PUBLIC kinefin_player Qt6::DBus Qt6::Gui Qt6::Network Qt6::Quick KF6::I18n SDL3::SDL3)

# Cast-out building blocks: SSDP and mDNS search, UPnP descriptions, SOAP, DIDL, the renderer device profile and the Cast V2 channel with its device authentication; Qt Core and Network, and libcrypto
add_library(kinefin_cast STATIC
    src/cast/CastChannel.cpp
    src/cast/CastChannel.h
    src/cast/CastController.cpp
    src/cast/CastController.h
    src/cast/CastDeviceAuth.cpp
    src/cast/CastDeviceAuth.h
    src/cast/CastNetwork.cpp
    src/cast/CastNetwork.h
    src/cast/CastProfile.cpp
    src/cast/CastProfile.h
    src/cast/CastProto.cpp
    src/cast/CastProto.h
    src/cast/CastRenderer.h
    src/cast/CastSender.cpp
    src/cast/CastSender.h
    src/cast/CastTargetsModel.cpp
    src/cast/CastTargetsModel.h
    src/cast/CastText.cpp
    src/cast/CastText.h
    src/cast/Didl.cpp
    src/cast/Didl.h
    src/cast/DlnaRenderer.cpp
    src/cast/DlnaRenderer.h
    src/cast/Mdns.cpp
    src/cast/Mdns.h
    src/cast/RendererProfile.cpp
    src/cast/RendererProfile.h
    src/cast/Soap.cpp
    src/cast/Soap.h
    src/cast/Ssdp.cpp
    src/cast/Ssdp.h
    src/cast/StreamProbe.cpp
    src/cast/StreamProbe.h
    src/cast/UpnpDescription.cpp
    src/cast/UpnpDescription.h
    src/cast/XmlTree.cpp
    src/cast/XmlTree.h
)
target_include_directories(kinefin_cast PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(kinefin_cast PRIVATE TRANSLATION_DOMAIN="kinefin")
target_link_libraries(kinefin_cast PUBLIC Qt6::Core Qt6::Network KF6::I18n)
target_link_libraries(kinefin_cast PRIVATE OpenSSL::Crypto)
# The pinned Cast roots; the resource is initialised by CastTrust::google() because the library is static
qt_add_resources(kinefin_cast "cast_roots"
    PREFIX "/cast"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openscreen"
    FILES third_party/openscreen/cast_root_ca.der third_party/openscreen/eureka_root_ca.der
)
target_link_libraries(kinefin_player PUBLIC kinefin_cast)

# The QML module lives in a static library of its own so that the UI tests can import it like the application does
foreach(page Main PlayerPage OsdButton MiniPlayer PictureInPictureWindow PlaybackInfoOverlay ShellHeader AboutDialog ShortcutsDialog DialogTitleBar RemoteSessionsDialog SyncPlayDialog MediaInfoDialog CastDialog CastTargetRow HomePage LibraryTile LibraryPage ResumeItemsPage PersonPage MediaCard CardMenu CardRow EpisodeRow ItemDetailsPage SearchPage QueuePage FilterDialog FilterChips GenreRailsView GenreRailsPage LibraryContent ItemListRow ItemRowGrid LoginPage UserPickerPage MessageBox ServersPage SettingsPage SettingRow UserPreferencesForm UserPreferencesSaveBar FavoritesPage FavoriteCard FavoritesBanners FavoritesSectionPage TopFade MediaListRow PlaylistsPage PlaylistPage PlaylistDialogs AddToPlaylistDialog AddToCollectionDialog CollectionDialogs EditMetadataDialog IdentifyDialog EditImagesDialog EditSubtitlesDialog PageToolbar PageBackButton FadeInOnShow PageTitle PlainToolTip PopupPlacement AutoScroll DownloadsPage DownloadRulesPage DownloadAllDialog RuleDialog SubtitleSearchDialog SavedFiltersDialog UploadSubtitleDialog PlainMenuItem)
    set_source_files_properties(src/qml/${page}.qml PROPERTIES QT_RESOURCE_ALIAS ${page}.qml)
endforeach()
# The images the pages draw themselves, beside the QML files they belong to
foreach(icon critic-fresh critic-rotten)
    set_source_files_properties(src/qml/icons/${icon}.svg PROPERTIES QT_RESOURCE_ALIAS icons/${icon}.svg)
endforeach()
qt_add_library(kinefin_ui STATIC)
qt_add_qml_module(kinefin_ui
    URI org.kde.kinefin
    VERSION 1.0
    QML_FILES
        src/qml/Main.qml
        src/qml/PlayerPage.qml
        src/qml/OsdButton.qml
        src/qml/MiniPlayer.qml
        src/qml/PictureInPictureWindow.qml
        src/qml/MediaListRow.qml
        src/qml/PlaylistsPage.qml
        src/qml/PlaylistPage.qml
        src/qml/PlaylistDialogs.qml
        src/qml/AddToPlaylistDialog.qml
        src/qml/PageToolbar.qml
        src/qml/PageBackButton.qml
        src/qml/FadeInOnShow.qml
        src/qml/PageTitle.qml
        src/qml/PlainToolTip.qml
        src/qml/PopupPlacement.qml
        src/qml/AutoScroll.qml
        src/qml/PlaybackInfoOverlay.qml
        src/qml/ShellHeader.qml
        src/qml/ShortcutsDialog.qml
        src/qml/DialogTitleBar.qml
        src/qml/AboutDialog.qml
        src/qml/RemoteSessionsDialog.qml
        src/qml/SyncPlayDialog.qml
        src/qml/MediaInfoDialog.qml
        src/qml/CastDialog.qml
        src/qml/CastTargetRow.qml
        src/qml/HomePage.qml
        src/qml/LibraryTile.qml
        src/qml/LibraryPage.qml
        src/qml/ResumeItemsPage.qml
        src/qml/PersonPage.qml
        src/qml/AddToCollectionDialog.qml
        src/qml/CollectionDialogs.qml
        src/qml/EditMetadataDialog.qml
        src/qml/IdentifyDialog.qml
        src/qml/EditImagesDialog.qml
        src/qml/EditSubtitlesDialog.qml
        src/qml/MediaCard.qml
        src/qml/CardMenu.qml
        src/qml/CardRow.qml
        src/qml/EpisodeRow.qml
        src/qml/ItemDetailsPage.qml
        src/qml/SearchPage.qml
        src/qml/QueuePage.qml
        src/qml/DownloadsPage.qml
        src/qml/DownloadRulesPage.qml
        src/qml/DownloadAllDialog.qml
        src/qml/RuleDialog.qml
        src/qml/SubtitleSearchDialog.qml
        src/qml/FilterDialog.qml
        src/qml/FilterChips.qml
        src/qml/SavedFiltersDialog.qml
        src/qml/UploadSubtitleDialog.qml
        src/qml/PlainMenuItem.qml
        src/qml/GenreRailsView.qml
        src/qml/GenreRailsPage.qml
        src/qml/LibraryContent.qml
        src/qml/TopFade.qml
        src/qml/ItemListRow.qml
        src/qml/ItemRowGrid.qml
        src/qml/LoginPage.qml
        src/qml/UserPickerPage.qml
        src/qml/MessageBox.qml
        src/qml/ServersPage.qml
        src/qml/SettingsPage.qml
        src/qml/SettingRow.qml
        src/qml/UserPreferencesForm.qml
        src/qml/UserPreferencesSaveBar.qml
        src/qml/FavoritesPage.qml
        src/qml/FavoriteCard.qml
        src/qml/FavoritesBanners.qml
        src/qml/FavoritesSectionPage.qml
    RESOURCES
        src/qml/icons/critic-fresh.svg
        src/qml/icons/critic-rotten.svg
    SOURCES
        src/app/ClientSettingsQml.h
        src/app/ConnectionController.cpp
        src/app/ConnectionController.h
        src/app/DownloadsController.cpp
        src/app/DownloadsController.h
        src/app/EpisodeLabelQml.cpp
        src/app/EpisodeLabelQml.h
        src/app/ImageNetworkFactory.cpp
        src/app/ImageNetworkFactory.h
        src/app/ModelsQml.h
        src/app/MiddleButtonWatcher.cpp
        src/app/MiddleButtonWatcher.h
        src/app/MouseNavigation.cpp
        src/app/MouseNavigation.h
        src/app/MpvVideoItem.cpp
        src/app/MpvVideoItem.h
        src/app/PipSurface.cpp
        src/app/PipSurface.h
        src/app/RemoteCommands.cpp
        src/app/RemoteCommands.h
        src/app/SpatialQml.h
)
target_include_directories(kinefin_ui PUBLIC src/app)
target_compile_definitions(kinefin_ui PRIVATE TRANSLATION_DOMAIN="kinefin")
target_link_libraries(kinefin_ui PUBLIC
    kinefin_core kinefin_player kinefin_integration MpvQt::MpvQt
    Qt6::Gui Qt6::Quick Qt6::Qml Qt6::QuickControls2
    KF6::I18n KF6::I18nQml
)
# KWin's picture-in-picture surface needs QtWaylandClient's private headers: the Flatpak SDK ships them, the Fedora build image has the CMake target without them. Without them PipSurface reports itself unavailable and the window stays an ordinary one
find_package(Qt6 QUIET COMPONENTS WaylandClient WaylandClientPrivate WaylandScannerTools)
find_program(KINEFIN_WAYLAND_SCANNER wayland-scanner)
set(KINEFIN_XX_PIP OFF)
if(TARGET Qt6::WaylandClientPrivate AND COMMAND qt6_generate_wayland_protocol_client_sources AND KINEFIN_WAYLAND_SCANNER)
    find_path(KINEFIN_WAYLAND_CLIENT_PRIVATE_DIR QtWaylandClient/private/qwaylandshellsurface_p.h
        PATHS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_HEADERS}/QtWaylandClient/${Qt6WaylandClient_VERSION}"
              "${QT6_INSTALL_PREFIX}/include/QtWaylandClient/${Qt6WaylandClient_VERSION}"
              "${QT6_INSTALL_PREFIX}/include/qt6/QtWaylandClient/${Qt6WaylandClient_VERSION}"
        NO_DEFAULT_PATH)
    # Qt's own copy first: the interface tables below then describe the same xdg-shell as QtWaylandClient's
    find_file(KINEFIN_XDG_SHELL_XML xdg-shell.xml
        PATHS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_DATA}/wayland/protocols/xdg-shell"
              "${QT6_INSTALL_PREFIX}/share/qt6/wayland/protocols/xdg-shell"
              "${QT6_INSTALL_PREFIX}/share/wayland-protocols/stable/xdg-shell"
        NO_DEFAULT_PATH)
    if(KINEFIN_WAYLAND_CLIENT_PRIVATE_DIR AND KINEFIN_XDG_SHELL_XML)
        # The scanner writes the protocols' interface tables as C sources
        enable_language(C)
        qt6_generate_wayland_protocol_client_sources(kinefin_ui FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wayland/xx-pip-v1.xml)
        # Only the interface tables of xdg-shell, which xx-pip-v1 refers to: its C++ wrappers come from QtWaylandClient, which exports them, and a second copy of those classes would stand in for Qt's own
        set(KINEFIN_XDG_SHELL_TABLES ${CMAKE_CURRENT_BINARY_DIR}/kinefin-xdg-shell-interfaces.c)
        add_custom_command(OUTPUT ${KINEFIN_XDG_SHELL_TABLES}
            COMMAND ${KINEFIN_WAYLAND_SCANNER} private-code ${KINEFIN_XDG_SHELL_XML} ${KINEFIN_XDG_SHELL_TABLES}
            DEPENDS ${KINEFIN_XDG_SHELL_XML}
            VERBATIM)
        target_sources(kinefin_ui PRIVATE ${KINEFIN_XDG_SHELL_TABLES})
        target_link_libraries(kinefin_ui PRIVATE Qt6::WaylandClientPrivate)
        target_compile_definitions(kinefin_ui PRIVATE KINEFIN_HAVE_XX_PIP)
        set(KINEFIN_XX_PIP ON)
    endif()
endif()
message(STATUS "KWin picture-in-picture surface (xx_pip_v1): ${KINEFIN_XX_PIP}")

add_executable(kinefin src/main.cpp)
qt_add_resources(kinefin "licenses"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES"
    FILES LICENSES/GPL-3.0-or-later.txt
)
target_link_libraries(kinefin PRIVATE
    kinefin_ui kinefin_uiplugin kinefin_core kinefin_player kinefin_integration
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Quick Qt6::Qml Qt6::QuickControls2 Qt6::DBus
    KF6::CoreAddons KF6::I18n KF6::I18nQml KF6::DBusAddons
)
target_include_directories(kinefin PRIVATE src/app)
# A development Flatpak names itself after its build time in About (build/flatpak/build-flatpak.sh writes the file for the length of the build); the version the server sees stays PROJECT_VERSION
set(KINEFIN_DISPLAY_VERSION "${PROJECT_VERSION}")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/flatpak/build-id")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/build/flatpak/build-id" KINEFIN_DISPLAY_VERSION LIMIT_COUNT 1 REGEX "^[0-9.]+$")
    if(NOT KINEFIN_DISPLAY_VERSION)
        set(KINEFIN_DISPLAY_VERSION "${PROJECT_VERSION}")
    endif()
endif()
target_compile_definitions(kinefin PRIVATE KINEFIN_VERSION="${PROJECT_VERSION}" KINEFIN_DISPLAY_VERSION="${KINEFIN_DISPLAY_VERSION}" TRANSLATION_DOMAIN="kinefin")

# po/ is flat (<language>.po), while ki18n_install expects <language>/<domain>.po and would install the catalogs under the wrong names: compile each one to <locale>/<language>/LC_MESSAGES/kinefin.mo instead
if(NOT KDE_INSTALL_LOCALEDIR)
    set(KDE_INSTALL_LOCALEDIR "share/locale")
endif()
find_package(Gettext REQUIRED)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/po/LINGUAS" KINEFIN_LANGUAGES)
set(KINEFIN_MO_FILES)
foreach(language IN LISTS KINEFIN_LANGUAGES)
    set(po_file "${CMAKE_CURRENT_SOURCE_DIR}/po/${language}.po")
    set(mo_file "${CMAKE_CURRENT_BINARY_DIR}/locale/${language}/kinefin.mo")
    add_custom_command(
        OUTPUT "${mo_file}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/locale/${language}"
        COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -c -o "${mo_file}" "${po_file}"
        DEPENDS "${po_file}"
        COMMENT "Compiling the ${language} translation"
        VERBATIM
    )
    install(FILES "${mo_file}" DESTINATION "${KDE_INSTALL_LOCALEDIR}/${language}/LC_MESSAGES")
    list(APPEND KINEFIN_MO_FILES "${mo_file}")
endforeach()
add_custom_target(kinefin-translations ALL DEPENDS ${KINEFIN_MO_FILES})

install(TARGETS kinefin ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(FILES data/io.github.the_beper.Kinefin.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES data/krunner/io.github.the_beper.Kinefin.desktop DESTINATION ${KDE_INSTALL_DATADIR}/krunner/dbusplugins)
install(DIRECTORY data/icons/hicolor DESTINATION ${KDE_INSTALL_ICONDIR})
install(FILES data/io.github.the_beper.Kinefin.metainfo.xml DESTINATION ${KDE_INSTALL_METAINFODIR})

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(test_playback_session tests/test_playback_session.cpp)
    target_link_libraries(test_playback_session PRIVATE kinefin_core Qt6::Test)
    add_test(NAME playback_session COMMAND test_playback_session)

    add_executable(test_playback_session_portable tests/test_playback_session_portable.cpp)
    target_link_libraries(test_playback_session_portable PRIVATE kinefin_core)
    add_test(NAME playback_session_portable COMMAND test_playback_session_portable)

    add_executable(test_jellyfin_client tests/test_jellyfin_client.cpp)
    target_compile_definitions(test_jellyfin_client PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_jellyfin_client PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME jellyfin_client COMMAND test_jellyfin_client)

    add_executable(test_device_profile tests/test_device_profile.cpp)
    target_compile_definitions(test_device_profile PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_device_profile PRIVATE kinefin_core Qt6::Test)
    add_test(NAME device_profile COMMAND test_device_profile)

    add_executable(test_media_source_selection tests/test_media_source_selection.cpp)
    target_compile_definitions(test_media_source_selection PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_media_source_selection PRIVATE kinefin_core Qt6::Test)
    add_test(NAME media_source_selection COMMAND test_media_source_selection)

    add_executable(test_playback_report tests/test_playback_report.cpp)
    target_compile_definitions(test_playback_report PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_playback_report PRIVATE kinefin_core Qt6::Test)
    add_test(NAME playback_report COMMAND test_playback_report)

    add_executable(test_playback_reporter tests/test_playback_reporter.cpp)
    target_compile_definitions(test_playback_reporter PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_playback_reporter PRIVATE kinefin_core Qt6::Test)
    add_test(NAME playback_reporter COMMAND test_playback_reporter)

    add_executable(test_trickplay tests/test_trickplay.cpp)
    target_link_libraries(test_trickplay PRIVATE kinefin_player Qt6::Test)
    add_test(NAME trickplay COMMAND test_trickplay)

    add_executable(test_playback_engine tests/test_playback_engine.cpp)
    target_compile_definitions(test_playback_engine PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_playback_engine PRIVATE kinefin_player Qt6::Test)
    add_test(NAME playback_engine COMMAND test_playback_engine)

    add_executable(test_bitrate_fallback tests/test_bitrate_fallback.cpp)
    target_link_libraries(test_bitrate_fallback PRIVATE kinefin_player Qt6::Test)
    add_test(NAME bitrate_fallback COMMAND test_bitrate_fallback)

    add_executable(test_series_preferences tests/test_series_preferences.cpp)
    target_link_libraries(test_series_preferences PRIVATE kinefin_player Qt6::Test)
    add_test(NAME series_preferences COMMAND test_series_preferences)

    add_executable(test_player_controller tests/test_player_controller.cpp)
    target_compile_definitions(test_player_controller PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_player_controller PRIVATE kinefin_player kinefin_core Qt6::Test Qt6::Network OpenSSL::Crypto)
    add_test(NAME player_controller COMMAND test_player_controller)

    add_executable(test_syncplay_controller tests/test_syncplay_controller.cpp)
    target_compile_definitions(test_syncplay_controller PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_syncplay_controller PRIVATE kinefin_player kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME syncplay_controller COMMAND test_syncplay_controller)

    # The MPRIS and screensaver tests need a session bus of their own
    add_executable(test_mpris tests/test_mpris.cpp)
    target_compile_definitions(test_mpris PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_mpris PRIVATE kinefin_integration kinefin_core Qt6::Test Qt6::Network Qt6::DBus)
    add_test(NAME mpris COMMAND dbus-run-session -- $<TARGET_FILE:test_mpris>)

    add_executable(test_krunner tests/test_krunner.cpp)
    target_compile_definitions(test_krunner PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_krunner PRIVATE kinefin_integration kinefin_core Qt6::Test Qt6::Network Qt6::DBus)
    add_test(NAME krunner COMMAND dbus-run-session -- $<TARGET_FILE:test_krunner>)

    add_executable(test_clip_exporter tests/test_clip_exporter.cpp)
    target_link_libraries(test_clip_exporter PRIVATE kinefin_player Qt6::Test)
    add_test(NAME clip_exporter COMMAND test_clip_exporter)

    add_executable(test_input_filter tests/test_input_filter.cpp)
    target_link_libraries(test_input_filter PRIVATE kinefin_integration Qt6::Test Qt6::Gui)
    add_test(NAME input_filter COMMAND test_input_filter)
    set_tests_properties(input_filter PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_gamepad_mapper tests/test_gamepad_mapper.cpp)
    target_link_libraries(test_gamepad_mapper PRIVATE kinefin_integration Qt6::Test)
    add_test(NAME gamepad_mapper COMMAND test_gamepad_mapper)

    add_executable(test_gamepad_input tests/test_gamepad_input.cpp)
    target_link_libraries(test_gamepad_input PRIVATE kinefin_integration Qt6::Test Qt6::Gui)
    add_test(NAME gamepad_input COMMAND test_gamepad_input)
    set_tests_properties(gamepad_input PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_cast_ssdp tests/test_cast_ssdp.cpp)
    target_link_libraries(test_cast_ssdp PRIVATE kinefin_cast Qt6::Test Qt6::Network)
    add_test(NAME cast_ssdp COMMAND test_cast_ssdp)

    add_executable(test_cast_description tests/test_cast_description.cpp)
    target_link_libraries(test_cast_description PRIVATE kinefin_cast Qt6::Test)
    add_test(NAME cast_description COMMAND test_cast_description)

    add_executable(test_cast_soap tests/test_cast_soap.cpp)
    target_link_libraries(test_cast_soap PRIVATE kinefin_cast Qt6::Test)
    add_test(NAME cast_soap COMMAND test_cast_soap)

    add_executable(test_cast_renderer_profile tests/test_cast_renderer_profile.cpp)
    target_link_libraries(test_cast_renderer_profile PRIVATE kinefin_cast Qt6::Test)
    add_test(NAME cast_renderer_profile COMMAND test_cast_renderer_profile)

    add_executable(test_cast_renderer tests/test_cast_renderer.cpp)
    target_link_libraries(test_cast_renderer PRIVATE kinefin_cast Qt6::Test Qt6::Network)
    add_test(NAME cast_renderer COMMAND test_cast_renderer)

    add_executable(test_cast_mdns tests/test_cast_mdns.cpp)
    target_link_libraries(test_cast_mdns PRIVATE kinefin_cast Qt6::Test Qt6::Network)
    target_compile_definitions(test_cast_mdns PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME cast_mdns COMMAND test_cast_mdns)

    add_executable(test_cast_proto tests/test_cast_proto.cpp)
    target_link_libraries(test_cast_proto PRIVATE kinefin_cast Qt6::Test)
    add_test(NAME cast_proto COMMAND test_cast_proto)

    add_executable(test_cast_device_auth tests/test_cast_device_auth.cpp)
    target_link_libraries(test_cast_device_auth PRIVATE kinefin_cast Qt6::Test OpenSSL::Crypto)
    add_test(NAME cast_device_auth COMMAND test_cast_device_auth)

    add_executable(test_cast_channel tests/test_cast_channel.cpp)
    target_link_libraries(test_cast_channel PRIVATE kinefin_cast Qt6::Test Qt6::Network OpenSSL::Crypto)
    add_test(NAME cast_channel COMMAND test_cast_channel)

    add_executable(test_cast_profile tests/test_cast_profile.cpp)
    target_link_libraries(test_cast_profile PRIVATE kinefin_cast Qt6::Test)
    add_test(NAME cast_profile COMMAND test_cast_profile)

    add_executable(test_cast_sender tests/test_cast_sender.cpp)
    target_link_libraries(test_cast_sender PRIVATE kinefin_cast Qt6::Test Qt6::Network OpenSSL::Crypto)
    add_test(NAME cast_sender COMMAND test_cast_sender)

    add_executable(test_cast_controller tests/test_cast_controller.cpp)
    target_link_libraries(test_cast_controller PRIVATE kinefin_cast Qt6::Test Qt6::Network OpenSSL::Crypto)
    add_test(NAME cast_controller COMMAND test_cast_controller)

    add_executable(test_screensaver_inhibitor tests/test_screensaver_inhibitor.cpp)
    target_compile_definitions(test_screensaver_inhibitor PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_screensaver_inhibitor PRIVATE kinefin_integration kinefin_core Qt6::Test Qt6::Network Qt6::DBus)
    add_test(NAME screensaver_inhibitor COMMAND dbus-run-session -- $<TARGET_FILE:test_screensaver_inhibitor>)

    add_executable(test_suspend_inhibitor tests/test_suspend_inhibitor.cpp)
    target_compile_definitions(test_suspend_inhibitor PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_suspend_inhibitor PRIVATE kinefin_integration kinefin_core Qt6::Test Qt6::Network Qt6::DBus)
    add_test(NAME suspend_inhibitor COMMAND dbus-run-session -- $<TARGET_FILE:test_suspend_inhibitor>)

    add_executable(test_remote_input tests/test_remote_input.cpp)
    target_link_libraries(test_remote_input PRIVATE kinefin_integration kinefin_core Qt6::Test Qt6::Gui Qt6::Network)
    add_test(NAME remote_input COMMAND test_remote_input)
    set_tests_properties(remote_input PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_spatial_navigation tests/test_spatial_navigation.cpp)
    target_link_libraries(test_spatial_navigation PRIVATE kinefin_integration Qt6::Test Qt6::Gui Qt6::Quick Qt6::Qml)
    add_test(NAME spatial_navigation COMMAND test_spatial_navigation)
    set_tests_properties(spatial_navigation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_jellyfin_socket tests/test_jellyfin_socket.cpp)
    target_compile_definitions(test_jellyfin_socket PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_jellyfin_socket PRIVATE kinefin_core Qt6::Test Qt6::Network Qt6::WebSockets)
    add_test(NAME jellyfin_socket COMMAND test_jellyfin_socket)

    add_executable(test_syncplay_protocol tests/test_syncplay_protocol.cpp)
    target_compile_definitions(test_syncplay_protocol PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_syncplay_protocol PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME syncplay_protocol COMMAND test_syncplay_protocol)

    add_executable(test_server_clock tests/test_server_clock.cpp)
    target_link_libraries(test_server_clock PRIVATE kinefin_core Qt6::Test)
    add_test(NAME server_clock COMMAND test_server_clock)

    add_executable(test_jellyfin_live tests/test_jellyfin_live.cpp)
    target_link_libraries(test_jellyfin_live PRIVATE kinefin_player kinefin_core Qt6::Test Qt6::Network Qt6::WebSockets)
    add_test(NAME jellyfin_live COMMAND test_jellyfin_live)

    add_executable(test_link_router tests/test_link_router.cpp)
    target_link_libraries(test_link_router PRIVATE kinefin_core Qt6::Test)
    add_test(NAME link_router COMMAND test_link_router)

    add_executable(test_profile_index tests/test_profile_index.cpp)
    target_link_libraries(test_profile_index PRIVATE kinefin_core Qt6::Test)
    add_test(NAME profile_index COMMAND test_profile_index)

    add_executable(test_profile_identity tests/test_profile_identity.cpp)
    target_link_libraries(test_profile_identity PRIVATE kinefin_core Qt6::Test)
    add_test(NAME profile_identity COMMAND test_profile_identity)

    add_executable(test_connection_controller tests/test_connection_controller.cpp)
    target_compile_definitions(test_connection_controller PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" TRANSLATION_DOMAIN="kinefin")
    target_link_libraries(test_connection_controller PRIVATE kinefin_ui kinefin_core Qt6::Test Qt6::Gui Qt6::Qml KF6::I18n)
    add_test(NAME connection_controller COMMAND test_connection_controller)
    add_executable(test_remote_commands tests/test_remote_commands.cpp)
    target_link_libraries(test_remote_commands PRIVATE kinefin_ui kinefin_core Qt6::Test)
    add_test(NAME remote_commands COMMAND test_remote_commands)

    add_executable(test_home_rows_model tests/test_home_rows_model.cpp)
    target_compile_definitions(test_home_rows_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_home_rows_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME home_rows_model COMMAND test_home_rows_model)

    add_executable(test_ui_pages tests/test_ui_pages.cpp)
    target_compile_definitions(test_ui_pages PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" TRANSLATION_DOMAIN="kinefin")
    target_link_libraries(test_ui_pages PRIVATE kinefin_ui kinefin_uiplugin kinefin_integration kinefin_core Qt6::Test Qt6::DBus Qt6::Gui Qt6::Widgets Qt6::Quick Qt6::QuickControls2 Qt6::Qml KF6::I18n KF6::I18nQml)
    # The UI tests take minutes in one process: the test functions are dealt out to shards that run side by side under `ctest -j`. Functions with a `_data` sibling are the slow ones and go first so no shard gets two of them
    set(KINEFIN_UI_SHARDS 6)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS tests/test_ui_pages.cpp)
    file(READ tests/test_ui_pages.cpp ui_pages_source)
    string(FIND "${ui_pages_source}" "private Q_SLOTS:" ui_pages_slots_at)
    string(SUBSTRING "${ui_pages_source}" ${ui_pages_slots_at} -1 ui_pages_source)
    string(REGEX MATCHALL "\n    void [A-Za-z0-9_]+\\(\\)\n" ui_pages_slots "${ui_pages_source}")
    set(ui_pages_data "")
    set(ui_pages_plain "")
    foreach(slot IN LISTS ui_pages_slots)
        string(REGEX REPLACE "^\n    void ([A-Za-z0-9_]+)\\(\\)\n$" "\\1" slot "${slot}")
        if(slot MATCHES "_data$")
            string(REGEX REPLACE "_data$" "" slot "${slot}")
            list(APPEND ui_pages_data "${slot}")
        elseif(NOT slot MATCHES "^(init|cleanup|initTestCase|cleanupTestCase)$")
            list(APPEND ui_pages_plain "${slot}")
        endif()
    endforeach()
    if(ui_pages_data)
        list(REMOVE_ITEM ui_pages_plain ${ui_pages_data})
    endif()
    set(ui_pages_all ${ui_pages_data} ${ui_pages_plain})
    set(index 0)
    foreach(slot IN LISTS ui_pages_all)
        math(EXPR shard "${index} % ${KINEFIN_UI_SHARDS}")
        list(APPEND ui_pages_shard_${shard} "${slot}")
        math(EXPR index "${index} + 1")
    endforeach()
    math(EXPR last_shard "${KINEFIN_UI_SHARDS} - 1")
    foreach(shard RANGE ${last_shard})
        if(ui_pages_shard_${shard})
            add_test(NAME ui_pages_${shard} COMMAND test_ui_pages ${ui_pages_shard_${shard}})
            # One processor each: on the two-core CI runner `ctest -j 2` then runs two shards at a time instead of one (PROCESSORS 2 made them strictly serial, 23 of the job's 40 minutes)
            set_tests_properties(ui_pages_${shard} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" PROCESSORS 1)
        endif()
    endforeach()
    add_test(NAME ui_pages_shards COMMAND ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE:test_ui_pages> "-DSHARDED=${ui_pages_all}" -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_ui_shards.cmake)
    set_tests_properties(ui_pages_shards PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_filter_options_model tests/test_filter_options_model.cpp)
    target_compile_definitions(test_filter_options_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_filter_options_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME filter_options_model COMMAND test_filter_options_model)

    add_executable(test_playlists tests/test_playlists.cpp)
    target_compile_definitions(test_playlists PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_playlists PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME playlists COMMAND test_playlists)

    add_executable(test_play_queue tests/test_play_queue.cpp)
    target_compile_definitions(test_play_queue PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_play_queue PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME play_queue COMMAND test_play_queue)

    add_executable(test_browse_state_store tests/test_browse_state_store.cpp)
    target_link_libraries(test_browse_state_store PRIVATE kinefin_core Qt6::Test)
    add_test(NAME browse_state_store COMMAND test_browse_state_store)

    add_executable(test_download_store tests/test_download_store.cpp)
    target_link_libraries(test_download_store PRIVATE kinefin_core Qt6::Test)
    add_test(NAME download_store COMMAND test_download_store)

    add_executable(test_download_transfer tests/test_download_transfer.cpp)
    target_compile_definitions(test_download_transfer PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_download_transfer PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME download_transfer COMMAND test_download_transfer)

    add_executable(test_download_manager tests/test_download_manager.cpp)
    target_compile_definitions(test_download_manager PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_download_manager PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME download_manager COMMAND test_download_manager)

    add_executable(test_downloads_model tests/test_downloads_model.cpp)
    target_link_libraries(test_downloads_model PRIVATE kinefin_core Qt6::Test)
    add_test(NAME downloads_model COMMAND test_downloads_model)

    add_executable(test_downloads_controller tests/test_downloads_controller.cpp)
    target_compile_definitions(test_downloads_controller PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" TRANSLATION_DOMAIN="kinefin")
    target_link_libraries(test_downloads_controller PRIVATE kinefin_ui kinefin_core Qt6::Test Qt6::Gui Qt6::Qml KF6::I18n)
    add_test(NAME downloads_controller COMMAND test_downloads_controller)

    add_executable(test_download_export tests/test_download_export.cpp)
    target_link_libraries(test_download_export PRIVATE kinefin_core Qt6::Test)
    add_test(NAME download_export COMMAND test_download_export)

    add_executable(test_watched_outbox tests/test_watched_outbox.cpp)
    target_link_libraries(test_watched_outbox PRIVATE kinefin_core Qt6::Test)
    add_test(NAME watched_outbox COMMAND test_watched_outbox)

    add_executable(test_network_meter tests/test_network_meter.cpp)
    target_link_libraries(test_network_meter PRIVATE kinefin_core Qt6::Test Qt6::DBus)
    add_test(NAME network_meter COMMAND test_network_meter)

    add_executable(test_genre_rails_model tests/test_genre_rails_model.cpp)
    target_compile_definitions(test_genre_rails_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_genre_rails_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME genre_rails_model COMMAND test_genre_rails_model)

    add_executable(test_user_configuration_model tests/test_user_configuration_model.cpp)
    target_compile_definitions(test_user_configuration_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_user_configuration_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME user_configuration_model COMMAND test_user_configuration_model)

    add_executable(test_search_model tests/test_search_model.cpp)
    target_compile_definitions(test_search_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_search_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME search_model COMMAND test_search_model)

    add_executable(test_item_children_model tests/test_item_children_model.cpp)
    target_compile_definitions(test_item_children_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_item_children_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME item_children_model COMMAND test_item_children_model)

    add_executable(test_item_details_model tests/test_item_details_model.cpp)
    target_compile_definitions(test_item_details_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_item_details_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME item_details_model COMMAND test_item_details_model)

    add_executable(test_image_network tests/test_image_network.cpp)
    target_compile_definitions(test_image_network PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_image_network PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME image_network COMMAND test_image_network)

    add_executable(test_library_items_model tests/test_library_items_model.cpp)
    target_compile_definitions(test_library_items_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_library_items_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME library_items_model COMMAND test_library_items_model)

    add_executable(test_library_subtab_controller tests/test_library_subtab_controller.cpp)
    target_compile_definitions(test_library_subtab_controller PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_library_subtab_controller PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME library_subtab_controller COMMAND test_library_subtab_controller)

    add_executable(test_favorites_model tests/test_favorites_model.cpp)
    target_compile_definitions(test_favorites_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_favorites_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME favorites_model COMMAND test_favorites_model)

    add_executable(test_remote_sessions_model tests/test_remote_sessions_model.cpp)
    target_compile_definitions(test_remote_sessions_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_remote_sessions_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME remote_sessions_model COMMAND test_remote_sessions_model)

    add_executable(test_resume_items_model tests/test_resume_items_model.cpp)
    target_compile_definitions(test_resume_items_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_resume_items_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME resume_items_model COMMAND test_resume_items_model)

    add_executable(test_server_credentials tests/test_server_credentials.cpp)
    target_link_libraries(test_server_credentials PRIVATE kinefin_core Qt6::Test)
    add_test(NAME server_credentials COMMAND test_server_credentials)

    add_executable(test_client_settings tests/test_client_settings.cpp)
    target_link_libraries(test_client_settings PRIVATE kinefin_core Qt6::Test)
    add_test(NAME client_settings COMMAND test_client_settings)

    add_executable(test_mpv_conf tests/test_mpv_conf.cpp)
    target_link_libraries(test_mpv_conf PRIVATE kinefin_player Qt6::Test)
    add_test(NAME mpv_conf COMMAND test_mpv_conf)

    add_executable(test_settings_parity tests/test_settings_parity.cpp)
    target_compile_definitions(test_settings_parity PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_settings_parity PRIVATE kinefin_core Qt6::Test)
    add_test(NAME settings_parity COMMAND test_settings_parity)

    add_executable(test_qml_text_sources tests/test_qml_text_sources.cpp)
    target_compile_definitions(test_qml_text_sources PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_qml_text_sources PRIVATE Qt6::Test)
    add_test(NAME qml_text_sources COMMAND test_qml_text_sources)

    add_executable(test_logging tests/test_logging.cpp)
    target_link_libraries(test_logging PRIVATE kinefin_core Qt6::Test)
    add_test(NAME logging COMMAND test_logging)

    add_executable(test_profile_commands tests/test_profile_commands.cpp)
    target_link_libraries(test_profile_commands PRIVATE kinefin_core Qt6::Test)
    add_test(NAME profile_commands COMMAND test_profile_commands)

    add_executable(test_kwallet_host tests/test_kwallet_host.cpp)
    target_link_libraries(test_kwallet_host PRIVATE kinefin_core Qt6::Test KF6::Wallet)
    add_test(NAME kwallet_host COMMAND test_kwallet_host)

    add_executable(test_server_credential_sync tests/test_server_credential_sync.cpp)
    target_compile_definitions(test_server_credential_sync PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_server_credential_sync PRIVATE kinefin_core Qt6::Test)
    add_test(NAME server_credential_sync COMMAND test_server_credential_sync)

    add_test(NAME shell_smoke
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/shell_smoke.py $<TARGET_FILE:kinefin>)

    add_test(NAME cli_profiles
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_profiles.py $<TARGET_FILE:kinefin>)

    add_test(NAME fixture_contracts
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_contracts.py)
endif()
