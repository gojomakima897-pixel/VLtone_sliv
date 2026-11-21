# daw — the Qt Widgets front-end.
# Network serves the AI assistant; WebEngineWidgets provides the isolated
# Chromium-based browser and its explicit download API.
set(DAW_QT_VERSION "6.8.3" CACHE STRING "Supported Qt release line")
if(WIN32)
    set(DAW_EXACT_QT_DEFAULT ON)
else()
    set(DAW_EXACT_QT_DEFAULT OFF)
endif()
option(DAW_REQUIRE_EXACT_QT_VERSION
       "Reject Qt versions other than DAW_QT_VERSION" ${DAW_EXACT_QT_DEFAULT})
if(DAW_REQUIRE_EXACT_QT_VERSION)
    find_package(Qt6 ${DAW_QT_VERSION} EXACT REQUIRED COMPONENTS
        Widgets Network WebEngineWidgets SerialPort)
else()
find_package(Qt6 ${DAW_QT_VERSION} REQUIRED COMPONENTS
        Widgets Network WebEngineWidgets SerialPort)
endif()
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
qt_standard_project_setup()   # enables AUTOMOC / AUTOUIC / AUTORCC

# Headers are listed too so AUTOMOC always sees the Q_OBJECT classes that live
# in them, even for files added after the last configure.
file(GLOB DAW_APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

qt_add_executable(daw ${DAW_APP_SOURCES})
daw_add_windows_metadata(daw "VLT Studio Pro.exe" "VLTStudioPro"
                         "VLT Studio Pro digital audio workstation")

target_compile_definitions(daw PRIVATE
    VLT_DEFAULT_API_ORIGIN=\"${VLT_DEFAULT_API_ORIGIN}\")

target_include_directories(daw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(daw PRIVATE daw_controller Qt6::Widgets Qt6::Network
                                  Qt6::WebEngineWidgets Qt6::SerialPort
                                  OpenSSL::Crypto)
if(APPLE)
    target_link_libraries(daw PRIVATE "-framework Security")
elseif(WIN32)
    target_link_libraries(daw PRIVATE Advapi32 Psapi)
endif()

add_subdirectory("${CMAKE_SOURCE_DIR}/reporter"
                 "${CMAKE_BINARY_DIR}/reporter")

if(WIN32)
    set_target_properties(daw PROPERTIES OUTPUT_NAME "VLT Studio Pro")
endif()
# Dev builds stay a plain console executable (keeps --selftest/--screenshot and
# stderr logging simple). Packaging builds become a proper GUI app: a .app
# bundle on macOS and a windowed (no console) exe on Windows.
if(DAW_PACKAGE)
    set_target_properties(daw PROPERTIES
        MACOSX_BUNDLE ON
        WIN32_EXECUTABLE ON)

    if(APPLE)
        # The icon travels as a bundle resource; CMake copies anything with this
        # source property into Contents/Resources.
        set(DAW_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/daw.icns")
        target_sources(daw PRIVATE ${DAW_ICNS})
        set_source_files_properties(${DAW_ICNS} PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)

        # A shipped app is "VLT Studio Pro", not "daw": the bundle's name is what the
        # Finder, the Dock and the menu bar all read.
        set(DAW_BUNDLE_NAME "VLT Studio Pro")
        set_target_properties(daw PROPERTIES
            OUTPUT_NAME "${DAW_BUNDLE_NAME}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${DAW_BUNDLE_NAME}"
            MACOSX_BUNDLE_EXECUTABLE_NAME "${DAW_BUNDLE_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.vltstudio.pro"
            MACOSX_BUNDLE_ICON_FILE "daw.icns"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    endif()
endif()

install(TARGETS daw
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin)

# Only when packaging: bundle the Qt runtime next to the app at install time.
# This expands to macdeployqt on macOS and windeployqt on Windows.
if(DAW_PACKAGE)
    if(APPLE)
        # The scanner and the watchdog are found *next to the executable*
        # (`applicationDirPath` / `executableDirectory`), so in a bundle they
        # belong in Contents/MacOS. Installed from here rather than from their
        # own directories so they land after the bundle has been laid down and
        # before macdeployqt walks it.
        install(TARGETS daw_scan daw_guard daw_reporter
                RUNTIME DESTINATION "${DAW_BUNDLE_NAME}.app/Contents/MacOS")
    endif()
    qt_generate_deploy_app_script(
        TARGET daw
        OUTPUT_SCRIPT DAW_DEPLOY_SCRIPT
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${DAW_DEPLOY_SCRIPT})
endif()
