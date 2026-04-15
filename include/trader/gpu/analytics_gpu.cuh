#ifndef TRADER_GPU_ANALYTICS_GPU_CUH
#define TRADER_GPU_ANALYTICS_GPU_CUH

#include <cstdint>
#include <cuda_runtime.h>
#include "trader/gpu/order_book_gpu.h"

namespace Trader {
namespace GPU {

/**
 * Device-side allocation owning the SoA arrays on the GPU.
 * The lifetime of pointers in `book` and `out` is tied to this object.
 */
struct DeviceBuffers {
    uint32_t num_symbols = 0;

    // Book buffers
    uint64_t* d_bid_prices  = nullptr;
    uint64_t* d_bid_volumes = nullptr;
    uint32_t* d_bid_counts  = nullptr;
    uint64_t* d_ask_prices  = nullptr;
    uint64_t* d_ask_volumes = nullptr;
    uint32_t* d_ask_counts  = nullptr;

    // Analytics output buffers
    uint64_t* d_best_bid           = nullptr;
    uint64_t* d_best_ask           = nullptr;
    uint64_t* d_spread             = nullptr;
    uint64_t* d_mid_price          = nullptr;
    uint64_t* d_total_bid_volume   = nullptr;
    uint64_t* d_total_ask_volume   = nullptr;
    uint64_t* d_vwap_bid_top10     = nullptr;
    uint64_t* d_vwap_ask_top10     = nullptr;
    int64_t*  d_imbalance_x10000   = nullptr;

    cudaError_t allocate(uint32_t n);
    void free();

    // Build a MultiSymbolBook view pointing at device memory.
    MultiSymbolBook device_book_view() const;

    // Build a SymbolAnalytics view pointing at device memory.
    SymbolAnalytics device_analytics_view() const;
};

/**
 * Copy a host-side MultiSymbolBook to GPU memory.
 * Caller must have already allocated device buffers with matching size.
 */
cudaError_t copy_book_h2d(const MultiSymbolBook& host_book, DeviceBuffers& d);

/**
 * Copy GPU-side analytics back to host.
 */
cudaError_t copy_analytics_d2h(const DeviceBuffers& d, SymbolAnalytics& host_out);

/**
 * Launch the analytics kernel.
 *
 * Configuration: one thread block per symbol.  Block size is fixed at
 * MAX_LEVELS_PER_SIDE threads (= 32, which is exactly one warp on all
 * modern GPUs — perfect for warp-level reductions, no need for
 * inter-warp synchronization).
 *
 * Each thread processes one price level (or sentinel).  The block then
 * cooperatively reduces volumes, computes VWAP numerator/denominator,
 * and produces all per-symbol outputs.
 *
 * @param d        Device buffers (must be populated via copy_book_h2d).
 * @param stream   CUDA stream (0 = default).
 * @return         cudaSuccess on launch success.
 */
cudaError_t launch_analytics_kernel(DeviceBuffers& d, cudaStream_t stream = 0);

} // namespace GPU
} // namespace Trader

#endif // TRADER_GPU_ANALYTICS_GPU_CUH
