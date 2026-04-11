#ifndef TRADER_MATCHING_MARKET_MANAGER_H
#define TRADER_MATCHING_MARKET_MANAGER_H

#include <cstdint>
#include <unordered_map>
#include <memory>
#include <vector>
#include "trader/matching/order.h"
#include "trader/matching/order_book.h"
#include "trader/matching/symbol.h"

namespace Trader {
namespace Matching {

/**
 * Market-wide statistics
 */
struct MarketStats {
    size_t symbol_count;
    size_t total_orders;
    size_t total_executions;
    uint64_t total_volume_traded;

    MarketStats()
        : symbol_count(0)
        , total_orders(0)
        , total_executions(0)
        , total_volume_traded(0) {}
};

/**
 * Market handler interface - callbacks for market events
 */
class MarketHandler {
public:
    virtual ~MarketHandler() = default;

    // Called when order is added to book
    virtual void on_order_added(const Order* order) {}

    // Called when order is executed
    virtual void on_execution(const Execution& execution) {}

    // Called when order is cancelled
    virtual void on_order_cancelled(const Order* order) {}

    // Called when order book is updated
    virtual void on_order_book_update(uint32_t symbol_id,
                                      const OrderBookStats& stats,
                                      const Core::Timestamp& timestamp) {
        (void)symbol_id;
        (void)stats;
        (void)timestamp;
    }
};

/**
 * Market manager - manages multiple order books (multi-symbol routing)
 *
 * Design principles:
 * - Deterministic order ID: Sequential counter (not random)
 * - Symbol registry: Deterministic symbol ID assignment
 * - Order routing: Route orders to correct order book by symbol
 * - Event notifications: Callbacks for market events
 */
class MarketManager {
public:
    MarketManager()
        : next_order_id_(1)  // Start from 1 (0 reserved for invalid)
        , total_executions_(0)
        , total_volume_traded_(0)
        , handler_(nullptr) {}

    /**
     * Set market event handler
     */
    void set_handler(MarketHandler* handler) {
        handler_ = handler;
    }

    /**
     * Get symbol registry
     */
    SymbolRegistry& symbol_registry() {
        return symbol_registry_;
    }

    const SymbolRegistry& symbol_registry() const {
        return symbol_registry_;
    }

    /**
     * Add order to market (deterministic order ID generation)
     */
    std::vector<Execution> add_order(uint32_t symbol_id, OrderSide side, OrderType type,
                                     uint64_t price, uint64_t quantity,
                                     uint64_t original_order_id,
                                     const Core::Timestamp& timestamp) {
        std::vector<Execution> executions;

        // Get or create order book
        OrderBook* book = get_or_create_order_book(symbol_id);
        if (!book) {
            return executions;
        }

        // Generate deterministic order ID
        uint64_t order_id = next_order_id_++;

        // Create order
        Order* order = new Order(order_id, original_order_id, symbol_id, side, type,
                                price, quantity, timestamp);

        // Add to order book (may execute immediately)
        executions = book->add_order(order, timestamp);

        // Track order
        orders_[order_id] = order;

        // Notify handler
        if (handler_) {
            handler_->on_order_added(order);

            for (const auto& exec : executions) {
                handler_->on_execution(exec);
                total_executions_++;
                total_volume_traded_ += exec.execution_quantity;
            }

            OrderBookStats stats = book->get_stats();
            handler_->on_order_book_update(symbol_id, stats, timestamp);
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
        uint32_t symbol_id = order->symbol_id;

        OrderBook* book = get_order_book(symbol_id);
        if (!book) {
            return false;
        }

        bool success = book->cancel_order(order_id, timestamp);

        if (success && handler_) {
            handler_->on_order_cancelled(order);

            OrderBookStats stats = book->get_stats();
            handler_->on_order_book_update(symbol_id, stats, timestamp);
        }

        return success;
    }

    /**
     * Execute/reduce order
     */
    bool execute_order(uint64_t order_id, uint64_t quantity, const Core::Timestamp& timestamp) {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            return false;
        }

        Order* order = it->second;
        uint32_t symbol_id = order->symbol_id;

        OrderBook* book = get_order_book(symbol_id);
        if (!book) {
            return false;
        }

        bool success = book->execute_order(order_id, quantity, timestamp);

        if (success) {
            total_volume_traded_ += quantity;

            if (handler_) {
                // Create execution record
                Execution exec(order_id, 0, order->price, quantity, timestamp);
                handler_->on_execution(exec);
                total_executions_++;

                OrderBookStats stats = book->get_stats();
                handler_->on_order_book_update(symbol_id, stats, timestamp);
            }
        }

        return success;
    }

    /**
     * Delete order completely
     */
    bool delete_order(uint64_t order_id) {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            return false;
        }

        Order* order = it->second;
        uint32_t symbol_id = order->symbol_id;

        OrderBook* book = get_order_book(symbol_id);
        if (!book) {
            return false;
        }

        Core::Timestamp delete_ts = order->timestamp;
        bool success = book->delete_order(order_id);

        if (success) {
            delete order;
            orders_.erase(it);

            if (handler_) {
                OrderBookStats stats = book->get_stats();
                handler_->on_order_book_update(symbol_id, stats, delete_ts);
            }
        }

        return success;
    }

    /**
     * Get order by ID
     */
    Order* get_order(uint64_t order_id) {
        auto it = orders_.find(order_id);
        return (it != orders_.end()) ? it->second : nullptr;
    }

    /**
     * Get order book for symbol
     */
    OrderBook* get_order_book(uint32_t symbol_id) {
        auto it = order_books_.find(symbol_id);
        return (it != order_books_.end()) ? it->second.get() : nullptr;
    }

    const OrderBook* get_order_book(uint32_t symbol_id) const {
        auto it = order_books_.find(symbol_id);
        return (it != order_books_.end()) ? it->second.get() : nullptr;
    }

    /**
     * Get order book by symbol name
     */
    OrderBook* get_order_book_by_name(const std::string& symbol_name) {
        uint32_t symbol_id;
        if (!symbol_registry_.get_symbol_id(symbol_name, symbol_id)) {
            return nullptr;
        }
        return get_order_book(symbol_id);
    }

    /**
     * Get all order books
     */
    std::vector<OrderBook*> get_all_order_books() {
        std::vector<OrderBook*> books;
        books.reserve(order_books_.size());
        for (auto& pair : order_books_) {
            books.push_back(pair.second.get());
        }
        return books;
    }

    /**
     * Get market statistics
     */
    MarketStats get_market_stats() const {
        MarketStats stats;
        stats.symbol_count = order_books_.size();
        stats.total_orders = orders_.size();
        stats.total_executions = total_executions_;
        stats.total_volume_traded = total_volume_traded_;
        return stats;
    }

    /**
     * Clear all data
     */
    void clear() {
        // Delete all orders
        for (auto& pair : orders_) {
            delete pair.second;
        }

        orders_.clear();
        order_books_.clear();
        symbol_registry_.clear();

        next_order_id_ = 1;
        total_executions_ = 0;
        total_volume_traded_ = 0;
    }

    /**
     * Reset order ID counter (for testing/determinism)
     */
    void reset_order_id_counter() {
        next_order_id_ = 1;
    }

private:
    SymbolRegistry symbol_registry_;
    std::unordered_map<uint32_t, std::unique_ptr<OrderBook>> order_books_;
    std::unordered_map<uint64_t, Order*> orders_;

    uint64_t next_order_id_;          // Next order ID (sequential, deterministic)
    size_t total_executions_;
    uint64_t total_volume_traded_;

    MarketHandler* handler_;

    /**
     * Get or create order book for symbol
     */
    OrderBook* get_or_create_order_book(uint32_t symbol_id) {
        auto it = order_books_.find(symbol_id);
        if (it != order_books_.end()) {
            return it->second.get();
        }

        // Create new order book
        auto book = std::make_unique<OrderBook>(symbol_id);
        OrderBook* book_ptr = book.get();
        order_books_[symbol_id] = std::move(book);

        return book_ptr;
    }
};

} // namespace Matching
} // namespace Trader

#endif // TRADER_MATCHING_MARKET_MANAGER_H
