#ifndef TRADER_GPU_RISK_GPU_H
#define TRADER_GPU_RISK_GPU_H

#include <cstdint>
#include <vector>

namespace Trader {
namespace GPU {

/**
 * Per-symbol position/risk input (Structure of Arrays).
 *
 * Populated from the strategy's PositionManager + current market prices.
 * One entry per symbol.
 */
struct SymbolRiskInput {
    uint32_t  num_symbols;

    int64_t*  position;         // [N] shares held (positive = long, negative = short)
    uint64_t* mid_price;        // [N] current mid price (from order book, in $0.0001 units)
    uint64_t* best_bid;         // [N] current best bid
    uint64_t* best_ask;         // [N] current best ask
    uint64_t* avg_buy_price;    // [N] weighted average buy price (in $0.0001)
    uint64_t* total_bid_vol;    // [N] total bid volume (liquidity available to sell into)
    uint64_t* total_ask_vol;    // [N] total ask volume (liquidity available to buy from)
    int64_t*  max_position;     // [N] per-symbol position limit
};

/**
 * Per-symbol risk output (Structure of Arrays).
 */
struct SymbolRiskOutput {
    uint32_t  num_symbols;

    int64_t*  exposure;             // [N] position × mid_price (dollar exposure, in $0.0001)
    int64_t*  unrealized_pnl;       // [N] (mid_price - avg_buy) × position (in $0.0001)
    uint64_t* liquidation_value;    // [N] how much we'd get if we sold all at best_bid
    uint64_t* worst_case_loss;      // [N] exposure × spread / mid (liquidity-adjusted risk)
    int64_t*  position_usage_pct;   // [N] abs(position) * 10000 / max_position (basis points)
    uint32_t* limit_breached;       // [N] 1 if abs(position) >= max_position, else 0
    int64_t*  inventory_skew;       // [N] position imbalance score for quote adjustment
};

/**
 * Portfolio-wide risk aggregates (single values).
 */
struct PortfolioRisk {
    int64_t  total_exposure;         // sum of abs(exposure) across all symbols
    int64_t  net_exposure;           // sum of signed exposure
    int64_t  total_unrealized_pnl;   // sum of unrealized P&L
    uint64_t total_worst_case;       // sum of worst-case loss
    uint32_t symbols_at_limit;       // count of symbols where limit_breached == 1
    uint32_t symbols_with_position;  // count of symbols with position != 0
};

// =============================================================================
// Host storage helpers
// =============================================================================

struct HostRiskInputStorage {
    std::vector<int64_t>  position;
    std::vector<uint64_t> mid_price;
    std::vector<uint64_t> best_bid;
    std::vector<uint64_t> best_ask;
    std::vector<uint64_t> avg_buy_price;
    std::vector<uint64_t> total_bid_vol;
    std::vector<uint64_t> total_ask_vol;
    std::vector<int64_t>  max_position;

    SymbolRiskInput view(uint32_t n) {
        position.assign(n, 0);
        mid_price.assign(n, 0);
        best_bid.assign(n, 0);
        best_ask.assign(n, 0);
        avg_buy_price.assign(n, 0);
        total_bid_vol.assign(n, 0);
        total_ask_vol.assign(n, 0);
        max_position.assign(n, 0);

        SymbolRiskInput inp;
        inp.num_symbols    = n;
        inp.position       = position.data();
        inp.mid_price      = mid_price.data();
        inp.best_bid       = best_bid.data();
        inp.best_ask       = best_ask.data();
        inp.avg_buy_price  = avg_buy_price.data();
        inp.total_bid_vol  = total_bid_vol.data();
        inp.total_ask_vol  = total_ask_vol.data();
        inp.max_position   = max_position.data();
        return inp;
    }
};

struct HostRiskOutputStorage {
    std::vector<int64_t>  exposure;
    std::vector<int64_t>  unrealized_pnl;
    std::vector<uint64_t> liquidation_value;
    std::vector<uint64_t> worst_case_loss;
    std::vector<int64_t>  position_usage_pct;
    std::vector<uint32_t> limit_breached;
    std::vector<int64_t>  inventory_skew;

    SymbolRiskOutput view(uint32_t n) {
        exposure.assign(n, 0);
        unrealized_pnl.assign(n, 0);
        liquidation_value.assign(n, 0);
        worst_case_loss.assign(n, 0);
        position_usage_pct.assign(n, 0);
        limit_breached.assign(n, 0);
        inventory_skew.assign(n, 0);

        SymbolRiskOutput out;
        out.num_symbols        = n;
        out.exposure           = exposure.data();
        out.unrealized_pnl     = unrealized_pnl.data();
        out.liquidation_value  = liquidation_value.data();
        out.worst_case_loss    = worst_case_loss.data();
        out.position_usage_pct = position_usage_pct.data();
        out.limit_breached     = limit_breached.data();
        out.inventory_skew     = inventory_skew.data();
        return out;
    }
};

} // namespace GPU
} // namespace Trader

#endif // TRADER_GPU_RISK_GPU_H
