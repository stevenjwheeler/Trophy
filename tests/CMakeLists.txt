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

# --- Existing Tests ---
# test_ProcessHeuristics removed (Heuristics replaced by Vulkan Layer)

add_executable(test_StorageManager test_StorageManager.cpp ${CMAKE_SOURCE_DIR}/src/core/StorageManager.cpp)
target_include_directories(test_StorageManager PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_StorageManager PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_StorageManager)

add_executable(test_SettingsManager test_SettingsManager.cpp ${CMAKE_SOURCE_DIR}/src/settings/SettingsManager.cpp)
target_include_directories(test_SettingsManager PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_SettingsManager PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_SettingsManager)

add_executable(test_GameWhitelist test_GameWhitelist.cpp ${CMAKE_SOURCE_DIR}/src/detection/GameWhitelist.cpp)
target_include_directories(test_GameWhitelist PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_GameWhitelist PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_GameWhitelist)

add_executable(test_Recording test_Recording.cpp ${CMAKE_SOURCE_DIR}/src/library/Recording.cpp)
target_include_directories(test_Recording PRIVATE ${TEST_INCLUDE_DIRS} ${AVFORMAT_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS})
target_link_libraries(test_Recording PRIVATE ${TEST_LINK_LIBS} ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES})
gtest_discover_tests(test_Recording)

add_executable(test_GameDetectionService test_GameDetectionService.cpp ${CMAKE_SOURCE_DIR}/src/detection/GameDetectionService.cpp ${CMAKE_SOURCE_DIR}/src/detection/GameWhitelist.cpp)
target_include_directories(test_GameDetectionService PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_GameDetectionService PRIVATE ${TEST_LINK_LIBS} Qt6::Network)
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