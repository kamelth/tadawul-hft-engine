#ifndef CORE_ENDIAN_H
#define CORE_ENDIAN_H

#include <cstdint>

namespace Core {

/**
 * Endian conversion utilities for deterministic binary data parsing
 *
 * NASDAQ ITCH data is big-endian, but most modern CPUs are little-endian.
 * These functions convert between host and network (big-endian) byte order.
 */
class Endian {
public:
    // Detect host endianness at compile time
    static constexpr bool is_little_endian() {
        return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
    }

    static constexpr bool is_big_endian() {
        return __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__;
    }

    // Byte swap functions (portable)
    static inline uint16_t swap16(uint16_t value) {
        return (value >> 8) | (value << 8);
    }

    static inline uint32_t swap32(uint32_t value) {
        return ((value >> 24) & 0x000000FF) |
               ((value >> 8)  & 0x0000FF00) |
               ((value << 8)  & 0x00FF0000) |
               ((value << 24) & 0xFF000000);
    }

    static inline uint64_t swap64(uint64_t value) {
        return ((value >> 56) & 0x00000000000000FFULL) |
               ((value >> 40) & 0x000000000000FF00ULL) |
               ((value >> 24) & 0x0000000000FF0000ULL) |
               ((value >> 8)  & 0x00000000FF000000ULL) |
               ((value << 8)  & 0x000000FF00000000ULL) |
               ((value << 24) & 0x0000FF0000000000ULL) |
               ((value << 40) & 0x00FF000000000000ULL) |
               ((value << 56) & 0xFF00000000000000ULL);
    }

    // Big-endian to host (ITCH data is big-endian)
    static inline uint16_t be16toh(uint16_t value) {
        if constexpr (is_little_endian()) {
            return swap16(value);
        } else {
            return value;
        }
    }

    static inline uint32_t be32toh(uint32_t value) {
        if constexpr (is_little_endian()) {
            return swap32(value);
        } else {
            return value;
        }
    }

    static inline uint64_t be64toh(uint64_t value) {
        if constexpr (is_little_endian()) {
            return swap64(value);
        } else {
            return value;
        }
    }

    // Host to big-endian
    static inline uint16_t htobe16(uint16_t value) {
        if constexpr (is_little_endian()) {
            return swap16(value);
        } else {
            return value;
        }
    }

    static inline uint32_t htobe32(uint32_t value) {
        if constexpr (is_little_endian()) {
            return swap32(value);
        } else {
            return value;
        }
    }

    static inline uint64_t htobe64(uint64_t value) {
        if constexpr (is_little_endian()) {
            return swap64(value);
        } else {
            return value;
        }
    }

    // Read values from byte array (big-endian)
    static inline uint16_t read_be16(const uint8_t* data) {
        return be16toh(*reinterpret_cast<const uint16_t*>(data));
    }

    static inline uint32_t read_be32(const uint8_t* data) {
        return be32toh(*reinterpret_cast<const uint32_t*>(data));
    }

    static inline uint64_t read_be64(const uint8_t* data) {
        return be64toh(*reinterpret_cast<const uint64_t*>(data));
    }

    // Read 48-bit value (used in ITCH for timestamps and order IDs)
    static inline uint64_t read_be48(const uint8_t* data) {
        uint64_t value = 0;
        for (int i = 0; i < 6; ++i) {
            value = (value << 8) | data[i];
        }
        return value;
    }

    // Write values to byte array (big-endian)
    static inline void write_be16(uint8_t* data, uint16_t value) {
        *reinterpret_cast<uint16_t*>(data) = htobe16(value);
    }

    static inline void write_be32(uint8_t* data, uint32_t value) {
        *reinterpret_cast<uint32_t*>(data) = htobe32(value);
    }

    static inline void write_be64(uint8_t* data, uint64_t value) {
        *reinterpret_cast<uint64_t*>(data) = htobe64(value);
    }

    // Write 48-bit value
    static inline void write_be48(uint8_t* data, uint64_t value) {
        for (int i = 5; i >= 0; --i) {
            data[i] = static_cast<uint8_t>(value & 0xFF);
            value >>= 8;
        }
    }
};

} // namespace Core

#endif // CORE_ENDIAN_H
