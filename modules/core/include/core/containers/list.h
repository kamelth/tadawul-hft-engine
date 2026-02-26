#ifndef CORE_CONTAINERS_LIST_H
#define CORE_CONTAINERS_LIST_H

#include <cstddef>
#include <iterator>

namespace Core {
namespace Containers {

/**
 * Intrusive doubly-linked list for deterministic FIFO ordering
 *
 * This is used for maintaining order queues within price levels.
 * Intrusive design means the list node is embedded in the data structure,
 * avoiding memory allocations and improving cache locality.
 *
 * Usage:
 *   struct Order {
 *       int id;
 *       ListNode<Order> node;  // Embedded list node
 *   };
 *
 *   List<Order, &Order::node> order_queue;
 */

/**
 * List node that must be embedded in the containing type
 */
template <typename T>
struct ListNode {
    T* prev = nullptr;
    T* next = nullptr;

    ListNode() = default;

    // Non-copyable, non-movable (intrusive nodes are tied to their owner)
    ListNode(const ListNode&) = delete;
    ListNode& operator=(const ListNode&) = delete;
};

/**
 * Intrusive doubly-linked list
 */
template <typename T, ListNode<T> T::*NodeMember>
class List {
public:
    List() : head_(nullptr), tail_(nullptr), size_(0) {}

    // Non-copyable (use explicit clone if needed)
    List(const List&) = delete;
    List& operator=(const List&) = delete;

    // Movable
    List(List&& other) noexcept
        : head_(other.head_), tail_(other.tail_), size_(other.size_) {
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.size_ = 0;
    }

    List& operator=(List&& other) noexcept {
        if (this != &other) {
            clear();
            head_ = other.head_;
            tail_ = other.tail_;
            size_ = other.size_;
            other.head_ = nullptr;
            other.tail_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    ~List() {
        // Intrusive list doesn't own its elements - just reset pointers
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
    }

    // Capacity
    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }

    // Access
    T* front() { return head_; }
    const T* front() const { return head_; }
    T* back() { return tail_; }
    const T* back() const { return tail_; }

    // Modifiers
    void push_back(T* item) {
        auto& node = item->*NodeMember;
        node.prev = tail_;
        node.next = nullptr;

        if (tail_) {
            (tail_->*NodeMember).next = item;
        } else {
            head_ = item;
        }

        tail_ = item;
        ++size_;
    }

    void push_front(T* item) {
        auto& node = item->*NodeMember;
        node.prev = nullptr;
        node.next = head_;

        if (head_) {
            (head_->*NodeMember).prev = item;
        } else {
            tail_ = item;
        }

        head_ = item;
        ++size_;
    }

    void pop_front() {
        if (!head_) return;

        T* old_head = head_;
        head_ = (old_head->*NodeMember).next;

        if (head_) {
            (head_->*NodeMember).prev = nullptr;
        } else {
            tail_ = nullptr;
        }

        auto& node = old_head->*NodeMember;
        node.prev = nullptr;
        node.next = nullptr;

        --size_;
    }

    void pop_back() {
        if (!tail_) return;

        T* old_tail = tail_;
        tail_ = (old_tail->*NodeMember).prev;

        if (tail_) {
            (tail_->*NodeMember).next = nullptr;
        } else {
            head_ = nullptr;
        }

        auto& node = old_tail->*NodeMember;
        node.prev = nullptr;
        node.next = nullptr;

        --size_;
    }

    void remove(T* item) {
        auto& node = item->*NodeMember;

        if (node.prev) {
            (node.prev->*NodeMember).next = node.next;
        } else {
            head_ = node.next;
        }

        if (node.next) {
            (node.next->*NodeMember).prev = node.prev;
        } else {
            tail_ = node.prev;
        }

        node.prev = nullptr;
        node.next = nullptr;

        --size_;
    }

    void clear() {
        T* current = head_;
        while (current) {
            T* next = (current->*NodeMember).next;
            auto& node = current->*NodeMember;
            node.prev = nullptr;
            node.next = nullptr;
            current = next;
        }
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
    }

    // Iterator support
    class Iterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        Iterator(T* ptr = nullptr) : ptr_(ptr) {}

        T& operator*() const { return *ptr_; }
        T* operator->() const { return ptr_; }

        Iterator& operator++() {
            ptr_ = (ptr_->*NodeMember).next;
            return *this;
        }

        Iterator operator++(int) {
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        Iterator& operator--() {
            ptr_ = (ptr_->*NodeMember).prev;
            return *this;
        }

        Iterator operator--(int) {
            Iterator tmp = *this;
            --(*this);
            return tmp;
        }

        bool operator==(const Iterator& other) const { return ptr_ == other.ptr_; }
        bool operator!=(const Iterator& other) const { return ptr_ != other.ptr_; }

    private:
        T* ptr_;
    };

    Iterator begin() { return Iterator(head_); }
    Iterator end() { return Iterator(nullptr); }

    class ConstIterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = const T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        ConstIterator(const T* ptr = nullptr) : ptr_(ptr) {}

        const T& operator*() const { return *ptr_; }
        const T* operator->() const { return ptr_; }

        ConstIterator& operator++() {
            ptr_ = (ptr_->*NodeMember).next;
            return *this;
        }

        ConstIterator operator++(int) {
            ConstIterator tmp = *this;
            ++(*this);
            return tmp;
        }

        ConstIterator& operator--() {
            ptr_ = (ptr_->*NodeMember).prev;
            return *this;
        }

        ConstIterator operator--(int) {
            ConstIterator tmp = *this;
            --(*this);
            return tmp;
        }

        bool operator==(const ConstIterator& other) const { return ptr_ == other.ptr_; }
        bool operator!=(const ConstIterator& other) const { return ptr_ != other.ptr_; }

    private:
        const T* ptr_;
    };

    ConstIterator begin() const { return ConstIterator(head_); }
    ConstIterator end() const { return ConstIterator(nullptr); }
    ConstIterator cbegin() const { return ConstIterator(head_); }
    ConstIterator cend() const { return ConstIterator(nullptr); }

private:
    T* head_;
    T* tail_;
    size_t size_;
};

} // namespace Containers
} // namespace Core

#endif // CORE_CONTAINERS_LIST_H
