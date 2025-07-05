#include <memory>
#include <atomic>
#include <string>
#include <stdio.h>
#include <list>


// 选择单向链表来实现
// 使用的四种内存序的介绍
// relaxed 只保证自己的操作是原子的，但是不保证多线程中的顺序性
// release 发布序，只保证当前操作进行的时候，其他线程读取不到这个数据，只能读到发布之后的数据
	// 例如当A线程以release序对std::atomic<int> a进行操作时，别的线程能够以acquire序读取到当前修改之后的最新数据
	// 而是自身可能读取到旧的数据
// acquire 获取序，只保证当前操作进行的时候，读取到的数据是其他原子序操作完的，即保证读取的时候没有修改
	// 例如当A线程对acquire序对std::atomic<int> a进行操作时，别的线程可能读取不到最新的数据
	// 而是自身一定会读取到其他线程以release序发布的最新数据
// acq_rel 获取 发布 序，保证当前操作获取 和 发布的连贯性！
	// 自身一定会读取到其他线程以release序发布的最新数据
	// 别的线程能够以acquire序读取到当前修改之后的最新数据


// 进入队列时 读取当前的头尾以及期望的位置，作为后续compare的基础
// 若一致且满足条件即当前未有线程对其进行操作，可行
// 否则需要重新读取，再走一遍循环


template<typename T>
struct Node
{
	static_assert(std::is_trivially_destructible<T>::value,
		"T must be trivially destructible for this simple lock-free queue.");
	T data;
	std::atomic<Node<T>*> next;
	std::atomic<unsigned int>internal_ref_count;
	std::atomic<unsigned int>external_ref_count;

	Node(const T& value) {
		data = value;
		next = nullptr;
		internal_ref_count = 1;
		external_ref_count = 0;
	}

	Node() {
		next = nullptr;
		internal_ref_count = 0;
		external_ref_count = 0;
	}
};

template<typename T>
class LockFreeQueue {
public:
	LockFreeQueue();
	~LockFreeQueue();
	LockFreeQueue(const LockFreeQueue& lockfreequeue) = delete;
	LockFreeQueue& operator = (const LockFreeQueue&) = delete;
	void push(const T& value);
	bool pop(T& value);
private:
	std::atomic<Node<T>*> head_;
	std::atomic<Node<T>*> tail_;
private:
	void release_node_ref(Node<T>* node);
	void free_external_counter(Node<T>* node);
	Node<T>* claim_node(std::atomic<Node<T>*>& p_node);
	void release_internal_counter(Node<T>* node);
};

template<typename T>
LockFreeQueue<T>::LockFreeQueue() {
	Node<T>* dummy = new Node<T>{};
	dummy->internal_ref_count.store(1);
	head_.store(dummy);
	tail_.store(dummy);
}

template<typename T>
LockFreeQueue<T>::~LockFreeQueue() {
	T dummy_value;
	while (pop(dummy_value)) {
	}
	Node<T>* last_node = head_.load(std::memory_order_relaxed);
	delete last_node;
}

template<typename T>
void LockFreeQueue<T>::release_node_ref(Node<T>* node) {
	if (node == nullptr) {
		return;
	}
	// memory_order_acq_rel 能确保prev_ext_ref正常读取到数字吗？
	unsigned int prev_ext_ref = node->external_ref_count.fetch_sub(1,std::memory_order_acq_rel);

	if (prev_ext_ref == 1 && node->internal_ref_count.load(std::memory_order_relaxed) == 0) {
		delete node;
	}
}


template <typename T>
void LockFreeQueue<T>::release_internal_counter(Node<T>* node) {
	if (node == nullptr) return;
	// 内部引用计数减1
	if (node->internal_ref_count.fetch_sub(1, std::memory_order_release) == 1) {
		// 如果这是最后一个内部引用，我们还需要检查外部引用是否也为0
		if (node->external_ref_count.load(std::memory_order_acquire) == 0) {
			delete node;
		}
	}
}

template<typename T>
void LockFreeQueue<T>::free_external_counter(Node<T>* node) {
	if (node == nullptr) {
		return;
	}
	unsigned int prev_count = node->external_ref_count.fetch_sub(1,std::memory_order_release);
	if (prev_count == 1) {
		if (node->internal_ref_count.fetch_sub(1, std::memory_order_acquire) == 1) {
			delete node;
		}
	}

}

template <typename T>
Node<T>* LockFreeQueue<T>::claim_node(std::atomic<Node<T>*>& p_node) {
	Node<T>* node = p_node.load(std::memory_order_relaxed);
	if (node) {
		node->external_ref_count.fetch_add(1, std::memory_order_relaxed);
	}
	return node;
}

template<typename T>
void LockFreeQueue<T>::push(const T& value) {
	Node<T>* node = new Node<T>{};
	while (true) {
		Node<T>* last = tail_.load(std::memory_order_relaxed);
		Node<T>* next = last->next.load(std::memory_order_relaxed);
		// 此时tail_节点未被更新
		if (last == tail_.load(std::memory_order_relaxed)) {
			if (next == nullptr) {
				// next 为空
				// 如果当前的last_next为nullptr
				// 通过发布 的内存序实现exchange
				// 这里不是必须要使用获取序的，因为主要目的是为了发布这个新的节点，不关心当前获取的节点是否是要等到别人发布再去获取
				// 发布序 确保其他线程能读到最新的last->next
				// 如果当前的不是nullptr 那就relax 序来 释放当前操作给到下一轮
				if (last->next.compare_exchange_strong(next, node, std::memory_order_rel, std::memory_order_relaxed)) {
					// 完成了修改
					// 此处更新tail_
					// 获取序 确保读取tail_的时候是最新的 
					// 发布序 确保其他线程能读到最新的tail_
					// 如果当前的不是last 那就说明有另一个线程协助完成了修改，通过relax 序来释放当前操作即可
					node->internal_ref_count.fetch_add(1, std::memory_order_relaxed);
					tail_.compare_exange_strong(last, node, std::memory_order_acq_rel, std::memory_order_relaxed);
					release_internal_counter(node);
					return;
				}
			}
			else {
				// tail_.next 非空
				// 说明此时有另外一个线程一个完成了节点的添加
				// 此时帮助这个“另一个线程”完成对tail_的更新
				// 如果当前tail_仍旧未被修改，则使用获取，发布序
				// 获取序来保证读取到的tail_是最新的
				// 发布序来保证在修改后其他的线程能看到最新的tail_
				tail_.compare_exange_strong(last, next, std::memory_order_acq_rel, std::memory_order_relaxed);

			}
		}
	}
}

template<typename T>
bool LockFreeQueue<T>::pop(T& value) {
	while (true) {
		Node<T>* first = claim_node(head_);
		first->external_ref_count.fetch_add(1,std::memory_order_relaxed);
		Node<T>* next = first->next.load(std::memory_order_acquire);
		Node<T>* last = claim_node(tail_);
		if (first == last) {
			free_external_counter(last);
			if (next == nullptr) {
				free_external_counter(first);
				return false;
			}
			// next 非空 但是 head_ == tail_
			// 说明此时是tail_没有往后推进
			// 协助推进！
			tail_.compare_exchange_strong(last,next,std::memory_order_acq_rel,std::memory_order_relaxed);
			free_external_counter(first);
			continue;
		}
		
		if (next != nullptr) {
			// next非空 将head_弹出
			if (head_.compare_exchange_strong(first, next, std::memory_order_acq_rel, std::memory_order_release)) {
				result = next->data;
				free_external_counter(last);
				free_external_counter(first);
				return true;
			}	
		}
		free_external_counter(last);
		free_external_counter(first);
	}
}


