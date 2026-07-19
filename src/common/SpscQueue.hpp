#pragma once
// ============================================================================
// Lock-free single-producer / single-consumer ring buffer.
//
// Threading contract of the whole system:
//   DDS listener threads  --push()-->  SpscQueue  --pop()-->  render thread
// The render thread NEVER blocks on DDS and DDS threads NEVER touch ImGui.
//
// T must be trivially copyable. Capacity is rounded up to a power of two.
// ============================================================================

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>

namespace radar {

template <typename T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscQueue payload must be trivially copyable");
public:
    explicit SpscQueue(std::size_t capacity_pow2)
        : mask_(next_pow2(capacity_pow2) - 1),
          buffer_(std::make_unique<T[]>(mask_ + 1)) {}

    // Producer side (DDS listener thread). Returns false when full; the
    // caller decides to drop or overwrite. overwrite_latest() never fails.
    bool push(const T& item) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire))
            return false; // full
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Like push, but never blocks. NOTE: on a full queue the NEW item is
    // dropped (never the oldest) because only the consumer may touch the
    // tail index - dropping the oldest from the producer side would race
    // the consumer's pop().
    void push_overwrite(const T& item) {
        (void)push(item); // full -> newest display sample dropped, by design
    }

    // Consumer side (render thread).
    bool pop(T& out) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false; // empty
        out = buffer_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

    template <typename Fn>
    std::size_t drain(Fn&& fn) {
        std::size_t n = 0;
        T item;
        while (pop(item)) { fn(item); ++n; }
        return n;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    static std::size_t next_pow2(std::size_t v) {
        std::size_t p = 64;
        while (p < v) p <<= 1;
        return p;
    }

    const std::size_t mask_;
    std::unique_ptr<T[]> buffer_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace radar
