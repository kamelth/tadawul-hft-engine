# Core utilities library
project(core)

# Core library sources (will be added as we implement)
set(CORE_SOURCES
    # source/timestamp.cpp
    # source/endian.cpp
    # Add more as implemented
)

# Core library headers
set(CORE_HEADERS
    include/core/timestamp.h
    include/core/endian.h
    include/core/containers/list.h
    include/core/containers/bintree_avl.h
)

# Create core library (header-only for now, will add sources later)
add_library(core INTERFACE)

target_include_directories(core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# When we add source files, we'll change to:
# add_library(core STATIC ${CORE_SOURCES})
# target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

message(STATUS "Core library configured (header-only)")
