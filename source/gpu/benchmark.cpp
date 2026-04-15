// gpu_benchmark - times multi-symbol order book analytics on CPU vs GPU.
//
// PRIMARY mode (use this for thesis): real ITCH-derived books, scalability
// sweep done by subsetting (take first N symbols).  Same data as the rest
// of the engine — directly comparable to Phase 5/6 numbers.
//
//   --itch-file <path>  --max-messages M  --symbols 1,10,100,1000,all
//
// FALLBACK mode (use if you can't get the ITCH file):
//   --data synthetic --symbols N
//
// Compute modes:
//   --cpu-only          (skip GPU timing — auto-set when built without CUDA)
//   default             (run both, report speedup)
//
// Output:
//   - Console report
//   - results/gpu_benchmark.csv (one row per symbol-count tested)

#include <iostream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <memory>

#include "trader/gpu/order_book_gpu.h"
#include "trader/gpu/analytics_cpu.h"

#ifdef HAVE_CUDA
#include <cuda_runtime.h>
#include "trader/gpu/analytics_gpu.cuh"
#endif

// ITCH support is optional — only compiled in if --data itch is needed.
#ifdef WITH_ITCH
#include "trader/providers/nasdaq/itch_reader.h"
#include "trader/providers/nasdaq/itch_handler.h"
#include "trader/matching/market_manager.h"
#include "trader/gpu/snapshot.h"
#endif

using namespace Trader::GPU;
using namespace std::chrono;

// -----------------------------------------------------------------------------
// Timing helpers
// -----------------------------------------------------------------------------

static double time_cpu_analytics(const MultiSymbolBook& book,
                                 SymbolAnalytics& out,
                                 int iterations)
{
    auto t0 = steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        compute_analytics_cpu(book, out);
    }
    auto t1 = steady_clock::now();
    return duration<double>(t1 - t0).count();
}

#ifdef HAVE_CUDA
static double time_gpu_analytics_kernel_only(DeviceBuffers& d, int iterations) {
    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    cudaEventRecord(start);
    for (int i = 0; i < iterations; ++i) {
        launch_analytics_kernel(d);
    }
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);

    float ms = 0.0f;
    cudaEventElapsedTime(&ms, start, stop);
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    return ms / 1000.0;
}

static double time_gpu_end_to_end(const MultiSymbolBook& host_book,
                                  SymbolAnalytics& host_out,
                                  DeviceBuffers& d,
                                  int iterations)
{
    auto t0 = steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        copy_book_h2d(host_book, d);
        launch_analytics_kernel(d);
        copy_analytics_d2h(d, host_out);
    }
    cudaDeviceSynchronize();
    auto t1 = steady_clock::now();
    return duration<double>(t1 - t0).count();
}
#endif

// -----------------------------------------------------------------------------
// Correctness check (GPU output must match CPU output bit-for-bit)
// -----------------------------------------------------------------------------

static bool analytics_equal(const SymbolAnalytics& a, const SymbolAnalytics& b) {
    if (a.num_symbols != b.num_symbols) return false;
    for (uint32_t i = 0; i < a.num_symbols; ++i) {
        if (a.best_bid[i]         != b.best_bid[i])         return false;
        if (a.best_ask[i]         != b.best_ask[i])         return false;
        if (a.spread[i]           != b.spread[i])           return false;
        if (a.mid_price[i]        != b.mid_price[i])        return false;
        if (a.total_bid_volume[i] != b.total_bid_volume[i]) return false;
        if (a.total_ask_volume[i] != b.total_ask_volume[i]) return false;
        if (a.vwap_bid_top10[i]   != b.vwap_bid_top10[i])   return false;
        if (a.vwap_ask_top10[i]   != b.vwap_ask_top10[i])   return false;
        if (a.imbalance_x10000[i] != b.imbalance_x10000[i]) return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Args
// -----------------------------------------------------------------------------

struct Args {
    std::string data_mode = "itch";        // "itch" (default) or "synthetic" (fallback)
    std::string itch_file;
    uint64_t    max_messages = 5'000'000;
    bool        cpu_only = false;
    bool        run_correctness = true;
    int         iterations = 1000;
    // 0 in this list means "use all available symbols from ITCH".
    std::vector<uint32_t> symbol_counts = {1, 10, 100, 1000, 0};
    uint64_t    seed = 42;
    std::string out_csv = "results/gpu_benchmark.csv";
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if      (arg == "--data")          a.data_mode = next("--data");
        else if (arg == "--itch-file")     a.itch_file = next("--itch-file");
        else if (arg == "--max-messages")  a.max_messages = std::strtoull(next("--max-messages").c_str(), nullptr, 10);
        else if (arg == "--cpu-only")      a.cpu_only = true;
        else if (arg == "--no-correctness") a.run_correctness = false;
        else if (arg == "--iters")         a.iterations = std::atoi(next("--iters").c_str());
        else if (arg == "--seed")          a.seed = std::strtoull(next("--seed").c_str(), nullptr, 10);
        else if (arg == "--out")           a.out_csv = next("--out");
        else if (arg == "--symbols") {
            std::string list = next("--symbols");
            a.symbol_counts.clear();
            size_t pos = 0;
            while (pos < list.size()) {
                size_t comma = list.find(',', pos);
                a.symbol_counts.push_back(static_cast<uint32_t>(std::strtoul(
                    list.substr(pos, comma - pos).c_str(), nullptr, 10)));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        else if (arg == "-h" || arg == "--help") {
            std::cout <<
                "Usage: gpu_benchmark [options]\n"
                "  --data itch|synthetic      Data source (default itch — REAL data)\n"
                "  --itch-file <path>         Path to ITCH50.gz (required for --data itch)\n"
                "  --max-messages N           ITCH messages to process (default 5M)\n"
                "  --symbols a,b,c,...        Symbol counts to test; 0 = use all from ITCH\n"
                "                             (default 1,10,100,1000,0)\n"
                "  --iters N                  Iterations per timing (default 1000)\n"
                "  --cpu-only                 Skip GPU timing\n"
                "  --no-correctness           Skip CPU/GPU equality check\n"
                "  --seed N                   RNG seed for synthetic data (default 42)\n"
                "  --out path.csv             Output CSV (default results/gpu_benchmark.csv)\n";
            std::exit(0);
        }
        else {
            std::cerr << "Unknown arg: " << arg << "\n";
            std::exit(1);
        }
    }
    return a;
}

// -----------------------------------------------------------------------------
// Build ITCH-derived snapshot (only if WITH_ITCH compiled in)
// -----------------------------------------------------------------------------

#ifdef WITH_ITCH
struct ITCHSnapshot {
    Trader::Matching::MarketManager market_manager;
    HostBookStorage storage;
    MultiSymbolBook book;

    bool load(const std::string& file, uint64_t max_messages) {
        using namespace Trader::Providers::NASDAQ;
        ITCHReader reader;
        if (!reader.open(file)) {
            std::cerr << "Failed to open ITCH file: " << file << "\n";
            return false;
        }
        ITCHHandler handler(market_manager);

        const size_t buffer_size = 65536;
        std::vector<uint8_t> buffer(buffer_size);
        uint64_t count = 0;
        while (!reader.eof()) {
            size_t length = reader.read_message(buffer.data(), buffer_size);
            if (length == 0) break;
            if (length < buffer_size) handler.process_message(buffer.data(), length);
            if (++count >= max_messages) break;
            if (count % 1'000'000 == 0) {
                std::cout << "  ITCH parsed " << count / 1'000'000 << "M messages\n";
            }
        }
        reader.close();
        std::cout << "  Built order books from " << count << " ITCH messages\n";

        book = snapshot_from_market_manager(market_manager, storage);
        std::cout << "  Snapshot: " << book.num_symbols << " symbols\n";
        return true;
    }
};
#endif

// -----------------------------------------------------------------------------
// Main benchmark
// -----------------------------------------------------------------------------

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    std::cout << "========================================\n";
    std::cout << "HFT Tadawul - GPU Analytics Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Data mode:   " << args.data_mode << "\n";
    std::cout << "Iterations:  " << args.iterations << "\n";
    std::cout << "CPU-only:    " << (args.cpu_only ? "yes" : "no") << "\n";

#ifndef HAVE_CUDA
    if (!args.cpu_only) {
        std::cout << "(Built without HAVE_CUDA — running CPU-only)\n";
        args.cpu_only = true;
    }
#endif

    // Open CSV output
    std::ofstream csv(args.out_csv);
    if (csv.is_open()) {
        csv << "num_symbols,cpu_total_sec,cpu_per_iter_us,"
               "gpu_kernel_total_sec,gpu_kernel_per_iter_us,"
               "gpu_e2e_total_sec,gpu_e2e_per_iter_us,"
               "speedup_kernel,speedup_e2e,correctness_ok\n";
    }

#ifdef WITH_ITCH
    // Build the ITCH-derived books ONCE (full set), then create subset views
    // for each value in args.symbol_counts.  Subsetting is cheap because the
    // SoA layout is contiguous — we just adjust num_symbols.
    std::unique_ptr<ITCHSnapshot> itch_snap;
    uint32_t itch_max_symbols = 0;
    if (args.data_mode == "itch") {
        if (args.itch_file.empty()) {
            std::cerr << "--data itch requires --itch-file\n";
            return 1;
        }
        itch_snap = std::make_unique<ITCHSnapshot>();
        if (!itch_snap->load(args.itch_file, args.max_messages)) return 1;
        itch_max_symbols = itch_snap->book.num_symbols;

        // Resolve symbol_counts: replace 0 with the actual max, and clamp others
        std::vector<uint32_t> resolved;
        for (uint32_t n : args.symbol_counts) {
            uint32_t r = (n == 0 || n > itch_max_symbols) ? itch_max_symbols : n;
            if (r > 0) resolved.push_back(r);
        }
        // Dedupe + sort for clean output
        std::sort(resolved.begin(), resolved.end());
        resolved.erase(std::unique(resolved.begin(), resolved.end()), resolved.end());
        args.symbol_counts = resolved;
        std::cout << "  ITCH provides " << itch_max_symbols << " symbols; "
                  << "sweeping " << args.symbol_counts.size() << " sizes\n";
    }
#else
    if (args.data_mode == "itch") {
        std::cerr << "Built without WITH_ITCH — recompile with -DWITH_ITCH.\n";
        return 1;
    }
#endif

    std::cout << "\n"
              << std::setw(10) << "Symbols"
              << std::setw(14) << "CPU/iter(us)"
              << std::setw(14) << "GPUk/iter(us)"
              << std::setw(14) << "GPUe2e/iter(us)"
              << std::setw(14) << "Speedup(k)"
              << std::setw(14) << "Speedup(e2e)"
              << std::setw(8)  << "Match"
              << "\n"
              << std::string(88, '-') << "\n";

    for (uint32_t n : args.symbol_counts) {
        // Build (or view) the book for this size
        HostBookStorage storage;
        MultiSymbolBook host_book;

#ifdef WITH_ITCH
        if (args.data_mode == "itch") {
            // Subset: take first N symbols from the full ITCH-derived book.
            // SoA layout is contiguous — we just clamp num_symbols.
            host_book = itch_snap->book;
            host_book.num_symbols = n;
        } else
#endif
        {
            host_book = storage.view(n);
            generate_synthetic_books(host_book, args.seed);
        }

        // Allocate output storage
        HostAnalyticsStorage cpu_out_store;
        SymbolAnalytics cpu_out = cpu_out_store.view(n);

        // CPU timing
        const double cpu_sec = time_cpu_analytics(host_book, cpu_out, args.iterations);
        const double cpu_us  = cpu_sec * 1e6 / args.iterations;

        double gpu_kernel_sec = 0.0, gpu_kernel_us = 0.0;
        double gpu_e2e_sec = 0.0,    gpu_e2e_us = 0.0;
        double speedup_k = 0.0,      speedup_e2e = 0.0;
        bool correctness = true;

#ifdef HAVE_CUDA
        if (!args.cpu_only) {
            DeviceBuffers d;
            cudaError_t err = d.allocate(n);
            if (err != cudaSuccess) {
                std::cerr << "GPU allocation failed for n=" << n
                          << ": " << cudaGetErrorString(err) << "\n";
                continue;
            }
            copy_book_h2d(host_book, d);

            // Warm-up
            launch_analytics_kernel(d);
            cudaDeviceSynchronize();

            gpu_kernel_sec = time_gpu_analytics_kernel_only(d, args.iterations);
            gpu_kernel_us  = gpu_kernel_sec * 1e6 / args.iterations;

            gpu_e2e_sec = time_gpu_end_to_end(host_book, cpu_out /*reused*/, d, args.iterations);
            gpu_e2e_us  = gpu_e2e_sec * 1e6 / args.iterations;

            speedup_k   = cpu_sec / gpu_kernel_sec;
            speedup_e2e = cpu_sec / gpu_e2e_sec;

            // Correctness: run GPU once, copy back, compare to CPU
            if (args.run_correctness) {
                HostAnalyticsStorage gpu_out_store;
                SymbolAnalytics gpu_out = gpu_out_store.view(n);
                copy_book_h2d(host_book, d);
                launch_analytics_kernel(d);
                copy_analytics_d2h(d, gpu_out);
                cudaDeviceSynchronize();

                // Recompute CPU on a fresh struct for comparison
                HostAnalyticsStorage cpu_ref_store;
                SymbolAnalytics cpu_ref = cpu_ref_store.view(n);
                compute_analytics_cpu(host_book, cpu_ref);

                correctness = analytics_equal(cpu_ref, gpu_out);
            }

            d.free();
        }
#endif

        std::cout << std::setw(10) << n
                  << std::setw(14) << std::fixed << std::setprecision(2) << cpu_us
                  << std::setw(14) << gpu_kernel_us
                  << std::setw(14) << gpu_e2e_us
                  << std::setw(14) << std::setprecision(2) << speedup_k << "x"
                  << std::setw(13) << speedup_e2e << "x"
                  << std::setw(8)  << (correctness ? "OK" : "FAIL")
                  << "\n";

        if (csv.is_open()) {
            csv << n << ","
                << cpu_sec << "," << cpu_us << ","
                << gpu_kernel_sec << "," << gpu_kernel_us << ","
                << gpu_e2e_sec << "," << gpu_e2e_us << ","
                << speedup_k << "," << speedup_e2e << ","
                << (correctness ? 1 : 0) << "\n";
        }
    }

    if (csv.is_open()) {
        csv.close();
        std::cout << "\nWrote " << args.out_csv << "\n";
    }

    return 0;
}
