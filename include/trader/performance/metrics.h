#ifndef TRADER_PERFORMANCE_METRICS_H
#define TRADER_PERFORMANCE_METRICS_H

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace Trader {
namespace Performance {

/**
 * High-resolution monotonic clock used for all wall-clock latency measurements.
 *
 * NOTE on determinism:
 *   The matching engine + strategy use ITCH message timestamps for all trading
 *   logic, so fills and PnL are fully reproducible. This module measures engine
 *   *wall-clock* performance (latency and throughput), which is how we compare
 *   CPU baseline vs. CUDA for the thesis. Distributions are reproducible within
 *   a few percent across runs on the same hardware.
 */
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

inline TimePoint now() { return Clock::now(); }

inline uint64_t ns_since(const TimePoint& start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

/**
 * Power-of-2 latency histogram.
 *
 * Bucket i covers [2^i, 2^(i+1)) nanoseconds. 64 buckets cover 1ns .. 2^64 ns
 * which is more than enough for any realistic latency.
 *
 * O(1) record, deterministic bucket mapping, no allocation on the hot path.
 */
class LatencyHistogram {
public:
    static constexpr size_t NUM_BUCKETS = 64;

    LatencyHistogram()
        : buckets_{}
        , count_(0)
        , min_ns_(UINT64_MAX)
        , max_ns_(0)
        , sum_ns_(0) {}

    inline void record(uint64_t ns) {
        size_t bucket = 0;
        if (ns > 0) {
            // floor(log2(ns))
            bucket = 63 - static_cast<size_t>(__builtin_clzll(ns));
            if (bucket >= NUM_BUCKETS) bucket = NUM_BUCKETS - 1;
        }
        ++buckets_[bucket];
        ++count_;
        sum_ns_ += ns;
        if (ns < min_ns_) min_ns_ = ns;
        if (ns > max_ns_) max_ns_ = ns;
    }

    uint64_t count() const { return count_; }
    uint64_t min_ns() const { return count_ ? min_ns_ : 0; }
    uint64_t max_ns() const { return max_ns_; }
    double mean_ns() const {
        return count_ ? static_cast<double>(sum_ns_) / static_cast<double>(count_) : 0.0;
    }

    /**
     * Return an upper-bound estimate of the p-th percentile (0.0..1.0).
     * Resolution is limited to power-of-2 bucket edges - good enough for
     * order-of-magnitude latency reporting and for comparing CPU vs GPU.
     */
    uint64_t percentile(double p) const {
        if (count_ == 0) return 0;
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
        uint64_t target = static_cast<uint64_t>(p * static_cast<double>(count_));
        if (target >= count_) target = count_ - 1;
        uint64_t cum = 0;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            cum += buckets_[i];
            if (cum > target) {
                return (i == 0) ? 1ULL : (1ULL << (i + 1));
            }
        }
        return max_ns_;
    }

    uint64_t p50() const { return percentile(0.50); }
    uint64_t p95() const { return percentile(0.95); }
    uint64_t p99() const { return percentile(0.99); }
    uint64_t p999() const { return percentile(0.999); }

    const std::array<uint64_t, NUM_BUCKETS>& buckets() const { return buckets_; }

    void write_csv(std::ostream& out) const {
        out << "bucket_low_ns,bucket_high_ns,count\n";
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            if (buckets_[i] == 0) continue;
            uint64_t low = (i == 0) ? 0 : (1ULL << i);
            uint64_t high = 1ULL << (i + 1);
            out << low << "," << high << "," << buckets_[i] << "\n";
        }
    }

    void reset() {
        buckets_.fill(0);
        count_ = 0;
        min_ns_ = UINT64_MAX;
        max_ns_ = 0;
        sum_ns_ = 0;
    }

private:
    std::array<uint64_t, NUM_BUCKETS> buckets_;
    uint64_t count_;
    uint64_t min_ns_;
    uint64_t max_ns_;
    uint64_t sum_ns_;
};

/**
 * Throughput counter with periodic sampling.
 *
 * record() is O(1) and lock-free. sample() captures a (elapsed, cumulative,
 * interval_rate) tuple so we can plot throughput over the run.
 */
class ThroughputCounter {
public:
    struct Sample {
        double elapsed_sec;      // seconds since start
        uint64_t cumulative;     // total items counted so far
        double interval_rate;    // items/sec since the previous sample
    };

    ThroughputCounter()
        : start_(now())
        , last_sample_time_(start_)
        , total_(0)
        , last_total_(0) {}

    inline void record() { ++total_; }
    inline void record(uint64_t n) { total_ += n; }

    uint64_t total() const { return total_; }

    double elapsed_seconds() const {
        return std::chrono::duration<double>(now() - start_).count();
    }

    double total_rate() const {
        double s = elapsed_seconds();
        return (s > 0.0) ? (static_cast<double>(total_) / s) : 0.0;
    }

    Sample sample() {
        TimePoint tp = now();
        double interval = std::chrono::duration<double>(tp - last_sample_time_).count();
        uint64_t delta = total_ - last_total_;
        double rate = (interval > 0.0) ? (static_cast<double>(delta) / interval) : 0.0;
        last_sample_time_ = tp;
        last_total_ = total_;
        double elapsed = std::chrono::duration<double>(tp - start_).count();
        samples_.push_back({elapsed, total_, rate});
        return samples_.back();
    }

    const std::vector<Sample>& samples() const { return samples_; }

    void write_csv(std::ostream& out) const {
        out << "elapsed_sec,cumulative,interval_rate\n";
        for (const auto& s : samples_) {
            out << std::fixed << std::setprecision(6) << s.elapsed_sec << ","
                << s.cumulative << ","
                << std::setprecision(2) << s.interval_rate << "\n";
        }
    }

    void reset() {
        start_ = now();
        last_sample_time_ = start_;
        total_ = 0;
        last_total_ = 0;
        samples_.clear();
    }

private:
    TimePoint start_;
    TimePoint last_sample_time_;
    uint64_t total_;
    uint64_t last_total_;
    std::vector<Sample> samples_;
};

/**
 * RAII scoped timer. Records wall-clock ns between construction and
 * destruction into the target histogram.
 */
class ScopedTimer {
public:
    explicit ScopedTimer(LatencyHistogram& h) : h_(h), start_(now()) {}
    ~ScopedTimer() { h_.record(ns_since(start_)); }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    LatencyHistogram& h_;
    TimePoint start_;
};

/**
 * Aggregate engine-wide metrics. Owned by main_strategy / main.
 */
struct EngineMetrics {
    // Per-stage wall-clock latency histograms (nanoseconds)
    LatencyHistogram itch_message;     // Full process_message() including callbacks
    LatencyHistogram strategy_decide;  // Strategy on_order_book_update + quote submit

    // Throughput counters
    ThroughputCounter itch_messages;
    ThroughputCounter book_events;
    ThroughputCounter strategy_quotes;

    // ITCH timestamp range (for reproducibility context)
    uint64_t first_itch_ns = 0;
    uint64_t last_itch_ns = 0;

    void note_itch_timestamp(uint64_t ns) {
        if (first_itch_ns == 0) first_itch_ns = ns;
        last_itch_ns = ns;
    }

    void write_report(std::ostream& out) const {
        auto row = [&](const char* label, const LatencyHistogram& h) {
            out << "  " << std::left << std::setw(22) << label
                << "count=" << std::right << std::setw(12) << h.count()
                << "  min=" << std::setw(8) << h.min_ns() << "ns"
                << "  mean=" << std::setw(8) << std::fixed << std::setprecision(0)
                << h.mean_ns() << "ns"
                << "  p50=" << std::setw(8) << h.p50() << "ns"
                << "  p95=" << std::setw(9) << h.p95() << "ns"
                << "  p99=" << std::setw(9) << h.p99() << "ns"
                << "  p99.9=" << std::setw(10) << h.p999() << "ns"
                << "  max=" << h.max_ns() << "ns"
                << "\n";
        };

        out << "========================================\n";
        out << "HFT Engine - Performance Report\n";
        out << "========================================\n\n";

        out << "Wall-clock latency (nanoseconds):\n";
        row("ITCH msg total", itch_message);
        row("Strategy decide", strategy_decide);

        out << "\nThroughput:\n";
        out << "  ITCH messages   : " << std::right << std::setw(14) << itch_messages.total()
            << "  rate=" << std::fixed << std::setprecision(0)
            << itch_messages.total_rate() << " msgs/s\n";
        out << "  Book events     : " << std::setw(14) << book_events.total()
            << "  rate=" << book_events.total_rate() << " evts/s\n";
        out << "  Strategy quotes : " << std::setw(14) << strategy_quotes.total()
            << "  rate=" << strategy_quotes.total_rate() << " quotes/s\n";

        out << "\nITCH timestamp range (market time, nanoseconds since midnight):\n";
        out << "  first = " << first_itch_ns << "\n";
        out << "  last  = " << last_itch_ns << "\n";
        if (last_itch_ns > first_itch_ns) {
            double market_hours =
                static_cast<double>(last_itch_ns - first_itch_ns) / 3.6e12;
            out << "  span  = " << std::setprecision(3) << market_hours << " h of market time\n";
        }

        out << "\nNotes:\n";
        out << "  * Latencies are wall-clock (std::chrono::steady_clock).\n";
        out << "  * Bucket resolution is power-of-2 in ns (p-values are upper bounds).\n";
        out << "  * Trading behavior (orders, fills, PnL) is driven by ITCH timestamps\n";
        out << "    and is deterministic - same input file always produces same trades.\n";
        out << "  * Wall-clock distributions are reproducible across runs (~5% variation).\n";
    }

    void write_artifacts(const std::string& dir) const {
        auto dump_hist = [&](const char* name, const LatencyHistogram& h) {
            std::ofstream f(dir + "/latency_" + name + ".csv");
            if (f.is_open()) h.write_csv(f);
        };
        dump_hist("itch_message", itch_message);
        dump_hist("strategy_decide", strategy_decide);

        auto dump_tput = [&](const char* name, const ThroughputCounter& t) {
            std::ofstream f(dir + "/throughput_" + name + ".csv");
            if (f.is_open()) t.write_csv(f);
        };
        dump_tput("itch_messages", itch_messages);
        dump_tput("book_events", book_events);
        dump_tput("strategy_quotes", strategy_quotes);
    }
};

} // namespace Performance
} // namespace Trader

#endif // TRADER_PERFORMANCE_METRICS_H
