#ifndef TRADER_STRATEGY_MARKET_MAKER_H
#define TRADER_STRATEGY_MARKET_MAKER_H

#include "trader/strategy/strategy_base.h"
#include "trader/strategy/position.h"
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
        uint64_t spread_ticks;         // Spread in ticks (1 tick = $0.0001)
        uint64_t quote_size;           // Size of each quote (shares)
        int64_t max_position;          // Max position (long, shares)
        uint64_t skew_per_share;       // Price skew per share of inventory (ticks)
        bool enable_inventory_skew;    // Whether to skew quotes based on inventory

        Params()
            : spread_ticks(10)         // Default: 10 ticks ($0.0010)
            , quote_size(100)          // Default: 100 shares
            , max_position(1000)       // Default: max 1000 shares
            , skew_per_share(1)        // Default: 1 tick per share
            , enable_inventory_skew(true) {}
    };

    explicit MarketMaker(const Params& params = Params())
        : params_(params)
        , position_manager_()
        , pending_orders_()
        , pending_cancellations_()
        , active_orders_()
        , last_mid_prices_() {}

    // StrategyBase interface implementation
    void on_order_book_update(
        uint32_t symbol_id,
        const Matching::OrderBookStats& stats,
        const Core::Timestamp& timestamp
    ) override {
        // Skip if no valid market (no bid or ask)
        if (stats.best_bid_price == 0 || stats.best_ask_price == 0) {
            return;
        }

        // Calculate mid-price
        uint64_t mid_price = (stats.best_bid_price + stats.best_ask_price) / 2;
        last_mid_prices_[symbol_id] = mid_price;

        // Get current position
        Position& position = position_manager_.get_position(symbol_id);
        int64_t current_position = position.get_shares();

        // Check if we're at max position (stop buying)
        bool can_buy = current_position < params_.max_position;
        bool can_sell = current_position > 0;

        // Cancel existing orders for this symbol
        cancel_symbol_orders(symbol_id);

        // Calculate inventory skew
        int64_t inventory_skew = 0;
        if (params_.enable_inventory_skew) {
            // Positive position → increase bid/ask (discourage buying)
            // Negative position → decrease bid/ask (encourage buying)
            inventory_skew = current_position * static_cast<int64_t>(params_.skew_per_share);
        }

        // Generate new quotes
        if (can_buy) {
            // Bid quote: mid - spread/2 + skew
            int64_t bid_offset = static_cast<int64_t>(params_.spread_ticks / 2) + inventory_skew;
            int64_t bid_price = static_cast<int64_t>(mid_price) - bid_offset;

            if (bid_price > 0 && bid_price < static_cast<int64_t>(stats.best_ask_price)) {
                StrategyOrder bid_order(
                    symbol_id,
                    Matching::OrderSide::Buy,
                    Matching::OrderType::Limit,
                    static_cast<uint64_t>(bid_price),
                    params_.quote_size,
                    timestamp
                );
                pending_orders_.push_back(bid_order);
            }
        }

        if (can_sell) {
            // Ask quote: mid + spread/2 + skew
            int64_t ask_offset = static_cast<int64_t>(params_.spread_ticks / 2) + inventory_skew;
            int64_t ask_price = static_cast<int64_t>(mid_price) + ask_offset;

            if (ask_price > static_cast<int64_t>(stats.best_bid_price)) {
                StrategyOrder ask_order(
                    symbol_id,
                    Matching::OrderSide::Sell,
                    Matching::OrderType::Limit,
                    static_cast<uint64_t>(ask_price),
                    params_.quote_size,
                    timestamp
                );
                pending_orders_.push_back(ask_order);
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
     * Register an order ID as active (called when order is submitted)
     */
    void register_order(uint64_t order_id, uint32_t symbol_id) {
        active_orders_[order_id] = symbol_id;
    }

    /**
     * Get position manager (for PnL reporting)
     */
    const PositionManager& get_position_manager() const {
        return position_manager_;
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
    Params params_;
    PositionManager position_manager_;
    std::vector<StrategyOrder> pending_orders_;
    std::vector<uint64_t> pending_cancellations_;
    std::unordered_map<uint64_t, uint32_t> active_orders_;  // order_id → symbol_id
    std::unordered_map<uint32_t, uint64_t> last_mid_prices_;  // symbol_id → mid_price

    /**
     * Cancel all active orders for a symbol
     */
    void cancel_symbol_orders(uint32_t symbol_id) {
        for (const auto& [order_id, oid_symbol_id] : active_orders_) {
            if (oid_symbol_id == symbol_id) {
                pending_cancellations_.push_back(order_id);
            }
        }
    }
};

} // namespace Strategy
} // namespace Trader

#endif // TRADER_STRATEGY_MARKET_MAKER_H
