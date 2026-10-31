cmake_minimum_required(VERSION 3.21)
project(TrophyReporters LANGUAGES C CXX)

# A Qt/FFmpeg-free build for architecture-specific reporters. In particular,
# do not ask a 32-bit build to find/link the host's 64-bit Vulkan loader: the
# layer forwards through loader-supplied function pointers and needs headers only.
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
find_path(VULKAN_HEADERS vulkan/vk_layer.h REQUIRED)
get_filename_component(TROPHY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(REPORTER_BITS 32)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(REPORTER_BITS 64)
else()
    message(FATAL_ERROR "Unsupported reporter pointer size: ${CMAKE_SIZEOF_VOID_P}")
endif()
message(STATUS "Building ${REPORTER_BITS}-bit Trophy reporters (no desktop enrollment)")

add_library(trophy_layer SHARED "${TROPHY_SOURCE_DIR}/src/vulkan_layer/trophy_layer.cpp")
target_include_directories(trophy_layer PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(trophy_layer PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(trophy_layer PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Kept next to its matching ELF, outside automatic loader enrollment paths.
# An explicit per-process loader path is required to use this standalone build.
file(READ "${TROPHY_SOURCE_DIR}/src/vulkan_layer/trophy_layer.json.in" manifest)
string(REPLACE "@LIBRARY_PATH@" "./libtrophy_layer.so" manifest "${manifest}")
string(JSON manifest SET "${manifest}" file_format_version "\"1.2.1\"")
string(JSON manifest SET "${manifest}" layer library_arch "\"${REPORTER_BITS}\"")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/trophy_layer.json" "${manifest}")
include(GNUInstallDirs)
# These are loaded by absolute path, not linked by the system linker. Keep one
# predictable companion location across Debian/Fedora multilib layouts.
set(REPORTER_INSTALL_DIR "lib/trophy/reporters${REPORTER_BITS}")
install(TARGETS trophy_layer LIBRARY DESTINATION "${REPORTER_INSTALL_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/trophy_layer.json" DESTINATION "${REPORTER_INSTALL_DIR}")

include(CTest)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_library(VulkanReporterNext SHARED "${TROPHY_SOURCE_DIR}/tests/VulkanReporterNext.cpp")
    target_include_directories(VulkanReporterNext PRIVATE "${VULKAN_HEADERS}")
    target_link_libraries(VulkanReporterNext PRIVATE Threads::Threads)
    add_library(VulkanReporterFixture SHARED "${TROPHY_SOURCE_DIR}/tests/VulkanReporterFixture.cpp")
    target_include_directories(VulkanReporterFixture PRIVATE "${VULKAN_HEADERS}")
    target_link_libraries(VulkanReporterFixture PRIVATE trophy_layer VulkanReporterNext)
    add_executable(test_VulkanReporter "${TROPHY_SOURCE_DIR}/tests/test_VulkanReporter.cpp")
    target_include_directories(test_VulkanReporter PRIVATE "${VULKAN_HEADERS}")
    target_compile_definitions(test_VulkanReporter PRIVATE
        TROPHY_LAYER_FILE="$<TARGET_FILE:trophy_layer>"
        VULKAN_REPORTER_FIXTURE_FILE="$<TARGET_FILE:VulkanReporterFixture>")
    target_link_libraries(test_VulkanReporter PRIVATE GTest::gtest_main Threads::Threads ${CMAKE_DL_LIBS})
    add_dependencies(test_VulkanReporter trophy_layer VulkanReporterFixture)
    gtest_discover_tests(test_VulkanReporter)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ReporterArtifacts.ElfAndManifest
        COMMAND "${Python3_EXECUTABLE}" "${TROPHY_SOURCE_DIR}/tests/test_ReporterArtifacts.py"
            --bits "${REPORTER_BITS}" --directory "${CMAKE_CURRENT_BINARY_DIR}")
endif()
