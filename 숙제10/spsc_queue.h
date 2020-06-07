#pragma once
#include <algorithm>
#include <atomic>
#include <climits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>


template <class T> struct QueueNode {
    template <typename... Param>
    QueueNode<T>(Param &&... args) : value{std::forward<Param>(args)...} {}

    T value;
    std::atomic<QueueNode<T> *> next = nullptr;
};

template <typename T>
// Single-Producer, Single-Consumer Queue
struct SPSCQueue {
  private:
    QueueNode<T> *volatile head;
    QueueNode<T> *volatile tail;
    std::atomic_uint64_t num_node;

    void inner_enq(QueueNode<T> &node);

  public:
    SPSCQueue<T>() : head{new QueueNode<T>}, tail{head}, num_node{0} {}
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
    template <typename... Param> void emplace(Param &&... args);
    bool is_empty() const { return head->next.load() == nullptr; }
    const T &peek() const;
    T &peek();
    uint64_t size() const { return num_node.load(std::memory_order_acquire); }
};

template <typename T>
inline void SPSCQueue<T>::inner_enq(QueueNode<T> &new_node) {
    QueueNode<T> *old_tail = tail;
    tail = &new_node;
    old_tail->next.store(&new_node, std::memory_order_release);
    num_node.fetch_add(1, std::memory_order_release);
}

template <typename T> inline std::optional<T> SPSCQueue<T>::deq() {
    std::optional<T> retval;
    QueueNode<T> *next_head = head->next.load(std::memory_order_acquire);

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
    this->inner_enq(*new QueueNode<T>{val});
}

template <typename T> inline void SPSCQueue<T>::enq(T &&val) {
    this->inner_enq(*new QueueNode<T>{std::move(val)});
}

template <typename T> inline const T &SPSCQueue<T>::peek() const {
    QueueNode<T> *old_next = head->next.load(std::memory_order_relaxed);
    if (old_next == nullptr)
        throw std::runtime_error("the SPSCQueue has been empty");
    return old_next->value;
}

template <typename T> inline T &SPSCQueue<T>::peek() {
    QueueNode<T> *old_next = head->next.load(std::memory_order_relaxed);
    if (old_next == nullptr)
        throw std::runtime_error("the SPSCQueue has been empty");
    return old_next->value;
}

template <typename T>
template <typename... Param>
inline void SPSCQueue<T>::emplace(Param &&... args) {
    this->inner_enq(*new QueueNode<T>{std::forward<Param>(args)...});
}