#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include "trader/providers/nasdaq/itch_reader.h"
#include "trader/providers/nasdaq/itch_handler.h"
#include "trader/matching/market_manager.h"
#include "trader/strategy/market_maker.h"
#include "trader/performance/metrics.h"
#include "trader/performance/market_impact.h"

using namespace Trader;
using namespace Trader::Providers::NASDAQ;
using namespace Trader::Matching;
using namespace Trader::Strategy;
using namespace Trader::Performance;

/**
 * Strategy-Aware Market Handler
 *
 * Connects market events to the trading strategy.
 */
class StrategyHandler : public MarketHandler {
public:
    StrategyHandler(MarketManager& market_manager, StrategyBase* strategy, EngineMetrics& metrics)
        : market_manager_(market_manager)
        , strategy_(strategy)
        , metrics_(metrics)
        , impact_collector_(nullptr)
        , order_count_(0)
        , execution_count_(0)
        , strategy_fills_(0)
        , report_interval_(100000) {}

    void set_impact_collector(MarketImpactCollector* c) { impact_collector_ = c; }

    void on_order_added(const Order* order) override {
        (void)order;
        ++order_count_;
        metrics_.book_events.record();
        if (order_count_ % report_interval_ == 0) {
            print_progress();
        }
    }

    void on_execution(const Execution& execution) override {
        ++execution_count_;
        metrics_.book_events.record();

        // Notify strategy of market execution
        // (Note: we don't have symbol_id in Execution, would need to track it separately)
        // For now, skip this notification as it's not critical for market maker
        (void)execution;
    }

    void on_order_book_update(uint32_t symbol_id,
                              const OrderBookStats& stats,
                              const Core::Timestamp& timestamp) override {
        metrics_.book_events.record();
        metrics_.note_itch_timestamp(timestamp.nanoseconds());
        if (impact_collector_) {
            impact_collector_->maybe_sample(timestamp.nanoseconds());
        }

        // Re-entrancy guard: when we submit a strategy order, the market
        // manager calls this callback again with the updated stats. We
        // must not invoke the strategy from inside its own quote flush,
        // or we'd recurse until the stack explodes.
        if (in_strategy_) return;
        if (!strategy_) return;

        // Only process if we have valid market (bid and ask present)
        if (stats.best_bid_price == 0 || stats.best_ask_price == 0) {
            return;
        }

        // Time the strategy decision path (callback + quote emission).
        in_strategy_ = true;
        {
            ScopedTimer t(metrics_.strategy_decide);
            strategy_->on_order_book_update(symbol_id, stats, timestamp);
            process_strategy_orders(timestamp);
        }
        in_strategy_ = false;
    }

    void print_progress() const {
        std::cout << "Processed: " << order_count_ << " orders, "
                  << execution_count_ << " executions, "
                  << strategy_fills_ << " strategy fills\r" << std::flush;
    }

    void print_final() const {
        std::cout << "\nFinal: " << order_count_ << " market orders, "
                  << execution_count_ << " market executions, "
                  << strategy_fills_ << " strategy fills" << std::endl;
    }

    uint64_t get_strategy_fills() const {
        return strategy_fills_;
    }

private:
    MarketManager& market_manager_;
    StrategyBase* strategy_;
    EngineMetrics& metrics_;
    MarketImpactCollector* impact_collector_;
    uint64_t order_count_;
    uint64_t execution_count_;
    uint64_t strategy_fills_;
    uint64_t report_interval_;

    // Re-entrancy guard for the full strategy callback (guards the whole
    // decide -> flush path so nested book updates from our own quote
    // submissions don't recurse into the strategy).
    bool in_strategy_ = false;

    void process_strategy_orders(const Core::Timestamp& timestamp) {
        if (!strategy_) {
            return;
        }

        // Get pending orders from strategy
        auto orders = strategy_->get_pending_orders();

        for (const auto& order : orders) {
            // Validate order
            if (order.quantity == 0 || order.price == 0) {
                continue;
            }

            metrics_.strategy_quotes.record();

            // Submit order to market
            auto executions = market_manager_.add_order(
                order.symbol_id,
                order.side,
                order.type,
                order.price,
                order.quantity,
                0,  // original_order_id (not used for strategy orders)
                order.timestamp
            );

            // Track fills
            strategy_fills_ += executions.size();

            // Notify strategy of immediate fills
            for (const auto& exec : executions) {
                strategy_->on_order_filled(
                    exec.order_id,
                    order.symbol_id,  // Use symbol_id from the strategy order
                    order.side,
                    exec.execution_price,     // Correct field name
                    exec.execution_quantity,  // Correct field name
                    order.timestamp
                );
            }

            // If order wasn't fully filled, register it as active
            // (for now we'll skip this - only handling immediate fills)
        }

        // Process pending cancellations using the current ITCH timestamp
        auto cancellations = strategy_->get_pending_cancellations();
        for (uint64_t order_id : cancellations) {
            market_manager_.cancel_order(order_id, timestamp);
        }
    }
};

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "HFT Tadawul Engine - Strategy Mode" << std::endl;
    std::cout << "========================================" << std::endl;

    // Parse command line arguments
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <itch_file> [--strategy market_maker] [symbol1 symbol2 ...]" << std::endl;
        std::cerr << "Example: " << argv[0] << " data/01302020.NASDAQ_ITCH50.gz --strategy market_maker AAPL MSFT" << std::endl;
        return 1;
    }

    std::string filename = argv[1];
    bool use_strategy = false;
    std::string strategy_name;
    std::vector<std::string> filter_symbols;
    uint64_t max_messages = 0;  // 0 = process entire file

    // Parse arguments
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            use_strategy = true;
            strategy_name = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--max-messages") == 0 && i + 1 < argc) {
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

    // Engine-wide metrics (wall-clock latency + throughput)
    EngineMetrics metrics;

    // Create strategy (if requested)
    MarketMaker* market_maker = nullptr;
    if (use_strategy) {
        if (strategy_name == "market_maker") {
            MarketMaker::Params params;
            params.spread_ticks = 10;        // 10 ticks = $0.0010
            params.quote_size = 100;         // 100 shares per quote
            params.max_position = 1000;      // Max 1000 shares
            params.enable_inventory_skew = true;

            market_maker = new MarketMaker(params);
            std::cout << "Strategy: Market Maker" << std::endl;
            std::cout << "  Spread: " << params.spread_ticks << " ticks ($"
                      << (params.spread_ticks / 10000.0) << ")" << std::endl;
            std::cout << "  Quote Size: " << params.quote_size << " shares" << std::endl;
            std::cout << "  Max Position: " << params.max_position << " shares" << std::endl;
        } else {
            std::cerr << "Unknown strategy: " << strategy_name << std::endl;
            return 1;
        }
    }

    // Create market impact collector (sample every 10s of ITCH time)
    MarketImpactCollector impact_collector(market_manager, 10'000'000'000ULL);

    // Create strategy handler
    StrategyHandler strategy_handler(market_manager, market_maker, metrics);
    strategy_handler.set_impact_collector(&impact_collector);
    market_manager.set_handler(&strategy_handler);

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
                // (parse -> dispatch -> book update -> strategy callback).
                ScopedTimer t(metrics.itch_message);
                success = itch_handler.process_message(buffer, length);
            }
            metrics.itch_messages.record();
            if (!success) {
                ++error_count;
                if (error_count > 100) {
                    std::cerr << "\nError: Too many processing errors, stopping." << std::endl;
                    break;
                }
            } else {
                error_count = 0;
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

    // Finalize symbol ordering
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

    // Print strategy statistics if enabled
    if (market_maker) {
        std::cout << "\n========================================" << std::endl;
        std::cout << "Strategy Statistics" << std::endl;
        std::cout << "========================================" << std::endl;

        std::cout << "Strategy Fills:   " << strategy_handler.get_strategy_fills() << std::endl;

        // Print per-symbol positions
        std::cout << "\nPositions:" << std::endl;
        std::cout << std::left << std::setw(10) << "Symbol"
                  << std::right << std::setw(12) << "Shares"
                  << std::setw(15) << "Avg Buy"
                  << std::setw(15) << "Avg Sell"
                  << std::setw(15) << "Realized PnL" << std::endl;
        std::cout << std::string(67, '-') << std::endl;

        const auto& position_manager = market_maker->get_position_manager();
        for (const auto& symbol : market_manager.symbol_registry().get_all_symbols()) {
            const Position* pos = position_manager.get_position(symbol.symbol_id);
            if (pos && (pos->get_shares() > 0 || pos->get_total_sold() > 0)) {
                std::cout << std::left << std::setw(10) << symbol.symbol_name
                          << std::right << std::setw(12) << pos->get_shares()
                          << std::setw(15) << std::fixed << std::setprecision(2)
                          << (pos->get_avg_buy_price() / 10000.0)
                          << std::setw(15) << (pos->get_avg_sell_price() / 10000.0)
                          << std::setw(15) << (pos->calculate_realized_pnl() / 10000.0)
                          << std::endl;
            }
        }

        std::cout << "\nTotal PnL: $" << std::fixed << std::setprecision(2)
                  << (market_maker->get_total_pnl() / 10000.0) << std::endl;
    }

    // Print per-symbol order book statistics
    std::cout << "\nPer-Symbol Order Book Statistics:" << std::endl;
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
                      << std::setw(12) << std::fixed << std::setprecision(2)
                      << (book_stats.best_bid_price / 10000.0)
                      << std::setw(12) << (book_stats.best_ask_price / 10000.0)
                      << std::setw(12) << (book_stats.spread / 10000.0) << std::endl;
        }
    }

    std::cout << "========================================" << std::endl;

    const std::string results_dir = "results";

    // ---- Market impact snapshots (HFT) ----
    std::cout << "\n";
    impact_collector.write_summary(std::cout);
    impact_collector.write_csv(results_dir + "/hft_snapshots.csv");

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

    // Cleanup
    if (market_maker) {
        delete market_maker;
    }

    return 0;
}
