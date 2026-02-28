#ifndef TRADER_STRATEGY_STRATEGY_BASE_H
#define TRADER_STRATEGY_STRATEGY_BASE_H

#include "trader/matching/order.h"
#include "trader/matching/order_book.h"
#include "core/timestamp.h"
#include <string>
#include <vector>

namespace Trader {
namespace Strategy {

/**
 * Strategy Order Request
 *
 * Represents an order that a strategy wants to submit.
 */
struct StrategyOrder {
    uint32_t symbol_id;
    Matching::OrderSide side;
    Matching::OrderType type;
    uint64_t price;      // Price in units of $0.0001
    uint64_t quantity;
    Core::Timestamp timestamp;

    StrategyOrder(uint32_t sid, Matching::OrderSide s, Matching::OrderType t,
                  uint64_t p, uint64_t q, const Core::Timestamp& ts)
        : symbol_id(sid), side(s), type(t), price(p), quantity(q), timestamp(ts) {}
};

/**
 * Strategy Base Interface
 *
 * Abstract interface for all trading strategies.
 * Strategies receive market events and generate trading decisions.
 */
class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    /**
     * Called when order book is updated (after add/execute/cancel/delete)
     */
    virtual void on_order_book_update(
        uint32_t symbol_id,
        const Matching::OrderBookStats& stats,
        const Core::Timestamp& timestamp
    ) = 0;

    /**
     * Called when a market execution occurs (trade happened)
     */
    virtual void on_execution(
        uint32_t symbol_id,
        const Matching::Execution& execution,
        const Core::Timestamp& timestamp
    ) = 0;

    /**
     * Called when one of our strategy orders gets filled
     */
    virtual void on_order_filled(
        uint64_t order_id,
        uint32_t symbol_id,
        Matching::OrderSide side,
        uint64_t fill_price,
        uint64_t fill_quantity,
        const Core::Timestamp& timestamp
    ) = 0;

    /**
     * Get pending orders that strategy wants to submit
     * Returns list of orders and clears internal pending queue
     */
    virtual std::vector<StrategyOrder> get_pending_orders() = 0;

    /**
     * Get pending cancellations (order IDs to cancel)
     * Returns list of order IDs and clears internal pending queue
     */
    virtual std::vector<uint64_t> get_pending_cancellations() = 0;

    /**
     * Get strategy name
     */
    virtual std::string get_name() const = 0;

    /**
     * Reset strategy state (for testing/replay)
     */
    virtual void reset() = 0;
};

} // namespace Strategy
} // namespace Trader

#endif // TRADER_STRATEGY_STRATEGY_BASE_H
