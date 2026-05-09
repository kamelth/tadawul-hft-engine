#ifndef TRADER_PERFORMANCE_MARKET_IMPACT_H
#define TRADER_PERFORMANCE_MARKET_IMPACT_H

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include "trader/matching/market_manager.h"

namespace Trader {
namespace Performance {

/**
 * Per-symbol snapshot of order book state at a point in ITCH time.
 */
struct MarketSnapshot {
    uint64_t timestamp_ns;     // ITCH nanoseconds since midnight
    uint32_t symbol_id;
    std::string symbol_name;
    uint64_t best_bid;         // Price in $0.0001 units
    uint64_t best_ask;
    uint64_t spread;           // ask - bid
    uint64_t bid_volume;       // Volume at best bid
    uint64_t ask_volume;       // Volume at best ask
    size_t   bid_levels;       // Number of bid price levels
    size_t   ask_levels;       // Number of ask price levels
    size_t   total_orders;     // Total orders in book
    double   relative_spread_bps; // spread / mid × 10000 (basis points)
};

/**
 * Aggregate market quality metrics over an entire run.
 */
struct MarketQualitySummary {
    uint64_t sample_count = 0;

    // Spread statistics (in $0.0001 units)
    double avg_spread = 0.0;
    double median_spread = 0.0;
    uint64_t min_spread = 0;
    uint64_t max_spread = 0;

    // Relative spread (basis points)
    double avg_relative_spread_bps = 0.0;

    // Depth statistics
    double avg_bid_volume = 0.0;
    double avg_ask_volume = 0.0;
    double avg_bid_levels = 0.0;
    double avg_ask_levels = 0.0;
    double avg_total_orders = 0.0;
};

/**
 * MarketImpactCollector
 *
 * Samples order book state for ALL tracked symbols at fixed ITCH time
 * intervals. Produces CSV files for later comparison between baseline
 * (no strategy) and HFT (with strategy) runs.
 *
 * Usage:
 *   MarketImpactCollector collector(market_manager, 10'000'000'000ULL);
 *   // ... in the handler callback:
 *   collector.maybe_sample(timestamp_ns);
 *   // ... at end:
 *   collector.write_csv("results/baseline_snapshots.csv");
 *   collector.write_summary(std::cout);
 */
class MarketImpactCollector {
public:
    /**
     * @param market_manager  Reference to the market manager (to query all books)
     * @param sample_interval_ns  Sampling period in ITCH nanoseconds (default 10 seconds)
     */
    MarketImpactCollector(Matching::MarketManager& market_manager,
                          uint64_t sample_interval_ns = 10'000'000'000ULL)
        : market_manager_(market_manager)
        , sample_interval_ns_(sample_interval_ns)
        , next_sample_time_(0)
        , initialized_(false) {}

    /**
     * Call on every order book update. Internally decides whether to take a
     * snapshot based on the ITCH timestamp.
     */
    void maybe_sample(uint64_t timestamp_ns) {
        if (!initialized_) {
            // First call — set the first sample boundary
            next_sample_time_ = timestamp_ns + sample_interval_ns_;
            initialized_ = true;
            // Take an initial snapshot at time zero
            take_snapshot(timestamp_ns);
            return;
        }

        if (timestamp_ns >= next_sample_time_) {
            take_snapshot(timestamp_ns);
            // Advance to next boundary (skip if we fell behind)
            while (next_sample_time_ <= timestamp_ns) {
                next_sample_time_ += sample_interval_ns_;
            }
        }
    }

    /**
     * Number of snapshots collected.
     */
    size_t snapshot_count() const { return snapshots_.size(); }

    /**
     * Write all snapshots to a CSV file.
     */
    void write_csv(const std::string& path) const {
        std::ofstream f(path);
        if (!f.is_open()) {
            std::cerr << "Warning: could not write " << path << std::endl;
            return;
        }

        f << "timestamp_ns,symbol,best_bid,best_ask,spread,relative_spread_bps,"
             "bid_volume,ask_volume,bid_levels,ask_levels,total_orders\n";

        for (const auto& s : snapshots_) {
            f << s.timestamp_ns << ","
              << s.symbol_name << ","
              << s.best_bid << ","
              << s.best_ask << ","
              << s.spread << ","
              << s.relative_spread_bps << ","
              << s.bid_volume << ","
              << s.ask_volume << ","
              << s.bid_levels << ","
              << s.ask_levels << ","
              << s.total_orders << "\n";
        }

        std::cout << "Wrote " << path << " (" << snapshots_.size()
                  << " snapshots)" << std::endl;
    }

    /**
     * Compute summary statistics and write a text report.
     */
    MarketQualitySummary compute_summary() const {
        MarketQualitySummary summary;
        if (snapshots_.empty()) return summary;

        // Only consider snapshots with valid bid AND ask (non-zero)
        std::vector<uint64_t> spreads;
        double sum_spread = 0;
        double sum_rel_bps = 0;
        double sum_bid_vol = 0, sum_ask_vol = 0;
        double sum_bid_lev = 0, sum_ask_lev = 0;
        double sum_orders = 0;
        uint64_t min_spread = UINT64_MAX, max_spread = 0;

        for (const auto& s : snapshots_) {
            if (s.best_bid == 0 || s.best_ask == 0) continue;
            spreads.push_back(s.spread);
            sum_spread += s.spread;
            sum_rel_bps += s.relative_spread_bps;
            sum_bid_vol += s.bid_volume;
            sum_ask_vol += s.ask_volume;
            sum_bid_lev += s.bid_levels;
            sum_ask_lev += s.ask_levels;
            sum_orders += s.total_orders;
            if (s.spread < min_spread) min_spread = s.spread;
            if (s.spread > max_spread) max_spread = s.spread;
        }

        uint64_t n = spreads.size();
        if (n == 0) return summary;

        summary.sample_count = n;
        summary.avg_spread = sum_spread / n;
        summary.avg_relative_spread_bps = sum_rel_bps / n;
        summary.min_spread = min_spread;
        summary.max_spread = max_spread;
        summary.avg_bid_volume = sum_bid_vol / n;
        summary.avg_ask_volume = sum_ask_vol / n;
        summary.avg_bid_levels = sum_bid_lev / n;
        summary.avg_ask_levels = sum_ask_lev / n;
        summary.avg_total_orders = sum_orders / n;

        // Median spread
        std::sort(spreads.begin(), spreads.end());
        summary.median_spread = (n % 2 == 0)
            ? (spreads[n/2 - 1] + spreads[n/2]) / 2.0
            : spreads[n/2];

        return summary;
    }

    void write_summary(std::ostream& os) const {
        auto s = compute_summary();
        os << "Market Quality Summary (" << s.sample_count << " samples)\n";
        os << std::string(50, '-') << "\n";
        os << std::fixed << std::setprecision(4);
        os << "  Avg spread:       $" << (s.avg_spread / 10000.0) << "\n";
        os << "  Avg relative:     " << s.avg_relative_spread_bps << " bps\n";
        os << "  Median spread:    $" << (s.median_spread / 10000.0) << "\n";
        os << "  Min spread:       $" << (s.min_spread / 10000.0) << "\n";
        os << "  Max spread:       $" << (s.max_spread / 10000.0) << "\n";
        os << std::setprecision(1);
        os << "  Avg bid volume:   " << s.avg_bid_volume << " shares\n";
        os << "  Avg ask volume:   " << s.avg_ask_volume << " shares\n";
        os << "  Avg bid levels:   " << s.avg_bid_levels << "\n";
        os << "  Avg ask levels:   " << s.avg_ask_levels << "\n";
        os << "  Avg total orders: " << s.avg_total_orders << "\n";
    }

private:
    Matching::MarketManager& market_manager_;
    uint64_t sample_interval_ns_;
    uint64_t next_sample_time_;
    bool initialized_;
    std::vector<MarketSnapshot> snapshots_;

    void take_snapshot(uint64_t timestamp_ns) {
        const auto& registry = market_manager_.symbol_registry();
        for (const auto& sym : registry.get_all_symbols()) {
            const Matching::OrderBook* book =
                market_manager_.get_order_book(sym.symbol_id);
            if (!book) continue;

            Matching::OrderBookStats stats = book->get_stats();
            uint64_t mid = (stats.best_bid_price + stats.best_ask_price) / 2;
            double rel_bps = (mid > 0)
                ? (static_cast<double>(stats.spread) * 10000.0) / mid
                : 0.0;
            snapshots_.push_back({
                timestamp_ns,
                sym.symbol_id,
                sym.symbol_name,
                stats.best_bid_price,
                stats.best_ask_price,
                stats.spread,
                stats.best_bid_volume,
                stats.best_ask_volume,
                stats.bid_level_count,
                stats.ask_level_count,
                stats.total_orders,
                rel_bps
            });
        }
    }
};

} // namespace Performance
} // namespace Trader

#endif // TRADER_PERFORMANCE_MARKET_IMPACT_H
