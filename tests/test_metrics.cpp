#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>
#include "trader/performance/metrics.h"

using namespace Trader::Performance;

int tests_run = 0;
int tests_passed = 0;

#define TEST(name) \
    void test_##name(); \
    void run_test_##name() { \
        tests_run++; \
        std::cout << "Running test: " << #name << "..."; \
        try { \
            test_##name(); \
            tests_passed++; \
            std::cout << " PASSED" << std::endl; \
        } catch (const std::exception& e) { \
            std::cout << " FAILED: " << e.what() << std::endl; \
        } catch (...) { \
            std::cout << " FAILED: unknown exception" << std::endl; \
        } \
    } \
    void test_##name()

#define ASSERT(condition) \
    if (!(condition)) { \
        throw std::runtime_error("Assertion failed: " #condition); \
    }

// ---------- LatencyHistogram ----------

TEST(histogram_empty) {
    LatencyHistogram h;
    ASSERT(h.count() == 0);
    ASSERT(h.min_ns() == 0);
    ASSERT(h.max_ns() == 0);
    ASSERT(h.mean_ns() == 0.0);
    ASSERT(h.p50() == 0);
    ASSERT(h.p99() == 0);
}

TEST(histogram_single_record) {
    LatencyHistogram h;
    h.record(500);  // 500ns -> bucket floor(log2(500)) = 8, range [256,512)
    ASSERT(h.count() == 1);
    ASSERT(h.min_ns() == 500);
    ASSERT(h.max_ns() == 500);
    ASSERT(h.mean_ns() == 500.0);
    // p50 on single sample should point at the bucket we hit.
    ASSERT(h.p50() >= 500);
}

TEST(histogram_many_records) {
    LatencyHistogram h;
    // Record 1000 samples uniformly in [100ns..10000ns].
    for (uint64_t i = 0; i < 1000; ++i) {
        h.record(100 + (i * 9900) / 999);
    }
    ASSERT(h.count() == 1000);
    ASSERT(h.min_ns() == 100);
    ASSERT(h.max_ns() >= 9900);
    // Sanity: p50 < p99 (monotonic percentiles)
    ASSERT(h.p50() <= h.p95());
    ASSERT(h.p95() <= h.p99());
    ASSERT(h.p99() <= h.p999());
}

TEST(histogram_zero_ns) {
    LatencyHistogram h;
    h.record(0);
    h.record(0);
    h.record(0);
    ASSERT(h.count() == 3);
    ASSERT(h.min_ns() == 0);
    // 0ns should land in bucket 0 without crashing
    ASSERT(h.p50() > 0 || h.max_ns() == 0);
}

TEST(histogram_large_ns) {
    LatencyHistogram h;
    h.record(1'000'000'000ULL);   // 1 second
    h.record(10'000'000'000ULL);  // 10 seconds
    ASSERT(h.count() == 2);
    ASSERT(h.max_ns() == 10'000'000'000ULL);
}

TEST(histogram_csv_output) {
    LatencyHistogram h;
    h.record(300);
    h.record(300);
    h.record(5000);
    std::ostringstream os;
    h.write_csv(os);
    std::string s = os.str();
    ASSERT(s.find("bucket_low_ns") != std::string::npos);
    ASSERT(s.find(",2\n") != std::string::npos || s.find(",1\n") != std::string::npos);
}

TEST(histogram_reset) {
    LatencyHistogram h;
    h.record(1000);
    h.record(2000);
    ASSERT(h.count() == 2);
    h.reset();
    ASSERT(h.count() == 0);
    ASSERT(h.max_ns() == 0);
}

// ---------- ScopedTimer ----------

TEST(scoped_timer_records_on_destruct) {
    LatencyHistogram h;
    {
        ScopedTimer t(h);
        // Burn a little time so the recorded value is > 0.
        volatile uint64_t acc = 0;
        for (uint64_t i = 0; i < 10000; ++i) acc += i;
        (void)acc;
    }
    ASSERT(h.count() == 1);
    ASSERT(h.max_ns() > 0);
}

// ---------- ThroughputCounter ----------

TEST(throughput_basic_count) {
    ThroughputCounter t;
    ASSERT(t.total() == 0);
    t.record();
    t.record();
    t.record(8);
    ASSERT(t.total() == 10);
}

TEST(throughput_sample_captures_delta) {
    ThroughputCounter t;
    for (int i = 0; i < 100; ++i) t.record();
    auto s1 = t.sample();
    ASSERT(s1.cumulative == 100);
    for (int i = 0; i < 50; ++i) t.record();
    auto s2 = t.sample();
    ASSERT(s2.cumulative == 150);
    ASSERT(t.samples().size() == 2);
}

TEST(throughput_rate_nonnegative) {
    ThroughputCounter t;
    t.record(1000);
    // Force at least 1 ms of wall clock so rate is meaningful
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    double r = t.total_rate();
    ASSERT(r >= 0.0);
}

TEST(throughput_csv_output) {
    ThroughputCounter t;
    t.record(42);
    t.sample();
    std::ostringstream os;
    t.write_csv(os);
    std::string s = os.str();
    ASSERT(s.find("elapsed_sec") != std::string::npos);
    ASSERT(s.find("42") != std::string::npos);
}

// ---------- EngineMetrics ----------

TEST(engine_metrics_report_runs) {
    EngineMetrics m;
    m.itch_message.record(200);
    m.itch_message.record(800);
    m.strategy_decide.record(1500);
    m.itch_messages.record(2);
    m.book_events.record();
    m.strategy_quotes.record();
    m.note_itch_timestamp(34'200'000'000'000ULL);  // 09:30:00
    m.note_itch_timestamp(57'600'000'000'000ULL);  // 16:00:00

    std::ostringstream os;
    m.write_report(os);
    std::string r = os.str();
    ASSERT(r.find("Performance Report") != std::string::npos);
    ASSERT(r.find("ITCH msg total") != std::string::npos);
    ASSERT(r.find("Strategy decide") != std::string::npos);
    ASSERT(r.find("Throughput") != std::string::npos);
}

TEST(engine_metrics_itch_range) {
    EngineMetrics m;
    m.note_itch_timestamp(1000);
    m.note_itch_timestamp(2000);
    m.note_itch_timestamp(5000);
    ASSERT(m.first_itch_ns == 1000);
    ASSERT(m.last_itch_ns == 5000);
}

int main() {
    std::cout << "====================================" << std::endl;
    std::cout << "Performance Metrics Tests" << std::endl;
    std::cout << "====================================" << std::endl;

    run_test_histogram_empty();
    run_test_histogram_single_record();
    run_test_histogram_many_records();
    run_test_histogram_zero_ns();
    run_test_histogram_large_ns();
    run_test_histogram_csv_output();
    run_test_histogram_reset();
    run_test_scoped_timer_records_on_destruct();
    run_test_throughput_basic_count();
    run_test_throughput_sample_captures_delta();
    run_test_throughput_rate_nonnegative();
    run_test_throughput_csv_output();
    run_test_engine_metrics_report_runs();
    run_test_engine_metrics_itch_range();

    std::cout << "====================================" << std::endl;
    std::cout << "Results: " << tests_passed << "/" << tests_run << " passed" << std::endl;
    std::cout << "====================================" << std::endl;

    return (tests_passed == tests_run) ? 0 : 1;
}
