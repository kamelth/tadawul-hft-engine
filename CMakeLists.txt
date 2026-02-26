cmake_minimum_required(VERSION 3.20)
project(tadawul-hft-engine VERSION 1.0.0 LANGUAGES CXX)

# Generate compile_commands.json for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Deterministic build flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")

# Include directories
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/modules/core/include
)

# Subdirectories
add_subdirectory(modules/core)
add_subdirectory(source)
add_subdirectory(tests)

# Optional: CUDA support (for Phase 7)
option(ENABLE_CUDA "Enable CUDA GPU acceleration" OFF)
if(ENABLE_CUDA)
    enable_language(CUDA)
    find_package(CUDA REQUIRED)
    message(STATUS "CUDA enabled - GPU acceleration will be built")
    add_subdirectory(modules/cuda)
endif()

# Print summary
message(STATUS "=== Build Configuration ===")
message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "CUDA: ${ENABLE_CUDA}")
message(STATUS "===========================")
