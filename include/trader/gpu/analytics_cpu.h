#ifndef TRADER_GPU_ANALYTICS_CPU_H
#define TRADER_GPU_ANALYTICS_CPU_H

#include <cstdint>
#include <algorithm>
#include "trader/gpu/order_book_gpu.h"

namespace Trader {
namespace GPU {

/**
 * CPU implementation of multi-symbol order book analytics.
 *
 * Serial loop over symbols.  This is the baseline that the GPU kernel
 * must beat on throughput / scalability.  Each symbol's analytics are
 * independent, which is why this workload parallelizes well.
 *
 * @param book   Input order books (host memory).
 * @param out    Output analytics (host memory, same num_symbols as book).
 */
inline void compute_analytics_cpu(const MultiSymbolBook& book,
                                  SymbolAnalytics& out)
{
    const uint32_t n = book.num_symbols;

    for (uint32_t s = 0; s < n; ++s) {
        const uint32_t bid_n = book.bid_counts[s];
        const uint32_t ask_n = book.ask_counts[s];
        const size_t base = static_cast<size_t>(s) * MAX_LEVELS_PER_SIDE;

        // ---- Best bid/ask, spread, mid ----
        const uint64_t best_bid = (bid_n > 0) ? book.bid_prices[base] : 0;
        const uint64_t best_ask = (ask_n > 0) ? book.ask_prices[base] : 0;

        out.best_bid[s]   = best_bid;
        out.best_ask[s]   = best_ask;
        out.spread[s]     = (best_bid > 0 && best_ask > 0) ? (best_ask - best_bid) : 0;
        out.mid_price[s]  = (best_bid > 0 && best_ask > 0) ? ((best_bid + best_ask) / 2) : 0;

        // ---- Total volumes and VWAP across top 10 levels ----
        uint64_t total_bid_vol = 0;
        uint64_t total_ask_vol = 0;
        uint64_t bid_vp_sum = 0;     // sum(price * volume) for top 10 bids
        uint64_t bid_v_sum  = 0;     // sum(volume) for top 10 bids
        uint64_t ask_vp_sum = 0;
        uint64_t ask_v_sum  = 0;

        for (uint32_t l = 0; l < bid_n; ++l) {
            const uint64_t v = book.bid_volumes[base + l];
            total_bid_vol += v;
            if (l < 10) {
                bid_vp_sum += book.bid_prices[base + l] * v;
                bid_v_sum  += v;
            }
        }
        for (uint32_t l = 0; l < ask_n; ++l) {
            const uint64_t v = book.ask_volumes[base + l];
            total_ask_vol += v;
            if (l < 10) {
                ask_vp_sum += book.ask_prices[base + l] * v;
                ask_v_sum  += v;
            }
        }

        out.total_bid_volume[s] = total_bid_vol;
        out.total_ask_volume[s] = total_ask_vol;
        out.vwap_bid_top10[s]   = (bid_v_sum > 0) ? (bid_vp_sum / bid_v_sum) : 0;
        out.vwap_ask_top10[s]   = (ask_v_sum > 0) ? (ask_vp_sum / ask_v_sum) : 0;

        // ---- Imbalance (signed, scaled by 10000 for fixed-point precision) ----
        const uint64_t total = total_bid_vol + total_ask_vol;
        if (total > 0) {
            const int64_t diff = static_cast<int64_t>(total_bid_vol) -
                                 static_cast<int64_t>(total_ask_vol);
            out.imbalance_x10000[s] = (diff * 10000) / static_cast<int64_t>(total);
        } else {
            out.imbalance_x10000[s] = 0;
        }
    }
}

/**
 * Compute aggregate market-wide metrics from per-symbol analytics.
 *
 * Done on the host regardless of CPU/GPU analytics implementation
 * (it's a small reduction over a small array).
 */
inline MarketAggregates compute_aggregates(const SymbolAnalytics& a)
{
    MarketAggregates agg{};
    agg.min_spread = UINT64_MAX;
    agg.max_spread = 0;

    uint64_t spread_sum = 0;
    uint64_t spread_count = 0;
    uint64_t total_liq = 0;
    uint64_t with_market = 0;

    for (uint32_t s = 0; s < a.num_symbols; ++s) {
        total_liq += a.total_bid_volume[s] + a.total_ask_volume[s];
        if (a.best_bid[s] > 0 && a.best_ask[s] > 0) {
            ++with_market;
            spread_sum += a.spread[s];
            ++spread_count;
            if (a.spread[s] < agg.min_spread) agg.min_spread = a.spread[s];
            if (a.spread[s] > agg.max_spread) agg.max_spread = a.spread[s];
        }
    }

    agg.total_liquidity = total_liq;
    agg.total_symbols_with_market = with_market;
    agg.avg_spread = (spread_count > 0)
        ? static_cast<double>(spread_sum) / static_cast<double>(spread_count)
        : 0.0;
    if (agg.min_spread == UINT64_MAX) agg.min_spread = 0;

    return agg;
}

} // namespace GPU
} // namespace Trader

#endif // TRADER_GPU_ANALYTICS_CPU_H
