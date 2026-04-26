#ifndef TRADER_GPU_VALIDATION_GPU_H
#define TRADER_GPU_VALIDATION_GPU_H

#include <cstdint>
#include <vector>

namespace Trader {
namespace GPU {

/**
 * Batch of pending orders to validate (Structure of Arrays).
 *
 * Each entry is one pending quote/order from the strategy.
 * The GPU validates all N orders in parallel.
 */
struct OrderBatch {
    uint32_t  num_orders;

    uint32_t* symbol_id;        // [N] which symbol this order targets
    uint32_t* side;             // [N] 0 = buy, 1 = sell
    uint64_t* price;            // [N] order price (in $0.0001)
    uint64_t* quantity;         // [N] order size (shares)
};

/**
 * Per-symbol market state needed for validation (Structure of Arrays).
 * Indexed by symbol_id.
 */
struct ValidationContext {
    uint32_t  num_symbols;

    uint64_t* best_bid;         // [S] current best bid
    uint64_t* best_ask;         // [S] current best ask
    uint64_t* mid_price;        // [S] current mid
    int64_t*  current_position; // [S] current inventory
    int64_t*  max_position;     // [S] position limit
    uint64_t* max_order_size;   // [S] max single order size
    uint64_t* price_band_pct;   // [S] max deviation from mid in bps (e.g., 500 = 5%)
};

/**
 * Per-order validation result (Structure of Arrays).
 */
struct ValidationResult {
    uint32_t  num_orders;

    uint32_t* valid;              // [N] 1 = passed all checks, 0 = rejected
    uint32_t* reject_reason;      // [N] bitmask of rejection reasons (see below)
    int64_t*  post_trade_position; // [N] position after this order would fill
    uint64_t* notional_value;     // [N] price × quantity (dollar value of order)
};

// Rejection reason bitmask
constexpr uint32_t REJECT_NONE            = 0;
constexpr uint32_t REJECT_POSITION_LIMIT  = (1 << 0);  // would breach position limit
constexpr uint32_t REJECT_SIZE_LIMIT      = (1 << 1);  // order too large
constexpr uint32_t REJECT_PRICE_BAND      = (1 << 2);  // price too far from mid
constexpr uint32_t REJECT_NO_MARKET       = (1 << 3);  // no valid market (bid=0 or ask=0)
constexpr uint32_t REJECT_ZERO_PRICE      = (1 << 4);  // price is zero
constexpr uint32_t REJECT_ZERO_QTY        = (1 << 5);  // quantity is zero

// =============================================================================
// Host storage helpers
// =============================================================================

struct HostOrderBatchStorage {
    std::vector<uint32_t> symbol_id;
    std::vector<uint32_t> side;
    std::vector<uint64_t> price;
    std::vector<uint64_t> quantity;

    OrderBatch view(uint32_t n) {
        symbol_id.assign(n, 0);
        side.assign(n, 0);
        price.assign(n, 0);
        quantity.assign(n, 0);

        OrderBatch b;
        b.num_orders = n;
        b.symbol_id  = symbol_id.data();
        b.side       = side.data();
        b.price      = price.data();
        b.quantity   = quantity.data();
        return b;
    }
};

struct HostValidationContextStorage {
    std::vector<uint64_t> best_bid;
    std::vector<uint64_t> best_ask;
    std::vector<uint64_t> mid_price;
    std::vector<int64_t>  current_position;
    std::vector<int64_t>  max_position;
    std::vector<uint64_t> max_order_size;
    std::vector<uint64_t> price_band_pct;

    ValidationContext view(uint32_t n) {
        best_bid.assign(n, 0);
        best_ask.assign(n, 0);
        mid_price.assign(n, 0);
        current_position.assign(n, 0);
        max_position.assign(n, 0);
        max_order_size.assign(n, 0);
        price_band_pct.assign(n, 0);

        ValidationContext c;
        c.num_symbols      = n;
        c.best_bid         = best_bid.data();
        c.best_ask         = best_ask.data();
        c.mid_price        = mid_price.data();
        c.current_position = current_position.data();
        c.max_position     = max_position.data();
        c.max_order_size   = max_order_size.data();
        c.price_band_pct   = price_band_pct.data();
        return c;
    }
};

struct HostValidationResultStorage {
    std::vector<uint32_t> valid;
    std::vector<uint32_t> reject_reason;
    std::vector<int64_t>  post_trade_position;
    std::vector<uint64_t> notional_value;

    ValidationResult view(uint32_t n) {
        valid.assign(n, 0);
        reject_reason.assign(n, 0);
        post_trade_position.assign(n, 0);
        notional_value.assign(n, 0);

        ValidationResult r;
        r.num_orders          = n;
        r.valid               = valid.data();
        r.reject_reason       = reject_reason.data();
        r.post_trade_position = post_trade_position.data();
        r.notional_value      = notional_value.data();
        return r;
    }
};

} // namespace GPU
} // namespace Trader

#endif // TRADER_GPU_VALIDATION_GPU_H
