#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace anc {

// Lock-free single-producer / single-consumer ring buffer.
//
// Capacity is rounded up to a power of two. Indices are monotonically
// increasing 64-bit counters; only the low bits index the storage. The
// producer must only ever call write(), the consumer only read(); the
// approximate readers (availableToRead/availableToWrite) may be called
// from a monitoring thread for metrics purposes.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padding is expected: cache-line separated indices
#endif
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t minCapacity) {
        size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        buf_.resize(cap);
        mask_ = cap - 1;
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    size_t capacity() const { return mask_ + 1; }

    // Producer side. Writes up to `count` elements, returns written count.
    size_t write(const T* src, size_t count) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t free = capacity() - (head - tail);
        const size_t n = count < free ? count : free;
        for (size_t i = 0; i < n; ++i) buf_[(head + i) & mask_] = src[i];
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // Consumer side. Reads up to `count` elements, returns read count.
    size_t read(T* dst, size_t count) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t avail = head - tail;
        const size_t n = count < avail ? count : avail;
        for (size_t i = 0; i < n; ++i) dst[i] = buf_[(tail + i) & mask_];
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    // Approximate; safe to call from a monitoring thread.
    size_t availableToRead() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    size_t availableToWrite() const { return capacity() - availableToRead(); }

    // Total elements ever written (monotonic producer counter).
    size_t written() const { return head_.load(std::memory_order_acquire); }

    // Total elements ever read (monotonic consumer counter).
    size_t readTotal() const { return tail_.load(std::memory_order_acquire); }

private:
    std::vector<T> buf_;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace anc
