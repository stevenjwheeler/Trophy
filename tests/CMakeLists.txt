# Trophy Unit Tests
find_package(GTest REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)

include(GoogleTest)

add_executable(test_InstallInfo test_InstallInfo.cpp
    ${CMAKE_SOURCE_DIR}/src/core/InstallInfo.cpp)
target_include_directories(test_InstallInfo PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_InstallInfo PRIVATE GTest::GTest GTest::gtest_main Qt6::Core Qt6::Test)
gtest_discover_tests(test_InstallInfo)

add_executable(test_DiscordPresence test_DiscordPresence.cpp
    test_DiscordGameArtwork.cpp
    ${CMAKE_SOURCE_DIR}/src/core/DiscordPresence.cpp
    ${CMAKE_SOURCE_DIR}/src/core/DiscordGameArtwork.cpp)
target_include_directories(test_DiscordPresence PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_DiscordPresence PRIVATE GTest::GTest GTest::gtest_main Qt6::Core Qt6::Network Qt6::Test)
gtest_discover_tests(test_DiscordPresence)

add_executable(test_DiscordPresenceUi test_DiscordPresenceUi.cpp)
target_include_directories(test_DiscordPresenceUi PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_DiscordPresenceUi PRIVATE SETTINGS_QML_FILE="${CMAKE_SOURCE_DIR}/qml/pages/SettingsPage.qml")
target_link_libraries(test_DiscordPresenceUi PRIVATE ${PROJECT_NAME} GTest::GTest Qt6::Quick Qt6::Qml Qt6::Test)
gtest_discover_tests(test_DiscordPresenceUi)

add_executable(test_DeletionQueue test_DeletionQueue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/DeletionQueue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/StorageManager.cpp)
target_include_directories(test_DeletionQueue PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_DeletionQueue PRIVATE GTest::GTest GTest::gtest_main Qt6::Core Qt6::Test)
gtest_discover_tests(test_DeletionQueue)

add_executable(test_ShareExport test_ShareExport.cpp ${CMAKE_SOURCE_DIR}/resources/resources.qrc
    ${CMAKE_SOURCE_DIR}/src/library/ShareExportEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/library/ShareExporter.cpp
    ${CMAKE_SOURCE_DIR}/src/core/StorageManager.cpp)
target_include_directories(test_ShareExport PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_ShareExport PRIVATE GTest::GTest GTest::gtest_main Qt6::Core Qt6::Gui Qt6::Test
    PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::SWSCALE PkgConfig::SWRESAMPLE)
gtest_discover_tests(test_ShareExport)

add_executable(test_ShareUi test_ShareUi.cpp ${CMAKE_SOURCE_DIR}/resources/resources.qrc)
target_compile_definitions(test_ShareUi PRIVATE SHARING_QML_DIR="${CMAKE_SOURCE_DIR}/qml/components")
target_link_libraries(test_ShareUi PRIVATE GTest::GTest Qt6::Quick Qt6::Qml Qt6::Test)
gtest_discover_tests(test_ShareUi)

add_executable(test_Theme test_Theme.cpp
    ${CMAKE_SOURCE_DIR}/src/core/WindowChrome.cpp
    ${CMAKE_SOURCE_DIR}/src/core/WindowChrome.h)
target_include_directories(test_Theme PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_Theme PRIVATE
    THEME_STYLES_DIR="${CMAKE_SOURCE_DIR}/qml/styles")
target_link_libraries(test_Theme PRIVATE GTest::GTest Qt6::Quick Qt6::Qml Qt6::Test)
gtest_discover_tests(test_Theme)

add_executable(test_GameIconProvider test_GameIconProvider.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/GameIconProvider.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/GameListModel.h
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc)
target_include_directories(test_GameIconProvider PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_GameIconProvider PRIVATE
    GAMES_QML_FILE="${CMAKE_SOURCE_DIR}/qml/pages/GameListPage.qml")
target_link_libraries(test_GameIconProvider PRIVATE
    GTest::GTest Qt6::Quick Qt6::Qml Qt6::Test)
gtest_discover_tests(test_GameIconProvider)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME AppImageAbiTest.Harness
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_AppImageAbi.py")
    add_test(NAME AppImageSmokeTest.Harness
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_AppImageSmoke.py")
    add_test(NAME NativePackageSmokeTest.Harness
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_NativePackageSmoke.py")
    add_test(NAME ReporterRuntimeTest.Harness
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_ReporterRuntimes.py")
    add_test(NAME NativeInstallSafetyTest.Harness
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_NativeInstallSafety.py")
    add_test(NAME DraftReleaseTest.Harness
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_DraftRelease.py")
    add_test(NAME AppRunLibvaTest.Harness
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_AppRun.py")
    set(multilib_preload_includes)
    foreach(directory IN LISTS Qt6Core_INCLUDE_DIRS)
        list(APPEND multilib_preload_includes --qt-include "${directory}")
    endforeach()
    add_test(NAME MultilibPreloadTest.NativeLoaders
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_MultilibPreload.py"
            --qt-library "$<TARGET_FILE:Qt6::Core>" ${multilib_preload_includes})
    set_tests_properties(MultilibPreloadTest.NativeLoaders PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
    add_test(NAME ReporterAbiTest.StaticFixtures
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_ReporterAbi.py")
endif()

add_executable(test_PlayerProbe test_PlayerProbe.cpp)
target_compile_definitions(test_PlayerProbe PRIVATE
    PLAYER_QML_FILE="${CMAKE_SOURCE_DIR}/qml/pages/PlayerPage.qml"
    AUDIO_METER_QML_FILE="${CMAKE_SOURCE_DIR}/qml/components/AudioLevelMeter.qml")
target_link_libraries(test_PlayerProbe PRIVATE GTest::GTest Qt6::Quick Qt6::Qml Qt6::Test Qt6::Multimedia)
gtest_discover_tests(test_PlayerProbe)

# Background low-resolution preview generation and cache lifecycle.
add_executable(test_PreviewCache test_PreviewCache.cpp
    ${CMAKE_SOURCE_DIR}/src/library/PreviewCache.cpp
    ${CMAKE_SOURCE_DIR}/src/library/PreviewCache.h)
target_include_directories(test_PreviewCache PRIVATE ${CMAKE_SOURCE_DIR}/src
    ${AVFORMAT_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${SWSCALE_INCLUDE_DIRS})
target_link_libraries(test_PreviewCache PRIVATE GTest::GTest Qt6::Core Qt6::Concurrent Qt6::Test Qt6::Quick
    ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${SWSCALE_LIBRARIES})
gtest_discover_tests(test_PreviewCache)

# Headless engine load of the real library card (rename rebind keeps
# thumbnail). Views never polish offscreen here, so this covers the card
# bindings only; the model side is covered in test_RecordingLibrary.
add_executable(test_RecordingCardBinding test_RecordingCardBinding.cpp
    ${CMAKE_SOURCE_DIR}/src/library/PreviewCache.cpp
    ${CMAKE_SOURCE_DIR}/src/library/PreviewCache.h
    ${CMAKE_SOURCE_DIR}/src/library/RecordingLibrary.cpp
    ${CMAKE_SOURCE_DIR}/src/library/RecordingLibrary.h
    ${CMAKE_SOURCE_DIR}/src/library/Recording.cpp
    ${CMAKE_SOURCE_DIR}/src/library/Recording.h
    ${CMAKE_SOURCE_DIR}/src/library/ThumbnailGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/library/ThumbnailGenerator.h
    ${CMAKE_SOURCE_DIR}/src/muxing/ReplayClipper.cpp
    ${CMAKE_SOURCE_DIR}/src/muxing/ReplayClipper.h
    ${CMAKE_SOURCE_DIR}/src/core/StorageManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/StorageManager.h
)
target_compile_definitions(test_RecordingCardBinding PRIVATE
    CARD_QML_FILE="${CMAKE_SOURCE_DIR}/qml/views/RecordingCard.qml")
target_include_directories(test_RecordingCardBinding PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)
target_link_libraries(test_RecordingCardBinding PRIVATE
    GTest::GTest
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::Test
    Qt6::Concurrent
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)
gtest_discover_tests(test_RecordingCardBinding)
qt_add_resources(test_RecordingCardBinding "recording_icon_test_resources"
    PREFIX "/icons"
    BASE "${CMAKE_SOURCE_DIR}/resources/icons"
    FILES "${CMAKE_SOURCE_DIR}/resources/icons/manual-recording.svg"
)

# Optional script API fixture: Node is test tooling only, never a runtime
# dependency for Trophy or for builds without it.
find_program(TROPHY_NODE_EXECUTABLE node)
if(TROPHY_NODE_EXECUTABLE)
    add_test(NAME GnomeExtensionTest.ReadOnlySnapshots
        COMMAND "${TROPHY_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_GnomeExtension.js"
            "${CMAKE_SOURCE_DIR}/resources/gnome/extension.js")
    add_test(NAME KWinScriptTest.WindowEventsAndFiltering
        COMMAND "${TROPHY_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_KWinScript.js"
            "${CMAKE_SOURCE_DIR}/resources/kwin/trophy-monitor/contents/code/main.js")
endif()

# Common includes and link libraries
set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src
    ${VAAPI_INCLUDE_DIRS}
)

# Warning flags for every test target (warnings only — never -Werror, so
# this can't red the suite; it surfaces the same bug classes -Wall caught
# in the main lib). Mirrors the main target + shim flag sets.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

set(TEST_LINK_LIBS
    GTest::GTest
    GTest::Main
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)

add_executable(test_ApplicationAudioProbe test_ApplicationAudioProbe.cpp
    test_ApplicationMediaCache.cpp)
target_include_directories(test_ApplicationAudioProbe PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ApplicationAudioProbe PRIVATE
    ${PROJECT_NAME} GTest::GTest Qt6::Widgets Qt6::Qml Qt6::Test)
gtest_discover_tests(test_ApplicationAudioProbe)

add_executable(test_ApplicationAuthorizePrompt test_ApplicationAuthorizePrompt.cpp)
target_include_directories(test_ApplicationAuthorizePrompt PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ApplicationAuthorizePrompt PRIVATE
    ${PROJECT_NAME} GTest::GTest Qt6::Widgets Qt6::Qml Qt6::Test)
gtest_discover_tests(test_ApplicationAuthorizePrompt)

add_executable(test_StorageManager test_StorageManager.cpp ${CMAKE_SOURCE_DIR}/src/core/StorageManager.cpp)
target_include_directories(test_StorageManager PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_StorageManager PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_StorageManager)

# --- NotificationService (pure dedup predicate; no daemon needed) ---
add_executable(test_NotificationService test_NotificationService.cpp ${CMAKE_SOURCE_DIR}/src/core/NotificationService.cpp)
target_include_directories(test_NotificationService PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_NotificationService PRIVATE ${TEST_LINK_LIBS} Qt6::DBus)
gtest_discover_tests(test_NotificationService)

# --- SystemTrayManager (QApplication offscreen; no tray host needed) ---
add_executable(test_SystemTray test_SystemTray.cpp ${CMAKE_SOURCE_DIR}/src/core/SystemTrayManager.cpp)
target_include_directories(test_SystemTray PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_SystemTray PRIVATE ${TEST_LINK_LIBS} Qt6::Widgets)
gtest_discover_tests(test_SystemTray)

# --- ActivityFeed (dashboard timeline persistence; hermetic file path) ---
add_executable(test_ActivityFeed test_ActivityFeed.cpp
    ${CMAKE_SOURCE_DIR}/src/library/ActivityFeed.cpp
    ${CMAKE_SOURCE_DIR}/src/library/ActivityFeed.h
)
target_include_directories(test_ActivityFeed PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ActivityFeed PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_ActivityFeed)

# --- AudioDriftCorrector (wall-clock servo; fake clock, no HW needed) ---
add_executable(test_AudioDriftCorrector test_AudioDriftCorrector.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioDriftCorrector.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioDriftCorrector.h
)
target_include_directories(test_AudioDriftCorrector PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)
target_link_libraries(test_AudioDriftCorrector PRIVATE
    ${TEST_LINK_LIBS}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
)
gtest_discover_tests(test_AudioDriftCorrector)

# --- AutostartManager (isolated config home; no session changes) ---
add_executable(test_Autostart test_Autostart.cpp ${CMAKE_SOURCE_DIR}/src/core/AutostartManager.cpp)
target_include_directories(test_Autostart PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_Autostart PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_Autostart)

# --- GlobalShortcutManager (no portal needed; routing is local) ---
add_executable(test_GlobalShortcuts test_GlobalShortcuts.cpp ${CMAKE_SOURCE_DIR}/src/core/GlobalShortcutManager.cpp)
target_include_directories(test_GlobalShortcuts PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_GlobalShortcuts PRIVATE ${TEST_LINK_LIBS} Qt6::DBus)
gtest_discover_tests(test_GlobalShortcuts)

# --- BugReport (prefilled GitHub issue URL + log tail; no daemon needed) ---
add_executable(test_BugReport test_BugReport.cpp
    ${CMAKE_SOURCE_DIR}/src/core/BugReport.cpp
    ${CMAKE_SOURCE_DIR}/src/core/BugReport.h
)
target_include_directories(test_BugReport PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_BugReport PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_BugReport)

# --- ControlServer (isolated socket path; no GUI needed) ---
add_executable(test_ControlServer test_ControlServer.cpp ${CMAKE_SOURCE_DIR}/src/core/ControlServer.cpp)
target_include_directories(test_ControlServer PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ControlServer PRIVATE ${TEST_LINK_LIBS} Qt6::Network)
gtest_discover_tests(test_ControlServer)

# --- UpdateChecker (pure statics; no network needed) ---
add_executable(test_UpdateChecker test_UpdateChecker.cpp ${CMAKE_SOURCE_DIR}/src/core/UpdateChecker.cpp)
target_include_directories(test_UpdateChecker PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_UpdateChecker PRIVATE ${TEST_LINK_LIBS} Qt6::Network)
gtest_discover_tests(test_UpdateChecker)

# --- WaveformCache (renders fixtures via ffmpeg CLI, skips without it) ---
add_executable(test_WaveformCache test_WaveformCache.cpp
    ${CMAKE_SOURCE_DIR}/src/library/WaveformCache.cpp
    ${CMAKE_SOURCE_DIR}/src/library/WaveformCache.h
)
target_include_directories(test_WaveformCache PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
)
target_link_libraries(test_WaveformCache PRIVATE
    ${TEST_LINK_LIBS}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
)
gtest_discover_tests(test_WaveformCache)

# --- FilmstripCache (renders a fixture via ffmpeg CLI, skips without it) ---
add_executable(test_FilmstripCache test_FilmstripCache.cpp
    ${CMAKE_SOURCE_DIR}/src/library/FilmstripCache.cpp
    ${CMAKE_SOURCE_DIR}/src/library/FilmstripCache.h
)
target_include_directories(test_FilmstripCache PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)
target_link_libraries(test_FilmstripCache PRIVATE
    ${TEST_LINK_LIBS}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)
gtest_discover_tests(test_FilmstripCache)

add_executable(test_SettingsManager test_SettingsManager.cpp 
    ${CMAKE_SOURCE_DIR}/src/settings/SettingsManager.cpp 
    ${CMAKE_SOURCE_DIR}/src/encoding/EncoderFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/FFmpegEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/IEncoder.h
)
target_include_directories(test_SettingsManager PRIVATE 
    ${TEST_INCLUDE_DIRS} 
    ${AVCODEC_INCLUDE_DIRS} 
    ${AVUTIL_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)
target_link_libraries(test_SettingsManager PRIVATE 
    ${TEST_LINK_LIBS} 
    ${AVCODEC_LIBRARIES} 
    ${AVUTIL_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)
gtest_discover_tests(test_SettingsManager)
if(EGL_FOUND)
    target_link_libraries(test_SettingsManager PRIVATE PkgConfig::EGL)
endif()


add_executable(test_GameWhitelist test_GameWhitelist.cpp ${CMAKE_SOURCE_DIR}/src/detection/GameWhitelist.cpp)
target_include_directories(test_GameWhitelist PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_GameWhitelist PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_GameWhitelist)

add_library(test_RecordingBackend STATIC
    ${CMAKE_SOURCE_DIR}/src/core/RecordingManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/RecordingSession.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ReplayManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/StorageManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/WindowService.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FrameDispatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/CircularFrameBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FrameTypes.cpp
    ${CMAKE_SOURCE_DIR}/src/settings/SettingsManager.cpp
    ${CMAKE_SOURCE_DIR}/src/capture/CaptureFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/capture/WaylandPortalCapture.cpp
    ${CMAKE_SOURCE_DIR}/src/capture/IVideoCaptureSource.h
    ${CMAKE_SOURCE_DIR}/src/audio/AudioMixer.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioDriftCorrector.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioDeviceEnumerator.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/PipeWireAudioSource.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/IAudioSource.h
    ${CMAKE_SOURCE_DIR}/src/encoding/EncoderFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/FFmpegEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/AudioEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/IEncoder.h
    ${CMAKE_SOURCE_DIR}/src/muxing/FFmpegMuxer.cpp
    ${CMAKE_SOURCE_DIR}/src/muxing/ReplayClipper.cpp
    ${CMAKE_SOURCE_DIR}/src/muxing/IMuxer.h
    ${CMAKE_SOURCE_DIR}/src/detection/GameDetectionService.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/GameListModel.h
    ${CMAKE_SOURCE_DIR}/src/detection/GameWhitelist.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/WaylandWindowMonitor.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/WaylandWindowMonitor.h
    ${CMAKE_SOURCE_DIR}/src/detection/KWinBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/KWinBackend.h
    ${CMAKE_SOURCE_DIR}/src/detection/GnomeBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/GnomeBackend.h
    ${CMAKE_SOURCE_DIR}/src/detection/HyprlandBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/HyprlandBackend.h
    ${CMAKE_SOURCE_DIR}/src/detection/SwayBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/SwayBackend.h
    ${CMAKE_SOURCE_DIR}/src/detection/WindowInfo.h
    ${CMAKE_SOURCE_DIR}/src/detection/WindowInfo.cpp
    ${CMAKE_SOURCE_DIR}/tests/RecordingManagerLinkStubs.cpp
)
target_include_directories(test_RecordingBackend PRIVATE ${TEST_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${SWRESAMPLE_INCLUDE_DIRS})
target_link_libraries(test_RecordingBackend PRIVATE ${TEST_LINK_LIBS} Qt6::DBus Qt6::Network Qt6::Concurrent PkgConfig::PIPEWIRE ${AVCODEC_LIBRARIES} ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES} ${SWSCALE_LIBRARIES} ${SWRESAMPLE_LIBRARIES})
# Propagate GTest to consumers of this static backend (they use gtest macros).
target_link_libraries(test_RecordingBackend PUBLIC GTest::GTest GTest::Main)
if(EGL_FOUND)
    # PUBLIC so executables linking this static lib inherit the EGL dep.
    target_link_libraries(test_RecordingBackend PUBLIC PkgConfig::EGL)
endif()

add_executable(test_RecordingManager test_RecordingManager.cpp)
target_include_directories(test_RecordingManager PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_RecordingManager PRIVATE test_RecordingBackend Qt6::Core Qt6::Network Qt6::Test)
gtest_discover_tests(test_RecordingManager)

add_executable(test_WaylandPortalCapture test_WaylandPortalCapture.cpp)
add_executable(test_PipeWireVideoFormat test_PipeWireVideoFormat.cpp)
target_include_directories(test_PipeWireVideoFormat PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_PipeWireVideoFormat PRIVATE ${TEST_LINK_LIBS} PkgConfig::PIPEWIRE)
gtest_discover_tests(test_PipeWireVideoFormat)
add_executable(test_PipeWireBufferLease test_PipeWireBufferLease.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FrameDispatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FrameTypes.cpp)
target_include_directories(test_PipeWireBufferLease PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_PipeWireBufferLease PRIVATE ${TEST_LINK_LIBS} PkgConfig::PIPEWIRE)
target_link_options(test_PipeWireBufferLease PRIVATE "-Wl,--wrap=pw_stream_queue_buffer"
    "-Wl,--wrap=pw_stream_update_params")
gtest_discover_tests(test_PipeWireBufferLease)
target_include_directories(test_WaylandPortalCapture PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_WaylandPortalCapture PRIVATE test_RecordingBackend Qt6::DBus Qt6::Test PkgConfig::PIPEWIRE)
find_program(TROPHY_DBUS_RUN_SESSION dbus-run-session)
if(TROPHY_DBUS_RUN_SESSION AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    # Fake portal owns the desktop portal name only on a private bus.
    set_property(TARGET test_WaylandPortalCapture PROPERTY TEST_LAUNCHER
        "${TROPHY_DBUS_RUN_SESSION};--config-file=${CMAKE_CURRENT_SOURCE_DIR}/private-session-bus.conf;--;${CMAKE_COMMAND};-E;env;TROPHY_PRIVATE_PORTAL_TESTS=1")
endif()
gtest_discover_tests(test_WaylandPortalCapture)
if(TROPHY_DBUS_RUN_SESSION AND CMAKE_VERSION VERSION_LESS 3.29)
    add_test(NAME WaylandPortalCaptureTest.PrivateBusIntegration
        COMMAND "${TROPHY_DBUS_RUN_SESSION}" "--config-file=${CMAKE_CURRENT_SOURCE_DIR}/private-session-bus.conf" --
            "${CMAKE_COMMAND}" -E env TROPHY_PRIVATE_PORTAL_TESTS=1
            $<TARGET_FILE:test_WaylandPortalCapture>)
endif()

add_executable(test_NotificationPortal test_NotificationPortal.cpp
    ${CMAKE_SOURCE_DIR}/src/core/NotificationService.cpp)
target_include_directories(test_NotificationPortal PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_NotificationPortal PRIVATE test_RecordingBackend
    GTest::GTest Qt6::DBus Qt6::Test PkgConfig::PIPEWIRE)
if(TROPHY_DBUS_RUN_SESSION AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    set_property(TARGET test_NotificationPortal PROPERTY TEST_LAUNCHER
        "${TROPHY_DBUS_RUN_SESSION};--;${CMAKE_COMMAND};-E;env;TROPHY_PRIVATE_PORTAL_TESTS=1")
endif()
gtest_discover_tests(test_NotificationPortal)
if(TROPHY_DBUS_RUN_SESSION AND CMAKE_VERSION VERSION_LESS 3.29)
    add_test(NAME NotificationPortalTest.PrivateBusIntegration
        COMMAND "${TROPHY_DBUS_RUN_SESSION}" -- "${CMAKE_COMMAND}" -E env
            TROPHY_PRIVATE_PORTAL_TESTS=1 $<TARGET_FILE:test_NotificationPortal>)
endif()

add_executable(test_GlobalShortcutPortal test_GlobalShortcutPortal.cpp
    ${CMAKE_SOURCE_DIR}/src/core/GlobalShortcutManager.cpp)
target_include_directories(test_GlobalShortcutPortal PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_GlobalShortcutPortal PRIVATE ${TEST_LINK_LIBS} Qt6::DBus Qt6::Test)
if(TROPHY_DBUS_RUN_SESSION AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    set_property(TARGET test_GlobalShortcutPortal PROPERTY TEST_LAUNCHER
        "${TROPHY_DBUS_RUN_SESSION};--;${CMAKE_COMMAND};-E;env;TROPHY_PRIVATE_PORTAL_TESTS=1")
endif()
gtest_discover_tests(test_GlobalShortcutPortal)
if(TROPHY_DBUS_RUN_SESSION AND CMAKE_VERSION VERSION_LESS 3.29)
    add_test(NAME GlobalShortcutPortalTest.PrivateBusIntegration
        COMMAND "${TROPHY_DBUS_RUN_SESSION}" -- "${CMAKE_COMMAND}" -E env
            TROPHY_PRIVATE_PORTAL_TESTS=1 $<TARGET_FILE:test_GlobalShortcutPortal>)
endif()

add_executable(test_PipeWireAudioSource test_PipeWireAudioSource.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/PipeWireAudioSource.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/PipeWireAudioSource.h
    ${CMAKE_SOURCE_DIR}/src/audio/IAudioSource.h)
target_include_directories(test_PipeWireAudioSource PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_PipeWireAudioSource PRIVATE ${TEST_LINK_LIBS} PkgConfig::PIPEWIRE)
gtest_discover_tests(test_PipeWireAudioSource)

if(Python3_Interpreter_FOUND)
    add_executable(test_AppAudioGroup test_AppAudioGroup.cpp)
    target_include_directories(test_AppAudioGroup PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(test_AppAudioGroup PRIVATE Trophy GTest::GTest GTest::Main Qt6::Test PkgConfig::PIPEWIRE)
    add_test(NAME AppAudioGroupTest.PrivatePipeWire
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_private_pipewire.py"
            $<TARGET_FILE:test_AppAudioGroup>)
    set_tests_properties(AppAudioGroupTest.PrivatePipeWire PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_executable(test_RecordingFailureIntegration test_RecordingFailureIntegration.cpp)
    target_include_directories(test_RecordingFailureIntegration PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(test_RecordingFailureIntegration PRIVATE Trophy GTest::GTest
        Qt6::Test Qt6::DBus Qt6::Network PkgConfig::PIPEWIRE PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL)
    foreach(case IN ITEMS NativeServiceLoss StartupServiceLoss DvrHandoffServiceLoss NativeFormatChanges PermissionRevocation DvrNativeFormatChanges DvrPermissionRevocation NativeGameExit TransientDeliveryStall RenegotiatedStalledCapture)
        add_test(NAME RecordingFailureTest.${case}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_private_pipewire.py"
                $<TARGET_FILE:test_RecordingFailureIntegration> "--gtest_filter=*/RecordingManagerTest.${case}*")
        set_tests_properties(RecordingFailureTest.${case} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endforeach()
endif()

add_executable(test_ReplayManager test_ReplayManager.cpp)
target_include_directories(test_ReplayManager PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ReplayManager PRIVATE test_RecordingBackend Qt6::Core Qt6::Network Qt6::Test)
gtest_discover_tests(test_ReplayManager)

add_executable(test_RecordingSession test_RecordingSession.cpp)
target_include_directories(test_RecordingSession PRIVATE ${TEST_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS})
target_link_libraries(test_RecordingSession PRIVATE test_RecordingBackend Qt6::Core Qt6::Test ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
gtest_discover_tests(test_RecordingSession)


add_executable(test_Recording test_Recording.cpp ${CMAKE_SOURCE_DIR}/src/library/Recording.cpp)
target_include_directories(test_Recording PRIVATE ${TEST_INCLUDE_DIRS} ${AVFORMAT_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS})
target_link_libraries(test_Recording PRIVATE ${TEST_LINK_LIBS} Qt6::Concurrent ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES} ${AVCODEC_LIBRARIES})
gtest_discover_tests(test_Recording)

add_executable(test_GameDetectionService test_GameDetectionService.cpp ${CMAKE_SOURCE_DIR}/src/detection/GameDetectionService.cpp ${CMAKE_SOURCE_DIR}/src/detection/GameListModel.h ${CMAKE_SOURCE_DIR}/src/detection/GameWhitelist.cpp ${CMAKE_SOURCE_DIR}/src/detection/WaylandWindowMonitor.cpp ${CMAKE_SOURCE_DIR}/src/detection/WaylandWindowMonitor.h ${CMAKE_SOURCE_DIR}/src/detection/KWinBackend.cpp ${CMAKE_SOURCE_DIR}/src/detection/KWinBackend.h ${CMAKE_SOURCE_DIR}/src/detection/GnomeBackend.cpp ${CMAKE_SOURCE_DIR}/src/detection/GnomeBackend.h ${CMAKE_SOURCE_DIR}/src/detection/HyprlandBackend.cpp ${CMAKE_SOURCE_DIR}/src/detection/HyprlandBackend.h ${CMAKE_SOURCE_DIR}/src/detection/SwayBackend.cpp ${CMAKE_SOURCE_DIR}/src/detection/SwayBackend.h ${CMAKE_SOURCE_DIR}/src/detection/WindowInfo.h ${CMAKE_SOURCE_DIR}/src/detection/WindowInfo.cpp)
target_include_directories(test_GameDetectionService PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_definitions(test_GameDetectionService PRIVATE TROPHY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_GameDetectionService PRIVATE ${TEST_LINK_LIBS} Qt6::Network Qt6::DBus)
# Every case uses the frozen reporter endpoint. Parallel CTest processes
# must not mistake another test's listener for the user's live app.
gtest_discover_tests(test_GameDetectionService PROPERTIES RESOURCE_LOCK trophy_detection_socket)

# --- test_EncoderTest (The New Matrix Test) ---
add_executable(test_EncoderTest
    test_EncoderTest.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/FFmpegEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/IEncoder.h
)

target_include_directories(test_EncoderTest PRIVATE 
    ${TEST_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

target_link_libraries(test_EncoderTest PRIVATE 
    ${TEST_LINK_LIBS}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)

gtest_discover_tests(test_EncoderTest)
if(EGL_FOUND)
    target_link_libraries(test_EncoderTest PRIVATE PkgConfig::EGL)
endif()


# --- Frame pipeline (dispatcher + circular buffer, no HW/daemon needed) ---
add_executable(test_FramePipeline test_FramePipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FrameDispatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/CircularFrameBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FrameTypes.cpp
)
target_include_directories(test_FramePipeline PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_FramePipeline PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_FramePipeline)

# --- AudioMixer (fake in-process sources, no PipeWire daemon needed) ---
add_executable(test_AudioMixer test_AudioMixer.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioMixer.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioMixer.h
    ${CMAKE_SOURCE_DIR}/src/audio/PipeWireAudioSource.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/IAudioSource.h
)
target_include_directories(test_AudioMixer PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_AudioMixer PRIVATE ${TEST_LINK_LIBS} PkgConfig::PIPEWIRE)
gtest_discover_tests(test_AudioMixer)

# --- PipeWire sync wait (bounded-loop logic, no daemon required) ---
add_executable(test_PipeWireSyncWait test_PipeWireSyncWait.cpp)
target_include_directories(test_PipeWireSyncWait PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_PipeWireSyncWait PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_PipeWireSyncWait)

# --- AudioEncoder (needs FFmpeg audio encoders at runtime, skips otherwise) ---
add_executable(test_AudioEncoder test_AudioEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/AudioEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/AudioEncoder.h
    ${CMAKE_SOURCE_DIR}/src/encoding/IEncoder.h
)
target_include_directories(test_AudioEncoder PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)
target_link_libraries(test_AudioEncoder PRIVATE
    ${TEST_LINK_LIBS}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
)
gtest_discover_tests(test_AudioEncoder)

# --- Muxer round-trip (needs libx264 + AAC at runtime, skips otherwise) ---
add_executable(test_Muxer test_Muxer.cpp
    ${CMAKE_SOURCE_DIR}/src/muxing/FFmpegMuxer.cpp
    ${CMAKE_SOURCE_DIR}/src/muxing/IMuxer.h
    ${CMAKE_SOURCE_DIR}/src/encoding/FFmpegEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/AudioEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/AudioEncoder.h
    ${CMAKE_SOURCE_DIR}/src/encoding/IEncoder.h
    ${CMAKE_SOURCE_DIR}/src/capture/IVideoCaptureSource.h
)
target_include_directories(test_Muxer PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)
target_link_libraries(test_Muxer PRIVATE
    ${TEST_LINK_LIBS}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)
gtest_discover_tests(test_Muxer)
if(EGL_FOUND)
    target_link_libraries(test_Muxer PRIVATE PkgConfig::EGL)
endif()


# --- ReplayClipper (synthetic segments; no encoder/HW needed) ---
add_executable(test_ReplayClipper test_ReplayClipper.cpp
    ${CMAKE_SOURCE_DIR}/src/muxing/FFmpegMuxer.cpp
    ${CMAKE_SOURCE_DIR}/src/muxing/ReplayClipper.cpp
    ${CMAKE_SOURCE_DIR}/src/muxing/IMuxer.h
    ${CMAKE_SOURCE_DIR}/src/encoding/FFmpegEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/AudioEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/AudioEncoder.h
    ${CMAKE_SOURCE_DIR}/src/encoding/IEncoder.h
    ${CMAKE_SOURCE_DIR}/src/capture/IVideoCaptureSource.h
)
target_include_directories(test_ReplayClipper PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)
target_link_libraries(test_ReplayClipper PRIVATE
    ${TEST_LINK_LIBS}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)
gtest_discover_tests(test_ReplayClipper)
if(EGL_FOUND)
    target_link_libraries(test_ReplayClipper PRIVATE PkgConfig::EGL)
endif()


# --- RecordingLibrary (dummy files; thumbnails fail gracefully) ---
add_executable(test_RecordingLibrary test_RecordingLibrary.cpp
    ${CMAKE_SOURCE_DIR}/src/library/RecordingLibrary.cpp
    ${CMAKE_SOURCE_DIR}/src/library/RecordingLibrary.h
    ${CMAKE_SOURCE_DIR}/src/library/Recording.cpp
    ${CMAKE_SOURCE_DIR}/src/library/Recording.h
    ${CMAKE_SOURCE_DIR}/src/library/ThumbnailGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/library/ThumbnailGenerator.h
    ${CMAKE_SOURCE_DIR}/src/muxing/ReplayClipper.cpp
    ${CMAKE_SOURCE_DIR}/src/muxing/ReplayClipper.h
    ${CMAKE_SOURCE_DIR}/src/core/StorageManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/StorageManager.h
)
target_include_directories(test_RecordingLibrary PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)
target_link_libraries(test_RecordingLibrary PRIVATE
    ${TEST_LINK_LIBS}
    Qt6::Concurrent
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)
gtest_discover_tests(test_RecordingLibrary)

# --- ThumbnailGenerator (renders a clip via ffmpeg CLI, skips without it) ---
add_executable(test_ThumbnailGenerator test_ThumbnailGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/library/ThumbnailGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/library/ThumbnailGenerator.h
)
target_include_directories(test_ThumbnailGenerator PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)
target_link_libraries(test_ThumbnailGenerator PRIVATE
    ${TEST_LINK_LIBS}
    Qt6::Concurrent
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)
gtest_discover_tests(test_ThumbnailGenerator)

if(EGL_FOUND)
    add_executable(test_EglDisplayLease test_EglDisplayLease.cpp)
    target_include_directories(test_EglDisplayLease PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(test_EglDisplayLease PRIVATE ${TEST_LINK_LIBS} PkgConfig::EGL)
    gtest_discover_tests(test_EglDisplayLease)
endif()

# --- DmaBufCudaImport (Stage B: real GBM DMA-BUF -> NVENC EGL import;
# skips without NVIDIA EGL / GBM / NVENC) ---
if(EGL_FOUND AND GBM_FOUND)
add_executable(test_DmaBufCudaImport test_DmaBufCudaImport.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/FFmpegEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/IEncoder.h
    ${CMAKE_SOURCE_DIR}/src/muxing/FFmpegMuxer.cpp
    ${CMAKE_SOURCE_DIR}/src/muxing/IMuxer.h
    ${CMAKE_SOURCE_DIR}/src/capture/IVideoCaptureSource.h
)
target_include_directories(test_DmaBufCudaImport PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)
target_link_libraries(test_DmaBufCudaImport PRIVATE
    ${TEST_LINK_LIBS}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    PkgConfig::EGL
    PkgConfig::GBM
)
gtest_discover_tests(test_DmaBufCudaImport)
endif()

if(VAAPI_FOUND)
    add_executable(test_VaapiVideoProcessor test_VaapiVideoProcessor.cpp)
    target_include_directories(test_VaapiVideoProcessor PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(test_VaapiVideoProcessor PRIVATE ${TEST_LINK_LIBS} ${CMAKE_DL_LIBS})
    gtest_discover_tests(test_VaapiVideoProcessor)
endif()

# --- EncoderFactory (detection + fallback, HW tests skip gracefully) ---
add_executable(test_EncoderFactory test_EncoderFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/EncoderFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/FFmpegEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/IEncoder.h
)
target_include_directories(test_EncoderFactory PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)
target_link_libraries(test_EncoderFactory PRIVATE
    ${TEST_LINK_LIBS}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)
gtest_discover_tests(test_EncoderFactory)
if(EGL_FOUND)
    target_link_libraries(test_EncoderFactory PRIVATE PkgConfig::EGL)
endif()


# --- WindowMonitor (KWin snapshot parsing + focus + WindowService delegation;
# no compositor or D-Bus session needed) ---
add_executable(test_WindowMonitor test_WindowMonitor.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
    ${CMAKE_SOURCE_DIR}/src/detection/WaylandWindowMonitor.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/WaylandWindowMonitor.h
    ${CMAKE_SOURCE_DIR}/src/detection/KWinBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/KWinBackend.h
    ${CMAKE_SOURCE_DIR}/src/detection/GnomeBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/GnomeBackend.h
    ${CMAKE_SOURCE_DIR}/src/detection/HyprlandBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/HyprlandBackend.h
    ${CMAKE_SOURCE_DIR}/src/detection/SwayBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/SwayBackend.h
    ${CMAKE_SOURCE_DIR}/src/detection/WindowInfo.h
    ${CMAKE_SOURCE_DIR}/src/detection/WindowInfo.cpp
    ${CMAKE_SOURCE_DIR}/src/core/WindowService.cpp
    ${CMAKE_SOURCE_DIR}/src/core/WindowService.h
)
target_include_directories(test_WindowMonitor PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_WindowMonitor PRIVATE ${TEST_LINK_LIBS} Qt6::DBus)
gtest_discover_tests(test_WindowMonitor)

# Never let the fake Shell own a name on the user's real session bus.
find_program(TROPHY_DBUS_RUN_SESSION dbus-run-session)
if(TROPHY_DBUS_RUN_SESSION)
    add_executable(test_GnomeBackend test_GnomeBackend.cpp
        ${CMAKE_SOURCE_DIR}/src/detection/GnomeBackend.cpp
        ${CMAKE_SOURCE_DIR}/src/detection/GnomeBackend.h
        ${CMAKE_SOURCE_DIR}/src/detection/WindowInfo.cpp
        ${CMAKE_SOURCE_DIR}/resources/resources.qrc)
    target_include_directories(test_GnomeBackend PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(test_GnomeBackend PRIVATE ${TEST_LINK_LIBS} Qt6::DBus)
    add_test(NAME GnomeBackendTest.PrivateBus
        COMMAND "${TROPHY_DBUS_RUN_SESSION}" -- ${CMAKE_COMMAND} -E env
            TROPHY_PRIVATE_GNOME_TEST=1 $<TARGET_FILE:test_GnomeBackend>)
endif()

# --- GlShimEnrollment (conf-file round-trip; hermetic via env overrides) ---
add_executable(test_GlShimEnrollment test_GlShimEnrollment.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/GlShimEnrollment.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/GlShimEnrollment.h
)
target_include_directories(test_GlShimEnrollment PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_GlShimEnrollment PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_GlShimEnrollment)

add_executable(test_GlShimReporter test_GlShimReporter.cpp)
target_link_libraries(test_GlShimReporter PRIVATE ${TEST_LINK_LIBS} Threads::Threads dl)
gtest_discover_tests(test_GlShimReporter)

add_executable(test_ReporterDlopen test_ReporterDlopen.cpp)
target_compile_definitions(test_ReporterDlopen PRIVATE TROPHY_GL_SHIM_FILE="$<TARGET_FILE:trophy_gl_shim>")
target_link_libraries(test_ReporterDlopen PRIVATE GTest::gtest_main dl)
add_dependencies(test_ReporterDlopen trophy_gl_shim)
gtest_discover_tests(test_ReporterDlopen)

# --- ShippedLibDirs (prefix-relative lib lookup contract; no env needed) ---
add_executable(test_ShippedLibDirs test_ShippedLibDirs.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/ShippedLibDirs.h
)
target_include_directories(test_ShippedLibDirs PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ShippedLibDirs PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_ShippedLibDirs)

# --- RuntimeLibraries (bundle/host inventory logged at startup) ---
add_executable(test_RuntimeLibraries test_RuntimeLibraries.cpp
    ${CMAKE_SOURCE_DIR}/src/core/RuntimeLibraries.cpp
    ${CMAKE_SOURCE_DIR}/src/core/RuntimeLibraries.h
)
target_include_directories(test_RuntimeLibraries PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_RuntimeLibraries PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_RuntimeLibraries)

# --- VulkanLayer (structural manifest writer; hermetic via XDG_DATA_HOME) ---
add_executable(test_VulkanLayer test_VulkanLayer.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/VulkanLayer.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/VulkanLayer.h
)
target_include_directories(test_VulkanLayer PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_VulkanLayer PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_VulkanLayer)

# Real reporter export, exercised without creating a Vulkan instance.
find_package(Threads REQUIRED)
add_library(VulkanReporterNext SHARED VulkanReporterNext.cpp)
target_include_directories(VulkanReporterNext PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(VulkanReporterNext PRIVATE Threads::Threads)
add_library(VulkanReporterFixture SHARED VulkanReporterFixture.cpp)
target_include_directories(VulkanReporterFixture PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(VulkanReporterFixture PRIVATE trophy_layer VulkanReporterNext)
add_executable(test_VulkanReporter test_VulkanReporter.cpp)
target_compile_definitions(test_VulkanReporter PRIVATE
    TROPHY_LAYER_FILE="$<TARGET_FILE:trophy_layer>"
    VULKAN_REPORTER_FIXTURE_FILE="$<TARGET_FILE:VulkanReporterFixture>")
target_link_libraries(test_VulkanReporter PRIVATE ${TEST_LINK_LIBS} Vulkan::Vulkan dl)
add_dependencies(test_VulkanReporter trophy_layer VulkanReporterFixture)
gtest_discover_tests(test_VulkanReporter)

# --- HardwareInfo (PCI mapping + gather smoke; degrades headless) ---
add_executable(test_HardwareInfo test_HardwareInfo.cpp
    ${CMAKE_SOURCE_DIR}/src/core/HardwareInfo.cpp
    ${CMAKE_SOURCE_DIR}/src/core/HardwareInfo.h
    ${CMAKE_SOURCE_DIR}/src/encoding/EncoderFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/FFmpegEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encoding/IEncoder.h
)
target_include_directories(test_HardwareInfo PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)
target_link_libraries(test_HardwareInfo PRIVATE
    ${TEST_LINK_LIBS}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)
if(LIBDRM_FOUND)
    target_link_libraries(test_HardwareInfo PRIVATE PkgConfig::LIBDRM)
endif()
gtest_discover_tests(test_HardwareInfo)
if(EGL_FOUND)
    target_link_libraries(test_HardwareInfo PRIVATE PkgConfig::EGL)
endif()
