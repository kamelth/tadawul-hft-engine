#ifndef TRADER_MATCHING_ORDER_BOOK_H
#define TRADER_MATCHING_ORDER_BOOK_H

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <functional>
#include "trader/matching/order.h"
#include "trader/matching/level.h"
#include "trader/matching/symbol.h"
#include "core/containers/bintree_avl.h"

namespace Trader {
namespace Matching {

/**
 * Order book statistics
 */
struct OrderBookStats {
    uint64_t best_bid_price;
    uint64_t best_ask_price;
    uint64_t best_bid_volume;
    uint64_t best_ask_volume;
    uint64_t spread;                // Bid-ask spread
    size_t bid_level_count;
    size_t ask_level_count;
    size_t total_orders;

    OrderBookStats()
        : best_bid_price(0)
        , best_ask_price(0)
        , best_bid_volume(0)
        , best_ask_volume(0)
        , spread(0)
        , bid_level_count(0)
        , ask_level_count(0)
        , total_orders(0) {}
};

/**
 * Order book - matching engine for a single symbol
 *
 * Design principles:
 * - Price-time priority: Best price first, then FIFO within level
 * - Deterministic matching: AVL tree for sorted price levels
 * - Fast best bid/ask: O(log n) via AVL tree min/max
 * - Order lookup: O(1) via hash map
 */
class OrderBook {
public:
    explicit OrderBook(uint32_t symbol_id)
        : symbol_id_(symbol_id)
        , order_count_(0) {}

    ~OrderBook() {
        clear();
    }

    // Get symbol ID
    uint32_t symbol_id() const { return symbol_id_; }

    // Get number of orders
    size_t order_count() const { return order_count_; }

    /**
     * Add order to the book
     * Returns executions if order matches immediately
     */
    std::vector<Execution> add_order(Order* order, const Core::Timestamp& timestamp) {
        std::vector<Execution> executions;

        if (!order || order->symbol_id != symbol_id_) {
            return executions;
        }

        // Activate the order
        order->activate(timestamp);

        // Try to match the order
        if (order->is_buy()) {
            match_buy_order(order, executions, timestamp);
        } else {
            match_sell_order(order, executions, timestamp);
        }

        // If order has remaining quantity, add to book
        if (order->remaining_quantity > 0) {
            add_order_to_book(order);
        }

        return executions;
    }

    /**
     * Cancel order
     */
    bool cancel_order(uint64_t order_id, const Core::Timestamp& timestamp) {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            return false;
        }

        Order* order = it->second;
        remove_order_from_book(order);
        order->cancel(timestamp);

        return true;
    }

    /**
     * Delete order (remove completely)
     */
    bool delete_order(uint64_t order_id) {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            return false;
        }

        Order* order = it->second;
        remove_order_from_book(order);
        orders_.erase(it);

        return true;
    }

    /**
     * Execute/reduce order quantity
     */
    bool execute_order(uint64_t order_id, uint64_t quantity, const Core::Timestamp& timestamp) {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            return false;
        }

        Order* order = it->second;

        // Update level volume before execution
        Level* level = find_level(order->price, order->is_buy());
        if (level) {
            uint64_t old_qty = order->remaining_quantity;
            order->execute(quantity, timestamp);
            level->update_order_volume(order, old_qty, order->remaining_quantity);

            // Remove order if fully filled
            if (order->is_filled()) {
                remove_order_from_book(order);
            }
        }

        return true;
    }

    /**
     * Get order by ID
     */
    Order* get_order(uint64_t order_id) {
        auto it = orders_.find(order_id);
        return (it != orders_.end()) ? it->second : nullptr;
    }

    /**
     * Get best bid price
     */
    uint64_t best_bid_price() const {
        const uint64_t* price = bid_levels_.find_max_key();
        return price ? *price : 0;
    }

    /**
     * Get best ask price
     */
    uint64_t best_ask_price() const {
        const uint64_t* price = ask_levels_.find_min_key();
        return price ? *price : 0;
    }

    /**
     * Get best bid level
     */
    const Level* best_bid() const {
        return bid_levels_.find_max();
    }

    const Level* best_ask() const {
        return ask_levels_.find_min();
    }

    /**
     * Get market depth (top N levels on each side)
     */
    void get_depth(size_t max_levels, std::vector<std::pair<uint64_t, uint64_t>>& bids,
                    std::vector<std::pair<uint64_t, uint64_t>>& asks) const {
        bids.clear();
        asks.clear();

        // Collect bid levels (highest to lowest)
        bid_levels_.traverse_inorder([&](uint64_t price, const Level& level) {
            if (bids.size() < max_levels) {
                bids.emplace_back(price, level.total_volume());
            }
        });

        // Reverse bids to get highest first
        std::reverse(bids.begin(), bids.end());

        // Collect ask levels (lowest to highest)
        ask_levels_.traverse_inorder([&](uint64_t price, const Level& level) {
            if (asks.size() < max_levels) {
                asks.emplace_back(price, level.total_volume());
            }
        });
    }

    /**
     * Get statistics
     */
    OrderBookStats get_stats() const {
        OrderBookStats stats;

        const Level* best_bid_level = best_bid();
        const Level* best_ask_level = best_ask();

        if (best_bid_level) {
            stats.best_bid_price = best_bid_level->price();
            stats.best_bid_volume = best_bid_level->total_volume();
        }

        if (best_ask_level) {
            stats.best_ask_price = best_ask_level->price();
            stats.best_ask_volume = best_ask_level->total_volume();
        }

        if (stats.best_bid_price > 0 && stats.best_ask_price > 0) {
            stats.spread = stats.best_ask_price - stats.best_bid_price;
        }

        stats.bid_level_count = bid_levels_.size();
        stats.ask_level_count = ask_levels_.size();
        stats.total_orders = order_count_;

        return stats;
    }

    /**
     * Clear all orders
     */
    void clear() {
        bid_levels_.clear();
        ask_levels_.clear();
        orders_.clear();
        order_count_ = 0;
    }

private:
    uint32_t symbol_id_;
    size_t order_count_;

    // Price levels (sorted by price)
    Core::Containers::BinTreeAVL<uint64_t, Level> bid_levels_;  // Higher prices first (max)
    Core::Containers::BinTreeAVL<uint64_t, Level> ask_levels_;  // Lower prices first (min)

    // Order lookup
    std::unordered_map<uint64_t, Order*> orders_;

    /**
     * Match buy order against ask side
     */
    void match_buy_order(Order* order, std::vector<Execution>& executions,
                        const Core::Timestamp& timestamp) {
        size_t exec_start = executions.size();
        while (order->remaining_quantity > 0) {
            // Get best ask
            Level* best_ask_level = ask_levels_.find_min();
            if (!best_ask_level) break;

            uint64_t best_ask = best_ask_level->price();

            // Check if prices cross
            if (order->type == OrderType::Limit && order->price < best_ask) {
                break;  // No match
            }

            // Match at this level
            best_ask_level->match(order, executions, timestamp);

            // Remove level if empty
            if (best_ask_level->empty()) {
                ask_levels_.erase(best_ask);
            }
        }

        // Remove filled resting orders from the book's lookup map so that
        // later ITCH Delete/Execute messages don't try to operate on orders
        // that are no longer in any Level's list.
        cleanup_filled_resting_orders(executions, exec_start);
    }

    /**
     * Match sell order against bid side
     */
    void match_sell_order(Order* order, std::vector<Execution>& executions,
                         const Core::Timestamp& timestamp) {
        size_t exec_start = executions.size();
        while (order->remaining_quantity > 0) {
            // Get best bid
            Level* best_bid_level = bid_levels_.find_max();
            if (!best_bid_level) break;

            uint64_t best_bid = best_bid_level->price();

            // Check if prices cross
            if (order->type == OrderType::Limit && order->price > best_bid) {
                break;  // No match
            }

            // Match at this level
            best_bid_level->match(order, executions, timestamp);

            // Remove level if empty
            if (best_bid_level->empty()) {
                bid_levels_.erase(best_bid);
            }
        }

        cleanup_filled_resting_orders(executions, exec_start);
    }

    /**
     * Remove filled resting orders from the book's orders_ map after matching.
     * Level::match() pops filled orders from the Level's intrusive list, but
     * they remain in orders_.  If a later ITCH Delete/Execute arrives for that
     * order_id, remove_order_from_book would call List::remove on an item no
     * longer in the list, corrupting head_/tail_.
     */
    void cleanup_filled_resting_orders(const std::vector<Execution>& executions,
                                       size_t from) {
        for (size_t i = from; i < executions.size(); ++i) {
            uint64_t match_id = executions[i].match_order_id;
            auto it = orders_.find(match_id);
            if (it != orders_.end() && it->second->is_filled()) {
                orders_.erase(it);
                --order_count_;
            }
        }
    }

    /**
     * Add order to book (no matching)
     */
    void add_order_to_book(Order* order) {
        if (order->is_buy()) {
            // Add to bid side
            Level* level = bid_levels_.find(order->price);
            if (!level) {
                bid_levels_.insert(order->price, Level(order->price));
                level = bid_levels_.find(order->price);
            }
            level->add_order(order);
        } else {
            // Add to ask side
            Level* level = ask_levels_.find(order->price);
            if (!level) {
                ask_levels_.insert(order->price, Level(order->price));
                level = ask_levels_.find(order->price);
            }
            level->add_order(order);
        }

        orders_[order->order_id] = order;
        ++order_count_;
    }

    /**
     * Remove order from book
     */
    void remove_order_from_book(Order* order) {
        Level* level = find_level(order->price, order->is_buy());
        if (level) {
            level->remove_order(order);

            // Remove level if empty
            if (level->empty()) {
                if (order->is_buy()) {
                    bid_levels_.erase(order->price);
                } else {
                    ask_levels_.erase(order->price);
                }
            }
        }

        orders_.erase(order->order_id);
        --order_count_;
    }

    /**
     * Find level by price
     */
    Level* find_level(uint64_t price, bool is_buy) {
        if (is_buy) {
            return bid_levels_.find(price);
        } else {
            return ask_levels_.find(price);
        }
    }
};

} // namespace Matching
} // namespace Trader

#endif // TRADER_MATCHING_ORDER_BOOK_H
