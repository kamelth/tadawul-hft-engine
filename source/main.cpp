#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include "trader/providers/nasdaq/itch_reader.h"
#include "trader/providers/nasdaq/itch_handler.h"
#include "trader/matching/market_manager.h"
#include "trader/performance/metrics.h"
#include "trader/performance/market_impact.h"

using namespace Trader;
using namespace Trader::Providers::NASDAQ;
using namespace Trader::Matching;
using namespace Trader::Performance;

/**
 * Simple progress callback with debugging + engine metrics integration
 */
class ProgressHandler : public MarketHandler {
public:
    explicit ProgressHandler(EngineMetrics& metrics)
        : metrics_(metrics)
        , impact_collector_(nullptr)
        , order_count_(0)
        , execution_count_(0)
        , report_interval_(100000)
        , debug_(false) {}

    void set_debug(bool debug) { debug_ = debug; }
    void set_impact_collector(MarketImpactCollector* c) { impact_collector_ = c; }

    void on_order_added(const Order* /*order*/) override {
        ++order_count_;
        metrics_.book_events.record();
        if (order_count_ % report_interval_ == 0) {
            print_progress();
        }
    }

    void on_execution(const Execution& /*execution*/) override {
        ++execution_count_;
        metrics_.book_events.record();
    }

    void on_order_book_update(uint32_t /*symbol_id*/,
                              const OrderBookStats& /*stats*/,
                              const Core::Timestamp& timestamp) override {
        metrics_.note_itch_timestamp(timestamp.nanoseconds());
        if (impact_collector_) {
            impact_collector_->maybe_sample(timestamp.nanoseconds());
        }
    }

    void print_progress() const {
        std::cout << "Processed: " << order_count_ << " orders, "
                  << execution_count_ << " executions\r" << std::flush;
    }

    void print_final() const {
        std::cout << "\nFinal: " << order_count_ << " orders, "
                  << execution_count_ << " executions" << std::endl;
    }

private:
    EngineMetrics& metrics_;
    MarketImpactCollector* impact_collector_;
    uint64_t order_count_;
    uint64_t execution_count_;
    uint64_t report_interval_;
    bool debug_;
};

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "HFT Tadawul Engine - ITCH Processor" << std::endl;
    std::cout << "========================================" << std::endl;

    // Parse command line arguments
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <itch_file> [symbol1 symbol2 ...]" << std::endl;
        std::cerr << "Example: " << argv[0] << " data/01302020.NASDAQ_ITCH50.gz AAPL MSFT AMZN" << std::endl;
        return 1;
    }

    std::string filename = argv[1];

    // Optional symbol filtering + max-messages flag
    std::vector<std::string> filter_symbols;
    uint64_t max_messages = 0;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--max-messages") == 0 && i + 1 < argc) {
            max_messages = std::strtoull(argv[i + 1], nullptr, 10);
            ++i;
        } else {
            filter_symbols.push_back(argv[i]);
        }
    }
    if (max_messages > 0) {
        std::cout << "Max messages: " << max_messages << std::endl;
    }

    // Create market manager
    MarketManager market_manager;

    // Engine-wide performance metrics
    EngineMetrics metrics;

    // Create market impact collector (sample every 10s of ITCH time)
    MarketImpactCollector impact_collector(market_manager, 10'000'000'000ULL);

    // Create progress handler
    ProgressHandler progress_handler(metrics);
    progress_handler.set_impact_collector(&impact_collector);
    market_manager.set_handler(&progress_handler);

    // Create ITCH handler
    ITCHHandler itch_handler(market_manager);

    // Enable symbol filtering if specified
    if (!filter_symbols.empty()) {
        std::cout << "Symbol filter enabled: ";
        for (const auto& symbol : filter_symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;
        itch_handler.enable_symbol_filter(filter_symbols);
    } else {
        std::cout << "Processing all symbols" << std::endl;
    }

    // Open ITCH file
    std::cout << "Opening file: " << filename << std::endl;
    ITCHReader reader;
    if (!reader.open(filename)) {
        std::cerr << "Error: Failed to open file: " << filename << std::endl;
        return 1;
    }

    // Process messages
    std::cout << "Processing ITCH messages..." << std::endl;

    const size_t buffer_size = 65536;
    uint8_t buffer[buffer_size];
    uint64_t message_count = 0;
    uint64_t error_count = 0;

    while (!reader.eof()) {
        size_t length = reader.read_message(buffer, buffer_size);
        if (length == 0) {
            break;
        }

        if (length > 0 && length < buffer_size) {
            bool success;
            {
                // Wall-clock timing of the full ITCH message processing path
                // (parse -> dispatch -> book update).
                ScopedTimer t(metrics.itch_message);
                success = itch_handler.process_message(buffer, length);
            }
            metrics.itch_messages.record();
            if (!success) {
                ++error_count;
                // Stop if too many consecutive errors
                if (error_count > 100) {
                    std::cerr << "\nError: Too many processing errors, stopping." << std::endl;
                    break;
                }
            } else {
                error_count = 0;  // Reset on success
            }
        }

        ++message_count;

        // Report progress + throughput sample every 1M messages
        if (message_count % 1000000 == 0) {
            auto sample = metrics.itch_messages.sample();
            std::cout << "Processed " << message_count / 1000000 << "M messages"
                      << "  (rate=" << std::fixed << std::setprecision(0)
                      << sample.interval_rate << " msgs/s)" << std::endl;
        }

        if (max_messages > 0 && message_count >= max_messages) {
            std::cout << "Reached --max-messages limit (" << max_messages
                      << "), stopping." << std::endl;
            break;
        }
    }

    reader.close();

    // Finalize symbol ordering for determinism
    market_manager.symbol_registry().finalize_deterministic_ordering();

    // Print final statistics
    std::cout << "\n========================================" << std::endl;
    std::cout << "Processing Complete" << std::endl;
    std::cout << "========================================" << std::endl;

    const ITCHStats& stats = itch_handler.get_stats();
    std::cout << "ITCH Messages:" << std::endl;
    std::cout << "  Total processed:  " << stats.messages_processed << std::endl;
    std::cout << "  Skipped:          " << stats.messages_skipped << std::endl;
    std::cout << "  Add orders:       " << stats.add_orders << std::endl;
    std::cout << "  Executions:       " << stats.executions << std::endl;
    std::cout << "  Cancels:          " << stats.cancels << std::endl;
    std::cout << "  Deletes:          " << stats.deletes << std::endl;
    std::cout << "  Replaces:         " << stats.replaces << std::endl;
    std::cout << "  Errors:           " << stats.errors << std::endl;

    const MarketStats& market_stats = market_manager.get_market_stats();
    std::cout << "\nMarket Statistics:" << std::endl;
    std::cout << "  Symbols:          " << market_stats.symbol_count << std::endl;
    std::cout << "  Total orders:     " << market_stats.total_orders << std::endl;
    std::cout << "  Total executions: " << market_stats.total_executions << std::endl;
    std::cout << "  Volume traded:    " << market_stats.total_volume_traded << std::endl;

    // Print per-symbol statistics
    std::cout << "\nPer-Symbol Statistics:" << std::endl;
    std::cout << std::left << std::setw(10) << "Symbol"
              << std::right << std::setw(12) << "Orders"
              << std::setw(12) << "Bid Price"
              << std::setw(12) << "Ask Price"
              << std::setw(12) << "Spread" << std::endl;
    std::cout << std::string(58, '-') << std::endl;

    for (const auto& symbol : market_manager.symbol_registry().get_all_symbols()) {
        const OrderBook* book = market_manager.get_order_book(symbol.symbol_id);
        if (book && book->order_count() > 0) {
            OrderBookStats book_stats = book->get_stats();
            std::cout << std::left << std::setw(10) << symbol.symbol_name
                      << std::right << std::setw(12) << book->order_count()
                      << std::setw(12) << (book_stats.best_bid_price / 10000.0)
                      << std::setw(12) << (book_stats.best_ask_price / 10000.0)
                      << std::setw(12) << (book_stats.spread / 10000.0) << std::endl;
        }
    }

    std::cout << "========================================" << std::endl;

    const std::string results_dir = "results";

    // ---- Market impact snapshots (baseline) ----
    std::cout << "\n";
    impact_collector.write_summary(std::cout);
    impact_collector.write_csv(results_dir + "/baseline_snapshots.csv");

    // ---- Performance report ----
    std::cout << "\n";
    metrics.write_report(std::cout);
    std::ofstream report_file(results_dir + "/performance_report.txt");
    if (report_file.is_open()) {
        metrics.write_report(report_file);
        std::cout << "\nWrote " << results_dir << "/performance_report.txt\n";
    } else {
        std::cerr << "\nWarning: could not write " << results_dir
                  << "/performance_report.txt (does the directory exist?)\n";
    }
    metrics.write_artifacts(results_dir);
    std::cout << "Wrote " << results_dir
              << "/latency_*.csv and throughput_*.csv\n";

    return 0;
}
