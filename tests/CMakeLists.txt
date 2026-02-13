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

# --- test_ProcessHeuristics ---
add_executable(test_ProcessHeuristics
    test_ProcessHeuristics.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/ProcessHeuristics.cpp
)
target_include_directories(test_ProcessHeuristics PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ProcessHeuristics PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_ProcessHeuristics)

# --- test_StorageManager ---
add_executable(test_StorageManager
    test_StorageManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/StorageManager.cpp
)
target_include_directories(test_StorageManager PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_StorageManager PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_StorageManager)

# --- test_SettingsManager ---
add_executable(test_SettingsManager
    test_SettingsManager.cpp
    ${CMAKE_SOURCE_DIR}/src/settings/SettingsManager.cpp
)
target_include_directories(test_SettingsManager PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_SettingsManager PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_SettingsManager)

# --- test_GameWhitelist ---
add_executable(test_GameWhitelist
    test_GameWhitelist.cpp
    ${CMAKE_SOURCE_DIR}/src/detection/GameWhitelist.cpp
)
target_include_directories(test_GameWhitelist PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_GameWhitelist PRIVATE ${TEST_LINK_LIBS})
gtest_discover_tests(test_GameWhitelist)

# --- test_Recording ---
add_executable(test_Recording
    test_Recording.cpp
    ${CMAKE_SOURCE_DIR}/src/library/Recording.cpp
)
target_include_directories(test_Recording PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_Recording PRIVATE
    ${TEST_LINK_LIBS}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
)
target_include_directories(test_Recording PRIVATE
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)
gtest_discover_tests(test_Recording)
