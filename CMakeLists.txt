cmake_minimum_required(VERSION 3.21)

project(Trophy
    VERSION 0.0.0
    DESCRIPTION "Game recording application for Linux"
    HOMEPAGE_URL "https://github.com/stevenjwheeler/trophy"
    LANGUAGES CXX C
)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt configuration
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Get version from Git tag, unless CI (or the user) passes -DAPP_VERSION
# explicitly — that wins so the stamped binary always matches the
# artifact name.
if(DEFINED APP_VERSION AND NOT APP_VERSION STREQUAL "")
  set(GIT_VERSION ${APP_VERSION})
else()
  # Local builds: mirror the CI scheme (<base>-<branch>-<sha>,
  # base = nearest tag) so a dev-branch binary never shows a
  # raw "vX.Y.Z-N-gSHA" describe string. Exact tag checkouts
  # keep the tag itself.
  execute_process(
      COMMAND git describe --tags --exact-match --dirty=-dirty
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      OUTPUT_VARIABLE GIT_EXACT_TAG
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
  )
  if(GIT_EXACT_TAG)
    set(GIT_VERSION ${GIT_EXACT_TAG})
  else()
    execute_process(
        COMMAND git describe --tags --abbrev=0
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BASE_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND git rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GIT_BASE_TAG AND GIT_BRANCH AND GIT_SHA AND NOT GIT_BRANCH STREQUAL "HEAD")
      set(GIT_VERSION "${GIT_BASE_TAG}-${GIT_BRANCH}-${GIT_SHA}")
    else()
      # Last resort (no tags at all): stamp the sha, dirty-flagged.
      # NOTE: version comes from configure-time git queries, so rebuilds
      # after a checkout keep the old APP_VERSION until you reconfigure.
      # Local dev rule: re-run cmake configure after switching branches.
      execute_process(
          COMMAND git describe --tags --always --dirty=-dirty
          WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
          OUTPUT_VARIABLE GIT_VERSION
          OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET
      )
    endif()
  endif()
endif()

# Fallback if git fails or no tags exist
if(NOT GIT_VERSION)
    set(GIT_VERSION ${PROJECT_VERSION})
endif()

add_compile_definitions(APP_VERSION=\"${GIT_VERSION}\")

# Options
option(BUILD_TESTING "Build tests" ON)
option(ENABLE_NVENC "Enable NVIDIA NVENC support" ON)
option(ENABLE_VAAPI "Enable VA-API support" ON)

# Find Qt6
find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    QuickControls2
    Multimedia
    DBus
    Concurrent
    Network
    Widgets
)

# Find Vulkan (for the detection layer)
find_package(Vulkan REQUIRED)

qt_standard_project_setup()

# Qt policies
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

# Find FFmpeg using pkg-config
find_package(PkgConfig REQUIRED)

pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

# Find PipeWire (Required)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
if(PIPEWIRE_FOUND)
    message(STATUS "PipeWire found: ${PIPEWIRE_VERSION}")
    add_compile_definitions(HAVE_PIPEWIRE)
endif()

# Find libdrm (for DMA-BUF zero-copy capture and DRM format constants)
pkg_check_modules(LIBDRM IMPORTED_TARGET libdrm)
if(LIBDRM_FOUND)
    message(STATUS "libdrm found: ${LIBDRM_VERSION}")
    add_compile_definitions(HAVE_LIBDRM)
endif()

# Find EGL (for DMA-BUF modifier queries offered to the compositor)
pkg_check_modules(EGL IMPORTED_TARGET egl)
if(EGL_FOUND)
    message(STATUS "EGL found: ${EGL_VERSION}")
    add_compile_definitions(HAVE_EGL)
endif()

# Find GBM (for DMA-BUF test buffers; optional, tests only)
pkg_check_modules(GBM IMPORTED_TARGET gbm)
if(GBM_FOUND)
    message(STATUS "GBM found: ${GBM_VERSION}")
    add_compile_definitions(HAVE_GBM)
endif()

# Find VA-API (optional)
if(ENABLE_VAAPI)
    pkg_check_modules(VAAPI IMPORTED_TARGET libva libva-drm)
    if(VAAPI_FOUND)
        message(STATUS "VA-API found: ${VAAPI_VERSION}")
        add_compile_definitions(HAVE_VAAPI)
    else()
        message(STATUS "VA-API not found")
    endif()
endif()

# PipeWire capture is found a few lines above (Wayland/PipeWire capture).

# Check for NVENC (just check if headers exist)
if(ENABLE_NVENC)
    # NVENC is accessed through FFmpeg, check if FFmpeg was built with NVENC
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES ${AVCODEC_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES PkgConfig::AVCODEC)
    check_c_source_compiles("
        #include <libavcodec/avcodec.h>
        int main() {
            const AVCodec *codec = avcodec_find_encoder_by_name(\"h264_nvenc\");
            return codec ? 0 : 1;
        }
    " HAVE_NVENC_IN_FFMPEG)

    if(HAVE_NVENC_IN_FFMPEG)
        message(STATUS "NVENC support available via FFmpeg")
        add_compile_definitions(HAVE_NVENC)
    else()
        message(STATUS "NVENC not available in FFmpeg build")
    endif()
endif()

# KWin monitor script: copy its contents to the build tree for dev runs
# (KWinBackend looks in <appDir>/kwin-scripts/trophy-monitor); install
# the same tree for packages.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/resources/kwin/"
     DESTINATION "${CMAKE_BINARY_DIR}/kwin-scripts")

# Detection shims (Vulkan layer + OpenGL LD_PRELOAD shim).
# Both report to /tmp/trophy-detection.sock with the same START/STOP protocol.
# NOTE: IEncoder has Q_OBJECT, so IEncoder.h (not a .cpp) is listed in test
# target sources wherever FFmpegEncoder.cpp is compiled — AUTOMOC generates
# the metaobject from the header. Same for the main Trophy target via
# ENCODING_HEADERS.
add_library(trophy_layer SHARED src/vulkan_layer/trophy_layer.cpp)
target_link_libraries(trophy_layer PRIVATE Vulkan::Vulkan)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(trophy_layer PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )
endif()
set_target_properties(trophy_layer PROPERTIES
    OUTPUT_NAME "trophy_layer"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vulkan_layer"
)

add_library(trophy_gl_shim SHARED src/gl_shim/trophy_gl_shim.c)
target_link_libraries(trophy_gl_shim PRIVATE dl)
set_target_properties(trophy_gl_shim PROPERTIES
    OUTPUT_NAME "trophy_gl_shim"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gl_shim"
    C_STANDARD 11
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(trophy_gl_shim PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Generate the implicit-layer JSON next to the built Vulkan layer so
# Application::ensureVulkanLayerRegistered() finds it in dev builds.
# NOTE: generator expressions don't expand in configure_file, so use the
# predictable output path (LIBRARY_OUTPUT_DIRECTORY + OUTPUT_NAME above).
set(LIBRARY_PATH "${CMAKE_BINARY_DIR}/vulkan_layer/libtrophy_layer.so")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_layer/trophy_layer.json.in"
    "${CMAKE_BINARY_DIR}/vulkan_layer/trophy_layer.json"
    @ONLY
)

# Source files
set(CORE_SOURCES
    src/core/Application.cpp
    src/core/BugReport.cpp
    src/core/HardwareInfo.cpp
    src/core/RecordingManager.cpp
    src/core/RecordingSession.cpp
    src/core/StorageManager.cpp
    src/core/NotificationService.cpp
    src/core/SystemTrayManager.cpp
    src/core/AutostartManager.cpp
    src/core/ControlServer.cpp
    src/core/GlobalShortcutManager.cpp
    src/core/ReplayManager.cpp
    src/core/UpdateChecker.cpp
    src/core/WindowService.cpp
    src/core/FrameDispatcher.cpp
    src/core/CircularFrameBuffer.cpp
    src/core/FrameTypes.cpp
)

set(CAPTURE_SOURCES
    src/capture/CaptureFactory.cpp
)


if(PIPEWIRE_FOUND)
    list(APPEND CAPTURE_SOURCES src/capture/WaylandPortalCapture.cpp)
endif()

set(AUDIO_SOURCES
    src/audio/AudioMixer.cpp
    src/audio/AudioDeviceEnumerator.cpp
    src/audio/PipeWireAudioSource.cpp
    src/audio/AudioDriftCorrector.cpp
)

set(ENCODING_SOURCES
    src/encoding/EncoderFactory.cpp
    src/encoding/FFmpegEncoder.cpp
    src/encoding/AudioEncoder.cpp
)

set(MUXING_SOURCES
    src/muxing/FFmpegMuxer.cpp
    src/muxing/ReplayClipper.cpp
)

set(DETECTION_SOURCES
    src/detection/GameIconProvider.cpp
    src/detection/GameDetectionService.cpp
    src/detection/GameWhitelist.cpp
)


list(APPEND DETECTION_SOURCES src/detection/WaylandWindowMonitor.cpp src/detection/KWinBackend.cpp src/detection/GlShimEnrollment.cpp src/detection/GnomeBackend.cpp src/detection/HyprlandBackend.cpp src/detection/SwayBackend.cpp src/detection/WindowInfo.cpp src/detection/VulkanLayer.cpp)

set(LIBRARY_SOURCES
    src/library/RecordingLibrary.cpp
    src/library/Recording.cpp
    src/library/ThumbnailGenerator.cpp
    src/library/ActivityFeed.cpp
    src/library/WaveformCache.cpp
    src/library/FilmstripCache.cpp
)

set(SETTINGS_SOURCES
    src/settings/SettingsManager.cpp
)

# Header files (for MOC)
set(CORE_HEADERS
    src/core/Application.h
    src/core/HardwareInfo.h
    src/core/RecordingManager.h
    src/core/RecordingSession.h
    src/core/StorageManager.h
    src/core/NotificationService.h
    src/core/SystemTrayManager.h
    src/core/AutostartManager.h
    src/core/ControlServer.h
    src/core/GlobalShortcutManager.h
    src/core/ReplayManager.h
    src/core/UpdateChecker.h
    src/core/WindowService.h
)

set(CAPTURE_HEADERS
    src/capture/IVideoCaptureSource.h
    src/capture/CaptureFactory.h
    src/capture/EglDmaBuf.h
)


if(PIPEWIRE_FOUND)
    list(APPEND CAPTURE_HEADERS src/capture/WaylandPortalCapture.h)
endif()

set(AUDIO_HEADERS
    src/audio/IAudioSource.h
    src/audio/AudioMixer.h
    src/audio/AudioDeviceEnumerator.h
    src/audio/PipeWireAudioSource.h
    src/audio/AudioDriftCorrector.h
)

set(ENCODING_HEADERS
    src/encoding/IEncoder.h
    src/encoding/EncoderFactory.h
    src/encoding/FFmpegEncoder.h
    src/encoding/AudioEncoder.h
)

set(MUXING_HEADERS
    src/muxing/IMuxer.h
    src/muxing/FFmpegMuxer.h
    src/muxing/TrophyMetadata.h
)

set(DETECTION_HEADERS
    src/detection/DefaultAppCatalog.h
    src/detection/GameIconProvider.h
    src/detection/GameIconSource.h
    src/detection/GameDetectionService.h
    src/detection/GameListModel.h
    src/detection/GameWhitelist.h
)


list(APPEND DETECTION_HEADERS src/detection/WaylandWindowMonitor.h src/detection/KWinBackend.h src/detection/WindowInfo.h src/detection/GlShimEnrollment.h src/detection/GnomeBackend.h src/detection/HyprlandBackend.h src/detection/SwayBackend.h src/detection/VulkanLayer.h)

set(LIBRARY_HEADERS
    src/library/RecordingLibrary.h
    src/library/Recording.h
    src/library/ThumbnailGenerator.h
    src/library/ActivityFeed.h
    src/library/WaveformCache.h
    src/library/FilmstripCache.h
)

set(SETTINGS_HEADERS
    src/settings/SettingsManager.h
)

# Combine all sources
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${CAPTURE_SOURCES}
    ${AUDIO_SOURCES}
    ${ENCODING_SOURCES}
    ${MUXING_SOURCES}
    ${DETECTION_SOURCES}
    ${LIBRARY_SOURCES}
    ${SETTINGS_SOURCES}
)

set(ALL_HEADERS
    ${CORE_HEADERS}
    ${CAPTURE_HEADERS}
    ${AUDIO_HEADERS}
    ${ENCODING_HEADERS}
    ${MUXING_HEADERS}
    ${DETECTION_HEADERS}
    ${LIBRARY_HEADERS}
    ${SETTINGS_HEADERS}
)

# Mark Theme.qml as singleton
set_source_files_properties(qml/styles/Theme.qml
    PROPERTIES
        QT_QML_SINGLETON_TYPE TRUE
)

# Generates .qmlls.ini files so your IDE/Linter can find the build artifacts
set(QT_QML_GENERATE_QMLLS_INI ON)

# Optional: Forces all QML artifacts into a single folder to simplify pathing
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)

# QML module
qt_add_qml_module(${PROJECT_NAME}
    URI TrophyBackend
    VERSION 1.0
    QML_FILES
        qml/main.qml
        qml/styles/Theme.qml
        qml/components/AudioLevelMeter.qml
        qml/components/NavigationRail.qml
        qml/components/WindowPickerDialog.qml
        qml/components/SetupWizard.qml
        qml/pages/DashboardPage.qml
        qml/pages/InfoPage.qml
        qml/pages/PlayerPage.qml
        qml/pages/SettingsPage.qml
        qml/pages/GameListPage.qml
        qml/views/RecordingGridView.qml
        qml/views/RecordingListView.qml
        qml/views/RecordingListItem.qml
        qml/views/RecordingCard.qml
        qml/views/RecordingGameIcon.qml
        qml/views/ActiveRecordingCard.qml
        qml/views/VideoPlayer.qml
        qml/controls/TrophyButton.qml
        qml/controls/AccentColorPicker.qml
        qml/controls/SettingsSectionCard.qml
        qml/controls/TrophyIcon.qml
        qml/controls/TrophyComboBox.qml
        qml/controls/TrophySlider.qml
        qml/controls/TrophyScrollBar.qml
        qml/controls/TrophyRadioButton.qml
        qml/controls/TrophySwitch.qml
        qml/controls/TrophyTextField.qml
        qml/controls/TrophySearchBox.qml
        qml/controls/TrophyDialog.qml
        qml/controls/TrophyWindowButton.qml
    SOURCES
        ${ALL_SOURCES}
        ${ALL_HEADERS}
    RESOURCES
        resources/resources.qrc
)

# Create the executable
qt_add_executable(${PROJECT_NAME}App
    src/main.cpp
)

target_link_libraries(${PROJECT_NAME}App PRIVATE
    ${PROJECT_NAME}
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
)

# Set executable output name
set_target_properties(${PROJECT_NAME}App PROPERTIES
    OUTPUT_NAME "trophy"
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::DBus
    Qt6::Concurrent
    PkgConfig::AVCODEC
    PkgConfig::AVFORMAT
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
    PkgConfig::SWRESAMPLE
    Qt6::Network
)

if(PIPEWIRE_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::PIPEWIRE)
endif()

if(VAAPI_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::VAAPI)
endif()

if(LIBDRM_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::LIBDRM)
endif()

if(EGL_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::EGL)
endif()


# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )
endif()

# Coverage configuration (only when requested)
option(ENABLE_COVERAGE "Enable coverage reporting" OFF)

if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(STATUS "Coverage reporting enabled")
        target_compile_options(${PROJECT_NAME} PUBLIC --coverage -fprofile-arcs -ftest-coverage)
        target_link_libraries(${PROJECT_NAME} PUBLIC --coverage)
        
        # Also apply to the App and Tests
        target_compile_options(${PROJECT_NAME}App PRIVATE --coverage)
        target_link_libraries(${PROJECT_NAME}App PRIVATE --coverage)
    endif()
endif()

# Installation
include(GNUInstallDirs)

# INSTALL_RPATH is a target property: setting CMAKE_INSTALL_RPATH here,
# after the targets were created, would not update their install RUNPATH.
# GNUInstallDirs gives the actual libdir (including multilib prefixes).
set_target_properties(${PROJECT_NAME}App PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")

install(TARGETS ${PROJECT_NAME}App
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# The QML module library holds every C++ class and the whole QML resource
# bundle (qrc:/qt/qml/TrophyBackend/...), and the app links against it. Not
# installing it produced an installed binary that could not start at all:
# "error while loading shared libraries: libTrophy.so". It lands in
# <prefix>/lib, which the app's install-time RUNPATH ($ORIGIN/../lib)
# already covers.
install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES packaging/trophy.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
)

install(FILES resources/icons/trophy.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
)

install(FILES resources/icons/default-apps/README.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/trophy/default-app-icons
)

install(TARGETS trophy_layer trophy_gl_shim
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/trophy
)
install(FILES "${CMAKE_BINARY_DIR}/vulkan_layer/trophy_layer.json"
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/trophy/vulkan_layer
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/kwin/"
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/trophy/kwin
)
# Native/AppImage desktop file is trophy.desktop; Flatpak post-install
# copies the source metadata alongside its renamed app-id desktop entry.
# Keep the launchable association truthful in each installed layout.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/packaging/io.github.stevenjwheeler.Trophy.metainfo.xml"
    TROPHY_NATIVE_METAINFO)
string(REPLACE "io.github.stevenjwheeler.Trophy.desktop" "trophy.desktop"
    TROPHY_NATIVE_METAINFO "${TROPHY_NATIVE_METAINFO}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/io.github.stevenjwheeler.Trophy.metainfo.xml"
    "${TROPHY_NATIVE_METAINFO}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/io.github.stevenjwheeler.Trophy.metainfo.xml"
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
)

# CPack configuration: stamp the tag/branch scheme (sanitized of the
# leading 'v'), not the hardcoded PROJECT_VERSION (0.0.0), so cpack
# artifacts agree with the binary's own version.
string(REGEX REPLACE "^v" "" CPACK_STAMP "${GIT_VERSION}")
set(CPACK_PACKAGE_NAME "trophy")
set(CPACK_PACKAGE_VENDOR "Trophy - Game recording and clipping")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${CPACK_STAMP}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

include(CPack)

# Testing
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# Print configuration summary
message(STATUS "")
message(STATUS "=== Trophy - Game recording and clipping Configuration ===")
message(STATUS "Version: ${GIT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "")
message(STATUS "Features:")
message(STATUS "  PipeWire: ${PIPEWIRE_FOUND}")
message(STATUS "  libdrm:   ${LIBDRM_FOUND}")
message(STATUS "  EGL:      ${EGL_FOUND}")
message(STATUS "  VA-API: ${VAAPI_FOUND}")
message(STATUS "  NVENC: ${HAVE_NVENC_IN_FFMPEG}")
message(STATUS "")
