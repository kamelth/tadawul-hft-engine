#ifndef CORE_TIMESTAMP_H
#define CORE_TIMESTAMP_H

#include <cstdint>
#include <string>
#include <ctime>

namespace Core {

/**
 * Timestamp class for deterministic time handling
 *
 * IMPORTANT: For determinism, this uses ITCH message timestamps only,
 * NOT system clock. All timestamps come from market data feed.
 *
 * ITCH timestamp format: Nanoseconds since midnight
 */
class Timestamp {
public:
    // Default constructor (epoch = 0)
    Timestamp() : nanoseconds_(0) {}

    // Construct from nanoseconds since midnight
    explicit Timestamp(uint64_t nanoseconds) : nanoseconds_(nanoseconds) {}

    // Construct from hours, minutes, seconds, nanoseconds
    Timestamp(uint32_t hours, uint32_t minutes, uint32_t seconds, uint32_t nanoseconds = 0)
        : nanoseconds_(hours * 3600000000000ULL +
                      minutes * 60000000000ULL +
                      seconds * 1000000000ULL +
                      nanoseconds) {}

    // Get nanoseconds since midnight
    uint64_t nanoseconds() const { return nanoseconds_; }

    // Get microseconds since midnight
    uint64_t microseconds() const { return nanoseconds_ / 1000; }

    // Get milliseconds since midnight
    uint64_t milliseconds() const { return nanoseconds_ / 1000000; }

    // Get seconds since midnight
    uint64_t seconds() const { return nanoseconds_ / 1000000000; }

    // Extract time components
    uint32_t hours() const { return static_cast<uint32_t>(nanoseconds_ / 3600000000000ULL); }
    uint32_t minutes() const { return static_cast<uint32_t>((nanoseconds_ % 3600000000000ULL) / 60000000000ULL); }
    uint32_t secs() const { return static_cast<uint32_t>((nanoseconds_ % 60000000000ULL) / 1000000000ULL); }
    uint32_t nanos() const { return static_cast<uint32_t>(nanoseconds_ % 1000000000ULL); }

    // Comparison operators (for deterministic ordering)
    bool operator==(const Timestamp& other) const { return nanoseconds_ == other.nanoseconds_; }
    bool operator!=(const Timestamp& other) const { return nanoseconds_ != other.nanoseconds_; }
    bool operator<(const Timestamp& other) const { return nanoseconds_ < other.nanoseconds_; }
    bool operator<=(const Timestamp& other) const { return nanoseconds_ <= other.nanoseconds_; }
    bool operator>(const Timestamp& other) const { return nanoseconds_ > other.nanoseconds_; }
    bool operator>=(const Timestamp& other) const { return nanoseconds_ >= other.nanoseconds_; }

    // Arithmetic operators
    Timestamp operator+(const Timestamp& other) const {
        return Timestamp(nanoseconds_ + other.nanoseconds_);
    }

    Timestamp operator-(const Timestamp& other) const {
        return Timestamp(nanoseconds_ - other.nanoseconds_);
    }

    Timestamp& operator+=(const Timestamp& other) {
        nanoseconds_ += other.nanoseconds_;
        return *this;
    }

    Timestamp& operator-=(const Timestamp& other) {
        nanoseconds_ -= other.nanoseconds_;
        return *this;
    }

    // Difference in nanoseconds
    int64_t diff_nanos(const Timestamp& other) const {
        return static_cast<int64_t>(nanoseconds_) - static_cast<int64_t>(other.nanoseconds_);
    }

    // Difference in microseconds
    int64_t diff_micros(const Timestamp& other) const {
        return diff_nanos(other) / 1000;
    }

    // Difference in milliseconds
    int64_t diff_millis(const Timestamp& other) const {
        return diff_nanos(other) / 1000000;
    }

    // String representation (HH:MM:SS.nnnnnnnnn)
    std::string to_string() const {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u.%09u",
                hours(), minutes(), secs(), nanos());
        return std::string(buffer);
    }

    // Parse from ITCH 48-bit timestamp (6 bytes, big-endian)
    static Timestamp from_itch_timestamp(const uint8_t* data) {
        uint64_t nanos = 0;
        // Big-endian 48-bit integer
        for (int i = 0; i < 6; ++i) {
            nanos = (nanos << 8) | data[i];
        }
        return Timestamp(nanos);
    }

    // Create timestamp from current system time (USE ONLY FOR DEBUGGING/LOGGING)
    // NEVER use this for actual trading logic - breaks determinism!
    static Timestamp now() {
        time_t now_time = time(nullptr);
        struct tm* tm_info = localtime(&now_time);
        return Timestamp(tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, 0);
    }

private:
    uint64_t nanoseconds_;  // Nanoseconds since midnight
};

} // namespace Core

#endif // CORE_TIMESTAMP_H
