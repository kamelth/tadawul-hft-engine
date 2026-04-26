#ifndef TRADER_GPU_RISK_CPU_H
#define TRADER_GPU_RISK_CPU_H

#include <cstdint>
#include <cstdlib>
#include "trader/gpu/risk_gpu.h"

namespace Trader {
namespace GPU {

/**
 * CPU baseline: compute per-symbol risk metrics (serial).
 *
 * This is the number the GPU risk kernel must beat.
 * Each symbol is independent — perfect for GPU parallelism.
 */
inline void compute_risk_cpu(const SymbolRiskInput& inp, SymbolRiskOutput& out) {
    const uint32_t n = inp.num_symbols;

    for (uint32_t s = 0; s < n; ++s) {
        const int64_t  pos     = inp.position[s];
        const uint64_t mid     = inp.mid_price[s];
        const uint64_t bid     = inp.best_bid[s];
        const uint64_t ask     = inp.best_ask[s];
        const uint64_t avg_buy = inp.avg_buy_price[s];
        const int64_t  max_pos = inp.max_position[s];

        // Exposure: position × mid_price (signed, in $0.0001 units)
        const int64_t exp = pos * static_cast<int64_t>(mid);
        out.exposure[s] = exp;

        // Unrealized P&L: (mid_price - avg_buy_price) × position
        if (pos > 0 && avg_buy > 0) {
            out.unrealized_pnl[s] = (static_cast<int64_t>(mid) - static_cast<int64_t>(avg_buy)) * pos;
        } else {
            out.unrealized_pnl[s] = 0;
        }

        // Liquidation value: if we sold all at best bid
        if (pos > 0 && bid > 0) {
            out.liquidation_value[s] = static_cast<uint64_t>(pos) * bid;
        } else {
            out.liquidation_value[s] = 0;
        }

        // Worst-case loss: exposure × spread / mid (liquidity-adjusted)
        // Represents the cost of an immediate liquidation through the spread
        if (mid > 0 && bid > 0 && ask > 0) {
            const uint64_t spread = ask - bid;
            const int64_t abs_pos = (pos >= 0) ? pos : -pos;
            out.worst_case_loss[s] = static_cast<uint64_t>(abs_pos) * spread;
        } else {
            out.worst_case_loss[s] = 0;
        }

        // Position usage: abs(position) * 10000 / max_position (in basis points)
        if (max_pos > 0) {
            const int64_t abs_pos = (pos >= 0) ? pos : -pos;
            out.position_usage_pct[s] = (abs_pos * 10000) / max_pos;
        } else {
            out.position_usage_pct[s] = 0;
        }

        // Limit breached?
        if (max_pos > 0) {
            const int64_t abs_pos = (pos >= 0) ? pos : -pos;
            out.limit_breached[s] = (abs_pos >= max_pos) ? 1 : 0;
        } else {
            out.limit_breached[s] = 0;
        }

        // Inventory skew: directional score for quote adjustment
        // Positive = long bias (should skew asks down to reduce inventory)
        // Negative = short bias (should skew bids up)
        // Range: [-10000, +10000]
        if (max_pos > 0) {
            out.inventory_skew[s] = (pos * 10000) / max_pos;
        } else {
            out.inventory_skew[s] = 0;
        }
    }
}

/**
 * Compute portfolio-wide risk aggregates from per-symbol outputs.
 */
inline PortfolioRisk compute_portfolio_risk(const SymbolRiskInput& inp,
                                            const SymbolRiskOutput& out) {
    PortfolioRisk pr{};
    for (uint32_t s = 0; s < out.num_symbols; ++s) {
        const int64_t exp = out.exposure[s];
        pr.total_exposure += (exp >= 0) ? exp : -exp;
        pr.net_exposure   += exp;
        pr.total_unrealized_pnl += out.unrealized_pnl[s];
        pr.total_worst_case += out.worst_case_loss[s];
        if (out.limit_breached[s]) ++pr.symbols_at_limit;
        if (inp.position[s] != 0) ++pr.symbols_with_position;
    }
    return pr;
}

} // namespace GPU
} // namespace Trader

#endif // TRADER_GPU_RISK_CPU_H
