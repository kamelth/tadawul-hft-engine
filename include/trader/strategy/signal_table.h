#ifndef TRADER_STRATEGY_SIGNAL_TABLE_H
#define TRADER_STRATEGY_SIGNAL_TABLE_H

#include <cstdint>
#include <cmath>
#include <unordered_map>
#include <array>

namespace Trader {
namespace Strategy {

/**
 * Per-symbol rolling signal store.
 *
 * Maintains a ring buffer of the last WINDOW mid-price ticks per symbol
 * and computes two real-time signals used by the adaptive market maker:
 *
 *  1. volatility_ticks  — std-dev of mid-price changes over WINDOW updates
 *                         (units: ticks, same unit as spread_ticks)
 *
 *  2. imbalance         — (bid_vol - ask_vol) * 10000 / (bid_vol + ask_vol)
 *                         range: [-10000, +10000]
 *                         positive = more bid liquidity (price likely rising)
 *                         negative = more ask liquidity (price likely falling)
 *
 * Both signals are computed purely on CPU from each order book update.
 * The GPU analytics kernel computes the same values at batch scale for
 * multi-symbol analytics — this class provides the lightweight real-time
 * version used by the trading strategy.
 */
class SignalTable {
public:
    static constexpr uint32_t WINDOW = 20;   // rolling window size (order book updates)

    SignalTable() = default;

    /**
     * Called on every order book update.
     * Updates the mid-price ring buffer and order flow imbalance.
     */
    void update(uint32_t symbol_id,
                uint64_t mid_price,
                uint64_t bid_volume,
                uint64_t ask_volume)
    {
        auto& state = states_[symbol_id];

        // --- rolling mid-price ring buffer ---
        state.mid_buf[state.head] = mid_price;
        state.head = (state.head + 1) % WINDOW;
        if (state.count < WINDOW) ++state.count;

        // --- order flow imbalance (fixed-point, scaled x10000) ---
        const uint64_t total = bid_volume + ask_volume;
        if (total > 0) {
            int64_t diff = static_cast<int64_t>(bid_volume)
                         - static_cast<int64_t>(ask_volume);
            state.imbalance_x10000 = (diff * 10000) / static_cast<int64_t>(total);
        } else {
            state.imbalance_x10000 = 0;
        }
    }

    /**
     * Volatility: std-dev of mid-price changes over the last WINDOW updates.
     * Returns value in ticks (same units as spread_ticks in MarketMaker::Params).
     * Returns 0 if fewer than 2 samples have been seen.
     */
    uint64_t volatility_ticks(uint32_t symbol_id) const {
        auto it = states_.find(symbol_id);
        if (it == states_.end() || it->second.count < 2) return 0;

        const auto& state = it->second;
        const uint32_t n = state.count;

        // Collect last n mid prices in chronological order
        // Ring buffer: head points to where the NEXT write goes (oldest slot)
        int64_t changes[WINDOW];
        uint32_t num_changes = 0;

        // Walk the ring in order (oldest → newest)
        uint32_t oldest = (state.head + WINDOW - n) % WINDOW;
        uint64_t prev   = state.mid_buf[oldest];

        for (uint32_t i = 1; i < n; ++i) {
            uint32_t idx = (oldest + i) % WINDOW;
            int64_t ch   = static_cast<int64_t>(state.mid_buf[idx])
                         - static_cast<int64_t>(prev);
            changes[num_changes++] = ch;
            prev = state.mid_buf[idx];
        }

        if (num_changes == 0) return 0;

        // Mean
        int64_t sum = 0;
        for (uint32_t i = 0; i < num_changes; ++i) sum += changes[i];
        int64_t mean = sum / static_cast<int64_t>(num_changes);

        // Variance
        int64_t var_sum = 0;
        for (uint32_t i = 0; i < num_changes; ++i) {
            int64_t d = changes[i] - mean;
            var_sum += d * d;
        }
        double var = static_cast<double>(var_sum) / static_cast<double>(num_changes);
        double sd  = std::sqrt(var);

        // Clamp to uint64: std-dev is always non-negative
        return static_cast<uint64_t>(sd + 0.5);   // round to nearest tick
    }

    /**
     * Order flow imbalance for a symbol.
     * Range: [-10000, +10000]  (fixed-point, divide by 10000 for fraction)
     *  +10000 = all liquidity on bid side  (buying pressure)
     *  -10000 = all liquidity on ask side  (selling pressure)
     */
    int64_t imbalance(uint32_t symbol_id) const {
        auto it = states_.find(symbol_id);
        if (it == states_.end()) return 0;
        return it->second.imbalance_x10000;
    }

    /**
     * How many updates have been seen for this symbol.
     */
    uint32_t sample_count(uint32_t symbol_id) const {
        auto it = states_.find(symbol_id);
        if (it == states_.end()) return 0;
        return it->second.count;
    }

    void clear() { states_.clear(); }

private:
    struct SymbolState {
        std::array<uint64_t, WINDOW> mid_buf{};
        uint32_t   head            = 0;
        uint32_t   count           = 0;
        int64_t    imbalance_x10000 = 0;
    };

    std::unordered_map<uint32_t, SymbolState> states_;
};

} // namespace Strategy
} // namespace Trader

#endif // TRADER_STRATEGY_SIGNAL_TABLE_H
