// Unit tests for the CPU implementation of multi-symbol order book analytics.
// (The CUDA implementation is tested via the benchmark binary's correctness
// check, which compares GPU output bit-for-bit against this CPU version.)

#include <iostream>
#include <stdexcept>
#include <string>
#include "trader/gpu/order_book_gpu.h"
#include "trader/gpu/analytics_cpu.h"

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

// -----------------------------------------------------------------------------

TEST(empty_book_zero_outputs) {
    HostBookStorage bs;
    HostAnalyticsStorage as;
    auto book = bs.view(3);
    auto out  = as.view(3);

    // No bids or asks set, counts default to 0
    compute_analytics_cpu(book, out);

    for (uint32_t s = 0; s < 3; ++s) {
        ASSERT_EQ(out.best_bid[s], 0u);
        ASSERT_EQ(out.best_ask[s], 0u);
        ASSERT_EQ(out.spread[s],   0u);
        ASSERT_EQ(out.mid_price[s],0u);
        ASSERT_EQ(out.total_bid_volume[s], 0u);
        ASSERT_EQ(out.total_ask_volume[s], 0u);
        ASSERT_EQ(out.imbalance_x10000[s], 0);
    }
}

TEST(single_symbol_one_level_each_side) {
    HostBookStorage bs;
    HostAnalyticsStorage as;
    auto book = bs.view(1);

    book.bid_counts[0] = 1;
    book.ask_counts[0] = 1;
    book.bid_prices[0]  = 1000;   // best bid
    book.bid_volumes[0] = 500;
    book.ask_prices[0]  = 1010;   // best ask
    book.ask_volumes[0] = 300;

    auto out = as.view(1);
    compute_analytics_cpu(book, out);

    ASSERT_EQ(out.best_bid[0],         1000u);
    ASSERT_EQ(out.best_ask[0],         1010u);
    ASSERT_EQ(out.spread[0],           10u);
    ASSERT_EQ(out.mid_price[0],        1005u);
    ASSERT_EQ(out.total_bid_volume[0], 500u);
    ASSERT_EQ(out.total_ask_volume[0], 300u);
    ASSERT_EQ(out.vwap_bid_top10[0],   1000u);  // single level, vwap = price
    ASSERT_EQ(out.vwap_ask_top10[0],   1010u);
    // Imbalance: (500 - 300) * 10000 / 800 = 2500
    ASSERT_EQ(out.imbalance_x10000[0], 2500);
}

TEST(vwap_top10_excludes_lower_levels) {
    // 12 bid levels, top 10 should be averaged; 11th and 12th excluded.
    HostBookStorage bs;
    HostAnalyticsStorage as;
    auto book = bs.view(1);

    book.bid_counts[0] = 12;
    book.ask_counts[0] = 0;
    // Top 10: prices 100..91, all volume = 1 → vwap = (100+99+...+91)/10 = 95.5 → 95 (integer div)
    for (int l = 0; l < 12; ++l) {
        book.bid_prices[l]  = 100 - l;
        book.bid_volumes[l] = 1;
    }

    auto out = as.view(1);
    compute_analytics_cpu(book, out);

    // sum(prices 100..91) = 955, sum(volumes 10) = 10 → vwap = 95
    ASSERT_EQ(out.vwap_bid_top10[0], 95u);
    // Total volume covers all 12 levels
    ASSERT_EQ(out.total_bid_volume[0], 12u);
}

TEST(empty_one_side_means_zero_spread_and_mid) {
    HostBookStorage bs;
    HostAnalyticsStorage as;
    auto book = bs.view(1);

    // Only bids
    book.bid_counts[0] = 2;
    book.ask_counts[0] = 0;
    book.bid_prices[0] = 500; book.bid_volumes[0] = 10;
    book.bid_prices[1] = 499; book.bid_volumes[1] = 20;

    auto out = as.view(1);
    compute_analytics_cpu(book, out);

    ASSERT_EQ(out.best_bid[0],   500u);
    ASSERT_EQ(out.best_ask[0],   0u);
    ASSERT_EQ(out.spread[0],     0u);   // no two-sided market
    ASSERT_EQ(out.mid_price[0],  0u);
    ASSERT_EQ(out.total_bid_volume[0], 30u);
    // Imbalance: 30 / 30 * 10000 = 10000
    ASSERT_EQ(out.imbalance_x10000[0], 10000);
}

TEST(synthetic_generator_is_deterministic) {
    HostBookStorage bs1, bs2;
    auto book1 = bs1.view(50);
    auto book2 = bs2.view(50);

    generate_synthetic_books(book1, 12345);
    generate_synthetic_books(book2, 12345);

    // Same seed → identical books
    for (size_t i = 0; i < bs1.bid_prices.size(); ++i) {
        ASSERT_EQ(bs1.bid_prices[i],  bs2.bid_prices[i]);
        ASSERT_EQ(bs1.bid_volumes[i], bs2.bid_volumes[i]);
        ASSERT_EQ(bs1.ask_prices[i],  bs2.ask_prices[i]);
        ASSERT_EQ(bs1.ask_volumes[i], bs2.ask_volumes[i]);
    }
    for (size_t i = 0; i < bs1.bid_counts.size(); ++i) {
        ASSERT_EQ(bs1.bid_counts[i], bs2.bid_counts[i]);
        ASSERT_EQ(bs1.ask_counts[i], bs2.ask_counts[i]);
    }
}

TEST(synthetic_different_seeds_diverge) {
    HostBookStorage bs1, bs2;
    auto book1 = bs1.view(50);
    auto book2 = bs2.view(50);
    generate_synthetic_books(book1, 1);
    generate_synthetic_books(book2, 2);
    // At least one element should differ
    bool any_diff = false;
    for (size_t i = 0; i < bs1.bid_prices.size(); ++i) {
        if (bs1.bid_prices[i] != bs2.bid_prices[i]) { any_diff = true; break; }
    }
    ASSERT(any_diff);
}

TEST(aggregates_match_per_symbol_sums) {
    HostBookStorage bs;
    HostAnalyticsStorage as;
    auto book = bs.view(100);
    generate_synthetic_books(book, 7);
    auto out = as.view(100);
    compute_analytics_cpu(book, out);

    auto agg = compute_aggregates(out);

    // total_liquidity == sum of per-symbol total volumes
    uint64_t expected_liq = 0;
    uint64_t expected_with_market = 0;
    for (uint32_t s = 0; s < 100; ++s) {
        expected_liq += out.total_bid_volume[s] + out.total_ask_volume[s];
        if (out.best_bid[s] > 0 && out.best_ask[s] > 0) ++expected_with_market;
    }
    ASSERT_EQ(agg.total_liquidity, expected_liq);
    ASSERT_EQ(agg.total_symbols_with_market, expected_with_market);
    ASSERT(agg.avg_spread >= 0.0);
}

TEST(scales_to_many_symbols) {
    // Just make sure 5000 symbols runs without crashing
    HostBookStorage bs;
    HostAnalyticsStorage as;
    auto book = bs.view(5000);
    generate_synthetic_books(book, 99);
    auto out = as.view(5000);
    compute_analytics_cpu(book, out);
    ASSERT_EQ(out.num_symbols, 5000u);
}

int main() {
    std::cout << "====================================\n";
    std::cout << "GPU Analytics (CPU baseline) Tests\n";
    std::cout << "====================================\n";

    run_test_empty_book_zero_outputs();
    run_test_single_symbol_one_level_each_side();
    run_test_vwap_top10_excludes_lower_levels();
    run_test_empty_one_side_means_zero_spread_and_mid();
    run_test_synthetic_generator_is_deterministic();
    run_test_synthetic_different_seeds_diverge();
    run_test_aggregates_match_per_symbol_sums();
    run_test_scales_to_many_symbols();

    std::cout << "====================================\n";
    std::cout << "Results: " << tests_passed << "/" << tests_run << " passed\n";
    std::cout << "====================================\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
