#pragma once
#include <algorithm>
#include <atomic>
#include <climits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

struct ThreadEpoch {
    std::atomic_ullong epoch{ULLONG_MAX};
    std::atomic<ThreadEpoch *> next{nullptr};
};

static std::atomic_ullong g_epoch{0};
static std::vector<ThreadEpoch *> t_epoch_list{64, nullptr};
static std::atomic_uint thread_num{0};
static thread_local bool local_epoch_initialized = false;
static thread_local ThreadEpoch my_epoch;
static constexpr unsigned epoch_increase_rate = 50;
static constexpr unsigned empty_rate = 100;
static thread_local unsigned counter = 0;

static void initialize_thread_epoch() {
    if (local_epoch_initialized == true) {
        return;
    }
    t_epoch_list[thread_num.fetch_add(1, std::memory_order_relaxed)] =
        &my_epoch;
    local_epoch_initialized = true;
}

template <class T> struct QueueNode {
    template <typename... Param>
    QueueNode<T>(Param &&... args) : value{std::forward<Param>(args)...} {}

    T value;
    std::atomic<QueueNode<T> *> next = nullptr;
};

template <typename T> struct RetiredNode {
    RetiredNode<T>() = default;
    RetiredNode<T>(QueueNode<T> *node, unsigned long long epoch)
        : node{node}, removed_epoch{epoch} {}

    QueueNode<T> *node{nullptr};
    unsigned long long removed_epoch;
};

template <typename T>
// Multi-Producer, Single-Consumer Queue
struct MPSCQueue {
  private:
    QueueNode<T> *volatile head;
    std::atomic<QueueNode<T> *> tail;
    static thread_local std::vector<RetiredNode<T>> retired_list;
    std::atomic_uint64_t num_node;

    void start_op() const;
    void end_op() const;
    void retire(QueueNode<T> *node);
    void empty();
    void inner_enq(QueueNode<T> &node);

  public:
    MPSCQueue<T>() : head{new QueueNode<T>}, tail{head}, num_node{0} {}
    ~MPSCQueue<T>() {
        if (head != nullptr) {
            while (head != tail) {
                auto old_head = head;
                head = head->next.load(std::memory_order_relaxed);
                delete old_head;
            }
        }
    }
    MPSCQueue<T>(const MPSCQueue<T> &) = delete;
    MPSCQueue<T>(MPSCQueue<T> &&) = delete;

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

template <typename T> inline void MPSCQueue<T>::start_op() const {
    initialize_thread_epoch();
    my_epoch.epoch.store(g_epoch.load(std::memory_order_acquire),
                         std::memory_order_release);
}

template <typename T> inline void MPSCQueue<T>::end_op() const {
    my_epoch.epoch.store(ULLONG_MAX, std::memory_order_release);
}

template <typename T> inline void MPSCQueue<T>::retire(QueueNode<T> *node) {
    retired_list.emplace_back(node, g_epoch.load(std::memory_order_acquire));
    counter++;
    if (counter % epoch_increase_rate == 0) {
        g_epoch.fetch_add(1, std::memory_order_release);
    }
    if (counter % empty_rate == 0) {
        this->empty();
    }
}

template <typename T> inline void MPSCQueue<T>::empty() {
    unsigned long long min_epoch = ULLONG_MAX;
    for (auto i = 0; i < thread_num.load(std::memory_order_relaxed); ++i) {
		auto thread_epoch = t_epoch_list[i];
        auto epoch = thread_epoch->epoch.load(std::memory_order_acquire);
        if (epoch < min_epoch) {
            min_epoch = epoch;
        }
    }

    auto removed_it = std::remove_if(retired_list.begin(), retired_list.end(),
                                     [min_epoch](RetiredNode<T> &r_node) {
                                         if (r_node.removed_epoch < min_epoch) {
                                             delete r_node.node;
                                             return true;
                                         }
                                         return false;
                                     });
    retired_list.erase(removed_it, retired_list.end());
}

template <typename T>
inline void MPSCQueue<T>::inner_enq(QueueNode<T> &new_node) {
    start_op();
    QueueNode<T> *old_tail;
    while (true) {
        old_tail = tail.load(std::memory_order_relaxed);
        auto old_next = old_tail->next.load(std::memory_order_relaxed);
        if (old_next != nullptr) {
            tail.compare_exchange_strong(old_tail, old_next);
            continue;
        }

        if (true ==
            old_tail->next.compare_exchange_strong(old_next, &new_node)) {
            tail.compare_exchange_strong(old_tail, &new_node);
            break;
        }
    }
    num_node.fetch_add(1, std::memory_order_release);
    end_op();
}

template <typename T> inline std::optional<T> MPSCQueue<T>::deq() {
    std::optional<T> retval;
    QueueNode<T> *next_head = head->next.load(std::memory_order_relaxed);

    if (next_head != nullptr) {
        auto old_head = head;
        head = next_head;
        retire(old_head);
        retval.emplace(std::move(next_head->value));
        num_node.fetch_sub(1, std::memory_order_release);
    }

    return retval;
}

template <typename T> inline void MPSCQueue<T>::enq(const T &val) {
    this->inner_enq(*new QueueNode<T>{val});
}

template <typename T> inline void MPSCQueue<T>::enq(T &&val) {
    this->inner_enq(*new QueueNode<T>{std::move(val)});
}

template <typename T> inline const T &MPSCQueue<T>::peek() const {
    QueueNode<T> *old_next = head->next.load(std::memory_order_relaxed);
    if (old_next == nullptr)
        throw std::runtime_error("the MPSCQueue has been empty");
    return old_next->value;
}

template <typename T> inline T &MPSCQueue<T>::peek() {
    QueueNode<T> *old_next = head->next.load(std::memory_order_relaxed);
    if (old_next == nullptr)
        throw std::runtime_error("the MPSCQueue has been empty");
    return old_next->value;
}

template <typename T>
template <typename... Param>
inline void MPSCQueue<T>::emplace(Param &&... args) {
    this->inner_enq(*new QueueNode<T>{std::forward<Param>(args)...});
}

template <typename T>
thread_local std::vector<RetiredNode<T>> MPSCQueue<T>::retired_list;
