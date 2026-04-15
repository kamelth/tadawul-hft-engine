// CUDA kernel implementation of multi-symbol order book analytics.
//
// Compile with nvcc.  This file is the GPU counterpart to analytics_cpu.h:
// same outputs, same semantics, parallel across symbols (1 block / symbol)
// and across price levels (32 threads / block, 1 warp).

#include <cstdint>
#include <cstdio>
#include <cuda_runtime.h>
#include "trader/gpu/order_book_gpu.h"
#include "trader/gpu/analytics_gpu.cuh"

namespace Trader {
namespace GPU {

// =============================================================================
// Device kernel
// =============================================================================

// We use exactly one warp (32 threads) per block.  This means we can use
// warp-level primitives (__shfl_*) for reduction without ever needing
// __syncthreads(), which is the fastest possible pattern on modern GPUs.
static_assert(MAX_LEVELS_PER_SIDE == 32, "Kernel assumes 32-level cap");

// Warp-wide sum reduction (CUDA Compute Capability >= 3.0 supports __shfl_xor_sync,
// CC >= 7.0 has __shfl_down_sync as the recommended path).
__device__ __forceinline__ uint64_t warp_sum_u64(uint64_t v) {
    // FULL_MASK = 0xffffffff (all 32 threads in warp participate)
    for (int offset = 16; offset > 0; offset >>= 1) {
        v += __shfl_down_sync(0xffffffff, v, offset);
    }
    return v;
}

__global__ void analytics_kernel(
    // Inputs (device pointers)
    const uint64_t* __restrict__ bid_prices,
    const uint64_t* __restrict__ bid_volumes,
    const uint32_t* __restrict__ bid_counts,
    const uint64_t* __restrict__ ask_prices,
    const uint64_t* __restrict__ ask_volumes,
    const uint32_t* __restrict__ ask_counts,
    uint32_t num_symbols,
    // Outputs
    uint64_t* __restrict__ best_bid,
    uint64_t* __restrict__ best_ask,
    uint64_t* __restrict__ spread,
    uint64_t* __restrict__ mid_price,
    uint64_t* __restrict__ total_bid_volume,
    uint64_t* __restrict__ total_ask_volume,
    uint64_t* __restrict__ vwap_bid_top10,
    uint64_t* __restrict__ vwap_ask_top10,
    int64_t*  __restrict__ imbalance_x10000)
{
    const uint32_t s = blockIdx.x;          // symbol id
    if (s >= num_symbols) return;

    const uint32_t lane = threadIdx.x;      // 0..31, level id
    const uint32_t bid_n = bid_counts[s];
    const uint32_t ask_n = ask_counts[s];

    // Each thread loads its own level (or 0 if beyond active count)
    const size_t base_idx = static_cast<size_t>(s) * MAX_LEVELS_PER_SIDE + lane;

    const uint64_t bp = (lane < bid_n) ? bid_prices[base_idx]  : 0ULL;
    const uint64_t bv = (lane < bid_n) ? bid_volumes[base_idx] : 0ULL;
    const uint64_t ap = (lane < ask_n) ? ask_prices[base_idx]  : 0ULL;
    const uint64_t av = (lane < ask_n) ? ask_volumes[base_idx] : 0ULL;

    // --- Total volumes (warp reduction over all 32 levels) ---
    const uint64_t total_bv = warp_sum_u64(bv);
    const uint64_t total_av = warp_sum_u64(av);

    // --- VWAP numerators / denominators for top 10 levels only ---
    const bool top10 = (lane < 10);
    const uint64_t bid_vp = top10 ? (bp * bv) : 0ULL;
    const uint64_t ask_vp = top10 ? (ap * av) : 0ULL;
    const uint64_t bid_v_top = top10 ? bv : 0ULL;
    const uint64_t ask_v_top = top10 ? av : 0ULL;

    const uint64_t bid_vp_sum = warp_sum_u64(bid_vp);
    const uint64_t ask_vp_sum = warp_sum_u64(ask_vp);
    const uint64_t bid_v_sum  = warp_sum_u64(bid_v_top);
    const uint64_t ask_v_sum  = warp_sum_u64(ask_v_top);

    // --- Lane 0 writes results ---
    if (lane == 0) {
        const uint64_t bb = (bid_n > 0) ? bid_prices[static_cast<size_t>(s) * MAX_LEVELS_PER_SIDE] : 0ULL;
        const uint64_t ba = (ask_n > 0) ? ask_prices[static_cast<size_t>(s) * MAX_LEVELS_PER_SIDE] : 0ULL;
        const uint64_t sp = (bb > 0 && ba > 0) ? (ba - bb) : 0ULL;
        const uint64_t mp = (bb > 0 && ba > 0) ? ((bb + ba) >> 1) : 0ULL;

        best_bid[s]         = bb;
        best_ask[s]         = ba;
        spread[s]           = sp;
        mid_price[s]        = mp;
        total_bid_volume[s] = total_bv;
        total_ask_volume[s] = total_av;
        vwap_bid_top10[s]   = (bid_v_sum > 0) ? (bid_vp_sum / bid_v_sum) : 0ULL;
        vwap_ask_top10[s]   = (ask_v_sum > 0) ? (ask_vp_sum / ask_v_sum) : 0ULL;

        const uint64_t total = total_bv + total_av;
        if (total > 0) {
            const int64_t diff = static_cast<int64_t>(total_bv) -
                                 static_cast<int64_t>(total_av);
            imbalance_x10000[s] = (diff * 10000) / static_cast<int64_t>(total);
        } else {
            imbalance_x10000[s] = 0;
        }
    }
}

// =============================================================================
// Host-side glue
// =============================================================================

#define CUDA_CHECK(call) do {                                                 \
    cudaError_t _err = (call);                                                \
    if (_err != cudaSuccess) {                                                \
        std::fprintf(stderr, "CUDA error %s:%d: %s\n",                        \
                     __FILE__, __LINE__, cudaGetErrorString(_err));           \
        return _err;                                                          \
    }                                                                         \
} while (0)

cudaError_t DeviceBuffers::allocate(uint32_t n) {
    free();
    num_symbols = n;
    const size_t levels_total = static_cast<size_t>(n) * MAX_LEVELS_PER_SIDE;

    CUDA_CHECK(cudaMalloc(&d_bid_prices,  levels_total * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_bid_volumes, levels_total * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_bid_counts,  n           * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_ask_prices,  levels_total * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_ask_volumes, levels_total * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_ask_counts,  n           * sizeof(uint32_t)));

    CUDA_CHECK(cudaMalloc(&d_best_bid,         n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_best_ask,         n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_spread,           n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_mid_price,        n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_total_bid_volume, n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_total_ask_volume, n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_vwap_bid_top10,   n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_vwap_ask_top10,   n * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_imbalance_x10000, n * sizeof(int64_t)));

    return cudaSuccess;
}

void DeviceBuffers::free() {
    auto safe_free = [](void* p) { if (p) cudaFree(p); };
    safe_free(d_bid_prices);   d_bid_prices = nullptr;
    safe_free(d_bid_volumes);  d_bid_volumes = nullptr;
    safe_free(d_bid_counts);   d_bid_counts = nullptr;
    safe_free(d_ask_prices);   d_ask_prices = nullptr;
    safe_free(d_ask_volumes);  d_ask_volumes = nullptr;
    safe_free(d_ask_counts);   d_ask_counts = nullptr;
    safe_free(d_best_bid);         d_best_bid = nullptr;
    safe_free(d_best_ask);         d_best_ask = nullptr;
    safe_free(d_spread);           d_spread = nullptr;
    safe_free(d_mid_price);        d_mid_price = nullptr;
    safe_free(d_total_bid_volume); d_total_bid_volume = nullptr;
    safe_free(d_total_ask_volume); d_total_ask_volume = nullptr;
    safe_free(d_vwap_bid_top10);   d_vwap_bid_top10 = nullptr;
    safe_free(d_vwap_ask_top10);   d_vwap_ask_top10 = nullptr;
    safe_free(d_imbalance_x10000); d_imbalance_x10000 = nullptr;
    num_symbols = 0;
}

MultiSymbolBook DeviceBuffers::device_book_view() const {
    MultiSymbolBook b;
    b.num_symbols = num_symbols;
    b.max_levels  = MAX_LEVELS_PER_SIDE;
    b.bid_prices  = d_bid_prices;
    b.bid_volumes = d_bid_volumes;
    b.bid_counts  = d_bid_counts;
    b.ask_prices  = d_ask_prices;
    b.ask_volumes = d_ask_volumes;
    b.ask_counts  = d_ask_counts;
    return b;
}

SymbolAnalytics DeviceBuffers::device_analytics_view() const {
    SymbolAnalytics a;
    a.num_symbols      = num_symbols;
    a.best_bid         = d_best_bid;
    a.best_ask         = d_best_ask;
    a.spread           = d_spread;
    a.mid_price        = d_mid_price;
    a.total_bid_volume = d_total_bid_volume;
    a.total_ask_volume = d_total_ask_volume;
    a.vwap_bid_top10   = d_vwap_bid_top10;
    a.vwap_ask_top10   = d_vwap_ask_top10;
    a.imbalance_x10000 = d_imbalance_x10000;
    return a;
}

cudaError_t copy_book_h2d(const MultiSymbolBook& h, DeviceBuffers& d) {
    const size_t levels_total = static_cast<size_t>(h.num_symbols) * MAX_LEVELS_PER_SIDE;
    CUDA_CHECK(cudaMemcpy(d.d_bid_prices,  h.bid_prices,
                          levels_total * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_bid_volumes, h.bid_volumes,
                          levels_total * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_bid_counts,  h.bid_counts,
                          h.num_symbols * sizeof(uint32_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_ask_prices,  h.ask_prices,
                          levels_total * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_ask_volumes, h.ask_volumes,
                          levels_total * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d.d_ask_counts,  h.ask_counts,
                          h.num_symbols * sizeof(uint32_t), cudaMemcpyHostToDevice));
    return cudaSuccess;
}

cudaError_t copy_analytics_d2h(const DeviceBuffers& d, SymbolAnalytics& h) {
    const uint32_t n = d.num_symbols;
    CUDA_CHECK(cudaMemcpy(h.best_bid,         d.d_best_bid,         n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.best_ask,         d.d_best_ask,         n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.spread,           d.d_spread,           n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.mid_price,        d.d_mid_price,        n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.total_bid_volume, d.d_total_bid_volume, n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.total_ask_volume, d.d_total_ask_volume, n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.vwap_bid_top10,   d.d_vwap_bid_top10,   n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.vwap_ask_top10,   d.d_vwap_ask_top10,   n * sizeof(uint64_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(h.imbalance_x10000, d.d_imbalance_x10000, n * sizeof(int64_t),  cudaMemcpyDeviceToHost));
    return cudaSuccess;
}

cudaError_t launch_analytics_kernel(DeviceBuffers& d, cudaStream_t stream) {
    if (d.num_symbols == 0) return cudaSuccess;

    const dim3 grid(d.num_symbols);
    const dim3 block(MAX_LEVELS_PER_SIDE);  // 32 threads = 1 warp

    analytics_kernel<<<grid, block, 0, stream>>>(
        d.d_bid_prices, d.d_bid_volumes, d.d_bid_counts,
        d.d_ask_prices, d.d_ask_volumes, d.d_ask_counts,
        d.num_symbols,
        d.d_best_bid, d.d_best_ask, d.d_spread, d.d_mid_price,
        d.d_total_bid_volume, d.d_total_ask_volume,
        d.d_vwap_bid_top10, d.d_vwap_ask_top10,
        d.d_imbalance_x10000);

    return cudaGetLastError();
}

} // namespace GPU
} // namespace Trader
