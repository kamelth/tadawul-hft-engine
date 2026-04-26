// CUDA kernels for real-time risk computation and batch order validation.
//
// Risk kernel:      1 thread per symbol (N symbols → N threads)
// Validation kernel: 1 thread per order  (M orders  → M threads)
//
// Both are embarrassingly parallel — no inter-thread communication needed.

#include <cstdint>
#include <cstdio>
#include <cuda_runtime.h>
#include "trader/gpu/risk_gpu.h"
#include "trader/gpu/validation_gpu.h"
#include "trader/gpu/risk_validation_gpu.cuh"

namespace Trader {
namespace GPU {

#define CUDA_CHECK(call) do {                                                 \
    cudaError_t _err = (call);                                                \
    if (_err != cudaSuccess) {                                                \
        std::fprintf(stderr, "CUDA error %s:%d: %s\n",                        \
                     __FILE__, __LINE__, cudaGetErrorString(_err));           \
        return _err;                                                          \
    }                                                                         \
} while (0)

// =============================================================================
// RISK KERNEL — 1 thread per symbol
// =============================================================================

__global__ void risk_kernel(
    // Input
    const int64_t*  __restrict__ position,
    const uint64_t* __restrict__ mid_price,
    const uint64_t* __restrict__ best_bid,
    const uint64_t* __restrict__ best_ask,
    const uint64_t* __restrict__ avg_buy_price,
    const int64_t*  __restrict__ max_position,
    uint32_t num_symbols,
    // Output
    int64_t*  __restrict__ exposure,
    int64_t*  __restrict__ unrealized_pnl,
    uint64_t* __restrict__ liquidation_value,
    uint64_t* __restrict__ worst_case_loss,
    int64_t*  __restrict__ position_usage_pct,
    uint32_t* __restrict__ limit_breached,
    int64_t*  __restrict__ inventory_skew)
{
    const uint32_t s = blockIdx.x * blockDim.x + threadIdx.x;
    if (s >= num_symbols) return;

    const int64_t  pos     = position[s];
    const uint64_t mid     = mid_price[s];
    const uint64_t bid     = best_bid[s];
    const uint64_t ask     = best_ask[s];
    const uint64_t avg_buy = avg_buy_price[s];
    const int64_t  max_pos = max_position[s];

    // Exposure
    exposure[s] = pos * static_cast<int64_t>(mid);

    // Unrealized P&L
    if (pos > 0 && avg_buy > 0) {
        unrealized_pnl[s] = (static_cast<int64_t>(mid) - static_cast<int64_t>(avg_buy)) * pos;
    } else {
        unrealized_pnl[s] = 0;
    }

    // Liquidation value
    if (pos > 0 && bid > 0) {
        liquidation_value[s] = static_cast<uint64_t>(pos) * bid;
    } else {
        liquidation_value[s] = 0;
    }

    // Worst-case loss (spread cost)
    if (mid > 0 && bid > 0 && ask > 0) {
        const uint64_t spread = ask - bid;
        const int64_t abs_pos = (pos >= 0) ? pos : -pos;
        worst_case_loss[s] = static_cast<uint64_t>(abs_pos) * spread;
    } else {
        worst_case_loss[s] = 0;
    }

    // Position usage (basis points)
    const int64_t abs_pos = (pos >= 0) ? pos : -pos;
    if (max_pos > 0) {
        position_usage_pct[s] = (abs_pos * 10000) / max_pos;
        limit_breached[s] = (abs_pos >= max_pos) ? 1 : 0;
        inventory_skew[s] = (pos * 10000) / max_pos;
    } else {
        position_usage_pct[s] = 0;
        limit_breached[s] = 0;
        inventory_skew[s] = 0;
    }
}

// =============================================================================
// VALIDATION KERNEL — 1 thread per order
// =============================================================================

__global__ void validation_kernel(
    // Order batch
    const uint32_t* __restrict__ symbol_id,
    const uint32_t* __restrict__ side,
    const uint64_t* __restrict__ price,
    const uint64_t* __restrict__ quantity,
    uint32_t num_orders,
    // Context (per symbol)
    const uint64_t* __restrict__ ctx_best_bid,
    const uint64_t* __restrict__ ctx_best_ask,
    const uint64_t* __restrict__ ctx_mid_price,
    const int64_t*  __restrict__ ctx_current_position,
    const int64_t*  __restrict__ ctx_max_position,
    const uint64_t* __restrict__ ctx_max_order_size,
    const uint64_t* __restrict__ ctx_price_band_pct,
    // Output (per order)
    uint32_t* __restrict__ valid,
    uint32_t* __restrict__ reject_reason,
    int64_t*  __restrict__ post_trade_position,
    uint64_t* __restrict__ notional_value)
{
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= num_orders) return;

    uint32_t reason = 0;  // REJECT_NONE

    const uint32_t sym = symbol_id[i];
    const uint32_t s   = side[i];
    const uint64_t p   = price[i];
    const uint64_t q   = quantity[i];

    // Basic sanity
    if (p == 0) reason |= (1 << 4);  // REJECT_ZERO_PRICE
    if (q == 0) reason |= (1 << 5);  // REJECT_ZERO_QTY

    // Market existence
    const uint64_t bid = ctx_best_bid[sym];
    const uint64_t ask = ctx_best_ask[sym];
    if (bid == 0 || ask == 0) reason |= (1 << 3);  // REJECT_NO_MARKET

    // Size limit
    const uint64_t max_sz = ctx_max_order_size[sym];
    if (max_sz > 0 && q > max_sz) reason |= (1 << 1);  // REJECT_SIZE_LIMIT

    // Post-trade position
    const int64_t cur_pos = ctx_current_position[sym];
    int64_t post_pos;
    if (s == 0) {  // buy
        post_pos = cur_pos + static_cast<int64_t>(q);
    } else {
        post_pos = cur_pos - static_cast<int64_t>(q);
    }
    post_trade_position[i] = post_pos;

    const int64_t max_pos = ctx_max_position[sym];
    if (max_pos > 0) {
        const int64_t abs_post = (post_pos >= 0) ? post_pos : -post_pos;
        if (abs_post > max_pos) reason |= (1 << 0);  // REJECT_POSITION_LIMIT
    }

    // Price band
    const uint64_t mid = ctx_mid_price[sym];
    const uint64_t band = ctx_price_band_pct[sym];
    if (mid > 0 && band > 0) {
        const int64_t diff = static_cast<int64_t>(p) - static_cast<int64_t>(mid);
        const uint64_t abs_diff = (diff >= 0) ? static_cast<uint64_t>(diff)
                                               : static_cast<uint64_t>(-diff);
        const uint64_t deviation_bps = (abs_diff * 10000) / mid;
        if (deviation_bps > band) reason |= (1 << 2);  // REJECT_PRICE_BAND
    }

    // Notional
    notional_value[i] = p * q;

    // Result
    reject_reason[i] = reason;
    valid[i] = (reason == 0) ? 1 : 0;
}

// =============================================================================
// Risk host-side glue
// =============================================================================

cudaError_t RiskDeviceBuffers::allocate(uint32_t n) {
    free();
    num_symbols = n;
    CUDA_CHECK(cudaMalloc(&d_position,      n * sizeof(int64_t)));
    CUDA_CHECK(cudaMalloc(&d_mid_price,     n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_best_bid,      n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_best_ask,      n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_avg_buy_price, n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_total_bid_vol, n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_total_ask_vol, n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_max_position,  n * sizeof(int64_t)));
    // Outputs
    CUDA_CHECK(cudaMalloc(&d_exposure,           n * sizeof(int64_t)));
    CUDA_CHECK(cudaMalloc(&d_unrealized_pnl,     n * sizeof(int64_t)));
    CUDA_CHECK(cudaMalloc(&d_liquidation_value,  n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_worst_case_loss,    n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_position_usage_pct, n * sizeof(int64_t)));
    CUDA_CHECK(cudaMalloc(&d_limit_breached,     n * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_inventory_skew,     n * sizeof(int64_t)));
    return cudaSuccess;
}

void RiskDeviceBuffers::free() {
    auto sf = [](void* p) { if (p) cudaFree(p); };
    sf(d_position);       d_position = nullptr;
    sf(d_mid_price);      d_mid_price = nullptr;
    sf(d_best_bid);       d_best_bid = nullptr;
    sf(d_best_ask);       d_best_ask = nullptr;
    sf(d_avg_buy_price);  d_avg_buy_price = nullptr;
    sf(d_total_bid_vol);  d_total_bid_vol = nullptr;
    sf(d_total_ask_vol);  d_total_ask_vol = nullptr;
    sf(d_max_position);   d_max_position = nullptr;
    sf(d_exposure);            d_exposure = nullptr;
    sf(d_unrealized_pnl);      d_unrealized_pnl = nullptr;
    sf(d_liquidation_value);   d_liquidation_value = nullptr;
    sf(d_worst_case_loss);     d_worst_case_loss = nullptr;
    sf(d_position_usage_pct);  d_position_usage_pct = nullptr;
    sf(d_limit_breached);      d_limit_breached = nullptr;
    sf(d_inventory_skew);      d_inventory_skew = nullptr;
    num_symbols = 0;
}

cudaError_t copy_risk_input_h2d(const SymbolRiskInput& h, RiskDeviceBuffers& d) {
    const uint32_t n = h.num_symbols;
    CUDA_CHECK(cudaMemcpy(d.d_position,      h.position,      n * sizeof(int64_t),  cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_mid_price,     h.mid_price,     n * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_best_bid,      h.best_bid,      n * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_best_ask,      h.best_ask,      n * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_avg_buy_price, h.avg_buy_price, n * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_total_bid_vol, h.total_bid_vol, n * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_total_ask_vol, h.total_ask_vol, n * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_max_position,  h.max_position,  n * sizeof(int64_t),  cudaMemcpyHostToDevice));
    return cudaSuccess;
}

cudaError_t copy_risk_output_d2h(const RiskDeviceBuffers& d, SymbolRiskOutput& h) {
    const uint32_t n = d.num_symbols;
    CUDA_CHECK(cudaMemcpy(h.exposure,           d.d_exposure,           n * sizeof(int64_t),  cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.unrealized_pnl,     d.d_unrealized_pnl,     n * sizeof(int64_t),  cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.liquidation_value,  d.d_liquidation_value,  n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.worst_case_loss,    d.d_worst_case_loss,    n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.position_usage_pct, d.d_position_usage_pct, n * sizeof(int64_t),  cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.limit_breached,     d.d_limit_breached,     n * sizeof(uint32_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.inventory_skew,     d.d_inventory_skew,     n * sizeof(int64_t),  cudaMemcpyDeviceToHost));
    return cudaSuccess;
}

cudaError_t launch_risk_kernel(RiskDeviceBuffers& d, cudaStream_t stream) {
    if (d.num_symbols == 0) return cudaSuccess;
    const uint32_t threads = 256;
    const uint32_t blocks = (d.num_symbols + threads - 1) / threads;

    risk_kernel<<<blocks, threads, 0, stream>>>(
        d.d_position, d.d_mid_price, d.d_best_bid, d.d_best_ask,
        d.d_avg_buy_price, d.d_max_position, d.num_symbols,
        d.d_exposure, d.d_unrealized_pnl, d.d_liquidation_value,
        d.d_worst_case_loss, d.d_position_usage_pct,
        d.d_limit_breached, d.d_inventory_skew);

    return cudaGetLastError();
}

// =============================================================================
// Validation host-side glue
// =============================================================================

cudaError_t ValidationDeviceBuffers::allocate(uint32_t n_orders, uint32_t n_symbols) {
    free();
    num_orders = n_orders;
    num_symbols = n_symbols;
    // Order batch
    CUDA_CHECK(cudaMalloc(&d_symbol_id, n_orders * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_side,      n_orders * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_price,     n_orders * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_quantity,  n_orders * sizeof(uint64_t)));
    // Context
    CUDA_CHECK(cudaMalloc(&d_ctx_best_bid,         n_symbols * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_ctx_best_ask,         n_symbols * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_ctx_mid_price,        n_symbols * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_ctx_current_position, n_symbols * sizeof(int64_t)));
    CUDA_CHECK(cudaMalloc(&d_ctx_max_position,     n_symbols * sizeof(int64_t)));
    CUDA_CHECK(cudaMalloc(&d_ctx_max_order_size,   n_symbols * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_ctx_price_band_pct,   n_symbols * sizeof(uint64_t)));
    // Output
    CUDA_CHECK(cudaMalloc(&d_valid,               n_orders * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_reject_reason,       n_orders * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_post_trade_position, n_orders * sizeof(int64_t)));
    CUDA_CHECK(cudaMalloc(&d_notional_value,      n_orders * sizeof(uint64_t)));
    return cudaSuccess;
}

void ValidationDeviceBuffers::free() {
    auto sf = [](void* p) { if (p) cudaFree(p); };
    sf(d_symbol_id);  d_symbol_id = nullptr;
    sf(d_side);       d_side = nullptr;
    sf(d_price);      d_price = nullptr;
    sf(d_quantity);   d_quantity = nullptr;
    sf(d_ctx_best_bid);         d_ctx_best_bid = nullptr;
    sf(d_ctx_best_ask);         d_ctx_best_ask = nullptr;
    sf(d_ctx_mid_price);        d_ctx_mid_price = nullptr;
    sf(d_ctx_current_position); d_ctx_current_position = nullptr;
    sf(d_ctx_max_position);     d_ctx_max_position = nullptr;
    sf(d_ctx_max_order_size);   d_ctx_max_order_size = nullptr;
    sf(d_ctx_price_band_pct);   d_ctx_price_band_pct = nullptr;
    sf(d_valid);               d_valid = nullptr;
    sf(d_reject_reason);       d_reject_reason = nullptr;
    sf(d_post_trade_position); d_post_trade_position = nullptr;
    sf(d_notional_value);      d_notional_value = nullptr;
    num_orders = 0;
    num_symbols = 0;
}

cudaError_t copy_validation_input_h2d(const OrderBatch& orders,
                                      const ValidationContext& ctx,
                                      ValidationDeviceBuffers& d) {
    const uint32_t n = orders.num_orders;
    const uint32_t s = ctx.num_symbols;
    CUDA_CHECK(cudaMemcpy(d.d_symbol_id, orders.symbol_id, n * sizeof(uint32_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_side,      orders.side,      n * sizeof(uint32_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_price,     orders.price,     n * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_quantity,  orders.quantity,   n * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_ctx_best_bid,         ctx.best_bid,         s * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_ctx_best_ask,         ctx.best_ask,         s * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_ctx_mid_price,        ctx.mid_price,        s * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_ctx_current_position, ctx.current_position, s * sizeof(int64_t),  cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_ctx_max_position,     ctx.max_position,     s * sizeof(int64_t),  cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_ctx_max_order_size,   ctx.max_order_size,   s * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_ctx_price_band_pct,   ctx.price_band_pct,   s * sizeof(uint64_t), cudaMemcpyHostToDevice));
    return cudaSuccess;
}

cudaError_t copy_validation_output_d2h(const ValidationDeviceBuffers& d,
                                       ValidationResult& h) {
    const uint32_t n = d.num_orders;
    CUDA_CHECK(cudaMemcpy(h.valid,               d.d_valid,               n * sizeof(uint32_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.reject_reason,       d.d_reject_reason,       n * sizeof(uint32_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.post_trade_position, d.d_post_trade_position, n * sizeof(int64_t),  cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.notional_value,      d.d_notional_value,      n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    return cudaSuccess;
}

cudaError_t launch_validation_kernel(ValidationDeviceBuffers& d, cudaStream_t stream) {
    if (d.num_orders == 0) return cudaSuccess;
    const uint32_t threads = 256;
    const uint32_t blocks = (d.num_orders + threads - 1) / threads;

    validation_kernel<<<blocks, threads, 0, stream>>>(
        d.d_symbol_id, d.d_side, d.d_price, d.d_quantity, d.num_orders,
        d.d_ctx_best_bid, d.d_ctx_best_ask, d.d_ctx_mid_price,
        d.d_ctx_current_position, d.d_ctx_max_position,
        d.d_ctx_max_order_size, d.d_ctx_price_band_pct,
        d.d_valid, d.d_reject_reason, d.d_post_trade_position,
        d.d_notional_value);

    return cudaGetLastError();
}

} // namespace GPU
} // namespace Trader
