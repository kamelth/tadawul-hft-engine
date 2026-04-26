#ifndef TRADER_GPU_VALIDATION_CPU_H
#define TRADER_GPU_VALIDATION_CPU_H

#include <cstdint>
#include <cstdlib>
#include "trader/gpu/validation_gpu.h"

namespace Trader {
namespace GPU {

/**
 * CPU baseline: validate a batch of orders serially.
 *
 * Each order is checked against:
 *   1. Zero price / zero quantity
 *   2. Valid market exists (bid > 0 && ask > 0)
 *   3. Order size within max_order_size
 *   4. Post-trade position within limits
 *   5. Price within band around mid-price
 *
 * Each order is independent — perfect for GPU parallelism.
 */
inline void validate_orders_cpu(const OrderBatch& orders,
                                const ValidationContext& ctx,
                                ValidationResult& out)
{
    const uint32_t n = orders.num_orders;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t reason = REJECT_NONE;

        const uint32_t sym  = orders.symbol_id[i];
        const uint32_t side = orders.side[i];
        const uint64_t price = orders.price[i];
        const uint64_t qty   = orders.quantity[i];

        // Basic sanity
        if (price == 0) reason |= REJECT_ZERO_PRICE;
        if (qty == 0)   reason |= REJECT_ZERO_QTY;

        // Market existence
        const uint64_t bid = ctx.best_bid[sym];
        const uint64_t ask = ctx.best_ask[sym];
        if (bid == 0 || ask == 0) reason |= REJECT_NO_MARKET;

        // Order size limit
        if (ctx.max_order_size[sym] > 0 && qty > ctx.max_order_size[sym]) {
            reason |= REJECT_SIZE_LIMIT;
        }

        // Post-trade position check
        const int64_t cur_pos = ctx.current_position[sym];
        int64_t post_pos;
        if (side == 0) {  // buy
            post_pos = cur_pos + static_cast<int64_t>(qty);
        } else {  // sell
            post_pos = cur_pos - static_cast<int64_t>(qty);
        }
        out.post_trade_position[i] = post_pos;

        if (ctx.max_position[sym] > 0) {
            const int64_t abs_post = (post_pos >= 0) ? post_pos : -post_pos;
            if (abs_post > ctx.max_position[sym]) {
                reason |= REJECT_POSITION_LIMIT;
            }
        }

        // Price band check: |price - mid| * 10000 / mid <= price_band_pct
        const uint64_t mid = ctx.mid_price[sym];
        if (mid > 0 && ctx.price_band_pct[sym] > 0) {
            const int64_t diff = static_cast<int64_t>(price) - static_cast<int64_t>(mid);
            const uint64_t abs_diff = (diff >= 0) ? static_cast<uint64_t>(diff)
                                                   : static_cast<uint64_t>(-diff);
            const uint64_t deviation_bps = (abs_diff * 10000) / mid;
            if (deviation_bps > ctx.price_band_pct[sym]) {
                reason |= REJECT_PRICE_BAND;
            }
        }

        // Notional value
        out.notional_value[i] = price * qty;

        // Final verdict
        out.reject_reason[i] = reason;
        out.valid[i] = (reason == REJECT_NONE) ? 1 : 0;
    }
}

/**
 * Summary statistics from validation results.
 */
struct ValidationSummary {
    uint32_t total_orders;
    uint32_t accepted;
    uint32_t rejected;
    uint32_t reject_position;    // count of REJECT_POSITION_LIMIT
    uint32_t reject_size;        // count of REJECT_SIZE_LIMIT
    uint32_t reject_price_band;  // count of REJECT_PRICE_BAND
    uint32_t reject_no_market;   // count of REJECT_NO_MARKET
    uint64_t total_notional;     // sum of notional for accepted orders
};

inline ValidationSummary compute_validation_summary(const ValidationResult& r) {
    ValidationSummary vs{};
    vs.total_orders = r.num_orders;
    for (uint32_t i = 0; i < r.num_orders; ++i) {
        if (r.valid[i]) {
            ++vs.accepted;
            vs.total_notional += r.notional_value[i];
        } else {
            ++vs.rejected;
        }
        if (r.reject_reason[i] & REJECT_POSITION_LIMIT) ++vs.reject_position;
        if (r.reject_reason[i] & REJECT_SIZE_LIMIT)     ++vs.reject_size;
        if (r.reject_reason[i] & REJECT_PRICE_BAND)     ++vs.reject_price_band;
        if (r.reject_reason[i] & REJECT_NO_MARKET)      ++vs.reject_no_market;
    }
    return vs;
}

} // namespace GPU
} // namespace Trader

#endif // TRADER_GPU_VALIDATION_CPU_H
