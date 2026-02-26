#include <iostream>
#include <cassert>
#include <vector>
#include "trader/matching/order.h"
#include "trader/matching/symbol.h"
#include "trader/matching/level.h"
#include "trader/matching/order_book.h"
#include "trader/matching/market_manager.h"

using namespace Trader::Matching;
using namespace Core;

// Test counters
int tests_run = 0;
int tests_passed = 0;

#define TEST(name) \
    void test_##name(); \
    void run_test_##name() { \
        tests_run++; \
        std::cout << "Running test: " << #name << "..."; \
        try { \
            test_##name(); \
            tests_passed++; \
            std::cout << " PASSED" << std::endl; \
        } catch (const std::exception& e) { \
            std::cout << " FAILED: " << e.what() << std::endl; \
        } \
    } \
    void test_##name()

#define ASSERT(condition) \
    if (!(condition)) { \
        throw std::runtime_error("Assertion failed: " #condition); \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        throw std::runtime_error("Assertion failed: " #a " == " #b); \
    }

// ============================================================================
// Order tests
// ============================================================================

TEST(order_creation) {
    Timestamp ts(10, 30, 0, 0);
    Order order(1, 1000, 0, OrderSide::Buy, OrderType::Limit, 10050, 100, ts);

    ASSERT_EQ(order.order_id, 1ULL);
    ASSERT_EQ(order.original_order_id, 1000ULL);
    ASSERT_EQ(order.symbol_id, 0U);
    ASSERT(order.is_buy());
    ASSERT(!order.is_sell());
    ASSERT_EQ(order.price, 10050ULL);
    ASSERT_EQ(order.quantity, 100ULL);
    ASSERT_EQ(order.remaining_quantity, 100ULL);
    ASSERT_EQ(order.executed_quantity, 0ULL);
    ASSERT_EQ(order.state, OrderState::New);
}

TEST(order_execution) {
    Timestamp ts(10, 30, 0, 0);
    Order order(1, 1000, 0, OrderSide::Buy, OrderType::Limit, 10050, 100, ts);

    order.activate(ts);
    ASSERT(order.is_active());

    // Partial fill
    order.execute(30, ts);
    ASSERT_EQ(order.executed_quantity, 30ULL);
    ASSERT_EQ(order.remaining_quantity, 70ULL);
    ASSERT_EQ(order.state, OrderState::PartiallyFilled);
    ASSERT(order.is_active());

    // Complete fill
    order.execute(70, ts);
    ASSERT_EQ(order.executed_quantity, 100ULL);
    ASSERT_EQ(order.remaining_quantity, 0ULL);
    ASSERT(order.is_filled());
    ASSERT(!order.is_active());
}

TEST(order_cancellation) {
    Timestamp ts(10, 30, 0, 0);
    Order order(1, 1000, 0, OrderSide::Buy, OrderType::Limit, 10050, 100, ts);

    order.activate(ts);
    order.cancel(ts);

    ASSERT_EQ(order.state, OrderState::Cancelled);
    ASSERT(!order.is_active());
}

// ============================================================================
// Symbol registry tests
// ============================================================================

TEST(symbol_registry_basic) {
    SymbolRegistry registry;

    uint32_t aapl_id = registry.register_symbol(1, "AAPL");
    uint32_t msft_id = registry.register_symbol(2, "MSFT");

    ASSERT_EQ(registry.size(), 2UL);
    ASSERT(registry.has_symbol("AAPL"));
    ASSERT(registry.has_symbol("MSFT"));
    ASSERT(!registry.has_symbol("GOOGL"));

    const Symbol* aapl = registry.get_symbol_by_name("AAPL");
    ASSERT(aapl != nullptr);
    ASSERT_EQ(aapl->symbol_name, "AAPL");
    ASSERT_EQ(aapl->stock_locate, 1);
}

TEST(symbol_registry_deterministic) {
    SymbolRegistry registry;

    // Register in random order
    registry.register_symbol(3, "TSLA");
    registry.register_symbol(1, "AAPL");
    registry.register_symbol(2, "MSFT");

    // Finalize for deterministic ordering (alphabetical)
    registry.finalize_deterministic_ordering();

    // Check IDs are now alphabetical
    uint32_t aapl_id, msft_id, tsla_id;
    ASSERT(registry.get_symbol_id("AAPL", aapl_id));
    ASSERT(registry.get_symbol_id("MSFT", msft_id));
    ASSERT(registry.get_symbol_id("TSLA", tsla_id));

    ASSERT(aapl_id < msft_id);
    ASSERT(msft_id < tsla_id);
}

// ============================================================================
// Price level tests
// ============================================================================

TEST(level_basic_operations) {
    Level level(10050);

    ASSERT_EQ(level.price(), 10050ULL);
    ASSERT_EQ(level.total_volume(), 0ULL);
    ASSERT(level.empty());

    Timestamp ts(10, 30, 0, 0);
    Order order1(1, 1000, 0, OrderSide::Buy, OrderType::Limit, 10050, 100, ts);
    Order order2(2, 1001, 0, OrderSide::Buy, OrderType::Limit, 10050, 50, ts);

    level.add_order(&order1);
    ASSERT_EQ(level.total_volume(), 100ULL);
    ASSERT_EQ(level.order_count(), 1UL);

    level.add_order(&order2);
    ASSERT_EQ(level.total_volume(), 150ULL);
    ASSERT_EQ(level.order_count(), 2UL);

    // Check FIFO order
    ASSERT_EQ(level.front()->order_id, 1ULL);
    ASSERT_EQ(level.back()->order_id, 2ULL);
}

TEST(level_matching_fifo) {
    Level level(10050);
    Timestamp ts(10, 30, 0, 0);

    // Add three orders to level (FIFO queue)
    Order order1(1, 1000, 0, OrderSide::Buy, OrderType::Limit, 10050, 100, ts);
    Order order2(2, 1001, 0, OrderSide::Buy, OrderType::Limit, 10050, 50, ts);
    Order order3(3, 1002, 0, OrderSide::Buy, OrderType::Limit, 10050, 75, ts);

    level.add_order(&order1);
    level.add_order(&order2);
    level.add_order(&order3);

    // Incoming sell order for 120 shares
    Order incoming(4, 2000, 0, OrderSide::Sell, OrderType::Limit, 10050, 120, ts);

    std::vector<Execution> executions;
    uint64_t matched = level.match(&incoming, executions, ts);

    // Should match 100 from order1, then 20 from order2
    ASSERT_EQ(matched, 120ULL);
    ASSERT_EQ(executions.size(), 2UL);
    ASSERT_EQ(executions[0].match_order_id, 1ULL);  // First in queue
    ASSERT_EQ(executions[0].execution_quantity, 100ULL);
    ASSERT_EQ(executions[1].match_order_id, 2ULL);  // Second in queue
    ASSERT_EQ(executions[1].execution_quantity, 20ULL);

    // Check remaining
    ASSERT(order1.is_filled());
    ASSERT_EQ(order2.remaining_quantity, 30ULL);
    ASSERT_EQ(order3.remaining_quantity, 75ULL);
}

// ============================================================================
// Order book tests
// ============================================================================

TEST(order_book_add_orders) {
    OrderBook book(0);
    Timestamp ts(10, 30, 0, 0);

    // Add buy order
    Order buy1(1, 1000, 0, OrderSide::Buy, OrderType::Limit, 10050, 100, ts);
    auto execs = book.add_order(&buy1, ts);
    ASSERT(execs.empty());  // No match yet

    // Add sell order (no cross)
    Order sell1(2, 1001, 0, OrderSide::Sell, OrderType::Limit, 10100, 50, ts);
    execs = book.add_order(&sell1, ts);
    ASSERT(execs.empty());  // No match

    // Check best bid/ask
    ASSERT_EQ(book.best_bid_price(), 10050ULL);
    ASSERT_EQ(book.best_ask_price(), 10100ULL);

    OrderBookStats stats = book.get_stats();
    ASSERT_EQ(stats.spread, 50ULL);
}

TEST(order_book_matching_simple) {
    OrderBook book(0);
    Timestamp ts(10, 30, 0, 0);

    // Add buy order at 10050
    Order buy1(1, 1000, 0, OrderSide::Buy, OrderType::Limit, 10050, 100, ts);
    book.add_order(&buy1, ts);

    // Add sell order at 10050 (crosses)
    Order sell1(2, 1001, 0, OrderSide::Sell, OrderType::Limit, 10050, 50, ts);
    auto execs = book.add_order(&sell1, ts);

    // Should have one execution
    ASSERT_EQ(execs.size(), 1UL);
    ASSERT_EQ(execs[0].execution_quantity, 50ULL);
    ASSERT_EQ(execs[0].execution_price, 10050ULL);

    // Buy order partially filled
    ASSERT_EQ(buy1.executed_quantity, 50ULL);
    ASSERT_EQ(buy1.remaining_quantity, 50ULL);

    // Sell order fully filled
    ASSERT(sell1.is_filled());
}

TEST(order_book_price_time_priority) {
    OrderBook book(0);
    Timestamp ts(10, 30, 0, 0);

    // Add multiple buy orders at different prices
    Order buy1(1, 1000, 0, OrderSide::Buy, OrderType::Limit, 10050, 100, ts);
    Order buy2(2, 1001, 0, OrderSide::Buy, OrderType::Limit, 10060, 50, ts);  // Better price
    Order buy3(3, 1002, 0, OrderSide::Buy, OrderType::Limit, 10060, 75, ts);  // Same price, later time

    book.add_order(&buy1, ts);
    book.add_order(&buy2, ts);
    book.add_order(&buy3, ts);

    // Best bid should be 10060
    ASSERT_EQ(book.best_bid_price(), 10060ULL);

    // Add sell order at 10060
    Order sell1(4, 2000, 0, OrderSide::Sell, OrderType::Limit, 10060, 100, ts);
    auto execs = book.add_order(&sell1, ts);

    // Should match with best price first, then time priority
    ASSERT_EQ(execs.size(), 2UL);
    ASSERT_EQ(execs[0].match_order_id, 2ULL);  // buy2 (better price, earlier time)
    ASSERT_EQ(execs[0].execution_quantity, 50ULL);
    ASSERT_EQ(execs[1].match_order_id, 3ULL);  // buy3 (same price, later time)
    ASSERT_EQ(execs[1].execution_quantity, 50ULL);
}

TEST(order_book_cancel_order) {
    OrderBook book(0);
    Timestamp ts(10, 30, 0, 0);

    Order buy1(1, 1000, 0, OrderSide::Buy, OrderType::Limit, 10050, 100, ts);
    book.add_order(&buy1, ts);

    ASSERT_EQ(book.order_count(), 1UL);

    bool cancelled = book.cancel_order(1, ts);
    ASSERT(cancelled);
    ASSERT_EQ(buy1.state, OrderState::Cancelled);
    ASSERT_EQ(book.order_count(), 0UL);
}

TEST(order_book_market_depth) {
    OrderBook book(0);
    Timestamp ts(10, 30, 0, 0);

    // Add multiple levels
    Order buy1(1, 1000, 0, OrderSide::Buy, OrderType::Limit, 10050, 100, ts);
    Order buy2(2, 1001, 0, OrderSide::Buy, OrderType::Limit, 10040, 50, ts);
    Order buy3(3, 1002, 0, OrderSide::Buy, OrderType::Limit, 10030, 75, ts);

    Order sell1(4, 2000, 0, OrderSide::Sell, OrderType::Limit, 10060, 80, ts);
    Order sell2(5, 2001, 0, OrderSide::Sell, OrderType::Limit, 10070, 60, ts);

    book.add_order(&buy1, ts);
    book.add_order(&buy2, ts);
    book.add_order(&buy3, ts);
    book.add_order(&sell1, ts);
    book.add_order(&sell2, ts);

    // Get depth
    std::vector<std::pair<uint64_t, uint64_t>> bids, asks;
    book.get_depth(3, bids, asks);

    // Bids should be sorted highest to lowest
    ASSERT_EQ(bids.size(), 3UL);
    ASSERT_EQ(bids[0].first, 10050ULL);
    ASSERT_EQ(bids[1].first, 10040ULL);
    ASSERT_EQ(bids[2].first, 10030ULL);

    // Asks should be sorted lowest to highest
    ASSERT_EQ(asks.size(), 2UL);
    ASSERT_EQ(asks[0].first, 10060ULL);
    ASSERT_EQ(asks[1].first, 10070ULL);
}

// ============================================================================
// Market manager tests
// ============================================================================

TEST(market_manager_basic) {
    MarketManager manager;

    // Register symbols
    uint32_t aapl_id = manager.symbol_registry().register_symbol(1, "AAPL");
    uint32_t msft_id = manager.symbol_registry().register_symbol(2, "MSFT");

    Timestamp ts(10, 30, 0, 0);

    // Add order for AAPL
    auto execs = manager.add_order(aapl_id, OrderSide::Buy, OrderType::Limit,
                                    10050, 100, 1000, ts);

    ASSERT(execs.empty());

    // Check order ID is deterministic (starts from 1)
    Order* order = manager.get_order(1);
    ASSERT(order != nullptr);
    ASSERT_EQ(order->order_id, 1ULL);
    ASSERT_EQ(order->symbol_id, aapl_id);
}

TEST(market_manager_multi_symbol) {
    MarketManager manager;

    uint32_t aapl_id = manager.symbol_registry().register_symbol(1, "AAPL");
    uint32_t msft_id = manager.symbol_registry().register_symbol(2, "MSFT");

    Timestamp ts(10, 30, 0, 0);

    // Add orders for different symbols
    manager.add_order(aapl_id, OrderSide::Buy, OrderType::Limit, 10050, 100, 1000, ts);
    manager.add_order(msft_id, OrderSide::Buy, OrderType::Limit, 20050, 50, 1001, ts);

    // Check each symbol has its own order book
    OrderBook* aapl_book = manager.get_order_book(aapl_id);
    OrderBook* msft_book = manager.get_order_book(msft_id);

    ASSERT(aapl_book != nullptr);
    ASSERT(msft_book != nullptr);
    ASSERT_EQ(aapl_book->best_bid_price(), 10050ULL);
    ASSERT_EQ(msft_book->best_bid_price(), 20050ULL);
}

TEST(market_manager_deterministic_ids) {
    MarketManager manager;

    uint32_t aapl_id = manager.symbol_registry().register_symbol(1, "AAPL");
    Timestamp ts(10, 30, 0, 0);

    // Add three orders
    manager.add_order(aapl_id, OrderSide::Buy, OrderType::Limit, 10050, 100, 1000, ts);
    manager.add_order(aapl_id, OrderSide::Sell, OrderType::Limit, 10100, 50, 1001, ts);
    manager.add_order(aapl_id, OrderSide::Buy, OrderType::Limit, 10060, 75, 1002, ts);

    // Check IDs are sequential
    ASSERT(manager.get_order(1) != nullptr);
    ASSERT(manager.get_order(2) != nullptr);
    ASSERT(manager.get_order(3) != nullptr);
    ASSERT(manager.get_order(4) == nullptr);
}

// ============================================================================
// Main test runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Order Book Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    // Order tests
    std::cout << "\n--- Order Tests ---" << std::endl;
    run_test_order_creation();
    run_test_order_execution();
    run_test_order_cancellation();

    // Symbol registry tests
    std::cout << "\n--- Symbol Registry Tests ---" << std::endl;
    run_test_symbol_registry_basic();
    run_test_symbol_registry_deterministic();

    // Price level tests
    std::cout << "\n--- Price Level Tests ---" << std::endl;
    run_test_level_basic_operations();
    run_test_level_matching_fifo();

    // Order book tests
    std::cout << "\n--- Order Book Tests ---" << std::endl;
    run_test_order_book_add_orders();
    run_test_order_book_matching_simple();
    run_test_order_book_price_time_priority();
    run_test_order_book_cancel_order();
    run_test_order_book_market_depth();

    // Market manager tests
    std::cout << "\n--- Market Manager Tests ---" << std::endl;
    run_test_market_manager_basic();
    run_test_market_manager_multi_symbol();
    run_test_market_manager_deterministic_ids();

    // Summary
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Tests run:    " << tests_run << std::endl;
    std::cout << "Tests passed: " << tests_passed << std::endl;
    std::cout << "Tests failed: " << (tests_run - tests_passed) << std::endl;
    std::cout << "========================================" << std::endl;

    if (tests_passed == tests_run) {
        std::cout << "\n✓ All tests passed!" << std::endl;
        return 0;
    } else {
        std::cout << "\n✗ Some tests failed!" << std::endl;
        return 1;
    }
}
