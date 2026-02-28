# Trading engine library and executable
project(trader)

# Main executable (simple ITCH processor)
add_executable(hft_engine main.cpp)
target_link_libraries(hft_engine PRIVATE core z)  # z for zlib (gzip support)

# Strategy executable (with market maker)
add_executable(hft_strategy main_strategy.cpp)
target_link_libraries(hft_strategy PRIVATE core z)

message(STATUS "HFT engine executables configured")
message(STATUS "  - hft_engine: ITCH processor")
message(STATUS "  - hft_strategy: Strategy mode with market maker")
