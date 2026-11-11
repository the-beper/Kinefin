# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(kinefin VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(ECM 6.8.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs6)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2 DBus Network WebSockets)
find_package(KF6 6.8 REQUIRED COMPONENTS Config CoreAddons I18n DBusAddons Wallet)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
qt_standard_project_setup(REQUIRES 6.6)
set(KINEFIN_MPVQT_URL "https://download.kde.org/stable/mpvqt/mpvqt-1.2.1.tar.xz"
    CACHE STRING "URL of the pinned mpvqt 1.2.1 tarball (see the Flatpak manifest)")
include(FetchContent)
FetchContent_Declare(mpvqt
    URL "${KINEFIN_MPVQT_URL}"
    URL_HASH SHA256=6ff5f56a0edd8248073446e5cc038f47d4b17c46b6af43cf438123d0ac911189
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
    src/models/CultureOptionsModel.cpp
    src/models/CultureOptionsModel.h
    src/models/FilterOptionsModel.cpp
    src/models/FilterOptionsModel.h
    src/models/FavoritesModel.cpp
    src/models/FavoritesModel.h
    src/models/FavoritesGroupsModel.cpp
    src/models/FavoritesGroupsModel.h
    src/models/UserConfigurationModel.cpp
    src/models/UserConfigurationModel.h
    src/models/GenreRailsModel.cpp
    src/models/GenreRailsModel.h
    src/models/PlayQueue.cpp
    src/models/PlayQueue.h
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
    src/integration/UpdateChecker.cpp
    src/integration/UpdateChecker.h
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
target_link_libraries(kinefin_core PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets KF6::Wallet KF6::ConfigCore KF6::I18n)

add_library(kinefin_player STATIC
    src/player/PlaybackEngine.cpp
    src/player/PlaybackEngine.h
)
target_include_directories(kinefin_player PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kinefin_player PUBLIC kinefin_core MpvQt::MpvQt)
if(mpvqt_BINARY_DIR)
    target_include_directories(kinefin_player PRIVATE ${mpvqt_BINARY_DIR}/src)
endif()

# The QML module lives in a static library of its own so that the UI tests can import it like the application does
foreach(page Main ShellHeader ShortcutsDialog RemotePage HomePage LibraryTile LibraryPage ResumeItemsPage MediaCard CardMenu CardRow EpisodeRow ItemDetailsPage SearchPage QueuePage FilterDialog GenreRailsView GenreRailsPage LibraryContent ItemListRow ItemRowGrid LoginPage UserPickerPage MessageBox ServersPage SettingsPage SettingsSectionPage SettingRow UserPreferencesPage FavoritesPage FavoritesGroupPage AboutPage)
    set_source_files_properties(src/qml/${page}.qml PROPERTIES QT_RESOURCE_ALIAS ${page}.qml)
endforeach()
qt_add_library(kinefin_ui STATIC)
qt_add_qml_module(kinefin_ui
    URI org.kde.kinefin
    VERSION 1.0
    QML_FILES
        src/qml/Main.qml
        src/qml/ShellHeader.qml
        src/qml/ShortcutsDialog.qml
        src/qml/RemotePage.qml
        src/qml/HomePage.qml
        src/qml/LibraryTile.qml
        src/qml/LibraryPage.qml
        src/qml/ResumeItemsPage.qml
        src/qml/MediaCard.qml
        src/qml/CardMenu.qml
        src/qml/CardRow.qml
        src/qml/EpisodeRow.qml
        src/qml/ItemDetailsPage.qml
        src/qml/SearchPage.qml
        src/qml/QueuePage.qml
        src/qml/FilterDialog.qml
        src/qml/GenreRailsView.qml
        src/qml/GenreRailsPage.qml
        src/qml/LibraryContent.qml
        src/qml/ItemListRow.qml
        src/qml/ItemRowGrid.qml
        src/qml/LoginPage.qml
        src/qml/UserPickerPage.qml
        src/qml/MessageBox.qml
        src/qml/ServersPage.qml
        src/qml/SettingsPage.qml
        src/qml/SettingsSectionPage.qml
        src/qml/SettingRow.qml
        src/qml/UserPreferencesPage.qml
        src/qml/FavoritesPage.qml
        src/qml/FavoritesGroupPage.qml
        src/qml/AboutPage.qml
    SOURCES
        src/app/ClientSettingsQml.h
        src/app/UpdateCheckerQml.h
        src/app/ConnectionController.cpp
        src/app/ConnectionController.h
        src/app/ImageNetworkFactory.cpp
        src/app/ImageNetworkFactory.h
        src/app/ModelsQml.h
        src/app/MouseNavigation.cpp
        src/app/MouseNavigation.h
)
target_include_directories(kinefin_ui PUBLIC src/app)
target_compile_definitions(kinefin_ui PRIVATE TRANSLATION_DOMAIN="kinefin")
target_link_libraries(kinefin_ui PUBLIC
    kinefin_core
    Qt6::Gui Qt6::Quick Qt6::Qml Qt6::QuickControls2
    KF6::I18n KF6::I18nQml
)

add_executable(kinefin src/main.cpp)
target_link_libraries(kinefin PRIVATE
    kinefin_ui kinefin_uiplugin kinefin_core
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::QuickControls2 Qt6::DBus
    KF6::CoreAddons KF6::I18n KF6::I18nQml KF6::DBusAddons
)
target_include_directories(kinefin PRIVATE src/app)
target_compile_definitions(kinefin PRIVATE KINEFIN_VERSION="${PROJECT_VERSION}" TRANSLATION_DOMAIN="kinefin")
ki18n_install(po)

install(TARGETS kinefin ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(FILES data/io.github.the_beper.Kinefin.desktop DESTINATION ${KDE_INSTALL_APPDIR})
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

    add_executable(test_playback_engine tests/test_playback_engine.cpp)
    target_compile_definitions(test_playback_engine PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_playback_engine PRIVATE kinefin_player Qt6::Test)
    add_test(NAME playback_engine COMMAND test_playback_engine)

    add_executable(test_jellyfin_socket tests/test_jellyfin_socket.cpp)
    target_compile_definitions(test_jellyfin_socket PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_jellyfin_socket PRIVATE kinefin_core Qt6::Test Qt6::Network Qt6::WebSockets)
    add_test(NAME jellyfin_socket COMMAND test_jellyfin_socket)

    add_executable(test_jellyfin_live tests/test_jellyfin_live.cpp)
    target_link_libraries(test_jellyfin_live PRIVATE kinefin_player kinefin_core Qt6::Test Qt6::Network Qt6::WebSockets)
    add_test(NAME jellyfin_live COMMAND test_jellyfin_live)

    add_executable(test_update_checker tests/test_update_checker.cpp)
    target_link_libraries(test_update_checker PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME update_checker COMMAND test_update_checker)

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

    add_executable(test_home_rows_model tests/test_home_rows_model.cpp)
    target_compile_definitions(test_home_rows_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_home_rows_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME home_rows_model COMMAND test_home_rows_model)

    add_executable(test_ui_pages tests/test_ui_pages.cpp)
    target_compile_definitions(test_ui_pages PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" TRANSLATION_DOMAIN="kinefin")
    target_link_libraries(test_ui_pages PRIVATE kinefin_ui kinefin_uiplugin kinefin_core Qt6::Test Qt6::Gui Qt6::Quick Qt6::Qml KF6::I18n KF6::I18nQml)
    add_test(NAME ui_pages COMMAND test_ui_pages)
    set_tests_properties(ui_pages PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_filter_options_model tests/test_filter_options_model.cpp)
    target_compile_definitions(test_filter_options_model PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_filter_options_model PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME filter_options_model COMMAND test_filter_options_model)

    add_executable(test_play_queue tests/test_play_queue.cpp)
    target_compile_definitions(test_play_queue PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_play_queue PRIVATE kinefin_core Qt6::Test Qt6::Network)
    add_test(NAME play_queue COMMAND test_play_queue)

    add_executable(test_browse_state_store tests/test_browse_state_store.cpp)
    target_link_libraries(test_browse_state_store PRIVATE kinefin_core Qt6::Test)
    add_test(NAME browse_state_store COMMAND test_browse_state_store)

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

    add_executable(test_settings_parity tests/test_settings_parity.cpp)
    target_compile_definitions(test_settings_parity PRIVATE KINEFIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_settings_parity PRIVATE kinefin_core Qt6::Test)
    add_test(NAME settings_parity COMMAND test_settings_parity)

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
