#ifndef TRADER_STRATEGY_POSITION_H
#define TRADER_STRATEGY_POSITION_H

#include <cstdint>
#include <unordered_map>

namespace Trader {
namespace Strategy {

/**
 * Position Tracker
 *
 * Tracks inventory for a single symbol.
 * Deterministic: no floating point, integer shares only.
 */
class Position {
public:
    Position()
        : shares_(0)
        , total_bought_(0)
        , total_sold_(0)
        , avg_buy_price_(0)
        , avg_sell_price_(0) {}

    /**
     * Add shares (buy)
     */
    void add_shares(uint64_t quantity, uint64_t price) {
        if (quantity == 0) return;

        // Update average buy price (weighted average)
        if (shares_ > 0) {
            uint64_t old_value = shares_ * avg_buy_price_;
            uint64_t new_value = quantity * price;
            avg_buy_price_ = (old_value + new_value) / (shares_ + quantity);
        } else {
            avg_buy_price_ = price;
        }

        shares_ += quantity;
        total_bought_ += quantity;
    }

    /**
     * Remove shares (sell)
     */
    void remove_shares(uint64_t quantity, uint64_t price) {
        if (quantity == 0) return;

        // Update average sell price (weighted average)
        if (total_sold_ > 0) {
            uint64_t old_value = total_sold_ * avg_sell_price_;
            uint64_t new_value = quantity * price;
            avg_sell_price_ = (old_value + new_value) / (total_sold_ + quantity);
        } else {
            avg_sell_price_ = price;
        }

        if (shares_ >= quantity) {
            shares_ -= quantity;
        } else {
            shares_ = 0;  // Prevent negative (short selling not allowed)
        }

        total_sold_ += quantity;
    }

    /**
     * Get current position (positive = long, 0 = flat)
     */
    int64_t get_shares() const {
        return static_cast<int64_t>(shares_);
    }

    /**
     * Get total shares bought
     */
    uint64_t get_total_bought() const {
        return total_bought_;
    }

    /**
     * Get total shares sold
     */
    uint64_t get_total_sold() const {
        return total_sold_;
    }

    /**
     * Get average buy price
     */
    uint64_t get_avg_buy_price() const {
        return avg_buy_price_;
    }

    /**
     * Get average sell price
     */
    uint64_t get_avg_sell_price() const {
        return avg_sell_price_;
    }

    /**
     * Calculate unrealized PnL (mark-to-market)
     * Returns PnL in units of $0.0001
     */
    int64_t calculate_unrealized_pnl(uint64_t current_price) const {
        if (shares_ == 0) return 0;

        // PnL = (current_price - avg_buy_price) * shares
        int64_t price_diff = static_cast<int64_t>(current_price) - static_cast<int64_t>(avg_buy_price_);
        return price_diff * static_cast<int64_t>(shares_);
    }

    /**
     * Calculate realized PnL
     * Returns PnL in units of $0.0001
     */
    int64_t calculate_realized_pnl() const {
        if (total_sold_ == 0) return 0;

        // Realized PnL = (avg_sell_price - avg_buy_price) * total_sold
        int64_t price_diff = static_cast<int64_t>(avg_sell_price_) - static_cast<int64_t>(avg_buy_price_);
        return price_diff * static_cast<int64_t>(total_sold_);
    }

    /**
     * Reset position
     */
    void reset() {
        shares_ = 0;
        total_bought_ = 0;
        total_sold_ = 0;
        avg_buy_price_ = 0;
        avg_sell_price_ = 0;
    }

private:
    uint64_t shares_;          // Current position (long only)
    uint64_t total_bought_;    // Total shares bought (lifetime)
    uint64_t total_sold_;      // Total shares sold (lifetime)
    uint64_t avg_buy_price_;   // Weighted average buy price
    uint64_t avg_sell_price_;  // Weighted average sell price
};

/**
 * Multi-Symbol Position Tracker
 *
 * Tracks positions across multiple symbols.
 */
class PositionManager {
public:
    PositionManager() = default;

    /**
     * Get position for a symbol (creates if doesn't exist)
     */
    Position& get_position(uint32_t symbol_id) {
        return positions_[symbol_id];
    }

    /**
     * Get position for a symbol (const)
     */
    const Position* get_position(uint32_t symbol_id) const {
        auto it = positions_.find(symbol_id);
        if (it != positions_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /**
     * Calculate total unrealized PnL across all symbols
     */
    int64_t calculate_total_unrealized_pnl(const std::unordered_map<uint32_t, uint64_t>& current_prices) const {
        int64_t total_pnl = 0;
        for (const auto& [symbol_id, position] : positions_) {
            auto it = current_prices.find(symbol_id);
            if (it != current_prices.end()) {
                total_pnl += position.calculate_unrealized_pnl(it->second);
            }
        }
        return total_pnl;
    }

    /**
     * Calculate total realized PnL across all symbols
     */
    int64_t calculate_total_realized_pnl() const {
        int64_t total_pnl = 0;
        for (const auto& [symbol_id, position] : positions_) {
            total_pnl += position.calculate_realized_pnl();
        }
        return total_pnl;
    }

    /**
     * Reset all positions
     */
    void reset() {
        positions_.clear();
    }

private:
    std::unordered_map<uint32_t, Position> positions_;
};

} // namespace Strategy
} // namespace Trader

#endif // TRADER_STRATEGY_POSITION_H
