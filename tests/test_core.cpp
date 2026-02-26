#include <iostream>
#include <cassert>
#include <cstring>
#include "core/timestamp.h"
#include "core/endian.h"
#include "core/containers/list.h"
#include "core/containers/bintree_avl.h"

using namespace Core;
using namespace Core::Containers;

// Test data structure for list
struct TestItem {
    int id;
    ListNode<TestItem> node;

    explicit TestItem(int i) : id(i) {}
};

// Test counters
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
        } \
    } \
    void test_##name()

#define ASSERT(condition) \
    if (!(condition)) { \
        throw std::runtime_error("Assertion failed: " #condition); \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        throw std::runtime_error("Assertion failed: " #a " == " #b); \
    }

// ============================================================================
// Timestamp tests
// ============================================================================

TEST(timestamp_construction) {
    Timestamp t1;
    ASSERT_EQ(t1.nanoseconds(), 0ULL);

    Timestamp t2(1000000000ULL);
    ASSERT_EQ(t2.nanoseconds(), 1000000000ULL);

    Timestamp t3(10, 30, 45, 123456789);
    ASSERT_EQ(t3.hours(), 10U);
    ASSERT_EQ(t3.minutes(), 30U);
    ASSERT_EQ(t3.secs(), 45U);
    ASSERT_EQ(t3.nanos(), 123456789U);
}

TEST(timestamp_conversions) {
    Timestamp t(0, 0, 1, 500000000);  // 1.5 seconds

    ASSERT_EQ(t.seconds(), 1ULL);
    ASSERT_EQ(t.milliseconds(), 1500ULL);
    ASSERT_EQ(t.microseconds(), 1500000ULL);
    ASSERT_EQ(t.nanoseconds(), 1500000000ULL);
}

TEST(timestamp_comparison) {
    Timestamp t1(1000);
    Timestamp t2(2000);
    Timestamp t3(1000);

    ASSERT(t1 < t2);
    ASSERT(t1 <= t2);
    ASSERT(t2 > t1);
    ASSERT(t2 >= t1);
    ASSERT(t1 == t3);
    ASSERT(t1 != t2);
}

TEST(timestamp_arithmetic) {
    Timestamp t1(1000);
    Timestamp t2(500);

    Timestamp t3 = t1 + t2;
    ASSERT_EQ(t3.nanoseconds(), 1500ULL);

    Timestamp t4 = t1 - t2;
    ASSERT_EQ(t4.nanoseconds(), 500ULL);

    ASSERT_EQ(t1.diff_nanos(t2), 500LL);
}

TEST(timestamp_itch_parsing) {
    uint8_t itch_data[6] = {0x00, 0x00, 0x00, 0x00, 0x03, 0xE8};  // 1000 in big-endian
    Timestamp t = Timestamp::from_itch_timestamp(itch_data);
    ASSERT_EQ(t.nanoseconds(), 1000ULL);
}

// ============================================================================
// Endian tests
// ============================================================================

TEST(endian_swap16) {
    uint16_t value = 0x1234;
    uint16_t swapped = Endian::swap16(value);
    ASSERT_EQ(swapped, 0x3412);
}

TEST(endian_swap32) {
    uint32_t value = 0x12345678;
    uint32_t swapped = Endian::swap32(value);
    ASSERT_EQ(swapped, 0x78563412U);
}

TEST(endian_swap64) {
    uint64_t value = 0x123456789ABCDEF0ULL;
    uint64_t swapped = Endian::swap64(value);
    ASSERT_EQ(swapped, 0xF0DEBC9A78563412ULL);
}

TEST(endian_be_conversions) {
    uint32_t value = 0x12345678;

    // Convert to big-endian and back
    uint32_t be = Endian::htobe32(value);
    uint32_t host = Endian::be32toh(be);

    ASSERT_EQ(host, value);
}

TEST(endian_read_be) {
    uint8_t data16[2] = {0x12, 0x34};
    uint16_t val16 = (static_cast<uint16_t>(data16[0]) << 8) | data16[1];
    ASSERT_EQ(val16, 0x1234);

    uint8_t data32[4] = {0x12, 0x34, 0x56, 0x78};
    uint32_t val32 = (static_cast<uint32_t>(data32[0]) << 24) |
                     (static_cast<uint32_t>(data32[1]) << 16) |
                     (static_cast<uint32_t>(data32[2]) << 8) |
                     data32[3];
    ASSERT_EQ(val32, 0x12345678U);
}

TEST(endian_read_be48) {
    uint8_t data[6] = {0x00, 0x00, 0x00, 0x00, 0x03, 0xE8};
    uint64_t value = Endian::read_be48(data);
    ASSERT_EQ(value, 1000ULL);
}

// ============================================================================
// List tests
// ============================================================================

TEST(list_construction) {
    List<TestItem, &TestItem::node> list;
    ASSERT(list.empty());
    ASSERT_EQ(list.size(), 0UL);
}

TEST(list_push_back) {
    List<TestItem, &TestItem::node> list;
    TestItem item1(1);
    TestItem item2(2);
    TestItem item3(3);

    list.push_back(&item1);
    list.push_back(&item2);
    list.push_back(&item3);

    ASSERT_EQ(list.size(), 3UL);
    ASSERT_EQ(list.front()->id, 1);
    ASSERT_EQ(list.back()->id, 3);
}

TEST(list_push_front) {
    List<TestItem, &TestItem::node> list;
    TestItem item1(1);
    TestItem item2(2);
    TestItem item3(3);

    list.push_front(&item1);
    list.push_front(&item2);
    list.push_front(&item3);

    ASSERT_EQ(list.size(), 3UL);
    ASSERT_EQ(list.front()->id, 3);
    ASSERT_EQ(list.back()->id, 1);
}

TEST(list_pop_operations) {
    List<TestItem, &TestItem::node> list;
    TestItem item1(1);
    TestItem item2(2);
    TestItem item3(3);

    list.push_back(&item1);
    list.push_back(&item2);
    list.push_back(&item3);

    list.pop_front();
    ASSERT_EQ(list.size(), 2UL);
    ASSERT_EQ(list.front()->id, 2);

    list.pop_back();
    ASSERT_EQ(list.size(), 1UL);
    ASSERT_EQ(list.back()->id, 2);
}

TEST(list_remove) {
    List<TestItem, &TestItem::node> list;
    TestItem item1(1);
    TestItem item2(2);
    TestItem item3(3);

    list.push_back(&item1);
    list.push_back(&item2);
    list.push_back(&item3);

    list.remove(&item2);
    ASSERT_EQ(list.size(), 2UL);
    ASSERT_EQ(list.front()->id, 1);
    ASSERT_EQ(list.back()->id, 3);
}

TEST(list_iteration) {
    List<TestItem, &TestItem::node> list;
    TestItem item1(1);
    TestItem item2(2);
    TestItem item3(3);

    list.push_back(&item1);
    list.push_back(&item2);
    list.push_back(&item3);

    int expected = 1;
    for (auto& item : list) {
        ASSERT_EQ(item.id, expected++);
    }
    ASSERT_EQ(expected, 4);
}

// ============================================================================
// AVL Tree tests
// ============================================================================

TEST(avl_construction) {
    BinTreeAVL<int, int> tree;
    ASSERT(tree.empty());
    ASSERT_EQ(tree.size(), 0UL);
}

TEST(avl_insert) {
    BinTreeAVL<int, int> tree;

    ASSERT(tree.insert(5, 50));
    ASSERT(tree.insert(3, 30));
    ASSERT(tree.insert(7, 70));

    ASSERT_EQ(tree.size(), 3UL);
    ASSERT(!tree.insert(5, 55));  // Duplicate key
    ASSERT_EQ(tree.size(), 3UL);
}

TEST(avl_find) {
    BinTreeAVL<int, int> tree;

    tree.insert(5, 50);
    tree.insert(3, 30);
    tree.insert(7, 70);

    int* val = tree.find(5);
    ASSERT(val != nullptr);
    ASSERT_EQ(*val, 50);

    val = tree.find(3);
    ASSERT(val != nullptr);
    ASSERT_EQ(*val, 30);

    val = tree.find(99);
    ASSERT(val == nullptr);
}

TEST(avl_min_max) {
    BinTreeAVL<int, int> tree;

    tree.insert(5, 50);
    tree.insert(3, 30);
    tree.insert(7, 70);
    tree.insert(1, 10);
    tree.insert(9, 90);

    const int* min_key = tree.find_min_key();
    const int* max_key = tree.find_max_key();

    ASSERT(min_key != nullptr);
    ASSERT(max_key != nullptr);
    ASSERT_EQ(*min_key, 1);
    ASSERT_EQ(*max_key, 9);
}

TEST(avl_erase) {
    BinTreeAVL<int, int> tree;

    tree.insert(5, 50);
    tree.insert(3, 30);
    tree.insert(7, 70);

    ASSERT(tree.erase(3));
    ASSERT_EQ(tree.size(), 2UL);
    ASSERT(tree.find(3) == nullptr);

    ASSERT(!tree.erase(99));  // Non-existent key
    ASSERT_EQ(tree.size(), 2UL);
}

TEST(avl_balance) {
    BinTreeAVL<int, int> tree;

    // Insert in sorted order (would create unbalanced tree without AVL)
    for (int i = 1; i <= 10; ++i) {
        tree.insert(i, i * 10);
    }

    ASSERT_EQ(tree.size(), 10UL);

    // Verify all values are still accessible
    for (int i = 1; i <= 10; ++i) {
        int* val = tree.find(i);
        ASSERT(val != nullptr);
        ASSERT_EQ(*val, i * 10);
    }
}

TEST(avl_traverse) {
    BinTreeAVL<int, int> tree;

    tree.insert(5, 50);
    tree.insert(3, 30);
    tree.insert(7, 70);
    tree.insert(1, 10);
    tree.insert(9, 90);

    int count = 0;
    int prev_key = 0;
    tree.traverse_inorder([&](int key, int value) {
        ASSERT(key > prev_key);  // Should be in sorted order
        prev_key = key;
        count++;
    });

    ASSERT_EQ(count, 5);
}

// ============================================================================
// Main test runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Core Utilities Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    // Timestamp tests
    std::cout << "\n--- Timestamp Tests ---" << std::endl;
    run_test_timestamp_construction();
    run_test_timestamp_conversions();
    run_test_timestamp_comparison();
    run_test_timestamp_arithmetic();
    run_test_timestamp_itch_parsing();

    // Endian tests
    std::cout << "\n--- Endian Tests ---" << std::endl;
    run_test_endian_swap16();
    run_test_endian_swap32();
    run_test_endian_swap64();
    run_test_endian_be_conversions();
    run_test_endian_read_be();
    run_test_endian_read_be48();

    // List tests
    std::cout << "\n--- List Tests ---" << std::endl;
    run_test_list_construction();
    run_test_list_push_back();
    run_test_list_push_front();
    run_test_list_pop_operations();
    run_test_list_remove();
    run_test_list_iteration();

    // AVL tree tests
    std::cout << "\n--- AVL Tree Tests ---" << std::endl;
    run_test_avl_construction();
    run_test_avl_insert();
    run_test_avl_find();
    run_test_avl_min_max();
    run_test_avl_erase();
    run_test_avl_balance();
    run_test_avl_traverse();

    // Summary
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Tests run:    " << tests_run << std::endl;
    std::cout << "Tests passed: " << tests_passed << std::endl;
    std::cout << "Tests failed: " << (tests_run - tests_passed) << std::endl;
    std::cout << "========================================" << std::endl;

    if (tests_passed == tests_run) {
        std::cout << "\n✓ All tests passed!" << std::endl;
        return 0;
    } else {
        std::cout << "\n✗ Some tests failed!" << std::endl;
        return 1;
    }
}
