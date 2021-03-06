#pragma once
#include <utility>
#include <atomic>
#include <optional>
#include <climits>
#include <vector>
#include <algorithm>

struct ThreadEpoch {
	std::atomic_ullong epoch{ ULLONG_MAX };
	std::atomic<ThreadEpoch*> next{ nullptr };
};

static std::atomic_ullong g_epoch{ 0 };
static std::atomic<ThreadEpoch*> t_epoch_list;
static thread_local bool local_epoch_initialized = false;
static thread_local ThreadEpoch my_epoch;
static constexpr unsigned epoch_increase_rate = 50;
static constexpr unsigned empty_rate = 100;
static thread_local unsigned counter = 0;

static void initialize_thread_epoch() {
	if (local_epoch_initialized == true) { return; }
	auto list_head = t_epoch_list.load(std::memory_order_relaxed);
	while (true) {
		my_epoch.next.store(list_head, std::memory_order_relaxed);
		if (true == t_epoch_list.compare_exchange_strong(list_head, &my_epoch)) {
			break;
		}
	}
	local_epoch_initialized = true;
}

template <typename T>
struct QueueNode {
	QueueNode<T>() = default;
	QueueNode<T>(const T& val) : value{ val } {}
	QueueNode<T>(T&& val) : value{ std::move(val) } {}

	T value;
	std::atomic<QueueNode<T>*> next = nullptr;
};

template <typename T>
struct RetiredNode {
	RetiredNode<T>() = default;
	RetiredNode<T>(QueueNode<T>* node, unsigned long long epoch) : node{ node }, removed_epoch{ epoch } {}

	QueueNode<T>* node{ nullptr };
	unsigned long long removed_epoch;
};

template <typename T>
// Multi-Producer, Single-Consumer Queue
struct MPSCQueue {
private:
	QueueNode<T>* volatile head;
	std::atomic<QueueNode<T>*> tail;
	struct RetiredList {
		static std::vector<RetiredNode<T>>& get() {
			static thread_local std::vector<RetiredNode<T>> retired_list;
			return retired_list;
		}
	};

	void start_op();
	void end_op();
	void retire(QueueNode<T>* node);
	void empty();
	void inner_enq(QueueNode<T>& node);

public:
	MPSCQueue<T>() : head{ new QueueNode<T> }, tail{ head } {}

	std::optional<T> deq();
	void enq(const T& val);
	void enq(T&& val);
};

template<typename T>
inline void MPSCQueue<T>::start_op()
{
	initialize_thread_epoch();
	my_epoch.epoch.store(g_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

template<typename T>
inline void MPSCQueue<T>::end_op()
{
	my_epoch.epoch.store(ULLONG_MAX, std::memory_order_relaxed);
}

template<typename T>
inline void MPSCQueue<T>::retire(QueueNode<T>* node)
{
	RetiredList::get().emplace_back(node, g_epoch.load(std::memory_order_relaxed));
	counter++;
	if (counter % epoch_increase_rate == 0) { g_epoch.fetch_add(1, std::memory_order_relaxed); }
	if (counter % empty_rate == 0) {
		this->empty();
	}
}

template<typename T>
inline void MPSCQueue<T>::empty()
{
	unsigned long long min_epoch = ULLONG_MAX;
	auto thread_epoch = t_epoch_list.load(std::memory_order_relaxed);
	while (true) {
		auto epoch = thread_epoch->epoch.load(std::memory_order_relaxed);
		if (min_epoch > epoch) { min_epoch = epoch; }

		auto next_t_epoch = thread_epoch->next.load(std::memory_order_relaxed);
		if (next_t_epoch == nullptr) { break; }
		thread_epoch = next_t_epoch;
	}

	auto& retired_list = RetiredList::get();
	auto removed_it = std::remove_if(retired_list.begin(), retired_list.end(), [min_epoch](RetiredNode<T>& r_node) {
		if (r_node.removed_epoch < min_epoch) {
			delete r_node.node;
			return true;
		}
		return false;
		});
	retired_list.erase(removed_it, retired_list.end());
}

template<typename T>
inline void MPSCQueue<T>::inner_enq(QueueNode<T>& new_node)
{
	start_op();
	QueueNode<T>* old_tail;
	while (true) {
		old_tail = tail.load(std::memory_order_acquire);
		auto old_next = old_tail->next.load(std::memory_order_acquire);
		if (old_next != nullptr) {
			tail.compare_exchange_strong(old_tail, old_next);
			continue;
		}

		if (true == old_tail->next.compare_exchange_strong(old_next, &new_node)) {
			tail.compare_exchange_strong(old_tail, &new_node);
			break;
		}
	}
	end_op();
}

template<typename T>
inline std::optional<T> MPSCQueue<T>::deq()
{
	start_op();
	std::optional<T> retval;
	QueueNode<T>* next_head = head->next.load(std::memory_order_acquire);

	if (next_head != nullptr) {
		auto old_head = head;
		head = next_head;
		retire(old_head);
		retval = std::move(next_head->value);
	}

	end_op();
	return retval;
}

template<typename T>
inline void MPSCQueue<T>::enq(const T& val)
{
	this->inner_enq(*new QueueNode<T>{ val });
}

template<typename T>
inline void MPSCQueue<T>::enq(T&& val)
{
	this->inner_enq(*new QueueNode<T>{ std::move(val) });
}
