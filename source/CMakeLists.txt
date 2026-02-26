# Trading engine library and executable
project(trader)

# Trader library sources (will be added as we implement)
set(TRADER_SOURCES
    # trader/matching/order.cpp
    # trader/matching/order_book.cpp
    # trader/matching/market_manager.cpp
    # trader/providers/nasdaq/itch_handler.cpp
    # trader/strategy/market_maker.cpp
    # Add more as implemented
)

# When we have sources, create library:
# add_library(trader STATIC ${TRADER_SOURCES})
# target_link_libraries(trader PUBLIC core)
# target_include_directories(trader PUBLIC ${PROJECT_SOURCE_DIR}/include)

# Main executable (will be implemented later)
# add_executable(hft_engine main.cpp)
# target_link_libraries(hft_engine PRIVATE trader core)

message(STATUS "Trader library configuration ready (no sources yet)")
