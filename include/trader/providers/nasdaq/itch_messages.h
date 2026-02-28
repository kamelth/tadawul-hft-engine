#ifndef TRADER_PROVIDERS_NASDAQ_ITCH_MESSAGES_H
#define TRADER_PROVIDERS_NASDAQ_ITCH_MESSAGES_H

#include <cstdint>
#include <cstring>

namespace Trader {
namespace Providers {
namespace NASDAQ {

/**
 * NASDAQ ITCH 5.0 Message Types
 *
 * All messages are in big-endian format.
 * All prices are in units of $0.0001 (4 decimal places).
 * All timestamps are nanoseconds since midnight.
 *
 * Reference: NASDAQ TotalView-ITCH 5.0 Specification
 */

// Disable struct padding for binary message parsing
#pragma pack(push, 1)

/**
 * Message header (common to all messages)
 */
struct MessageHeader {
    uint16_t length;        // Message length (excluding these 2 bytes)
    uint8_t message_type;   // Message type identifier
};

/**
 * System Event Message (Type 'S')
 * Indicates start/end of market hours, start/end of system hours
 */
struct SystemEventMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];   // Nanoseconds since midnight (48-bit)
    uint8_t event_code;     // 'O' = Start of Messages, 'S' = Start of System Hours,
                            // 'Q' = Start of Market Hours, 'M' = End of Market Hours,
                            // 'E' = End of System Hours, 'C' = End of Messages
};

/**
 * Stock Directory Message (Type 'R')
 * Contains symbol information for each trading symbol
 */
struct StockDirectoryMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    char stock[8];          // Stock symbol (right-padded with spaces)
    uint8_t market_category;
    uint8_t financial_status;
    uint32_t round_lot_size;
    uint8_t round_lots_only;
    uint8_t issue_classification;
    char issue_sub_type[2];
    uint8_t authenticity;
    uint8_t short_sale_threshold;
    uint8_t ipo_flag;
    uint8_t luld_reference_price_tier;
    uint8_t etp_flag;
    uint32_t etp_leverage_factor;
    uint8_t inverse_indicator;
};

/**
 * Stock Trading Action Message (Type 'H')
 * Indicates current trading state of a security
 */
struct StockTradingActionMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    char stock[8];
    uint8_t trading_state;  // 'H' = Halted, 'P' = Paused, 'Q' = Quotation Only, 'T' = Trading
    uint8_t reserved;
    char reason[4];
};

/**
 * Add Order Message (Type 'A')
 * Add a new order to the book (no MPID attribution)
 */
struct AddOrderMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint8_t order_reference[8];  // Order reference number (64-bit)
    uint8_t buy_sell;            // 'B' = Buy, 'S' = Sell
    uint32_t shares;             // Number of shares
    char stock[8];               // Stock symbol
    uint32_t price;              // Price in units of $0.0001
};

/**
 * Add Order with MPID Message (Type 'F')
 * Add a new order with market participant attribution
 */
struct AddOrderMPIDMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint8_t order_reference[8];
    uint8_t buy_sell;
    uint32_t shares;
    char stock[8];
    uint32_t price;
    char attribution[4];         // Market participant identifier
};

/**
 * Order Executed Message (Type 'E')
 * Order has been executed in whole or in part
 */
struct OrderExecutedMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint8_t order_reference[8];
    uint32_t executed_shares;    // Number of shares executed
    uint8_t match_number[8];     // Match number (64-bit)
};

/**
 * Order Executed with Price Message (Type 'C')
 * Order executed with a different price (for cross trades)
 */
struct OrderExecutedWithPriceMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint8_t order_reference[8];
    uint32_t executed_shares;
    uint8_t match_number[8];
    uint8_t printable;           // 'Y' = Printable, 'N' = Non-Printable
    uint32_t execution_price;    // Execution price
};

/**
 * Order Cancel Message (Type 'X')
 * Reduce the displayed quantity of an order
 */
struct OrderCancelMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint8_t order_reference[8];
    uint32_t cancelled_shares;   // Number of shares cancelled
};

/**
 * Order Delete Message (Type 'D')
 * Remove an order from the book completely
 */
struct OrderDeleteMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint8_t order_reference[8];
};

/**
 * Order Replace Message (Type 'U')
 * Replace an existing order with a new order
 */
struct OrderReplaceMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint8_t original_order_reference[8];
    uint8_t new_order_reference[8];
    uint32_t shares;             // New shares
    uint32_t price;              // New price
};

/**
 * Trade Message (Type 'P')
 * Non-cross trade executed
 */
struct TradeMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint8_t order_reference[8];
    uint8_t buy_sell;
    uint32_t shares;
    char stock[8];
    uint32_t price;
    uint8_t match_number[8];
};

/**
 * Cross Trade Message (Type 'Q')
 * Cross trade executed
 */
struct CrossTradeMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint64_t shares;
    char stock[8];
    uint32_t cross_price;
    uint8_t match_number[8];
    uint8_t cross_type;
};

/**
 * Broken Trade Message (Type 'B')
 * Indicates that a trade should be broken/cancelled
 */
struct BrokenTradeMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint8_t match_number[8];
};

/**
 * NOII Message (Type 'I')
 * Net Order Imbalance Indicator
 */
struct NOIIMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint64_t paired_shares;
    uint64_t imbalance_shares;
    uint8_t imbalance_direction;  // 'B' = Buy, 'S' = Sell, 'N' = No Imbalance, 'O' = Insufficient
    char stock[8];
    uint32_t far_price;
    uint32_t near_price;
    uint32_t current_reference_price;
    uint8_t cross_type;
    uint8_t price_variation;
};

#pragma pack(pop)

/**
 * Helper functions for parsing ITCH fields
 */
class ITCHParser {
public:
    // Parse 48-bit timestamp to uint64_t (nanoseconds since midnight)
    static uint64_t parse_timestamp(const uint8_t* data) {
        uint64_t timestamp = 0;
        for (int i = 0; i < 6; ++i) {
            timestamp = (timestamp << 8) | data[i];
        }
        return timestamp;
    }

    // Parse 64-bit order reference number
    static uint64_t parse_order_reference(const uint8_t* data) {
        uint64_t ref = 0;
        for (int i = 0; i < 8; ++i) {
            ref = (ref << 8) | data[i];
        }
        return ref;
    }

    // Parse stock symbol (trim trailing spaces)
    static std::string parse_stock(const char* stock, size_t length = 8) {
        std::string result(stock, length);
        // Trim trailing spaces
        size_t end = result.find_last_not_of(' ');
        if (end != std::string::npos) {
            result = result.substr(0, end + 1);
        }
        return result;
    }

    // Convert big-endian uint16 to host
    static uint16_t be16toh(uint16_t value) {
        #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return ((value >> 8) & 0x00FF) | ((value << 8) & 0xFF00);
        #else
        return value;
        #endif
    }

    // Convert big-endian uint32 to host
    static uint32_t be32toh(uint32_t value) {
        #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return ((value >> 24) & 0x000000FF) |
               ((value >> 8)  & 0x0000FF00) |
               ((value << 8)  & 0x00FF0000) |
               ((value << 24) & 0xFF000000);
        #else
        return value;
        #endif
    }

    // Convert big-endian uint64 to host
    static uint64_t be64toh(uint64_t value) {
        #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return ((value >> 56) & 0x00000000000000FFULL) |
               ((value >> 40) & 0x000000000000FF00ULL) |
               ((value >> 24) & 0x0000000000FF0000ULL) |
               ((value >> 8)  & 0x00000000FF000000ULL) |
               ((value << 8)  & 0x000000FF00000000ULL) |
               ((value << 24) & 0x0000FF0000000000ULL) |
               ((value << 40) & 0x00FF000000000000ULL) |
               ((value << 56) & 0xFF00000000000000ULL);
        #else
        return value;
        #endif
    }
};

} // namespace NASDAQ
} // namespace Providers
} // namespace Trader

#endif // TRADER_PROVIDERS_NASDAQ_ITCH_MESSAGES_H
