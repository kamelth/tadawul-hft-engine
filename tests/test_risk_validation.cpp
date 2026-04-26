// Unit tests for the CPU baselines of risk computation and order validation.
// (The CUDA kernels are tested via the benchmark binary's correctness check,
// which compares GPU output bit-for-bit against these CPU implementations.)

#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include "trader/gpu/risk_gpu.h"
#include "trader/gpu/risk_cpu.h"
#include "trader/gpu/validation_gpu.h"
#include "trader/gpu/validation_cpu.h"

using namespace Trader::GPU;

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

#define ASSERT(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond)

#define ASSERT_EQ(a, b) \
    do { auto _a = (a); auto _b = (b); \
         if (_a != _b) throw std::runtime_error( \
             std::string("Expected ") + std::to_string(_b) + \
             " got " + std::to_string(_a)); } while(0)

// =============================================================================
// RISK TESTS
// =============================================================================

TEST(risk_zero_position) {
    HostRiskInputStorage is;
    HostRiskOutputStorage os;
    auto inp = is.view(1);
    auto out = os.view(1);

    inp.mid_price[0]    = 100000;  // $10.0000
    inp.best_bid[0]     = 99900;
    inp.best_ask[0]     = 100100;
    inp.max_position[0] = 1000;

    compute_risk_cpu(inp, out);

    ASSERT_EQ(out.exposure[0], 0);           // 0 * mid = 0
    ASSERT_EQ(out.unrealized_pnl[0], 0);     // no position
    ASSERT_EQ(out.liquidation_value[0], 0u);  // no long
    ASSERT_EQ(out.worst_case_loss[0], 0u);    // abs(0) * spread = 0
    ASSERT_EQ(out.position_usage_pct[0], 0);
    ASSERT_EQ(out.limit_breached[0], 0u);
    ASSERT_EQ(out.inventory_skew[0], 0);
}

TEST(risk_long_position) {
    HostRiskInputStorage is;
    HostRiskOutputStorage os;
    auto inp = is.view(1);
    auto out = os.view(1);

    inp.position[0]      = 500;
    inp.mid_price[0]     = 100000;  // $10.0000
    inp.best_bid[0]      = 99900;
    inp.best_ask[0]      = 100100;
    inp.avg_buy_price[0] = 99000;   // bought at $9.9000
    inp.max_position[0]  = 1000;

    compute_risk_cpu(inp, out);

    // Exposure: 500 * 100000 = 50,000,000
    ASSERT_EQ(out.exposure[0], 50000000);
    // Unrealized P&L: (100000 - 99000) * 500 = 500,000
    ASSERT_EQ(out.unrealized_pnl[0], 500000);
    // Liquidation: 500 * 99900 = 49,950,000
    ASSERT_EQ(out.liquidation_value[0], 49950000u);
    // Worst-case: 500 * (100100 - 99900) = 500 * 200 = 100,000
    ASSERT_EQ(out.worst_case_loss[0], 100000u);
    // Position usage: 500 * 10000 / 1000 = 5000 bps (50%)
    ASSERT_EQ(out.position_usage_pct[0], 5000);
    ASSERT_EQ(out.limit_breached[0], 0u);
    // Inventory skew: 500 * 10000 / 1000 = 5000
    ASSERT_EQ(out.inventory_skew[0], 5000);
}

TEST(risk_short_position) {
    HostRiskInputStorage is;
    HostRiskOutputStorage os;
    auto inp = is.view(1);
    auto out = os.view(1);

    inp.position[0]      = -300;
    inp.mid_price[0]     = 50000;
    inp.best_bid[0]      = 49800;
    inp.best_ask[0]      = 50200;
    inp.avg_buy_price[0] = 0;       // short, no avg buy
    inp.max_position[0]  = 300;

    compute_risk_cpu(inp, out);

    // Exposure: -300 * 50000 = -15,000,000
    ASSERT_EQ(out.exposure[0], -15000000);
    // Unrealized P&L: 0 (pos < 0)
    ASSERT_EQ(out.unrealized_pnl[0], 0);
    // Liquidation: 0 (pos not > 0)
    ASSERT_EQ(out.liquidation_value[0], 0u);
    // Worst-case: 300 * 400 = 120,000
    ASSERT_EQ(out.worst_case_loss[0], 120000u);
    // Position usage: 300 * 10000 / 300 = 10000 bps (100%)
    ASSERT_EQ(out.position_usage_pct[0], 10000);
    // At limit: abs(-300) >= 300
    ASSERT_EQ(out.limit_breached[0], 1u);
    // Inventory skew: -300 * 10000 / 300 = -10000
    ASSERT_EQ(out.inventory_skew[0], -10000);
}

TEST(risk_multi_symbol) {
    const uint32_t N = 3;
    HostRiskInputStorage is;
    HostRiskOutputStorage os;
    auto inp = is.view(N);
    auto out = os.view(N);

    // Symbol 0: long
    inp.position[0] = 100;  inp.mid_price[0] = 10000;
    inp.best_bid[0] = 9900; inp.best_ask[0] = 10100;
    inp.avg_buy_price[0] = 9500; inp.max_position[0] = 500;

    // Symbol 1: no position
    inp.mid_price[1] = 20000; inp.best_bid[1] = 19800; inp.best_ask[1] = 20200;
    inp.max_position[1] = 1000;

    // Symbol 2: short, at limit
    inp.position[2] = -200; inp.mid_price[2] = 5000;
    inp.best_bid[2] = 4900; inp.best_ask[2] = 5100;
    inp.max_position[2] = 200;

    compute_risk_cpu(inp, out);

    ASSERT_EQ(out.exposure[0], 1000000);    // 100 * 10000
    ASSERT_EQ(out.exposure[1], 0);
    ASSERT_EQ(out.exposure[2], -1000000);   // -200 * 5000

    ASSERT_EQ(out.limit_breached[0], 0u);
    ASSERT_EQ(out.limit_breached[1], 0u);
    ASSERT_EQ(out.limit_breached[2], 1u);
}

TEST(portfolio_risk_aggregates) {
    const uint32_t N = 2;
    HostRiskInputStorage is;
    HostRiskOutputStorage os;
    auto inp = is.view(N);
    auto out = os.view(N);

    inp.position[0] = 100;  inp.mid_price[0] = 10000;
    inp.best_bid[0] = 9900; inp.best_ask[0] = 10100;
    inp.avg_buy_price[0] = 9500; inp.max_position[0] = 500;

    inp.position[1] = -50;  inp.mid_price[1] = 20000;
    inp.best_bid[1] = 19800; inp.best_ask[1] = 20200;
    inp.max_position[1] = 1000;

    compute_risk_cpu(inp, out);
    auto pr = compute_portfolio_risk(inp, out);

    // exposure[0] = 100*10000 = 1,000,000  (positive)
    // exposure[1] = -50*20000 = -1,000,000 (negative)
    ASSERT_EQ(pr.total_exposure, 2000000);  // sum of abs
    ASSERT_EQ(pr.net_exposure, 0);          // they cancel
    ASSERT_EQ(pr.symbols_with_position, 2u);
    ASSERT_EQ(pr.symbols_at_limit, 0u);
}

// =============================================================================
// VALIDATION TESTS
// =============================================================================

TEST(validation_accept_valid_order) {
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(1);
    auto ctx    = vcs.view(1);
    auto result = vrs.view(1);

    orders.symbol_id[0] = 0;
    orders.side[0]      = 0;      // buy
    orders.price[0]     = 10000;
    orders.quantity[0]  = 100;

    ctx.best_bid[0]         = 9900;
    ctx.best_ask[0]         = 10100;
    ctx.mid_price[0]        = 10000;
    ctx.current_position[0] = 0;
    ctx.max_position[0]     = 1000;
    ctx.max_order_size[0]   = 500;
    ctx.price_band_pct[0]   = 500;  // 5%

    validate_orders_cpu(orders, ctx, result);

    ASSERT_EQ(result.valid[0], 1u);
    ASSERT_EQ(result.reject_reason[0], REJECT_NONE);
    ASSERT_EQ(result.post_trade_position[0], 100);
    ASSERT_EQ(result.notional_value[0], 1000000u);  // 10000 * 100
}

TEST(validation_reject_zero_price) {
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(1);
    auto ctx    = vcs.view(1);
    auto result = vrs.view(1);

    orders.symbol_id[0] = 0;
    orders.side[0]      = 0;
    orders.price[0]     = 0;       // zero price
    orders.quantity[0]  = 100;

    ctx.best_bid[0] = 9900; ctx.best_ask[0] = 10100;
    ctx.mid_price[0] = 10000; ctx.max_position[0] = 1000;

    validate_orders_cpu(orders, ctx, result);

    ASSERT_EQ(result.valid[0], 0u);
    ASSERT(result.reject_reason[0] & REJECT_ZERO_PRICE);
}

TEST(validation_reject_zero_qty) {
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(1);
    auto ctx    = vcs.view(1);
    auto result = vrs.view(1);

    orders.symbol_id[0] = 0;
    orders.side[0]      = 1;
    orders.price[0]     = 10000;
    orders.quantity[0]  = 0;       // zero quantity

    ctx.best_bid[0] = 9900; ctx.best_ask[0] = 10100;
    ctx.mid_price[0] = 10000; ctx.max_position[0] = 1000;

    validate_orders_cpu(orders, ctx, result);

    ASSERT_EQ(result.valid[0], 0u);
    ASSERT(result.reject_reason[0] & REJECT_ZERO_QTY);
}

TEST(validation_reject_no_market) {
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(1);
    auto ctx    = vcs.view(1);
    auto result = vrs.view(1);

    orders.symbol_id[0] = 0;
    orders.side[0]      = 0;
    orders.price[0]     = 10000;
    orders.quantity[0]  = 100;

    ctx.best_bid[0] = 0;    // no market
    ctx.best_ask[0] = 0;
    ctx.mid_price[0] = 10000; ctx.max_position[0] = 1000;

    validate_orders_cpu(orders, ctx, result);

    ASSERT_EQ(result.valid[0], 0u);
    ASSERT(result.reject_reason[0] & REJECT_NO_MARKET);
}

TEST(validation_reject_size_limit) {
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(1);
    auto ctx    = vcs.view(1);
    auto result = vrs.view(1);

    orders.symbol_id[0] = 0;
    orders.side[0]      = 0;
    orders.price[0]     = 10000;
    orders.quantity[0]  = 600;     // exceeds max_order_size

    ctx.best_bid[0] = 9900; ctx.best_ask[0] = 10100;
    ctx.mid_price[0] = 10000; ctx.max_position[0] = 10000;
    ctx.max_order_size[0] = 500;

    validate_orders_cpu(orders, ctx, result);

    ASSERT_EQ(result.valid[0], 0u);
    ASSERT(result.reject_reason[0] & REJECT_SIZE_LIMIT);
}

TEST(validation_reject_position_limit) {
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(1);
    auto ctx    = vcs.view(1);
    auto result = vrs.view(1);

    orders.symbol_id[0] = 0;
    orders.side[0]      = 0;       // buy
    orders.price[0]     = 10000;
    orders.quantity[0]  = 200;

    ctx.best_bid[0] = 9900; ctx.best_ask[0] = 10100;
    ctx.mid_price[0] = 10000;
    ctx.current_position[0] = 900;
    ctx.max_position[0]     = 1000;  // post-trade = 1100 > 1000
    ctx.max_order_size[0]   = 500;

    validate_orders_cpu(orders, ctx, result);

    ASSERT_EQ(result.valid[0], 0u);
    ASSERT(result.reject_reason[0] & REJECT_POSITION_LIMIT);
    ASSERT_EQ(result.post_trade_position[0], 1100);
}

TEST(validation_reject_price_band) {
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(1);
    auto ctx    = vcs.view(1);
    auto result = vrs.view(1);

    orders.symbol_id[0] = 0;
    orders.side[0]      = 0;
    orders.price[0]     = 12000;   // 20% above mid (2000 bps)
    orders.quantity[0]  = 100;

    ctx.best_bid[0] = 9900; ctx.best_ask[0] = 10100;
    ctx.mid_price[0]      = 10000;
    ctx.max_position[0]   = 10000;
    ctx.max_order_size[0] = 500;
    ctx.price_band_pct[0] = 500;   // 5% = 500 bps; 20% > 5%

    validate_orders_cpu(orders, ctx, result);

    ASSERT_EQ(result.valid[0], 0u);
    ASSERT(result.reject_reason[0] & REJECT_PRICE_BAND);
}

TEST(validation_sell_reduces_position) {
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(1);
    auto ctx    = vcs.view(1);
    auto result = vrs.view(1);

    orders.symbol_id[0] = 0;
    orders.side[0]      = 1;       // sell
    orders.price[0]     = 10000;
    orders.quantity[0]  = 200;

    ctx.best_bid[0] = 9900; ctx.best_ask[0] = 10100;
    ctx.mid_price[0] = 10000;
    ctx.current_position[0] = 500;
    ctx.max_position[0]     = 1000;
    ctx.max_order_size[0]   = 500;
    ctx.price_band_pct[0]   = 500;

    validate_orders_cpu(orders, ctx, result);

    ASSERT_EQ(result.valid[0], 1u);
    ASSERT_EQ(result.post_trade_position[0], 300);  // 500 - 200
}

TEST(validation_multiple_rejections) {
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(1);
    auto ctx    = vcs.view(1);
    auto result = vrs.view(1);

    orders.symbol_id[0] = 0;
    orders.side[0]      = 0;
    orders.price[0]     = 0;       // REJECT_ZERO_PRICE
    orders.quantity[0]  = 0;       // REJECT_ZERO_QTY

    ctx.best_bid[0] = 0;          // REJECT_NO_MARKET
    ctx.best_ask[0] = 0;
    ctx.mid_price[0] = 10000;
    ctx.max_position[0] = 1000;

    validate_orders_cpu(orders, ctx, result);

    ASSERT_EQ(result.valid[0], 0u);
    ASSERT(result.reject_reason[0] & REJECT_ZERO_PRICE);
    ASSERT(result.reject_reason[0] & REJECT_ZERO_QTY);
    ASSERT(result.reject_reason[0] & REJECT_NO_MARKET);
}

TEST(validation_batch_mixed) {
    const uint32_t N = 4;
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(N);
    auto ctx    = vcs.view(2);  // 2 symbols
    auto result = vrs.view(N);

    // Set up context for 2 symbols
    ctx.best_bid[0] = 9900;  ctx.best_ask[0] = 10100; ctx.mid_price[0] = 10000;
    ctx.current_position[0] = 0; ctx.max_position[0] = 1000;
    ctx.max_order_size[0] = 500; ctx.price_band_pct[0] = 500;

    ctx.best_bid[1] = 4900;  ctx.best_ask[1] = 5100; ctx.mid_price[1] = 5000;
    ctx.current_position[1] = 0; ctx.max_position[1] = 500;
    ctx.max_order_size[1] = 200; ctx.price_band_pct[1] = 300;

    // Order 0: valid buy on symbol 0
    orders.symbol_id[0] = 0; orders.side[0] = 0;
    orders.price[0] = 10000; orders.quantity[0] = 100;

    // Order 1: valid sell on symbol 1
    orders.symbol_id[1] = 1; orders.side[1] = 1;
    orders.price[1] = 5000; orders.quantity[1] = 50;

    // Order 2: rejected (size too big on symbol 1)
    orders.symbol_id[2] = 1; orders.side[2] = 0;
    orders.price[2] = 5000; orders.quantity[2] = 300;  // > 200

    // Order 3: rejected (zero price)
    orders.symbol_id[3] = 0; orders.side[3] = 0;
    orders.price[3] = 0; orders.quantity[3] = 100;

    validate_orders_cpu(orders, ctx, result);

    ASSERT_EQ(result.valid[0], 1u);
    ASSERT_EQ(result.valid[1], 1u);
    ASSERT_EQ(result.valid[2], 0u);
    ASSERT(result.reject_reason[2] & REJECT_SIZE_LIMIT);
    ASSERT_EQ(result.valid[3], 0u);
    ASSERT(result.reject_reason[3] & REJECT_ZERO_PRICE);
}

TEST(validation_summary) {
    const uint32_t N = 3;
    HostOrderBatchStorage obs;
    HostValidationContextStorage vcs;
    HostValidationResultStorage vrs;

    auto orders = obs.view(N);
    auto ctx    = vcs.view(1);
    auto result = vrs.view(N);

    ctx.best_bid[0] = 9900; ctx.best_ask[0] = 10100; ctx.mid_price[0] = 10000;
    ctx.current_position[0] = 0; ctx.max_position[0] = 1000;
    ctx.max_order_size[0] = 500; ctx.price_band_pct[0] = 500;

    // Valid
    orders.symbol_id[0] = 0; orders.side[0] = 0;
    orders.price[0] = 10000; orders.quantity[0] = 100;
    // Rejected: size
    orders.symbol_id[1] = 0; orders.side[1] = 0;
    orders.price[1] = 10000; orders.quantity[1] = 600;
    // Rejected: no market (bid=0)
    orders.symbol_id[2] = 0; orders.side[2] = 1;
    orders.price[2] = 10000; orders.quantity[2] = 100;

    // Hack: set bid to 0 for order 2 test — we can't per-order, so use separate ctx
    // Actually the ctx is per-symbol, so order 2 also uses symbol 0's ctx.
    // Let's make order 2 have zero price instead for a clear rejection.
    orders.price[2] = 0;

    validate_orders_cpu(orders, ctx, result);
    auto vs = compute_validation_summary(result);

    ASSERT_EQ(vs.total_orders, 3u);
    ASSERT_EQ(vs.accepted, 1u);
    ASSERT_EQ(vs.rejected, 2u);
    ASSERT_EQ(vs.reject_size, 1u);
    ASSERT_EQ(vs.total_notional, 1000000u);  // only accepted: 10000 * 100
}

// =============================================================================

int main() {
    std::cout << "============================================\n";
    std::cout << "Risk & Validation (CPU baseline) Tests\n";
    std::cout << "============================================\n";

    // Risk tests
    run_test_risk_zero_position();
    run_test_risk_long_position();
    run_test_risk_short_position();
    run_test_risk_multi_symbol();
    run_test_portfolio_risk_aggregates();

    // Validation tests
    run_test_validation_accept_valid_order();
    run_test_validation_reject_zero_price();
    run_test_validation_reject_zero_qty();
    run_test_validation_reject_no_market();
    run_test_validation_reject_size_limit();
    run_test_validation_reject_position_limit();
    run_test_validation_reject_price_band();
    run_test_validation_sell_reduces_position();
    run_test_validation_multiple_rejections();
    run_test_validation_batch_mixed();
    run_test_validation_summary();

    std::cout << "============================================\n";
    std::cout << "Results: " << tests_passed << "/" << tests_run << " passed\n";
    std::cout << "============================================\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
