#ifndef TRADER_MATCHING_ORDER_H
#define TRADER_MATCHING_ORDER_H

#include <cstdint>
#include <string>
#include "core/timestamp.h"
#include "core/containers/list.h"

namespace Trader {
namespace Matching {

/**
 * Order side (Buy or Sell)
 */
enum class OrderSide : uint8_t {
    Buy = 0,
    Sell = 1
};

/**
 * Order type
 */
enum class OrderType : uint8_t {
    Market = 0,    // Execute immediately at best available price
    Limit = 1      // Execute only at specified price or better
};

/**
 * Order state (lifecycle)
 */
enum class OrderState : uint8_t {
    New = 0,              // Just created, not yet in book
    Active = 1,           // In order book, waiting for match
    PartiallyFilled = 2,  // Some quantity executed, rest still active
    Filled = 3,           // Fully executed
    Cancelled = 4,        // Cancelled by user
    Rejected = 5          // Rejected (invalid price, insufficient funds, etc.)
};

/**
 * Order time-in-force
 */
enum class OrderTimeInForce : uint8_t {
    GoodTilCancel = 0,  // GTC - Stays in book until filled or cancelled
    ImmediateOrCancel = 1,  // IOC - Execute immediately, cancel remainder
    FillOrKill = 2      // FOK - Execute fully immediately or cancel entire order
};

/**
 * Order struct - represents a single order in the trading system
 *
 * Design principles:
 * - Deterministic: Uses ITCH timestamp, sequential order ID
 * - Integer prices: Prices in cents to avoid floating-point errors
 * - Intrusive list: Embedded list node for zero-allocation FIFO ordering
 */
struct Order {
    // Order identification
    uint64_t order_id;              // Unique order ID (sequential, deterministic)
    uint64_t original_order_id;     // Original ITCH order reference number
    uint32_t symbol_id;             // Symbol ID (deterministic mapping)

    // Order details
    OrderSide side;                 // Buy or Sell
    OrderType type;                 // Market or Limit
    OrderTimeInForce time_in_force; // GTC, IOC, FOK
    OrderState state;               // Current state

    // Price and quantity (integer arithmetic for determinism)
    uint64_t price;                 // Price in cents (e.g., $100.50 = 10050)
    uint64_t quantity;              // Total quantity
    uint64_t remaining_quantity;    // Quantity not yet executed
    uint64_t executed_quantity;     // Quantity executed so far

    // Timestamps (from ITCH, not system clock)
    Core::Timestamp timestamp;      // Order creation time (from ITCH)
    Core::Timestamp last_update;    // Last modification time

    // Intrusive list node (for price level FIFO ordering)
    Core::Containers::ListNode<Order> list_node;

    // Constructor
    Order()
        : order_id(0)
        , original_order_id(0)
        , symbol_id(0)
        , side(OrderSide::Buy)
        , type(OrderType::Limit)
        , time_in_force(OrderTimeInForce::GoodTilCancel)
        , state(OrderState::New)
        , price(0)
        , quantity(0)
        , remaining_quantity(0)
        , executed_quantity(0)
        , timestamp()
        , last_update()
        , list_node() {}

    Order(uint64_t id, uint64_t orig_id, uint32_t sym_id, OrderSide s, OrderType t,
          uint64_t p, uint64_t qty, const Core::Timestamp& ts)
        : order_id(id)
        , original_order_id(orig_id)
        , symbol_id(sym_id)
        , side(s)
        , type(t)
        , time_in_force(OrderTimeInForce::GoodTilCancel)
        , state(OrderState::New)
        , price(p)
        , quantity(qty)
        , remaining_quantity(qty)
        , executed_quantity(0)
        , timestamp(ts)
        , last_update(ts)
        , list_node() {}

    // Check if order is active (can be matched)
    bool is_active() const {
        return state == OrderState::Active || state == OrderState::PartiallyFilled;
    }

    // Check if order is buy side
    bool is_buy() const {
        return side == OrderSide::Buy;
    }

    // Check if order is sell side
    bool is_sell() const {
        return side == OrderSide::Sell;
    }

    // Check if order is fully filled
    bool is_filled() const {
        return state == OrderState::Filled || remaining_quantity == 0;
    }

    // Check if order can be partially filled
    bool allows_partial_fill() const {
        return time_in_force != OrderTimeInForce::FillOrKill;
    }

    // Execute a portion of the order
    void execute(uint64_t qty, const Core::Timestamp& ts) {
        if (qty > remaining_quantity) {
            qty = remaining_quantity;
        }

        executed_quantity += qty;
        remaining_quantity -= qty;
        last_update = ts;

        if (remaining_quantity == 0) {
            state = OrderState::Filled;
        } else if (executed_quantity > 0) {
            state = OrderState::PartiallyFilled;
        }
    }

    // Cancel the order
    void cancel(const Core::Timestamp& ts) {
        state = OrderState::Cancelled;
        last_update = ts;
    }

    // Activate the order (place in book)
    void activate(const Core::Timestamp& ts) {
        state = OrderState::Active;
        last_update = ts;
    }

    // Reject the order
    void reject(const Core::Timestamp& ts) {
        state = OrderState::Rejected;
        last_update = ts;
    }

    // String representation
    std::string to_string() const {
        std::string result = "Order{";
        result += "id=" + std::to_string(order_id);
        result += ", symbol=" + std::to_string(symbol_id);
        result += ", side=" + std::string(side == OrderSide::Buy ? "Buy" : "Sell");
        result += ", type=" + std::string(type == OrderType::Market ? "Market" : "Limit");
        result += ", price=" + std::to_string(price);
        result += ", qty=" + std::to_string(quantity);
        result += ", remaining=" + std::to_string(remaining_quantity);
        result += ", executed=" + std::to_string(executed_quantity);
        result += ", state=";

        switch (state) {
            case OrderState::New: result += "New"; break;
            case OrderState::Active: result += "Active"; break;
            case OrderState::PartiallyFilled: result += "PartiallyFilled"; break;
            case OrderState::Filled: result += "Filled"; break;
            case OrderState::Cancelled: result += "Cancelled"; break;
            case OrderState::Rejected: result += "Rejected"; break;
        }

        result += "}";
        return result;
    }
};

/**
 * Order execution result
 */
struct Execution {
    uint64_t order_id;              // Order that was executed
    uint64_t match_order_id;        // Order it matched against
    uint64_t execution_price;       // Price of execution
    uint64_t execution_quantity;    // Quantity executed
    Core::Timestamp timestamp;      // Execution time

    Execution()
        : order_id(0)
        , match_order_id(0)
        , execution_price(0)
        , execution_quantity(0)
        , timestamp() {}

    Execution(uint64_t oid, uint64_t match_oid, uint64_t price, uint64_t qty, const Core::Timestamp& ts)
        : order_id(oid)
        , match_order_id(match_oid)
        , execution_price(price)
        , execution_quantity(qty)
        , timestamp(ts) {}
};

} // namespace Matching
} // namespace Trader

#endif // TRADER_MATCHING_ORDER_H
