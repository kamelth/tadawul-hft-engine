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
#include "trader/strategy/signal_table.h"
#include "trader/performance/metrics.h"
#include "trader/performance/market_impact.h"
#include "trader/risk/risk_manager.h"

/**
 * TradeLogger — writes one CSV row per strategy fill.
 *
 * Columns (all prices in raw $0.0001 integer units):
 *   timestamp_ns, symbol, side, fill_price, fill_qty, position_after, realized_pnl
 */
class TradeLogger {
public:
    explicit TradeLogger(const std::string& path) : file_(path) {
        if (file_.is_open()) {
            file_ << "timestamp_ns,symbol,side,fill_price,fill_qty,"
                     "position_after,realized_pnl,mid_at_fill,effective_spread_bps\n";
        } else {
            std::cerr << "Warning: could not open trade log: " << path << "\n";
        }
    }

    bool is_open() const { return file_.is_open(); }

    void log(uint64_t ts_ns, const std::string& symbol, const char* side,
             uint64_t fill_price, uint64_t fill_qty,
             int64_t position_after, int64_t realized_pnl,
             uint64_t mid_at_fill) {
        if (!file_.is_open()) return;

        // Effective spread = 2 × |fill_price - mid| / mid × 10000  (basis points)
        double eff_bps = (mid_at_fill > 0)
            ? (2.0 * std::abs(static_cast<int64_t>(fill_price)
                              - static_cast<int64_t>(mid_at_fill))
               * 10000.0) / mid_at_fill
            : 0.0;

        file_ << ts_ns << ',' << symbol << ',' << side << ','
              << fill_price << ',' << fill_qty << ','
              << position_after << ',' << realized_pnl << ','
              << mid_at_fill << ',' << eff_bps << '\n';
    }

    void flush() { if (file_.is_open()) file_.flush(); }

private:
    std::ofstream file_;
};

using namespace Trader;
using namespace Trader::Providers::NASDAQ;
using namespace Trader::Matching;
using namespace Trader::Strategy;
using namespace Trader::Performance;
using namespace Trader::Risk;

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
        , market_maker_ptr_(nullptr)
        , trade_logger_(nullptr)
        , signal_table_(nullptr)
        , risk_manager_(nullptr)
        , order_count_(0)
        , execution_count_(0)
        , strategy_fills_(0)
        , report_interval_(100000) {}

    void set_impact_collector(MarketImpactCollector* c) { impact_collector_ = c; }
    void set_market_maker(MarketMaker* mm) { market_maker_ptr_ = mm; }
    void set_trade_logger(TradeLogger* logger) { trade_logger_ = logger; }
    void set_signal_table(Strategy::SignalTable* st) { signal_table_ = st; }
    void set_risk_manager(RiskManager* rm) { risk_manager_ = rm; }

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

        // Detect passive fills: did this incoming order hit one of our resting quotes?
        if (market_maker_ptr_ && execution.match_order_id != 0) {
            uint32_t sym_id = 0;
            Matching::OrderSide side;
            if (market_maker_ptr_->lookup_active_order(execution.match_order_id, sym_id, side)) {
                // Our resting order was filled — notify strategy
                strategy_->on_order_filled(
                    execution.match_order_id,
                    sym_id,
                    side,
                    execution.execution_price,
                    execution.execution_quantity,
                    execution.timestamp
                );

                ++strategy_fills_;

                // Log to trade log
                if (trade_logger_) {
                    int64_t pos_after = market_maker_ptr_->get_position(sym_id);
                    int64_t realized_pnl = 0;
                    const Strategy::Position* pos =
                        market_maker_ptr_->get_position_manager().get_position(sym_id);
                    if (pos) realized_pnl = pos->calculate_realized_pnl();

                    const Matching::Symbol* sym =
                        market_manager_.symbol_registry().get_symbol(sym_id);
                    std::string sym_name = sym ? sym->symbol_name
                                               : std::to_string(sym_id);
                    const char* side_str =
                        (side == Matching::OrderSide::Buy) ? "BUY" : "SELL";

                    uint64_t mid_at_fill = market_maker_ptr_->get_mid_price(sym_id);
                    trade_logger_->log(execution.timestamp.nanoseconds(), sym_name, side_str,
                                       execution.execution_price, execution.execution_quantity,
                                       pos_after, realized_pnl, mid_at_fill);
                }

                // Refresh portfolio risk after each fill
                if (risk_manager_) {
                    risk_manager_->refresh_portfolio(
                        market_maker_ptr_->get_position_manager(),
                        market_maker_ptr_->get_mid_prices());
                }
            }
        }
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

        // Feed signal table before strategy runs (volatility + flow imbalance)
        if (signal_table_) {
            signal_table_->update(symbol_id,
                                  (stats.best_bid_price + stats.best_ask_price) / 2,
                                  stats.best_bid_volume,
                                  stats.best_ask_volume);
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
    MarketMaker*           market_maker_ptr_;
    TradeLogger*           trade_logger_;
    Strategy::SignalTable* signal_table_;
    RiskManager*           risk_manager_;
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
        if (orders.empty()) return;

        // ── Portfolio-level gate ─────────────────────────────────────────────
        // If total exposure > limit OR total P&L < stop-loss, block all quotes.
        if (risk_manager_ && !risk_manager_->is_portfolio_ok()) {
            return;
        }

        // ── Batch order validation ───────────────────────────────────────────
        // Build per-order entries and per-symbol context for risk validation.
        std::vector<bool> allowed(orders.size(), true);
        if (risk_manager_ && market_maker_ptr_) {
            std::vector<RiskManager::OrderEntry> entries;
            entries.reserve(orders.size());
            std::unordered_map<uint32_t, RiskManager::SymbolContext> ctx;

            for (const auto& order : orders) {
                entries.push_back({
                    order.symbol_id,
                    (order.side == Matching::OrderSide::Buy) ? 0u : 1u,
                    order.price,
                    order.quantity
                });
                if (ctx.find(order.symbol_id) == ctx.end()) {
                    uint64_t mid = market_maker_ptr_->get_mid_price(order.symbol_id);
                    uint64_t bid = 0, ask = 0;
                    const OrderBook* book = market_manager_.get_order_book(order.symbol_id);
                    if (book) {
                        OrderBookStats bs = book->get_stats();
                        bid = bs.best_bid_price;
                        ask = bs.best_ask_price;
                    }
                    ctx[order.symbol_id] = {
                        bid, ask, mid,
                        market_maker_ptr_->get_position(order.symbol_id)
                    };
                }
            }
            allowed = risk_manager_->validate_batch(entries, ctx);
        }

        for (size_t oi = 0; oi < orders.size(); ++oi) {
            const auto& order = orders[oi];

            if (order.quantity == 0 || order.price == 0) continue;
            if (!allowed[oi]) continue;

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

            // Track immediate fills
            strategy_fills_ += executions.size();

            // If order was not fully filled immediately, it rests in the book.
            // Register it so on_execution() can detect passive fills later.
            if (market_maker_ptr_) {
                uint64_t filled_qty = 0;
                for (const auto& exec : executions) filled_qty += exec.execution_quantity;
                if (filled_qty < order.quantity) {
                    uint64_t resting_id = market_manager_.get_last_order_id();
                    market_maker_ptr_->register_order(resting_id, order.symbol_id, order.side);
                }
            }

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

                // Log fill to trade log (position already updated by on_order_filled)
                if (trade_logger_ && market_maker_ptr_) {
                    int64_t pos_after = market_maker_ptr_->get_position(order.symbol_id);
                    int64_t realized_pnl = 0;
                    const Strategy::Position* pos =
                        market_maker_ptr_->get_position_manager().get_position(order.symbol_id);
                    if (pos) realized_pnl = pos->calculate_realized_pnl();

                    const Matching::Symbol* sym =
                        market_manager_.symbol_registry().get_symbol(order.symbol_id);
                    std::string sym_name = sym ? sym->symbol_name
                                               : std::to_string(order.symbol_id);
                    const char* side_str =
                        (order.side == Matching::OrderSide::Buy) ? "BUY" : "SELL";

                    uint64_t mid_at_fill = market_maker_ptr_->get_mid_price(order.symbol_id);
                    trade_logger_->log(order.timestamp.nanoseconds(), sym_name, side_str,
                                       exec.execution_price, exec.execution_quantity,
                                       pos_after, realized_pnl, mid_at_fill);
                }
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
    std::string trade_log_path;  // empty = disable trade log
    bool use_adaptive = false;        // enable v2 adaptive spread + flow skew
    bool use_adaptive_vol_only = false;  // enable only volatility-adaptive spread (no flow skew)
    RiskManager::Mode risk_mode = RiskManager::Mode::None;
    RiskManager::Limits risk_limits;

    // Parse arguments
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            use_strategy = true;
            strategy_name = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--max-messages") == 0 && i + 1 < argc) {
            max_messages = std::strtoull(argv[i + 1], nullptr, 10);
            ++i;
        } else if (strcmp(argv[i], "--trade-log") == 0 && i + 1 < argc) {
            trade_log_path = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--adaptive") == 0) {
            use_adaptive = true;
        } else if (strcmp(argv[i], "--adaptive-vol") == 0) {
            use_adaptive_vol_only = true;
        } else if (strcmp(argv[i], "--risk-mode") == 0 && i + 1 < argc) {
            std::string rm = argv[++i];
            if      (rm == "cpu") risk_mode = RiskManager::Mode::CPU;
            else if (rm == "gpu") risk_mode = RiskManager::Mode::GPU;
            else if (rm == "none") risk_mode = RiskManager::Mode::None;
            else { std::cerr << "Unknown --risk-mode: " << rm << "\n"; return 1; }
        } else if (strcmp(argv[i], "--max-exposure") == 0 && i + 1 < argc) {
            risk_limits.max_gross_exposure = std::stoll(argv[++i]) * 10000LL; // convert $ to $0.0001
        } else if (strcmp(argv[i], "--stop-loss") == 0 && i + 1 < argc) {
            risk_limits.stop_loss_threshold = std::stoll(argv[++i]) * 10000LL;
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
            params.spread_ticks          = 10;
            params.quote_size            = 100;
            params.max_position          = 1000;
            params.enable_inventory_skew = true;

            if (use_adaptive || use_adaptive_vol_only) {
                params.enable_adaptive_spread = true;
                params.vol_multiplier         = 3;    // spread = 3 × σ
                params.min_spread_ticks       = 5;    // floor  $0.0005
                params.max_spread_ticks       = 200;  // ceiling $0.0200
            }
            if (use_adaptive) {
                params.enable_flow_skew       = true;
                params.flow_skew_factor       = 2;    // 2 ticks per 1000 imbalance units
            }

            const char* mode_tag = use_adaptive         ? " (ADAPTIVE v2 vol+flow)"
                                 : use_adaptive_vol_only ? " (ADAPTIVE v2 vol-only)"
                                 : " (Fixed v1)";
            market_maker = new MarketMaker(params);
            std::cout << "Strategy: Market Maker" << mode_tag << std::endl;
            std::cout << "  Spread: " << params.spread_ticks << " ticks ($"
                      << (params.spread_ticks / 10000.0) << ")" << std::endl;
            std::cout << "  Quote Size: " << params.quote_size << " shares" << std::endl;
            std::cout << "  Max Position: " << params.max_position << " shares" << std::endl;
            if (use_adaptive || use_adaptive_vol_only) {
                std::cout << "  Adaptive spread: ON  (vol_mult=" << params.vol_multiplier
                          << ", min=" << params.min_spread_ticks
                          << ", max=" << params.max_spread_ticks << " ticks)" << std::endl;
                std::cout << "  Flow skew:       "
                          << (use_adaptive ? "ON  (factor=" + std::to_string(params.flow_skew_factor) + ")" : "OFF")
                          << std::endl;
            }
        } else {
            std::cerr << "Unknown strategy: " << strategy_name << std::endl;
            return 1;
        }
    }

    // Create risk manager
    RiskManager* risk_manager = nullptr;
    if (risk_mode != RiskManager::Mode::None) {
        if (use_strategy) {
            risk_limits.max_position = 1000; // sync with MarketMaker params
            risk_manager = new RiskManager(risk_mode, risk_limits);
            const char* rm_str = (risk_mode == RiskManager::Mode::GPU) ? "GPU" : "CPU";
            std::cout << "Risk Management: " << rm_str << " mode\n";
            std::cout << "  Max gross exposure: $"
                      << (risk_limits.max_gross_exposure / 10000.0) << "\n";
            std::cout << "  Stop-loss:          $"
                      << (risk_limits.stop_loss_threshold / 10000.0) << "\n";
            std::cout << "  Max position/sym:   " << risk_limits.max_position << " shares\n";
            std::cout << "  Price band:         " << risk_limits.price_band_bps << " bps\n";
        }
    } else {
        std::cout << "Risk Management: disabled (use --risk-mode cpu|gpu to enable)\n";
    }

    // Create market impact collector (sample every 10s of ITCH time)
    MarketImpactCollector impact_collector(market_manager, 10'000'000'000ULL);

    // Results directory (used by trade log, market impact, and performance report)
    const std::string results_dir = "results";

    // Create trade logger (writes fills CSV for Python validation)
    // Default path when strategy is active and no --trade-log given: results/trade_log.csv
    if (use_strategy && trade_log_path.empty()) {
        trade_log_path = results_dir + "/trade_log.csv";
    }
    TradeLogger* trade_logger = nullptr;
    if (!trade_log_path.empty()) {
        trade_logger = new TradeLogger(trade_log_path);
        if (trade_logger->is_open()) {
            std::cout << "Trade log: " << trade_log_path << std::endl;
        }
    }

    // Create signal table — feeds volatility σ and order-flow imbalance to the
    // adaptive market maker.  Created unconditionally so the handler can always
    // call set_signal_table(); the market maker only reads it when its own
    // enable_adaptive_spread / enable_flow_skew flags are true.
    Strategy::SignalTable signal_table;

    // Create strategy handler
    StrategyHandler strategy_handler(market_manager, market_maker, metrics);
    strategy_handler.set_impact_collector(&impact_collector);
    if (market_maker) {
        strategy_handler.set_market_maker(market_maker);
        market_maker->set_signal_table(&signal_table);
    }
    if (trade_logger) {
        strategy_handler.set_trade_logger(trade_logger);
    }
    strategy_handler.set_signal_table(&signal_table);
    if (risk_manager) {
        strategy_handler.set_risk_manager(risk_manager);
    }
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

    // ---- Risk management report ----
    if (risk_manager) {
        risk_manager->write_report(std::cout);
        delete risk_manager;
    }

    // Cleanup
    if (trade_logger) {
        trade_logger->flush();
        std::cout << "Wrote " << trade_log_path << "\n";
        delete trade_logger;
    }
    if (market_maker) {
        delete market_maker;
    }

    return 0;
}
