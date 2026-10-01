# Trophy Unit Tests
find_package(GTest REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)

include(GoogleTest)

# Common includes and link libraries
set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src
)

set(TEST_LINK_LIBS
    GTest::GTest
    GTest::Main
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)

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

# --- ControlServer (isolated socket path; no GUI needed) ---
add_executable(test_ControlServer test_ControlServer.cpp ${CMAKE_SOURCE_DIR}/src/core/ControlServer.cpp)
target_include_directories(test_ControlServer PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ControlServer PRIVATE ${TEST_LINK_LIBS} Qt6::Network)
gtest_discover_tests(test_ControlServer)

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
target_include_directories(test_RecordingBackend PRIVATE ${TEST_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS})
target_link_libraries(test_RecordingBackend PRIVATE ${TEST_LINK_LIBS} Qt6::DBus Qt6::Network PkgConfig::PIPEWIRE ${AVCODEC_LIBRARIES} ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES} ${SWSCALE_LIBRARIES})
if(EGL_FOUND)
    # PUBLIC so executables linking this static lib inherit the EGL dep.
    target_link_libraries(test_RecordingBackend PUBLIC PkgConfig::EGL)
endif()

add_executable(test_RecordingManager test_RecordingManager.cpp)
target_include_directories(test_RecordingManager PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_RecordingManager PRIVATE test_RecordingBackend Qt6::Core Qt6::Test)
gtest_discover_tests(test_RecordingManager)

add_executable(test_ReplayManager test_ReplayManager.cpp)
target_include_directories(test_ReplayManager PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ReplayManager PRIVATE test_RecordingBackend Qt6::Core Qt6::Test)
gtest_discover_tests(test_ReplayManager)


add_executable(test_Recording test_Recording.cpp ${CMAKE_SOURCE_DIR}/src/library/Recording.cpp)
target_include_directories(test_Recording PRIVATE ${TEST_INCLUDE_DIRS} ${AVFORMAT_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS})
target_link_libraries(test_Recording PRIVATE ${TEST_LINK_LIBS} ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES})
gtest_discover_tests(test_Recording)

add_executable(test_GameDetectionService test_GameDetectionService.cpp ${CMAKE_SOURCE_DIR}/src/detection/GameDetectionService.cpp ${CMAKE_SOURCE_DIR}/src/detection/GameWhitelist.cpp ${CMAKE_SOURCE_DIR}/src/detection/WaylandWindowMonitor.cpp ${CMAKE_SOURCE_DIR}/src/detection/WaylandWindowMonitor.h ${CMAKE_SOURCE_DIR}/src/detection/KWinBackend.cpp ${CMAKE_SOURCE_DIR}/src/detection/KWinBackend.h ${CMAKE_SOURCE_DIR}/src/detection/GnomeBackend.cpp ${CMAKE_SOURCE_DIR}/src/detection/GnomeBackend.h ${CMAKE_SOURCE_DIR}/src/detection/HyprlandBackend.cpp ${CMAKE_SOURCE_DIR}/src/detection/HyprlandBackend.h ${CMAKE_SOURCE_DIR}/src/detection/SwayBackend.cpp ${CMAKE_SOURCE_DIR}/src/detection/SwayBackend.h ${CMAKE_SOURCE_DIR}/src/detection/WindowInfo.h ${CMAKE_SOURCE_DIR}/src/detection/WindowInfo.cpp)
target_include_directories(test_GameDetectionService PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_GameDetectionService PRIVATE ${TEST_LINK_LIBS} Qt6::Network Qt6::DBus)
gtest_discover_tests(test_GameDetectionService)

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

# --- GlShimEnrollment (conf-file round-trip; hermetic via env overrides) ---
add_executable(test_GlShimEnrollment test_GlShimEnrollment.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/GlShimEnrollment.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/GlShimEnrollment.h
)
target_include_directories(test_GlShimEnrollment PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_GlShimEnrollment PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_GlShimEnrollment)

# --- ShippedLibDirs (prefix-relative lib lookup contract; no env needed) ---
add_executable(test_ShippedLibDirs test_ShippedLibDirs.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/ShippedLibDirs.h
)
target_include_directories(test_ShippedLibDirs PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ShippedLibDirs PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_ShippedLibDirs)

# --- VulkanLayer (structural manifest writer; hermetic via XDG_DATA_HOME) ---
add_executable(test_VulkanLayer test_VulkanLayer.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/VulkanLayer.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/VulkanLayer.h
)
target_include_directories(test_VulkanLayer PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_VulkanLayer PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_VulkanLayer)

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
