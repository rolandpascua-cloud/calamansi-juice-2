# Tray application subproject
# Note: This is included from parent CMakeLists.txt via add_subdirectory(tray)
# Parent project already sets C++ standard and fetches dependencies
#
# Targets (renamed from lemonade-sdk/lemonade as part of the Calamansi Juice 2
# rebrand; old names survive as build/install-time aliases, see
# src/cpp/include/lemon/deprecated_alias.h and MERGING.md):
#   Windows: CalamansiServer.exe (formerly LemonadeServer.exe; SUBSYSTEM:WINDOWS, embeds server via lemonade-server-core)
#   macOS:   calamansi-tray (formerly lemonade-tray; lightweight tray client, connects to running router)
#   Linux:   calamansi-tray (formerly lemonade-tray; lightweight tray client, when AppIndicator3 found)

# ============================================================
# Platform-specific tray sources and dependencies
# ============================================================

set(TRAY_PLATFORM_SOURCES "")
set(TRAY_PLATFORM_LIBS "")
set(TRAY_INCLUDE_DIRS "")
set(TRAY_DEFINITIONS "")

if(WIN32)
    set(TRAY_PLATFORM_SOURCES
        platform/windows_tray.cpp
    )
    set(TRAY_PLATFORM_LIBS
        user32
        shell32
        ole32
        oleaut32
        comctl32
    )

elseif(APPLE)
    set(TRAY_PLATFORM_SOURCES
        platform/macos_tray.mm
    )

    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    find_library(USERNOTIFICATIONS_LIBRARY UserNotifications REQUIRED)
    find_library(METAL_LIBRARY Metal REQUIRED)

    set(TRAY_PLATFORM_LIBS
        ${COCOA_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${USERNOTIFICATIONS_LIBRARY}
        ${METAL_LIBRARY}
    )

    set_source_files_properties(
        platform/macos_tray.mm
        PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
    )

elseif(UNIX)  # Linux
    option(REQUIRE_LINUX_TRAY
        "Enable Linux system tray support (requires AppIndicator3; GTK3 not needed when using the glib variant)"
        OFF)

    if(REQUIRE_LINUX_TRAY)
        find_package(PkgConfig REQUIRED)

        pkg_check_modules(GTK3 gtk+-3.0)

        pkg_check_modules(APP_INDICATOR_GLIB ayatana-appindicator-glib-0.1)
        if(NOT APP_INDICATOR_GLIB_FOUND)
            pkg_check_modules(APP_INDICATOR_GLIB ayatana-appindicator-glib)
        endif()
        if(NOT APP_INDICATOR_GLIB_FOUND)
            pkg_check_modules(APP_INDICATOR3 ayatana-appindicator3-0.1)
            if(APP_INDICATOR3_FOUND)
                set(HAVE_AYATANA_APPINDICATOR TRUE)
            else()
                pkg_check_modules(APP_INDICATOR3 appindicator3-0.1)
            endif()
        endif()

        pkg_check_modules(LIBNOTIFY libnotify)

        if(NOT (APP_INDICATOR_GLIB_FOUND OR (GTK3_FOUND AND APP_INDICATOR3_FOUND)))
            message(FATAL_ERROR "REQUIRE_LINUX_TRAY=ON but required dependencies not found. "
                "Install one of: ayatana-appindicator-glib-devel (GTK-free), "
                "or gtk3-devel + ayatana-appindicator3-devel / libappindicator-gtk3-devel.")
        endif()

        set(_LINUX_TRAY_BUILD ON)

        if(APP_INDICATOR_GLIB_FOUND)
            set(TRAY_PLATFORM_LIBS ${APP_INDICATOR_GLIB_LIBRARIES})
            set(TRAY_INCLUDE_DIRS ${APP_INDICATOR_GLIB_INCLUDE_DIRS})
            set(TRAY_DEFINITIONS HAVE_APPINDICATOR HAVE_AYATANA_APPINDICATOR_GLIB)
            pkg_check_modules(DBUSMENU_GLIB dbusmenu-glib-0.4)
            if(DBUSMENU_GLIB_FOUND)
                list(APPEND TRAY_PLATFORM_LIBS ${DBUSMENU_GLIB_LIBRARIES})
                list(APPEND TRAY_INCLUDE_DIRS ${DBUSMENU_GLIB_INCLUDE_DIRS})
                list(APPEND TRAY_DEFINITIONS HAVE_DBUSMENU_GLIB)
            else()
                message(WARNING "dbusmenu-glib-0.4 not found — tray menus will not "
                    "work on GNOME Shell (install libdbusmenu-glib-dev or equivalent)")
            endif()
        else()
            set(TRAY_PLATFORM_LIBS
                ${GTK3_LIBRARIES}
                ${APP_INDICATOR3_LIBRARIES}
            )
            set(TRAY_INCLUDE_DIRS
                ${GTK3_INCLUDE_DIRS}
                ${APP_INDICATOR3_INCLUDE_DIRS}
            )
            set(TRAY_DEFINITIONS HAVE_APPINDICATOR)
            if(HAVE_AYATANA_APPINDICATOR)
                list(APPEND TRAY_DEFINITIONS HAVE_AYATANA_APPINDICATOR)
            endif()
        endif()

        if(LIBNOTIFY_FOUND)
            list(APPEND TRAY_PLATFORM_LIBS ${LIBNOTIFY_LIBRARIES})
            list(APPEND TRAY_DEFINITIONS HAVE_LIBNOTIFY)
            list(APPEND TRAY_INCLUDE_DIRS ${LIBNOTIFY_INCLUDE_DIRS})
        endif()

        set(TRAY_PLATFORM_SOURCES platform/linux_tray.cpp)
    endif()
endif()

# ============================================================
# Common tray UI sources (shared across all platform targets)
# ============================================================
set(TRAY_UI_SOURCES
    tray_ui.cpp
    platform/tray_factory.cpp
)

# path_utils and json_utils are needed by TrayUI but on Windows they're already in lemonade-server-core
if(NOT WIN32)
    list(APPEND TRAY_UI_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/path_utils.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/json_utils.cpp
    )
    # Add platform-specific path implementation
    if(APPLE)
        list(APPEND TRAY_UI_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/platform/path_macos.cpp
        )
    else()
        list(APPEND TRAY_UI_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/platform/path_linux.cpp
        )
    endif()
endif()

# Helper function: apply lightweight tray deps (httplib, json, CLI11) to a target
function(apply_tray_deps target_name)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )

    target_link_libraries(${target_name} PRIVATE nlohmann_json::nlohmann_json)

    if(USE_SYSTEM_HTTPLIB)
        target_link_libraries(${target_name} PRIVATE lemonade-httplib)
    else()
        target_link_libraries(${target_name} PRIVATE httplib::httplib)
    endif()

    if(USE_SYSTEM_CLI11)
        target_include_directories(${target_name} PRIVATE ${CLI11_INCLUDE_DIRS})
    else()
        target_link_libraries(${target_name} PRIVATE CLI11::CLI11)
    endif()
endfunction()

# ============================================================
# Windows: CalamansiServer.exe (SUBSYSTEM:WINDOWS, embedded server)
# ============================================================
if(WIN32)
    add_executable(CalamansiServer WIN32
        main.cpp
        ${TRAY_UI_SOURCES}
        ${TRAY_PLATFORM_SOURCES}
    )

    # Add version resource
    target_sources(CalamansiServer PRIVATE version.rc)

    # Embedded server: link the full server core OBJECT library
    target_link_libraries(CalamansiServer PRIVATE lemonade-server-core)

    # Tray platform libraries
    target_link_libraries(CalamansiServer PRIVATE ${TRAY_PLATFORM_LIBS})

    # Lightweight tray deps (httplib, json, CLI11)
    apply_tray_deps(CalamansiServer)

    # Unicode support
    target_compile_definitions(CalamansiServer PRIVATE UNICODE _UNICODE)

    set_target_properties(CalamansiServer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )

    # Copy resources to CalamansiServer.exe output directory
    add_custom_command(TARGET CalamansiServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/resources
            $<TARGET_FILE_DIR:CalamansiServer>/resources
        COMMENT "Copying resources to CalamansiServer output directory"
    )
    add_dependencies(CalamansiServer copy_resources)

    # Backward-compat alias: also produce a same-directory LemonadeServer.exe
    # copy next to the real CalamansiServer.exe (Windows symlinks can require
    # elevated privileges, so use a file copy here rather than a symlink; see
    # src/cpp/include/lemon/deprecated_alias.h for the runtime warning).
    add_custom_command(TARGET CalamansiServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:CalamansiServer>"
            "$<TARGET_FILE_DIR:CalamansiServer>/LemonadeServer.exe"
        COMMENT "Creating deprecated alias LemonadeServer.exe -> CalamansiServer.exe"
    )
endif()

# ============================================================
# macOS: calamansi-tray (lightweight tray client, formerly lemonade-tray)
# ============================================================
if(APPLE)
    add_executable(calamansi-tray
        main.cpp
        ${TRAY_UI_SOURCES}
        ${TRAY_PLATFORM_SOURCES}
    )

    apply_tray_deps(calamansi-tray)
    target_link_libraries(calamansi-tray PRIVATE ${TRAY_PLATFORM_LIBS})

    # Enable ARC for Objective-C++ files
    target_compile_options(calamansi-tray PRIVATE -fobjc-arc)

    set_target_properties(calamansi-tray PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    # Backward-compat alias: also produce a same-directory "lemonade-tray"
    # symlink next to the real calamansi-tray binary.
    add_custom_command(TARGET calamansi-tray POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE_NAME:calamansi-tray>" "lemonade-tray"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:calamansi-tray>"
        COMMENT "Creating deprecated alias lemonade-tray -> calamansi-tray"
    )
endif()

# ============================================================
# Linux: calamansi-tray (lightweight tray client, formerly lemonade-tray, when AppIndicator3 found)
# ============================================================
if(UNIX AND NOT APPLE AND _LINUX_TRAY_BUILD)
    add_executable(calamansi-tray
        main.cpp
        ${TRAY_UI_SOURCES}
        ${TRAY_PLATFORM_SOURCES}
    )

    apply_tray_deps(calamansi-tray)

    find_package(Threads REQUIRED)
    target_link_libraries(calamansi-tray PRIVATE
        Threads::Threads
        ${TRAY_PLATFORM_LIBS}
    )

    target_include_directories(calamansi-tray PRIVATE ${TRAY_INCLUDE_DIRS})
    target_compile_definitions(calamansi-tray PRIVATE ${TRAY_DEFINITIONS})

    set_target_properties(calamansi-tray PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    # Backward-compat alias: also produce a same-directory "lemonade-tray"
    # symlink next to the real calamansi-tray binary.
    add_custom_command(TARGET calamansi-tray POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE_NAME:calamansi-tray>" "lemonade-tray"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:calamansi-tray>"
        COMMENT "Creating deprecated alias lemonade-tray -> calamansi-tray"
    )

    install(TARGETS calamansi-tray RUNTIME DESTINATION bin)

    # No autostart desktop entry — Linux users start calamansi-tray manually

    # Symlink in /usr/bin if not installing to /usr
    if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
        install(CODE "
            file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
            file(RELATIVE_PATH _symlink_target \"/usr/bin\" \"${CMAKE_INSTALL_PREFIX}/bin/calamansi-tray\")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E create_symlink
                    \"\${_symlink_target}\"
                    \"\$ENV{DESTDIR}/usr/bin/calamansi-tray\"
            )
        ")
    endif()
endif()

# ============================================================
# macOS Signing (Release builds only)
# ============================================================
if(APPLE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Configuring signing for Tray Application (calamansi-tray)")

    if(NOT DEFINED SIGNING_IDENTITY OR NOT DEFINED ENTITLEMENTS_PATH)
        message(FATAL_ERROR "Signing variables missing! Ensure add_subdirectory(tray) is called AFTER identities are set in root CMakeLists.txt")
    endif()

    add_custom_command(TARGET calamansi-tray POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "--- Signing calamansi-tray ---"
        COMMAND codesign --force --options runtime --timestamp --entitlements "${ENTITLEMENTS_PATH}" --sign "${SIGNING_IDENTITY}" -v $<TARGET_FILE:calamansi-tray>
        COMMENT "Signing calamansi-tray with Hardened Runtime"
        VERBATIM
    )
endif()

# ============================================================
# Configuration summary
# ============================================================
message(STATUS "=== Calamansi Tray Configuration ===")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
if(WIN32)
    message(STATUS "CalamansiServer.exe (embedded server + tray): YES")
elseif(APPLE)
    message(STATUS "calamansi-tray (client): YES")
elseif(UNIX)
    if(_LINUX_TRAY_BUILD)
        if(APP_INDICATOR_GLIB_FOUND)
            message(STATUS "calamansi-tray: ENABLED (ayatana-appindicator-glib, GTK-free)")
        else()
            message(STATUS "calamansi-tray: ENABLED (GTK3 + AppIndicator3)")
        endif()
        if(REQUIRE_LINUX_TRAY AND LIBNOTIFY_FOUND)
            message(STATUS "  Notifications: ENABLED (libnotify)")
        endif()
    else()
        if(REQUIRE_LINUX_TRAY)
            message(STATUS "calamansi-tray: DISABLED (REQUIRE_LINUX_TRAY=ON but deps not found)")
        else()
            message(STATUS "calamansi-tray: DISABLED (REQUIRE_LINUX_TRAY=OFF)")
        endif()
    endif()
endif()
message(STATUS "====================================")
