#ifndef TRADER_GPU_RISK_VALIDATION_GPU_CUH
#define TRADER_GPU_RISK_VALIDATION_GPU_CUH

#include <cstdint>
#include <cuda_runtime.h>
#include "trader/gpu/risk_gpu.h"
#include "trader/gpu/validation_gpu.h"

namespace Trader {
namespace GPU {

// =============================================================================
// Risk kernel device buffers
// =============================================================================

struct RiskDeviceBuffers {
    uint32_t num_symbols = 0;

    // Input
    int64_t*  d_position       = nullptr;
    uint64_t* d_mid_price      = nullptr;
    uint64_t* d_best_bid       = nullptr;
    uint64_t* d_best_ask       = nullptr;
    uint64_t* d_avg_buy_price  = nullptr;
    uint64_t* d_total_bid_vol  = nullptr;
    uint64_t* d_total_ask_vol  = nullptr;
    int64_t*  d_max_position   = nullptr;

    // Output
    int64_t*  d_exposure            = nullptr;
    int64_t*  d_unrealized_pnl      = nullptr;
    uint64_t* d_liquidation_value   = nullptr;
    uint64_t* d_worst_case_loss     = nullptr;
    int64_t*  d_position_usage_pct  = nullptr;
    uint32_t* d_limit_breached      = nullptr;
    int64_t*  d_inventory_skew      = nullptr;

    cudaError_t allocate(uint32_t n);
    void free();
};

cudaError_t copy_risk_input_h2d(const SymbolRiskInput& h, RiskDeviceBuffers& d);
cudaError_t copy_risk_output_d2h(const RiskDeviceBuffers& d, SymbolRiskOutput& h);
cudaError_t launch_risk_kernel(RiskDeviceBuffers& d, cudaStream_t stream = 0);

// =============================================================================
// Validation kernel device buffers
// =============================================================================

struct ValidationDeviceBuffers {
    uint32_t num_orders  = 0;
    uint32_t num_symbols = 0;

    // Order batch input
    uint32_t* d_symbol_id  = nullptr;
    uint32_t* d_side       = nullptr;
    uint64_t* d_price      = nullptr;
    uint64_t* d_quantity   = nullptr;

    // Context input (per symbol)
    uint64_t* d_ctx_best_bid         = nullptr;
    uint64_t* d_ctx_best_ask         = nullptr;
    uint64_t* d_ctx_mid_price        = nullptr;
    int64_t*  d_ctx_current_position = nullptr;
    int64_t*  d_ctx_max_position     = nullptr;
    uint64_t* d_ctx_max_order_size   = nullptr;
    uint64_t* d_ctx_price_band_pct   = nullptr;

    // Output (per order)
    uint32_t* d_valid               = nullptr;
    uint32_t* d_reject_reason       = nullptr;
    int64_t*  d_post_trade_position = nullptr;
    uint64_t* d_notional_value      = nullptr;

    cudaError_t allocate(uint32_t n_orders, uint32_t n_symbols);
    void free();
};

cudaError_t copy_validation_input_h2d(const OrderBatch& orders,
                                      const ValidationContext& ctx,
                                      ValidationDeviceBuffers& d);
cudaError_t copy_validation_output_d2h(const ValidationDeviceBuffers& d,
                                       ValidationResult& h);
cudaError_t launch_validation_kernel(ValidationDeviceBuffers& d, cudaStream_t stream = 0);

} // namespace GPU
} // namespace Trader

#endif // TRADER_GPU_RISK_VALIDATION_GPU_CUH
