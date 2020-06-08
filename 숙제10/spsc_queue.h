#pragma once
#include <algorithm>
#include <atomic>
#include <climits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

template <typename T>
// Single-Producer, Single-Consumer Queue
struct SPSCQueue {
    struct QueueNode {
        template <typename... Param>
        QueueNode(Param &&... args) : value{std::forward<Param>(args)...} {}

        T value;
        std::atomic<QueueNode *> next = nullptr;
    };

  private:
    QueueNode *volatile head;
    QueueNode *volatile tail;
    std::atomic_uint64_t num_node;

    void inner_enq(QueueNode &node);

  public:
    SPSCQueue<T>() : head{new QueueNode}, tail{head}, num_node{0} {}
    ~SPSCQueue<T>() {
        if (head != nullptr) {
            while (head != tail) {
                auto old_head = head;
                head = head->next.load(std::memory_order_relaxed);
                delete old_head;
            }
        }
    }
    SPSCQueue<T>(const SPSCQueue<T> &) = delete;
    SPSCQueue<T>(SPSCQueue<T> &&) = delete;

    std::optional<T> deq();
    void enq(const T &val);
    void enq(T &&val);
    template <typename F> void for_each(F &&func) {
        while (true) {
            auto el = this->deq();
            if (!el)
                return;
            func(std::move(*el));
        }
    }
    template <typename... Param> void emplace(Param &&... args);
    bool is_empty() const { return head->next.load() == nullptr; }
    const T &peek() const;
    T &peek();
    uint64_t size() const { return num_node.load(std::memory_order_acquire); }
};

template <typename T>
inline void SPSCQueue<T>::inner_enq(QueueNode &new_node) {
    QueueNode *old_tail = tail;
    tail = &new_node;
    old_tail->next.store(&new_node, std::memory_order_release);
    num_node.fetch_add(1, std::memory_order_release);
}

template <typename T> inline std::optional<T> SPSCQueue<T>::deq() {
    std::optional<T> retval;
    QueueNode *next_head = head->next.load(std::memory_order_acquire);

    if (next_head != nullptr) {
        auto old_head = head;
        head = next_head;
        delete old_head;
        retval.emplace(std::move(next_head->value));
        num_node.fetch_sub(1, std::memory_order_release);
    }

    return retval;
}

template <typename T> inline void SPSCQueue<T>::enq(const T &val) {
    this->inner_enq(*new QueueNode{val});
}

template <typename T> inline void SPSCQueue<T>::enq(T &&val) {
    this->inner_enq(*new QueueNode{std::move(val)});
}

template <typename T> inline const T &SPSCQueue<T>::peek() const {
    QueueNode *old_next = head->next.load(std::memory_order_relaxed);
    if (old_next == nullptr)
        throw std::runtime_error("the SPSCQueue has been empty");
    return old_next->value;
}

template <typename T> inline T &SPSCQueue<T>::peek() {
    QueueNode *old_next = head->next.load(std::memory_order_relaxed);
    if (old_next == nullptr)
        throw std::runtime_error("the SPSCQueue has been empty");
    return old_next->value;
}

template <typename T>
template <typename... Param>
inline void SPSCQueue<T>::emplace(Param &&... args) {
    this->inner_enq(*new QueueNode{std::forward<Param>(args)...});
}