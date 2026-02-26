#ifndef CORE_CONTAINERS_BINTREE_AVL_H
#define CORE_CONTAINERS_BINTREE_AVL_H

#include <cstddef>
#include <algorithm>
#include <functional>
#include <utility>

namespace Core {
namespace Containers {

/**
 * AVL Tree for deterministic sorted price levels
 *
 * Self-balancing binary search tree ensuring O(log n) operations.
 * Used in order book to maintain sorted price levels (best bid/ask tracking).
 *
 * Key properties:
 * - Deterministic ordering (strict weak ordering via Compare)
 * - Balanced tree (height difference ≤ 1)
 * - No duplicate keys
 */

template <typename Key, typename Value, typename Compare = std::less<Key>>
class BinTreeAVL {
private:
    struct Node {
        Key key;
        Value value;
        Node* left;
        Node* right;
        Node* parent;
        int height;

        Node(const Key& k, const Value& v)
            : key(k), value(v), left(nullptr), right(nullptr), parent(nullptr), height(1) {}

        Node(const Key& k, Value&& v)
            : key(k), value(std::move(v)), left(nullptr), right(nullptr), parent(nullptr), height(1) {}
    };

public:
    BinTreeAVL() : root_(nullptr), size_(0), compare_() {}

    ~BinTreeAVL() {
        clear();
    }

    // Non-copyable (for now - can implement if needed)
    BinTreeAVL(const BinTreeAVL&) = delete;
    BinTreeAVL& operator=(const BinTreeAVL&) = delete;

    // Capacity
    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }

    // Modifiers
    bool insert(const Key& key, const Value& value) {
        bool inserted = false;
        root_ = insert_internal(root_, nullptr, key, value, inserted);
        if (inserted) ++size_;
        return inserted;
    }

    bool insert(const Key& key, Value&& value) {
        bool inserted = false;
        root_ = insert_internal_move(root_, nullptr, key, std::move(value), inserted);
        if (inserted) ++size_;
        return inserted;
    }

    bool erase(const Key& key) {
        bool erased = false;
        root_ = erase_internal(root_, key, erased);
        if (erased) --size_;
        return erased;
    }

    void clear() {
        clear_internal(root_);
        root_ = nullptr;
        size_ = 0;
    }

    // Lookup
    Value* find(const Key& key) {
        Node* node = find_internal(root_, key);
        return node ? &node->value : nullptr;
    }

    const Value* find(const Key& key) const {
        Node* node = find_internal(root_, key);
        return node ? &node->value : nullptr;
    }

    // Min/Max (for best bid/ask)
    Value* find_min() {
        Node* node = find_min_internal(root_);
        return node ? &node->value : nullptr;
    }

    const Value* find_min() const {
        Node* node = find_min_internal(root_);
        return node ? &node->value : nullptr;
    }

    Value* find_max() {
        Node* node = find_max_internal(root_);
        return node ? &node->value : nullptr;
    }

    const Value* find_max() const {
        Node* node = find_max_internal(root_);
        return node ? &node->value : nullptr;
    }

    const Key* find_min_key() const {
        Node* node = find_min_internal(root_);
        return node ? &node->key : nullptr;
    }

    const Key* find_max_key() const {
        Node* node = find_max_internal(root_);
        return node ? &node->key : nullptr;
    }

    // In-order traversal (for testing/debugging)
    template <typename Func>
    void traverse_inorder(Func func) const {
        traverse_inorder_internal(root_, func);
    }

private:
    Node* root_;
    size_t size_;
    Compare compare_;

    // Helper: get height of node
    int height(Node* node) const {
        return node ? node->height : 0;
    }

    // Helper: update height of node
    void update_height(Node* node) {
        if (node) {
            node->height = 1 + std::max(height(node->left), height(node->right));
        }
    }

    // Helper: get balance factor
    int balance_factor(Node* node) const {
        return node ? height(node->left) - height(node->right) : 0;
    }

    // Rotation: right rotate
    Node* rotate_right(Node* y) {
        Node* x = y->left;
        Node* T2 = x->right;

        // Perform rotation
        x->right = y;
        y->left = T2;

        // Update parents
        x->parent = y->parent;
        y->parent = x;
        if (T2) T2->parent = y;

        // Update heights
        update_height(y);
        update_height(x);

        return x;
    }

    // Rotation: left rotate
    Node* rotate_left(Node* x) {
        Node* y = x->right;
        Node* T2 = y->left;

        // Perform rotation
        y->left = x;
        x->right = T2;

        // Update parents
        y->parent = x->parent;
        x->parent = y;
        if (T2) T2->parent = x;

        // Update heights
        update_height(x);
        update_height(y);

        return y;
    }

    // Insert and balance
    Node* insert_internal(Node* node, Node* parent, const Key& key, const Value& value, bool& inserted) {
        // Base case: found insertion point
        if (!node) {
            inserted = true;
            Node* new_node = new Node(key, value);
            new_node->parent = parent;
            return new_node;
        }

        // Recursive insert
        if (compare_(key, node->key)) {
            node->left = insert_internal(node->left, node, key, value, inserted);
        } else if (compare_(node->key, key)) {
            node->right = insert_internal(node->right, node, key, value, inserted);
        } else {
            // Key already exists
            inserted = false;
            return node;
        }

        // Update height
        update_height(node);

        // Balance the tree
        return balance(node);
    }

    // Insert with move semantics
    Node* insert_internal_move(Node* node, Node* parent, const Key& key, Value&& value, bool& inserted) {
        // Base case: found insertion point
        if (!node) {
            inserted = true;
            Node* new_node = new Node(key, std::move(value));
            new_node->parent = parent;
            return new_node;
        }

        // Recursive insert
        if (compare_(key, node->key)) {
            node->left = insert_internal_move(node->left, node, key, std::move(value), inserted);
        } else if (compare_(node->key, key)) {
            node->right = insert_internal_move(node->right, node, key, std::move(value), inserted);
        } else {
            // Key already exists
            inserted = false;
            return node;
        }

        // Update height
        update_height(node);

        // Balance the tree
        return balance(node);
    }

    // Erase and balance
    Node* erase_internal(Node* node, const Key& key, bool& erased) {
        if (!node) {
            erased = false;
            return nullptr;
        }

        // Recursive search
        if (compare_(key, node->key)) {
            node->left = erase_internal(node->left, key, erased);
        } else if (compare_(node->key, key)) {
            node->right = erase_internal(node->right, key, erased);
        } else {
            // Found node to delete
            erased = true;

            // Case 1: No children or one child
            if (!node->left || !node->right) {
                Node* child = node->left ? node->left : node->right;
                if (child) child->parent = node->parent;
                delete node;
                return child;
            }

            // Case 2: Two children - replace with in-order successor
            Node* successor = find_min_internal(node->right);
            node->key = successor->key;
            node->value = std::move(successor->value);
            node->right = erase_internal(node->right, successor->key, erased);
            erased = true; // Already marked as erased
        }

        if (!node) return nullptr;

        // Update height
        update_height(node);

        // Balance the tree
        return balance(node);
    }

    // Balance node
    Node* balance(Node* node) {
        int bf = balance_factor(node);

        // Left heavy
        if (bf > 1) {
            // Left-Right case
            if (balance_factor(node->left) < 0) {
                node->left = rotate_left(node->left);
            }
            // Left-Left case
            return rotate_right(node);
        }

        // Right heavy
        if (bf < -1) {
            // Right-Left case
            if (balance_factor(node->right) > 0) {
                node->right = rotate_right(node->right);
            }
            // Right-Right case
            return rotate_left(node);
        }

        return node;
    }

    // Find node
    Node* find_internal(Node* node, const Key& key) const {
        if (!node) return nullptr;

        if (compare_(key, node->key)) {
            return find_internal(node->left, key);
        } else if (compare_(node->key, key)) {
            return find_internal(node->right, key);
        } else {
            return node;
        }
    }

    // Find min
    Node* find_min_internal(Node* node) const {
        while (node && node->left) {
            node = node->left;
        }
        return node;
    }

    // Find max
    Node* find_max_internal(Node* node) const {
        while (node && node->right) {
            node = node->right;
        }
        return node;
    }

    // Clear tree
    void clear_internal(Node* node) {
        if (!node) return;
        clear_internal(node->left);
        clear_internal(node->right);
        delete node;
    }

    // In-order traversal
    template <typename Func>
    void traverse_inorder_internal(Node* node, Func func) const {
        if (!node) return;
        traverse_inorder_internal(node->left, func);
        func(node->key, node->value);
        traverse_inorder_internal(node->right, func);
    }
};

} // namespace Containers
} // namespace Core

#endif // CORE_CONTAINERS_BINTREE_AVL_H
