#ifndef TRADER_MATCHING_SYMBOL_H
#define TRADER_MATCHING_SYMBOL_H

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <algorithm>

namespace Trader {
namespace Matching {

/**
 * Symbol information
 */
struct Symbol {
    uint32_t symbol_id;        // Unique symbol ID (deterministic)
    uint16_t stock_locate;     // NASDAQ stock locate code (from ITCH)
    std::string symbol_name;   // Ticker symbol (e.g., "AAPL", "MSFT")
    uint64_t shares_outstanding; // Total shares outstanding
    uint32_t lot_size;         // Minimum trading lot size
    uint64_t tick_size;        // Minimum price increment (in cents)

    Symbol()
        : symbol_id(0)
        , stock_locate(0)
        , symbol_name()
        , shares_outstanding(0)
        , lot_size(1)
        , tick_size(1) {}  // 1 cent default

    Symbol(uint32_t id, uint16_t locate, const std::string& name)
        : symbol_id(id)
        , stock_locate(locate)
        , symbol_name(name)
        , shares_outstanding(0)
        , lot_size(1)
        , tick_size(1) {}
};

/**
 * Symbol registry - manages symbol mappings
 *
 * Design principles:
 * - Deterministic ID assignment: sorted by symbol name (alphabetical)
 * - Thread-safe lookups (read-only after initialization)
 * - Fast lookups: O(log n) for name/locate, O(1) for ID
 */
class SymbolRegistry {
public:
    SymbolRegistry() : next_symbol_id_(0) {}

    /**
     * Register a new symbol (deterministic ID assignment)
     * Returns symbol ID
     */
    uint32_t register_symbol(uint16_t stock_locate, const std::string& symbol_name) {
        // Check if symbol already registered
        auto it = name_to_id_.find(symbol_name);
        if (it != name_to_id_.end()) {
            return it->second;
        }

        // Check if stock locate already registered
        auto it2 = locate_to_id_.find(stock_locate);
        if (it2 != locate_to_id_.end()) {
            return it2->second;
        }

        // Assign new ID
        uint32_t symbol_id = next_symbol_id_++;

        // Create symbol
        Symbol symbol(symbol_id, stock_locate, symbol_name);

        // Store in maps
        symbols_.push_back(symbol);
        name_to_id_[symbol_name] = symbol_id;
        locate_to_id_[stock_locate] = symbol_id;

        return symbol_id;
    }

    /**
     * Get symbol by ID
     */
    const Symbol* get_symbol(uint32_t symbol_id) const {
        if (symbol_id >= symbols_.size()) {
            return nullptr;
        }
        return &symbols_[symbol_id];
    }

    /**
     * Get symbol by name
     */
    const Symbol* get_symbol_by_name(const std::string& symbol_name) const {
        auto it = name_to_id_.find(symbol_name);
        if (it == name_to_id_.end()) {
            return nullptr;
        }
        return get_symbol(it->second);
    }

    /**
     * Get symbol by NASDAQ stock locate code
     */
    const Symbol* get_symbol_by_locate(uint16_t stock_locate) const {
        auto it = locate_to_id_.find(stock_locate);
        if (it == locate_to_id_.end()) {
            return nullptr;
        }
        return get_symbol(it->second);
    }

    /**
     * Get symbol ID by name
     */
    bool get_symbol_id(const std::string& symbol_name, uint32_t& symbol_id) const {
        auto it = name_to_id_.find(symbol_name);
        if (it == name_to_id_.end()) {
            return false;
        }
        symbol_id = it->second;
        return true;
    }

    /**
     * Get symbol ID by locate code
     */
    bool get_symbol_id_by_locate(uint16_t stock_locate, uint32_t& symbol_id) const {
        auto it = locate_to_id_.find(stock_locate);
        if (it == locate_to_id_.end()) {
            return false;
        }
        symbol_id = it->second;
        return true;
    }

    /**
     * Check if symbol exists
     */
    bool has_symbol(const std::string& symbol_name) const {
        return name_to_id_.find(symbol_name) != name_to_id_.end();
    }

    bool has_symbol_by_locate(uint16_t stock_locate) const {
        return locate_to_id_.find(stock_locate) != locate_to_id_.end();
    }

    /**
     * Get number of registered symbols
     */
    size_t size() const {
        return symbols_.size();
    }

    /**
     * Get all symbols (sorted by ID)
     */
    const std::vector<Symbol>& get_all_symbols() const {
        return symbols_;
    }

    /**
     * Sort symbols alphabetically and reassign IDs (for determinism)
     * Call this after registering all symbols but before trading starts
     */
    void finalize_deterministic_ordering() {
        if (symbols_.empty()) return;

        // Create sorted vector of symbols by name
        std::vector<Symbol> sorted_symbols = symbols_;
        std::sort(sorted_symbols.begin(), sorted_symbols.end(),
                  [](const Symbol& a, const Symbol& b) {
                      return a.symbol_name < b.symbol_name;
                  });

        // Reassign IDs deterministically
        symbols_.clear();
        name_to_id_.clear();
        locate_to_id_.clear();

        for (uint32_t i = 0; i < sorted_symbols.size(); ++i) {
            Symbol& symbol = sorted_symbols[i];
            symbol.symbol_id = i;

            symbols_.push_back(symbol);
            name_to_id_[symbol.symbol_name] = i;
            locate_to_id_[symbol.stock_locate] = i;
        }

        next_symbol_id_ = static_cast<uint32_t>(symbols_.size());
    }

    /**
     * Clear all symbols
     */
    void clear() {
        symbols_.clear();
        name_to_id_.clear();
        locate_to_id_.clear();
        next_symbol_id_ = 0;
    }

private:
    std::vector<Symbol> symbols_;                    // All symbols (indexed by symbol_id)
    std::map<std::string, uint32_t> name_to_id_;     // Symbol name -> ID (ordered map for determinism)
    std::map<uint16_t, uint32_t> locate_to_id_;      // Stock locate -> ID (ordered map for determinism)
    uint32_t next_symbol_id_;                        // Next available symbol ID
};

} // namespace Matching
} // namespace Trader

#endif // TRADER_MATCHING_SYMBOL_H
