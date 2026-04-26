# Trading engine library and executable
project(trader)

# Main executable (simple ITCH processor)
add_executable(hft_engine main.cpp)
target_link_libraries(hft_engine PRIVATE core z)  # z for zlib (gzip support)

# Strategy executable (with market maker)
add_executable(hft_strategy main_strategy.cpp)
target_link_libraries(hft_strategy PRIVATE core z)

# GPU benchmark (CPU-only mode; GPU mode compiled on Colab with nvcc)
add_executable(gpu_benchmark gpu/benchmark.cpp)
target_link_libraries(gpu_benchmark PRIVATE core)

add_executable(risk_validation_benchmark gpu/risk_validation_benchmark.cpp)
target_link_libraries(risk_validation_benchmark PRIVATE core)

message(STATUS "HFT engine executables configured")
message(STATUS "  - hft_engine: ITCH processor")
message(STATUS "  - hft_strategy: Strategy mode with market maker")
message(STATUS "  - gpu_benchmark: Analytics CPU/GPU benchmark")
message(STATUS "  - risk_validation_benchmark: Risk/Validation CPU/GPU benchmark")
