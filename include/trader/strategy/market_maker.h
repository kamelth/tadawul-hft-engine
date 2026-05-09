#ifndef TRADER_STRATEGY_MARKET_MAKER_H
#define TRADER_STRATEGY_MARKET_MAKER_H

#include "trader/strategy/strategy_base.h"
#include "trader/strategy/position.h"
#include "trader/strategy/signal_table.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Trader {
namespace Strategy {

/**
 * Market Maker Strategy
 *
 * Provides two-sided liquidity by quoting bid and ask prices.
 *
 * Features:
 * - Fixed spread around mid-price
 * - Inventory skewing (adjust quotes based on position)
 * - Position limits (max inventory)
 * - Deterministic behavior (no randomness)
 */
class MarketMaker : public StrategyBase {
public:
    /**
     * Market Maker Parameters (all deterministic)
     */
    struct Params {
        uint64_t spread_ticks;         // Base spread (ticks) — used when volatility=0
        uint64_t quote_size;           // Size of each quote (shares)
        int64_t  max_position;         // Max position (long, shares)
        uint64_t skew_per_share;       // Inventory skew: ticks per share held
        bool     enable_inventory_skew;

        // ── v2: GPU-signal-driven adaptive parameters ──────────────────────
        bool     enable_adaptive_spread;  // Scale spread by realized volatility
        uint64_t vol_multiplier;          // spread = max(spread_ticks, vol_mult × σ)
        uint64_t min_spread_ticks;        // Floor: never quote tighter than this
        uint64_t max_spread_ticks;        // Ceiling: never quote wider than this

        bool     enable_flow_skew;        // Skew quotes against order-flow imbalance
        uint64_t flow_skew_factor;        // Additional skew ticks per 1000 imbalance units

        Params()
            : spread_ticks(10)
            , quote_size(100)
            , max_position(1000)
            , skew_per_share(1)
            , enable_inventory_skew(true)
            , enable_adaptive_spread(false)
            , vol_multiplier(3)           // spread = 3 × σ  (3-sigma band)
            , min_spread_ticks(5)         // never tighter than $0.0005
            , max_spread_ticks(200)       // never wider than $0.0200
            , enable_flow_skew(false)
            , flow_skew_factor(2)         // 2 ticks per 1000 imbalance units
        {}
    };

    explicit MarketMaker(const Params& params = Params())
        : params_(params)
        , signals_(nullptr)
        , position_manager_()
        , pending_orders_()
        , pending_cancellations_()
        , active_orders_()
        , last_mid_prices_() {}

    /**
     * Attach the live signal table (volatility + flow imbalance).
     * Must be set before trading starts if adaptive features are enabled.
     */
    void set_signal_table(const SignalTable* signals) { signals_ = signals; }

    // StrategyBase interface implementation
    void on_order_book_update(
        uint32_t symbol_id,
        const Matching::OrderBookStats& stats,
        const Core::Timestamp& timestamp
    ) override {
        if (stats.best_bid_price == 0 || stats.best_ask_price == 0) return;

        uint64_t mid_price = (stats.best_bid_price + stats.best_ask_price) / 2;
        last_mid_prices_[symbol_id] = mid_price;

        Position& position        = position_manager_.get_position(symbol_id);
        int64_t   current_position = position.get_shares();
        bool can_buy  = current_position < params_.max_position;
        bool can_sell = current_position > 0;

        cancel_symbol_orders(symbol_id);

        // ── Signal 1: Volatility-adaptive spread ──────────────────────────
        uint64_t half_spread = params_.spread_ticks / 2;  // baseline fallback

        if (params_.enable_adaptive_spread && signals_ != nullptr) {
            uint64_t sigma = signals_->volatility_ticks(symbol_id);
            if (sigma > 0) {
                uint64_t adaptive = params_.vol_multiplier * sigma;
                // Floor: never TIGHTER than the baseline spread_ticks
                // (min_spread_ticks is an additional absolute lower bound)
                adaptive = std::max({adaptive, params_.min_spread_ticks, params_.spread_ticks});
                // Ceiling
                adaptive = std::min(adaptive, params_.max_spread_ticks);
                half_spread = adaptive / 2;
            }
        }

        // ── Signal 2: Order-flow imbalance skew ───────────────────────────
        // flow_skew > 0  →  buying pressure  (price likely rising)
        // flow_skew < 0  →  selling pressure (price likely falling)
        //
        // Applied SYMMETRICALLY (quote-following / market-shift):
        //   both bid and ask shift in the direction of the imbalance.
        //
        //   Buying pressure  → shift both quotes UP:
        //     bid higher  (we follow the market, not selling out early)
        //     ask higher  (extract premium from aggressive buyers)
        //
        //   Selling pressure → shift both quotes DOWN:
        //     bid lower   (don't buy into a falling market)
        //     ask lower   (stay competitive against an eager seller side)
        //
        // This is the "Avellaneda-Stoikov" approach: the whole quote ladder
        // tracks the inferred fair value shift rather than just widening one side.
        int64_t flow_skew = 0;
        if (params_.enable_flow_skew && signals_ != nullptr) {
            int64_t imb = signals_->imbalance(symbol_id);  // [-10000, +10000]
            flow_skew = (imb * static_cast<int64_t>(params_.flow_skew_factor)) / 1000;
        }

        // ── Inventory skew (symmetric — shifts whole quote to reduce inventory) ──
        int64_t inventory_skew = 0;
        if (params_.enable_inventory_skew) {
            inventory_skew = current_position
                           * static_cast<int64_t>(params_.skew_per_share);
        }

        // ── Generate quotes ───────────────────────────────────────────────
        // bid:  mid - half_spread - inventory_skew + flow_skew
        //   inventory_skew: when long, lower bid (buy less aggressively)
        //   flow_skew:      positive = buying pressure → bid shifts UP (follow market)
        //                   negative = selling pressure → bid shifts DOWN (don't catch knife)
        //
        // ask:  mid + half_spread - inventory_skew + flow_skew
        //   inventory_skew: when long, lower ask (sell more to reduce inventory)
        //   flow_skew:      positive = buying pressure → ask shifts UP (charge premium)
        //                   negative = selling pressure → ask shifts DOWN (stay competitive)
        if (can_buy) {
            int64_t bid_price = static_cast<int64_t>(mid_price)
                              - static_cast<int64_t>(half_spread)
                              - inventory_skew
                              + flow_skew;

            if (bid_price > 0 && bid_price < static_cast<int64_t>(stats.best_ask_price)) {
                pending_orders_.push_back(StrategyOrder(
                    symbol_id, Matching::OrderSide::Buy, Matching::OrderType::Limit,
                    static_cast<uint64_t>(bid_price), params_.quote_size, timestamp));
            }
        }

        if (can_sell) {
            int64_t ask_price = static_cast<int64_t>(mid_price)
                              + static_cast<int64_t>(half_spread)
                              - inventory_skew
                              + flow_skew;

            if (ask_price > static_cast<int64_t>(stats.best_bid_price)) {
                pending_orders_.push_back(StrategyOrder(
                    symbol_id, Matching::OrderSide::Sell, Matching::OrderType::Limit,
                    static_cast<uint64_t>(ask_price), params_.quote_size, timestamp));
            }
        }
    }

    void on_execution(
        uint32_t symbol_id,
        const Matching::Execution& execution,
        const Core::Timestamp& timestamp
    ) override {
        // Market executions don't affect our strategy directly
        // We react to our own fills via on_order_filled()
        (void)symbol_id;
        (void)execution;
        (void)timestamp;
    }

    void on_order_filled(
        uint64_t order_id,
        uint32_t symbol_id,
        Matching::OrderSide side,
        uint64_t fill_price,
        uint64_t fill_quantity,
        const Core::Timestamp& timestamp
    ) override {
        (void)timestamp;

        // Update position
        Position& position = position_manager_.get_position(symbol_id);

        if (side == Matching::OrderSide::Buy) {
            position.add_shares(fill_quantity, fill_price);
        } else {
            position.remove_shares(fill_quantity, fill_price);
        }

        // Remove from active orders
        active_orders_.erase(order_id);
    }

    std::vector<StrategyOrder> get_pending_orders() override {
        std::vector<StrategyOrder> orders = std::move(pending_orders_);
        pending_orders_.clear();
        return orders;
    }

    std::vector<uint64_t> get_pending_cancellations() override {
        std::vector<uint64_t> cancellations = std::move(pending_cancellations_);
        pending_cancellations_.clear();
        return cancellations;
    }

    std::string get_name() const override {
        return "MarketMaker";
    }

    void reset() override {
        position_manager_.reset();
        pending_orders_.clear();
        pending_cancellations_.clear();
        active_orders_.clear();
        last_mid_prices_.clear();
    }

    // Additional methods for monitoring

    /**
     * Register a resting order for passive-fill tracking.
     * Called after add_order() when the order was not immediately fully filled.
     */
    void register_order(uint64_t order_id, uint32_t symbol_id, Matching::OrderSide side) {
        active_orders_[order_id] = {symbol_id, side};
    }

    /**
     * Look up a resting strategy order by ID.
     * Returns true and fills symbol_id/side if found; false if not ours.
     */
    bool lookup_active_order(uint64_t order_id,
                             uint32_t& symbol_id,
                             Matching::OrderSide& side) const {
        auto it = active_orders_.find(order_id);
        if (it == active_orders_.end()) return false;
        symbol_id = it->second.first;
        side      = it->second.second;
        return true;
    }

    /**
     * Get position manager (for PnL reporting)
     */
    const PositionManager& get_position_manager() const {
        return position_manager_;
    }

    /**
     * Get last known mid-price for a symbol (used for effective spread logging)
     */
    uint64_t get_mid_price(uint32_t symbol_id) const {
        auto it = last_mid_prices_.find(symbol_id);
        return (it != last_mid_prices_.end()) ? it->second : 0;
    }

    const std::unordered_map<uint32_t, uint64_t>& get_mid_prices() const {
        return last_mid_prices_;
    }

    /**
     * Get current position for a symbol
     */
    int64_t get_position(uint32_t symbol_id) const {
        const Position* pos = position_manager_.get_position(symbol_id);
        return pos ? pos->get_shares() : 0;
    }

    /**
     * Get total PnL (realized + unrealized)
     */
    int64_t get_total_pnl() const {
        int64_t realized = position_manager_.calculate_total_realized_pnl();
        int64_t unrealized = position_manager_.calculate_total_unrealized_pnl(last_mid_prices_);
        return realized + unrealized;
    }

private:
    Params             params_;
    const SignalTable* signals_;    // not owned — set via set_signal_table()
    PositionManager    position_manager_;
    std::vector<StrategyOrder> pending_orders_;
    std::vector<uint64_t> pending_cancellations_;
    // order_id → {symbol_id, side}  (resting quotes awaiting passive fill)
    std::unordered_map<uint64_t, std::pair<uint32_t, Matching::OrderSide>> active_orders_;
    std::unordered_map<uint32_t, uint64_t> last_mid_prices_;  // symbol_id → mid_price

    /**
     * Cancel all active orders for a symbol
     */
    void cancel_symbol_orders(uint32_t symbol_id) {
        for (const auto& [order_id, sym_side] : active_orders_) {
            if (sym_side.first == symbol_id) {
                pending_cancellations_.push_back(order_id);
            }
        }
    }
};

} // namespace Strategy
} // namespace Trader

#endif // TRADER_STRATEGY_MARKET_MAKER_H
