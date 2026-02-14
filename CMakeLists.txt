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

# Get version from Git tag
execute_process(
    COMMAND git describe --tags --always
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

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
)

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

# Find PipeWire (optional, with fallback to PulseAudio)
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
if(PIPEWIRE_FOUND)
    message(STATUS "PipeWire found: ${PIPEWIRE_VERSION}")
    add_compile_definitions(HAVE_PIPEWIRE)
else()
    message(STATUS "PipeWire not found, will use PulseAudio only")
endif()

# Find PulseAudio
pkg_check_modules(PULSE IMPORTED_TARGET libpulse-simple)
if(PULSE_FOUND)
    message(STATUS "PulseAudio found: ${PULSE_VERSION}")
    add_compile_definitions(HAVE_PULSEAUDIO)
else()
    message(WARNING "PulseAudio not found")
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

# Find X11 (for X11 capture support)
find_package(X11 COMPONENTS Xext)
if(X11_FOUND)
    message(STATUS "X11 found")
    add_compile_definitions(HAVE_X11)
    if(X11_Xext_FOUND)
        add_compile_definitions(HAVE_XEXT)
    endif()
else()
    message(STATUS "X11 not found, X11 capture disabled")
endif()

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

# Source files
set(CORE_SOURCES
    src/core/Application.cpp
    src/core/RecordingManager.cpp
    src/core/RecordingSession.cpp
    src/core/StorageManager.cpp
    src/core/NotificationService.cpp
    src/core/WindowService.cpp
)

set(CAPTURE_SOURCES
    src/capture/CaptureFactory.cpp
)

if(X11_FOUND)
    list(APPEND CAPTURE_SOURCES src/capture/X11WindowCapture.cpp)
endif()

if(PIPEWIRE_FOUND)
    list(APPEND CAPTURE_SOURCES src/capture/WaylandPortalCapture.cpp)
endif()

set(AUDIO_SOURCES
    src/audio/AudioMixer.cpp
    src/audio/AudioDeviceEnumerator.cpp
)

if(PIPEWIRE_FOUND)
    list(APPEND AUDIO_SOURCES src/audio/PipeWireAudioSource.cpp)
endif()

if(PULSE_FOUND)
    list(APPEND AUDIO_SOURCES src/audio/PulseAudioSource.cpp)
endif()

set(ENCODING_SOURCES
    src/encoding/EncoderFactory.cpp
    src/encoding/FFmpegEncoder.cpp
    src/encoding/AudioEncoder.cpp
)

set(MUXING_SOURCES
    src/muxing/FFmpegMuxer.cpp
)

set(DETECTION_SOURCES
    src/detection/GameDetectionService.cpp
    src/detection/ProcessHeuristics.cpp
    src/detection/GameWhitelist.cpp
)

if(X11_FOUND)
    list(APPEND DETECTION_SOURCES src/detection/X11WindowMonitor.cpp)
endif()

list(APPEND DETECTION_SOURCES src/detection/WaylandWindowMonitor.cpp)

set(LIBRARY_SOURCES
    src/library/RecordingLibrary.cpp
    src/library/Recording.cpp
    src/library/ThumbnailGenerator.cpp
)

set(SETTINGS_SOURCES
    src/settings/SettingsManager.cpp
)

# Header files (for MOC)
set(CORE_HEADERS
    src/core/Application.h
    src/core/RecordingManager.h
    src/core/RecordingSession.h
    src/core/StorageManager.h
    src/core/NotificationService.h
    src/core/WindowService.h
)

set(CAPTURE_HEADERS
    src/capture/IVideoCaptureSource.h
    src/capture/CaptureFactory.h
)

if(X11_FOUND)
    list(APPEND CAPTURE_HEADERS src/capture/X11WindowCapture.h)
endif()

if(PIPEWIRE_FOUND)
    list(APPEND CAPTURE_HEADERS src/capture/WaylandPortalCapture.h)
endif()

set(AUDIO_HEADERS
    src/audio/IAudioSource.h
    src/audio/AudioMixer.h
    src/audio/AudioDeviceEnumerator.h
)

if(PIPEWIRE_FOUND)
    list(APPEND AUDIO_HEADERS src/audio/PipeWireAudioSource.h)
endif()

if(PULSE_FOUND)
    list(APPEND AUDIO_HEADERS src/audio/PulseAudioSource.h)
endif()

set(ENCODING_HEADERS
    src/encoding/IEncoder.h
    src/encoding/EncoderFactory.h
    src/encoding/FFmpegEncoder.h
    src/encoding/AudioEncoder.h
)

set(MUXING_HEADERS
    src/muxing/IMuxer.h
    src/muxing/FFmpegMuxer.h
)

set(DETECTION_HEADERS
    src/detection/GameDetectionService.h
    src/detection/ProcessHeuristics.h
    src/detection/GameWhitelist.h
)

if(X11_FOUND)
    list(APPEND DETECTION_HEADERS src/detection/X11WindowMonitor.h)
endif()

list(APPEND DETECTION_HEADERS src/detection/WaylandWindowMonitor.h)

set(LIBRARY_HEADERS
    src/library/RecordingLibrary.h
    src/library/Recording.h
    src/library/ThumbnailGenerator.h
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

# QML module
qt_add_qml_module(${PROJECT_NAME}
    URI GameRecorder
    VERSION 1.0
    QML_FILES
        qml/main.qml
        qml/styles/Theme.qml
        qml/components/RecordingIndicator.qml
        qml/components/AudioLevelMeter.qml
        qml/components/StorageUsageBar.qml
        qml/components/NavigationRail.qml
        qml/components/WindowPickerDialog.qml
        qml/pages/DashboardPage.qml
        qml/pages/RecordingsPage.qml
        qml/pages/InfoPage.qml
        qml/pages/SettingsPage.qml
        qml/pages/GameListPage.qml
        qml/views/RecordingGridView.qml
        qml/views/RecordingListView.qml
        qml/views/RecordingListItem.qml
        qml/views/RecordingCard.qml
        qml/views/VideoPlayer.qml
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
    Qt6::Multimedia
    Qt6::DBus
    Qt6::Concurrent
    PkgConfig::AVCODEC
    PkgConfig::AVFORMAT
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
    PkgConfig::SWRESAMPLE
)

if(PIPEWIRE_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::PIPEWIRE)
endif()

if(PULSE_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::PULSE)
endif()

if(VAAPI_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::VAAPI)
endif()

if(X11_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE X11::X11)
    if(X11_Xext_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE X11::Xext)
    endif()
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

# Installation
include(GNUInstallDirs)

install(TARGETS ${PROJECT_NAME}App
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES packaging/trophy.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
)

install(FILES resources/icons/trophy.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
)

# CPack configuration
set(CPACK_PACKAGE_NAME "trophy")
set(CPACK_PACKAGE_VENDOR "Trophy - Game recording and clipping")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
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
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "")
message(STATUS "Features:")
message(STATUS "  PipeWire: ${PIPEWIRE_FOUND}")
message(STATUS "  PulseAudio: ${PULSE_FOUND}")
message(STATUS "  VA-API: ${VAAPI_FOUND}")
message(STATUS "  NVENC: ${HAVE_NVENC_IN_FFMPEG}")
message(STATUS "  X11 capture: ${X11_FOUND}")
message(STATUS "")
