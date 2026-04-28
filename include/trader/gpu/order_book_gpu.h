#ifndef TRADER_GPU_ORDER_BOOK_GPU_H
#define TRADER_GPU_ORDER_BOOK_GPU_H

#include <cstdint>
#include <vector>
#include <random>

namespace Trader {
namespace GPU {

/**
 * Maximum price levels per side (per symbol).
 *
 * Real order books can have hundreds of levels, but most analytics only
 * need the top N. 32 is a power-of-2 (good for GPU reductions) and
 * captures the meaningful liquidity in practice.
 */
constexpr uint32_t MAX_LEVELS_PER_SIDE = 32;

/**
 * Multi-Symbol Order Book (Structure of Arrays)
 *
 * Layout designed for GPU coalesced memory access:
 * - bid_prices[s * MAX_LEVELS + l]   = price at level l for symbol s
 * - bid_volumes[s * MAX_LEVELS + l]  = volume at level l for symbol s
 * - bid_counts[s]                    = number of active bid levels for symbol s
 * - same for asks
 *
 * The same struct is used by both CPU and GPU code; for GPU, the pointers
 * point to device memory.
 */
struct MultiSymbolBook {
    uint32_t  num_symbols;     // N — number of symbols in this batch
    uint32_t  max_levels;      // Always MAX_LEVELS_PER_SIDE (carried for clarity)

    uint64_t* bid_prices;      // [N * MAX_LEVELS_PER_SIDE]
    uint64_t* bid_volumes;     // [N * MAX_LEVELS_PER_SIDE]
    uint32_t* bid_counts;      // [N]

    uint64_t* ask_prices;      // [N * MAX_LEVELS_PER_SIDE]
    uint64_t* ask_volumes;     // [N * MAX_LEVELS_PER_SIDE]
    uint32_t* ask_counts;      // [N]
};

/**
 * Per-symbol analytics output (Structure of Arrays).
 *
 * Each array has one entry per symbol.
 */
struct SymbolAnalytics {
    uint32_t  num_symbols;

    uint64_t* best_bid;            // [N] best (highest) bid price
    uint64_t* best_ask;            // [N] best (lowest) ask price
    uint64_t* spread;              // [N] best_ask - best_bid (0 if either side empty)
    uint64_t* mid_price;           // [N] (best_bid + best_ask) / 2

    uint64_t* total_bid_volume;    // [N] sum of all bid level volumes
    uint64_t* total_ask_volume;    // [N] sum of all ask level volumes

    uint64_t* vwap_bid_top10;      // [N] volume-weighted avg price, top 10 bid levels
    uint64_t* vwap_ask_top10;      // [N] volume-weighted avg price, top 10 ask levels

    int64_t*  imbalance_x10000;    // [N] (bid_vol - ask_vol) * 10000 / (bid_vol + ask_vol)
    uint64_t* volatility_ticks;   // [N] rolling std-dev of mid-price changes (in ticks)
};

/**
 * Aggregate market-wide metrics (single values).
 */
struct MarketAggregates {
    uint64_t total_liquidity;       // sum across all symbols of (bid_vol + ask_vol)
    uint64_t total_symbols_with_market;  // count of symbols with both bid and ask
    double   avg_spread;            // averaged across symbols with valid market
    uint64_t max_spread;
    uint64_t min_spread;
};

// =============================================================================
// Host-side helpers (CPU memory allocation, synthetic data)
// =============================================================================

/**
 * Allocate a MultiSymbolBook in host memory (std::vector backed).
 * The returned book's pointers are owned by the provided storage object.
 */
struct HostBookStorage {
    std::vector<uint64_t> bid_prices;
    std::vector<uint64_t> bid_volumes;
    std::vector<uint32_t> bid_counts;
    std::vector<uint64_t> ask_prices;
    std::vector<uint64_t> ask_volumes;
    std::vector<uint32_t> ask_counts;

    MultiSymbolBook view(uint32_t num_symbols) {
        const size_t levels_total = static_cast<size_t>(num_symbols) * MAX_LEVELS_PER_SIDE;
        bid_prices.assign(levels_total, 0);
        bid_volumes.assign(levels_total, 0);
        bid_counts.assign(num_symbols, 0);
        ask_prices.assign(levels_total, 0);
        ask_volumes.assign(levels_total, 0);
        ask_counts.assign(num_symbols, 0);

        MultiSymbolBook book;
        book.num_symbols = num_symbols;
        book.max_levels  = MAX_LEVELS_PER_SIDE;
        book.bid_prices  = bid_prices.data();
        book.bid_volumes = bid_volumes.data();
        book.bid_counts  = bid_counts.data();
        book.ask_prices  = ask_prices.data();
        book.ask_volumes = ask_volumes.data();
        book.ask_counts  = ask_counts.data();
        return book;
    }
};

struct HostAnalyticsStorage {
    std::vector<uint64_t> best_bid;
    std::vector<uint64_t> best_ask;
    std::vector<uint64_t> spread;
    std::vector<uint64_t> mid_price;
    std::vector<uint64_t> total_bid_volume;
    std::vector<uint64_t> total_ask_volume;
    std::vector<uint64_t> vwap_bid_top10;
    std::vector<uint64_t> vwap_ask_top10;
    std::vector<int64_t>  imbalance_x10000;
    std::vector<uint64_t> volatility_ticks;

    SymbolAnalytics view(uint32_t num_symbols) {
        best_bid.assign(num_symbols, 0);
        best_ask.assign(num_symbols, 0);
        spread.assign(num_symbols, 0);
        mid_price.assign(num_symbols, 0);
        total_bid_volume.assign(num_symbols, 0);
        total_ask_volume.assign(num_symbols, 0);
        vwap_bid_top10.assign(num_symbols, 0);
        vwap_ask_top10.assign(num_symbols, 0);
        imbalance_x10000.assign(num_symbols, 0);
        volatility_ticks.assign(num_symbols, 0);

        SymbolAnalytics a;
        a.num_symbols       = num_symbols;
        a.best_bid          = best_bid.data();
        a.best_ask          = best_ask.data();
        a.spread            = spread.data();
        a.mid_price         = mid_price.data();
        a.total_bid_volume  = total_bid_volume.data();
        a.total_ask_volume  = total_ask_volume.data();
        a.vwap_bid_top10    = vwap_bid_top10.data();
        a.vwap_ask_top10    = vwap_ask_top10.data();
        a.imbalance_x10000  = imbalance_x10000.data();
        a.volatility_ticks  = volatility_ticks.data();
        return a;
    }
};

/**
 * Generate synthetic order book data deterministically (seeded RNG).
 *
 * For each symbol, generates:
 * - A "fair" mid-price chosen uniformly in [10000, 5000000] ($1.00 - $500.00)
 * - Bid levels descending from mid - tick_size, with random volumes
 * - Ask levels ascending from mid + tick_size, with random volumes
 * - Some symbols may have empty sides (1% probability) to test edge cases
 *
 * Same seed produces identical books — required for deterministic benchmarks.
 */
inline void generate_synthetic_books(MultiSymbolBook& book, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> mid_dist(10000ULL, 5'000'000ULL);
    std::uniform_int_distribution<uint64_t> vol_dist(100ULL, 10000ULL);
    std::uniform_int_distribution<uint32_t> level_dist(8, MAX_LEVELS_PER_SIDE);
    std::uniform_int_distribution<uint64_t> tick_dist(1ULL, 100ULL);
    std::uniform_int_distribution<uint32_t> empty_roll(0, 99);

    for (uint32_t s = 0; s < book.num_symbols; ++s) {
        const uint64_t mid = mid_dist(rng);
        const uint64_t tick = tick_dist(rng);

        // 1% chance of an empty bid side
        const uint32_t bid_levels = (empty_roll(rng) == 0) ? 0 : level_dist(rng);
        // 1% chance of an empty ask side
        const uint32_t ask_levels = (empty_roll(rng) == 0) ? 0 : level_dist(rng);

        book.bid_counts[s] = bid_levels;
        book.ask_counts[s] = ask_levels;

        // Bids: descending from (mid - tick)
        for (uint32_t l = 0; l < bid_levels; ++l) {
            const size_t idx = static_cast<size_t>(s) * MAX_LEVELS_PER_SIDE + l;
            book.bid_prices[idx]  = (mid > (l + 1) * tick) ? mid - (l + 1) * tick : 1;
            book.bid_volumes[idx] = vol_dist(rng);
        }

        // Asks: ascending from (mid + tick)
        for (uint32_t l = 0; l < ask_levels; ++l) {
            const size_t idx = static_cast<size_t>(s) * MAX_LEVELS_PER_SIDE + l;
            book.ask_prices[idx]  = mid + (l + 1) * tick;
            book.ask_volumes[idx] = vol_dist(rng);
        }
    }
}

// =============================================================================
// Snapshot from real OrderBook (ITCH-built) into SoA layout
// =============================================================================

} // namespace GPU
} // namespace Trader

#endif // TRADER_GPU_ORDER_BOOK_GPU_H
