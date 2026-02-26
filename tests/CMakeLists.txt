# Unit tests
project(tests)

# We'll use a simple testing approach (can add Catch2 later if needed)

# Test executables
add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE core)

# add_executable(test_order_book test_order_book.cpp)
# target_link_libraries(test_order_book PRIVATE trader core)

# add_executable(test_itch_handler test_itch_handler.cpp)
# target_link_libraries(test_itch_handler PRIVATE trader core)

# add_executable(test_deterministic test_deterministic.cpp)
# target_link_libraries(test_deterministic PRIVATE trader core)

message(STATUS "Test executables configured")
