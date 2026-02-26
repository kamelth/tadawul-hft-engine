#ifndef TRADER_MATCHING_LEVEL_H
#define TRADER_MATCHING_LEVEL_H

#include <cstdint>
#include "trader/matching/order.h"
#include "core/containers/list.h"

namespace Trader {
namespace Matching {

/**
 * Price level - aggregates all orders at a specific price
 *
 * Design principles:
 * - FIFO ordering: Orders matched in time priority (first in, first out)
 * - Intrusive list: Zero-allocation order queue
 * - Volume tracking: Aggregate volume at this price level
 */
class Level {
public:
    Level()
        : price_(0)
        , total_volume_(0)
        , order_count_(0) {}

    explicit Level(uint64_t price)
        : price_(price)
        , total_volume_(0)
        , order_count_(0) {}

    // Move constructor
    Level(Level&& other) noexcept
        : price_(other.price_)
        , total_volume_(other.total_volume_)
        , order_count_(other.order_count_)
        , orders_(std::move(other.orders_)) {
        other.price_ = 0;
        other.total_volume_ = 0;
        other.order_count_ = 0;
    }

    // Move assignment
    Level& operator=(Level&& other) noexcept {
        if (this != &other) {
            price_ = other.price_;
            total_volume_ = other.total_volume_;
            order_count_ = other.order_count_;
            orders_ = std::move(other.orders_);

            other.price_ = 0;
            other.total_volume_ = 0;
            other.order_count_ = 0;
        }
        return *this;
    }

    // Non-copyable
    Level(const Level&) = delete;
    Level& operator=(const Level&) = delete;

    // Get price of this level
    uint64_t price() const { return price_; }

    // Get total volume at this level
    uint64_t total_volume() const { return total_volume_; }

    // Get number of orders at this level
    size_t order_count() const { return order_count_; }

    // Check if level is empty
    bool empty() const { return orders_.empty(); }

    // Add order to this level (at the back for FIFO)
    void add_order(Order* order) {
        orders_.push_back(order);
        total_volume_ += order->remaining_quantity;
        ++order_count_;
    }

    // Remove order from this level
    void remove_order(Order* order) {
        if (total_volume_ >= order->remaining_quantity) {
            total_volume_ -= order->remaining_quantity;
        } else {
            total_volume_ = 0;
        }

        orders_.remove(order);
        --order_count_;
    }

    // Update volume when order is partially filled
    void update_order_volume(Order* order, uint64_t old_quantity, uint64_t new_quantity) {
        if (old_quantity >= new_quantity) {
            uint64_t delta = old_quantity - new_quantity;
            if (total_volume_ >= delta) {
                total_volume_ -= delta;
            } else {
                total_volume_ = 0;
            }
        } else {
            uint64_t delta = new_quantity - old_quantity;
            total_volume_ += delta;
        }
    }

    // Get first order in FIFO queue (best time priority)
    Order* front() {
        return orders_.front();
    }

    const Order* front() const {
        return orders_.front();
    }

    // Get last order in FIFO queue
    Order* back() {
        return orders_.back();
    }

    const Order* back() const {
        return orders_.back();
    }

    // Iterator support for traversing orders
    using Iterator = typename Core::Containers::List<Order, &Order::list_node>::Iterator;
    using ConstIterator = typename Core::Containers::List<Order, &Order::list_node>::ConstIterator;

    Iterator begin() { return orders_.begin(); }
    Iterator end() { return orders_.end(); }
    ConstIterator begin() const { return orders_.begin(); }
    ConstIterator end() const { return orders_.end(); }
    ConstIterator cbegin() const { return orders_.cbegin(); }
    ConstIterator cend() const { return orders_.cend(); }

    // Match against incoming order (execute orders in FIFO order)
    // Returns total quantity matched
    uint64_t match(Order* incoming_order, std::vector<Execution>& executions,
                   const Core::Timestamp& timestamp) {
        uint64_t total_matched = 0;

        // Match against orders in FIFO order (front of queue first)
        while (!orders_.empty() && incoming_order->remaining_quantity > 0) {
            Order* resting_order = orders_.front();

            // Determine execution quantity (min of both orders' remaining)
            uint64_t exec_qty = std::min(incoming_order->remaining_quantity,
                                         resting_order->remaining_quantity);

            // Execute both orders
            uint64_t old_resting_qty = resting_order->remaining_quantity;
            resting_order->execute(exec_qty, timestamp);
            incoming_order->execute(exec_qty, timestamp);

            // Update level volume
            update_order_volume(resting_order, old_resting_qty, resting_order->remaining_quantity);

            // Record execution
            executions.emplace_back(
                incoming_order->order_id,
                resting_order->order_id,
                price_,  // Execution at resting order's price
                exec_qty,
                timestamp
            );

            total_matched += exec_qty;

            // Remove resting order if fully filled
            if (resting_order->is_filled()) {
                orders_.pop_front();
                --order_count_;
            }
        }

        return total_matched;
    }

private:
    uint64_t price_;                                            // Price of this level
    uint64_t total_volume_;                                     // Total volume at this level
    size_t order_count_;                                        // Number of orders
    Core::Containers::List<Order, &Order::list_node> orders_;  // FIFO order queue
};

} // namespace Matching
} // namespace Trader

#endif // TRADER_MATCHING_LEVEL_H
