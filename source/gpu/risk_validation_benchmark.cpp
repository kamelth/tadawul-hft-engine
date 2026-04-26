// Benchmark for risk computation and batch order validation: CPU vs GPU.
//
// Usage:
//   risk_validation_benchmark [options]
//     --symbols N,M,...        Symbol counts for risk kernel (default 1,10,100,1000,5000)
//     --orders  N,M,...        Order counts for validation kernel (default 1,10,100,1000,10000,100000)
//     --iters N                Iterations per timing (default 1000)
//     --cpu-only               Skip GPU timing
//     --out path.csv           Output CSV (default results/risk_validation_benchmark.csv)

#include <iostream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#include "trader/gpu/risk_gpu.h"
#include "trader/gpu/risk_cpu.h"
#include "trader/gpu/validation_gpu.h"
#include "trader/gpu/validation_cpu.h"

#ifdef HAVE_CUDA
#include <cuda_runtime.h>
#include "trader/gpu/risk_validation_gpu.cuh"
#endif

using namespace Trader::GPU;
using namespace std::chrono;

// =============================================================================
// Synthetic data generators
// =============================================================================

static void generate_risk_data(SymbolRiskInput& inp, uint64_t seed) {
    uint64_t rng = seed;
    auto next = [&]() -> uint64_t {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        return rng;
    };
    for (uint32_t s = 0; s < inp.num_symbols; ++s) {
        const uint64_t mid = 10000 + (next() % 990001);  // $1.0000 to $100.0000
        inp.mid_price[s]     = mid;
        inp.best_bid[s]      = mid - (next() % 100 + 1);
        inp.best_ask[s]      = mid + (next() % 100 + 1);
        inp.position[s]      = static_cast<int64_t>(next() % 2001) - 1000; // -1000 to 1000
        inp.avg_buy_price[s] = (inp.position[s] > 0) ? mid - (next() % 500) : 0;
        inp.total_bid_vol[s] = next() % 100000;
        inp.total_ask_vol[s] = next() % 100000;
        inp.max_position[s]  = 500 + (next() % 1500);
    }
}

static void generate_validation_data(OrderBatch& orders, ValidationContext& ctx,
                                     uint32_t num_symbols, uint64_t seed) {
    uint64_t rng = seed;
    auto next = [&]() -> uint64_t {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        return rng;
    };

    // Set up context for num_symbols
    for (uint32_t s = 0; s < num_symbols; ++s) {
        const uint64_t mid = 10000 + (next() % 990001);
        ctx.best_bid[s]         = mid - (next() % 100 + 1);
        ctx.best_ask[s]         = mid + (next() % 100 + 1);
        ctx.mid_price[s]        = mid;
        ctx.current_position[s] = static_cast<int64_t>(next() % 501) - 250;
        ctx.max_position[s]     = 500 + (next() % 1500);
        ctx.max_order_size[s]   = 100 + (next() % 400);
        ctx.price_band_pct[s]   = 200 + (next() % 800);  // 2% to 10%
    }

    // Generate orders
    for (uint32_t i = 0; i < orders.num_orders; ++i) {
        orders.symbol_id[i] = next() % num_symbols;
        orders.side[i]      = next() % 2;
        const uint64_t mid  = ctx.mid_price[orders.symbol_id[i]];
        // Most orders near mid, some outliers
        const int64_t offset = static_cast<int64_t>(next() % 2000) - 1000;
        orders.price[i]     = static_cast<uint64_t>(static_cast<int64_t>(mid) + offset);
        orders.quantity[i]  = 1 + (next() % 300);
    }
}

// =============================================================================
// CPU timing
// =============================================================================

static double time_risk_cpu(SymbolRiskInput& inp, SymbolRiskOutput& out, int iters) {
    auto t0 = steady_clock::now();
    for (int i = 0; i < iters; ++i) compute_risk_cpu(inp, out);
    return duration<double>(steady_clock::now() - t0).count();
}

static double time_validation_cpu(const OrderBatch& orders, const ValidationContext& ctx,
                                  ValidationResult& out, int iters) {
    auto t0 = steady_clock::now();
    for (int i = 0; i < iters; ++i) validate_orders_cpu(orders, ctx, out);
    return duration<double>(steady_clock::now() - t0).count();
}

// =============================================================================
// GPU timing (CUDA only)
// =============================================================================

#ifdef HAVE_CUDA
static double time_risk_gpu_kernel(RiskDeviceBuffers& d, int iters) {
    cudaEvent_t start, stop;
    cudaEventCreate(&start); cudaEventCreate(&stop);
    cudaEventRecord(start);
    for (int i = 0; i < iters; ++i) launch_risk_kernel(d);
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, start, stop);
    cudaEventDestroy(start); cudaEventDestroy(stop);
    return ms / 1000.0;
}

static double time_risk_gpu_e2e(const SymbolRiskInput& inp, SymbolRiskOutput& out,
                                RiskDeviceBuffers& d, int iters) {
    auto t0 = steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        copy_risk_input_h2d(inp, d);
        launch_risk_kernel(d);
        copy_risk_output_d2h(d, out);
    }
    cudaDeviceSynchronize();
    return duration<double>(steady_clock::now() - t0).count();
}

static double time_validation_gpu_kernel(ValidationDeviceBuffers& d, int iters) {
    cudaEvent_t start, stop;
    cudaEventCreate(&start); cudaEventCreate(&stop);
    cudaEventRecord(start);
    for (int i = 0; i < iters; ++i) launch_validation_kernel(d);
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, start, stop);
    cudaEventDestroy(start); cudaEventDestroy(stop);
    return ms / 1000.0;
}

static double time_validation_gpu_e2e(const OrderBatch& orders, const ValidationContext& ctx,
                                      ValidationResult& out, ValidationDeviceBuffers& d,
                                      int iters) {
    auto t0 = steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        copy_validation_input_h2d(orders, ctx, d);
        launch_validation_kernel(d);
        copy_validation_output_d2h(d, out);
    }
    cudaDeviceSynchronize();
    return duration<double>(steady_clock::now() - t0).count();
}

// Correctness: compare GPU vs CPU risk output
static bool risk_outputs_equal(const SymbolRiskOutput& a, const SymbolRiskOutput& b) {
    for (uint32_t i = 0; i < a.num_symbols; ++i) {
        if (a.exposure[i]           != b.exposure[i])           return false;
        if (a.unrealized_pnl[i]     != b.unrealized_pnl[i])     return false;
        if (a.liquidation_value[i]  != b.liquidation_value[i])  return false;
        if (a.worst_case_loss[i]    != b.worst_case_loss[i])    return false;
        if (a.position_usage_pct[i] != b.position_usage_pct[i]) return false;
        if (a.limit_breached[i]     != b.limit_breached[i])     return false;
        if (a.inventory_skew[i]     != b.inventory_skew[i])     return false;
    }
    return true;
}

static bool validation_outputs_equal(const ValidationResult& a, const ValidationResult& b) {
    for (uint32_t i = 0; i < a.num_orders; ++i) {
        if (a.valid[i]               != b.valid[i])               return false;
        if (a.reject_reason[i]       != b.reject_reason[i])       return false;
        if (a.post_trade_position[i] != b.post_trade_position[i]) return false;
        if (a.notional_value[i]      != b.notional_value[i])      return false;
    }
    return true;
}
#endif

// =============================================================================
// Args
// =============================================================================

struct Args {
    std::vector<uint32_t> symbol_counts = {1, 10, 100, 1000, 5000};
    std::vector<uint32_t> order_counts  = {1, 10, 100, 1000, 10000, 100000};
    int  iterations = 1000;
    bool cpu_only   = false;
    std::string out_csv = "results/risk_validation_benchmark.csv";
};

static std::vector<uint32_t> parse_list(const std::string& s) {
    std::vector<uint32_t> v;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        v.push_back(static_cast<uint32_t>(std::strtoul(s.substr(pos, comma - pos).c_str(), nullptr, 10)));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return v;
}

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << arg << "\n"; std::exit(1); }
            return argv[++i];
        };
        if      (arg == "--symbols")   a.symbol_counts = parse_list(next());
        else if (arg == "--orders")    a.order_counts  = parse_list(next());
        else if (arg == "--iters")     a.iterations = std::atoi(next().c_str());
        else if (arg == "--cpu-only")  a.cpu_only = true;
        else if (arg == "--out")       a.out_csv = next();
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: risk_validation_benchmark [options]\n"
                         "  --symbols N,M,...   Symbol counts for risk kernel\n"
                         "  --orders  N,M,...   Order counts for validation kernel\n"
                         "  --iters N           Iterations per timing (default 1000)\n"
                         "  --cpu-only          Skip GPU\n"
                         "  --out path.csv      Output CSV\n";
            std::exit(0);
        }
    }
    return a;
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    std::cout << "=============================================\n";
    std::cout << "HFT Tadawul - Risk/Validation GPU Benchmark\n";
    std::cout << "=============================================\n";
    std::cout << "Iterations: " << args.iterations << "\n";

#ifndef HAVE_CUDA
    if (!args.cpu_only) {
        std::cout << "(Built without HAVE_CUDA — running CPU-only)\n";
        args.cpu_only = true;
    }
#endif

    std::ofstream csv(args.out_csv);
    if (csv.is_open()) {
        csv << "kernel,size,cpu_total_sec,cpu_per_iter_us,"
               "gpu_kernel_total_sec,gpu_kernel_per_iter_us,"
               "gpu_e2e_total_sec,gpu_e2e_per_iter_us,"
               "speedup_kernel,speedup_e2e,correctness_ok\n";
    }

    // =========================================================================
    // RISK KERNEL BENCHMARK
    // =========================================================================
    std::cout << "\n--- Risk Kernel (1 thread/symbol) ---\n";
    std::cout << std::setw(10) << "Symbols"
              << std::setw(14) << "CPU/iter(us)"
              << std::setw(14) << "GPUk/iter(us)"
              << std::setw(14) << "GPUe2e/iter(us)"
              << std::setw(12) << "Speedup(k)"
              << std::setw(12) << "Speedup(e2e)"
              << std::setw(8)  << "Match"
              << "\n" << std::string(84, '-') << "\n";

    for (uint32_t n : args.symbol_counts) {
        HostRiskInputStorage is;
        HostRiskOutputStorage os;
        auto inp = is.view(n);
        auto out = os.view(n);
        generate_risk_data(inp, 42);

        double cpu_sec = time_risk_cpu(inp, out, args.iterations);
        double cpu_us  = cpu_sec * 1e6 / args.iterations;

        double gk_sec = 0, gk_us = 0, ge_sec = 0, ge_us = 0;
        double sk = 0, se = 0;
        bool correct = true;

#ifdef HAVE_CUDA
        if (!args.cpu_only) {
            RiskDeviceBuffers d;
            if (d.allocate(n) != cudaSuccess) { std::cerr << "alloc fail\n"; continue; }
            copy_risk_input_h2d(inp, d);
            launch_risk_kernel(d);
            cudaDeviceSynchronize();

            gk_sec = time_risk_gpu_kernel(d, args.iterations);
            gk_us  = gk_sec * 1e6 / args.iterations;

            ge_sec = time_risk_gpu_e2e(inp, out, d, args.iterations);
            ge_us  = ge_sec * 1e6 / args.iterations;

            sk = cpu_sec / gk_sec;
            se = cpu_sec / ge_sec;

            // Correctness
            HostRiskOutputStorage cpu_os, gpu_os;
            auto cpu_out = cpu_os.view(n);
            auto gpu_out = gpu_os.view(n);
            compute_risk_cpu(inp, cpu_out);
            copy_risk_input_h2d(inp, d);
            launch_risk_kernel(d);
            copy_risk_output_d2h(d, gpu_out);
            cudaDeviceSynchronize();
            correct = risk_outputs_equal(cpu_out, gpu_out);

            d.free();
        }
#endif
        std::cout << std::setw(10) << n
                  << std::setw(14) << std::fixed << std::setprecision(2) << cpu_us
                  << std::setw(14) << gk_us
                  << std::setw(14) << ge_us
                  << std::setw(11) << std::setprecision(2) << sk << "x"
                  << std::setw(11) << se << "x"
                  << std::setw(8)  << (correct ? "OK" : "FAIL")
                  << "\n";

        if (csv.is_open()) {
            csv << "risk," << n << ","
                << cpu_sec << "," << cpu_us << ","
                << gk_sec << "," << gk_us << ","
                << ge_sec << "," << ge_us << ","
                << sk << "," << se << ","
                << (correct ? 1 : 0) << "\n";
        }
    }

    // =========================================================================
    // VALIDATION KERNEL BENCHMARK
    // =========================================================================
    const uint32_t val_num_symbols = 100;  // context: 100 symbols

    std::cout << "\n--- Validation Kernel (1 thread/order, " << val_num_symbols << " symbols) ---\n";
    std::cout << std::setw(10) << "Orders"
              << std::setw(14) << "CPU/iter(us)"
              << std::setw(14) << "GPUk/iter(us)"
              << std::setw(14) << "GPUe2e/iter(us)"
              << std::setw(12) << "Speedup(k)"
              << std::setw(12) << "Speedup(e2e)"
              << std::setw(8)  << "Match"
              << "\n" << std::string(84, '-') << "\n";

    for (uint32_t n : args.order_counts) {
        HostOrderBatchStorage obs;
        HostValidationContextStorage vcs;
        HostValidationResultStorage vrs;

        auto orders = obs.view(n);
        auto ctx    = vcs.view(val_num_symbols);
        auto result = vrs.view(n);
        generate_validation_data(orders, ctx, val_num_symbols, 42);

        double cpu_sec = time_validation_cpu(orders, ctx, result, args.iterations);
        double cpu_us  = cpu_sec * 1e6 / args.iterations;

        double gk_sec = 0, gk_us = 0, ge_sec = 0, ge_us = 0;
        double sk = 0, se = 0;
        bool correct = true;

#ifdef HAVE_CUDA
        if (!args.cpu_only) {
            ValidationDeviceBuffers d;
            if (d.allocate(n, val_num_symbols) != cudaSuccess) {
                std::cerr << "alloc fail\n"; continue;
            }
            copy_validation_input_h2d(orders, ctx, d);
            launch_validation_kernel(d);
            cudaDeviceSynchronize();

            gk_sec = time_validation_gpu_kernel(d, args.iterations);
            gk_us  = gk_sec * 1e6 / args.iterations;

            ge_sec = time_validation_gpu_e2e(orders, ctx, result, d, args.iterations);
            ge_us  = ge_sec * 1e6 / args.iterations;

            sk = cpu_sec / gk_sec;
            se = cpu_sec / ge_sec;

            // Correctness
            HostValidationResultStorage cpu_rs, gpu_rs;
            auto cpu_out = cpu_rs.view(n);
            auto gpu_out = gpu_rs.view(n);
            validate_orders_cpu(orders, ctx, cpu_out);
            copy_validation_input_h2d(orders, ctx, d);
            launch_validation_kernel(d);
            copy_validation_output_d2h(d, gpu_out);
            cudaDeviceSynchronize();
            correct = validation_outputs_equal(cpu_out, gpu_out);

            d.free();
        }
#endif
        std::cout << std::setw(10) << n
                  << std::setw(14) << std::fixed << std::setprecision(2) << cpu_us
                  << std::setw(14) << gk_us
                  << std::setw(14) << ge_us
                  << std::setw(11) << std::setprecision(2) << sk << "x"
                  << std::setw(11) << se << "x"
                  << std::setw(8)  << (correct ? "OK" : "FAIL")
                  << "\n";

        if (csv.is_open()) {
            csv << "validation," << n << ","
                << cpu_sec << "," << cpu_us << ","
                << gk_sec << "," << gk_us << ","
                << ge_sec << "," << ge_us << ","
                << sk << "," << se << ","
                << (correct ? 1 : 0) << "\n";
        }
    }

    if (csv.is_open()) {
        csv.close();
        std::cout << "\nWrote " << args.out_csv << "\n";
    }

    return 0;
}
