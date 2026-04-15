#ifndef TRADER_GPU_SNAPSHOT_H
#define TRADER_GPU_SNAPSHOT_H

#include <cstdint>
#include <vector>
#include <utility>
#include "trader/gpu/order_book_gpu.h"
#include "trader/matching/market_manager.h"

namespace Trader {
namespace GPU {

/**
 * Snapshot all order books in a MarketManager into the SoA layout used by
 * the GPU benchmark.
 *
 * Symbols with empty books are still included (with bid_count=0,
 * ask_count=0).  Top MAX_LEVELS_PER_SIDE levels per side are captured.
 *
 * @param market_manager  Source order books (built from ITCH data).
 * @param storage         Backing host memory; will be resized.
 * @return  A MultiSymbolBook view referring to storage.
 */
inline MultiSymbolBook snapshot_from_market_manager(
    const Matching::MarketManager& market_manager,
    HostBookStorage& storage)
{
    const auto& registry = market_manager.symbol_registry();
    const auto symbols = registry.get_all_symbols();
    const uint32_t n = static_cast<uint32_t>(symbols.size());

    MultiSymbolBook book = storage.view(n);

    for (uint32_t s = 0; s < n; ++s) {
        const Matching::OrderBook* ob =
            market_manager.get_order_book(symbols[s].symbol_id);
        if (!ob) continue;

        // Extract top MAX_LEVELS_PER_SIDE levels per side.  OrderBook::get_depth
        // already returns bids high-to-low and asks low-to-high.
        std::vector<std::pair<uint64_t, uint64_t>> bids, asks;
        ob->get_depth(MAX_LEVELS_PER_SIDE, bids, asks);

        const uint32_t bid_n = static_cast<uint32_t>(bids.size());
        const uint32_t ask_n = static_cast<uint32_t>(asks.size());
        book.bid_counts[s] = bid_n;
        book.ask_counts[s] = ask_n;

        for (uint32_t l = 0; l < bid_n; ++l) {
            const size_t idx = static_cast<size_t>(s) * MAX_LEVELS_PER_SIDE + l;
            book.bid_prices[idx]  = bids[l].first;
            book.bid_volumes[idx] = bids[l].second;
        }
        for (uint32_t l = 0; l < ask_n; ++l) {
            const size_t idx = static_cast<size_t>(s) * MAX_LEVELS_PER_SIDE + l;
            book.ask_prices[idx]  = asks[l].first;
            book.ask_volumes[idx] = asks[l].second;
        }
    }

    return book;
}

} // namespace GPU
} // namespace Trader

#endif // TRADER_GPU_SNAPSHOT_H
