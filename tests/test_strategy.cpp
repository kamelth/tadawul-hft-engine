#include <iostream>
#include <cassert>
#include "trader/strategy/position.h"
#include "trader/strategy/market_maker.h"
#include "trader/matching/order_book.h"

using namespace Trader::Strategy;
using namespace Trader::Matching;

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
        } catch (...) { \
            std::cout << " FAILED: unknown exception" << std::endl; \
        } \
    } \
    void test_##name()

#define ASSERT(condition) \
    if (!(condition)) { \
        throw std::runtime_error("Assertion failed: " #condition); \
    }

TEST(position_initial_state) {
    Position pos;
    ASSERT(pos.get_shares() == 0);
    ASSERT(pos.get_total_bought() == 0);
    ASSERT(pos.get_total_sold() == 0);
}

TEST(position_buy_shares) {
    Position pos;
    pos.add_shares(100, 50000);  // 100 shares @ $5.0000
    ASSERT(pos.get_shares() == 100);
    ASSERT(pos.get_total_bought() == 100);
    ASSERT(pos.get_avg_buy_price() == 50000);
}

TEST(position_buy_then_sell) {
    Position pos;
    pos.add_shares(100, 50000);  // Buy 100 @ $5.00
    pos.remove_shares(50, 52000);  // Sell 50 @ $5.20

    ASSERT(pos.get_shares() == 50);  // 50 remaining
    ASSERT(pos.get_total_sold() == 50);

    // Realized PnL = (52000 - 50000) * 50 = 2000 * 50 = 100000 units = $10.00
    int64_t realized = pos.calculate_realized_pnl();
    ASSERT(realized == 100000);  // $10.00 in units of $0.0001
}

TEST(position_unrealized_pnl) {
    Position pos;
    pos.add_shares(100, 50000);  // Buy 100 @ $5.00

    // Current price $5.50
    int64_t unrealized = pos.calculate_unrealized_pnl(55000);
    // (55000 - 50000) * 100 = 5000 * 100 = 500000 units = $50.00
    ASSERT(unrealized == 500000);
}

TEST(position_weighted_average) {
    Position pos;
    pos.add_shares(100, 50000);  // 100 @ $5.00
    pos.add_shares(100, 60000);  // 100 @ $6.00

    // Average = (100*5.00 + 100*6.00) / 200 = 5.50
    ASSERT(pos.get_avg_buy_price() == 55000);
    ASSERT(pos.get_shares() == 200);
}

TEST(position_manager_multiple_symbols) {
    PositionManager pm;

    Position& pos1 = pm.get_position(1);
    pos1.add_shares(100, 50000);

    Position& pos2 = pm.get_position(2);
    pos2.add_shares(200, 60000);

    // Use the const version which returns a pointer
    const PositionManager& pm_const = pm;
    const Position* pos1_ptr = pm_const.get_position(1);
    const Position* pos2_ptr = pm_const.get_position(2);
    ASSERT(pos1_ptr && pos1_ptr->get_shares() == 100);
    ASSERT(pos2_ptr && pos2_ptr->get_shares() == 200);
}

TEST(position_manager_total_pnl) {
    PositionManager pm;

    Position& pos1 = pm.get_position(1);
    pos1.add_shares(100, 50000);
    pos1.remove_shares(50, 52000);  // (52000-50000)*50 = 100000 = $10.00

    Position& pos2 = pm.get_position(2);
    pos2.add_shares(100, 60000);
    pos2.remove_shares(50, 61000);  // (61000-60000)*50 = 50000 = $5.00

    int64_t total_realized = pm.calculate_total_realized_pnl();
    ASSERT(total_realized == 150000);  // $10.00 + $5.00 = $15.00
}

TEST(market_maker_generate_quotes) {
    MarketMaker::Params params;
    params.spread_ticks = 10;      // $0.0010
    params.quote_size = 100;
    params.max_position = 1000;
    params.enable_inventory_skew = false;  // Disable for simple test

    MarketMaker mm(params);

    // First, buy some shares so we can test both bid and ask generation
    mm.on_order_filled(1, 1, OrderSide::Buy, 50000, 500, Core::Timestamp(0));

    OrderBookStats stats;
    stats.best_bid_price = 50000;  // $5.0000
    stats.best_ask_price = 50010;  // $5.0010
    stats.best_bid_volume = 1000;
    stats.best_ask_volume = 1000;
    stats.spread = 10;

    mm.on_order_book_update(1, stats, Core::Timestamp(0));

    // Get pending orders
    auto orders = mm.get_pending_orders();

    // Should generate 2 orders (bid and ask) since we now have inventory
    ASSERT(orders.size() == 2);

    // Find bid and ask orders
    bool found_bid = false;
    bool found_ask = false;

    for (const auto& order : orders) {
        if (order.side == OrderSide::Buy) {
            ASSERT(order.quantity == 100);
            ASSERT(order.price < stats.best_ask_price);  // Must not cross spread
            found_bid = true;
        } else {
            ASSERT(order.quantity == 100);
            ASSERT(order.price > stats.best_bid_price);  // Must not cross spread
            found_ask = true;
        }
    }

    ASSERT(found_bid);
    ASSERT(found_ask);
}

TEST(market_maker_position_limits) {
    MarketMaker::Params params;
    params.spread_ticks = 10;
    params.quote_size = 100;
    params.max_position = 1000;

    MarketMaker mm(params);

    // Buy up to max position
    for (int i = 0; i < 10; ++i) {
        mm.on_order_filled(i, 1, OrderSide::Buy, 50000, 100, Core::Timestamp(0));
    }

    ASSERT(mm.get_position(1) == 1000);  // At max position

    // Now update order book - should not generate buy order
    OrderBookStats stats;
    stats.best_bid_price = 50000;
    stats.best_ask_price = 50010;
    stats.best_bid_volume = 1000;
    stats.best_ask_volume = 1000;

    mm.on_order_book_update(1, stats, Core::Timestamp(0));
    auto orders = mm.get_pending_orders();

    // Should only generate sell order (not buy)
    ASSERT(orders.size() == 1);
    ASSERT(orders[0].side == OrderSide::Sell);
}

TEST(market_maker_pnl_tracking) {
    MarketMaker::Params params;
    params.spread_ticks = 10;
    params.quote_size = 100;
    params.max_position = 1000;

    MarketMaker mm(params);

    // Buy 100 @ $5.00
    mm.on_order_filled(1, 1, OrderSide::Buy, 50000, 100, Core::Timestamp(0));

    // Sell 100 @ $5.10
    mm.on_order_filled(2, 1, OrderSide::Sell, 51000, 100, Core::Timestamp(0));

    // PnL = (51000 - 50000) * 100 = 1000 * 100 = 100000 units = $10.00
    int64_t pnl = mm.get_total_pnl();
    ASSERT(pnl == 100000);
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Strategy Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    // Position tests
    run_test_position_initial_state();
    run_test_position_buy_shares();
    run_test_position_buy_then_sell();
    run_test_position_unrealized_pnl();
    run_test_position_weighted_average();

    // Position manager tests
    run_test_position_manager_multiple_symbols();
    run_test_position_manager_total_pnl();

    // Market maker tests
    run_test_market_maker_generate_quotes();
    run_test_market_maker_position_limits();
    run_test_market_maker_pnl_tracking();

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << tests_passed << "/" << tests_run << " tests passed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (tests_run == tests_passed) ? 0 : 1;
}
