#pragma once
#include <vector>
#include <mutex>
#include <cstdint>
#include <cstddef>

// Fixed-capacity ring. Producer = ETW consumer thread, consumer = UI thread.
// When full, the OLDEST record is overwritten and `overwritten()` counts it.
// The detail log is allowed to lose records; the aggregated per-folder
// statistics are updated on the producer side and are never lost.
template <class T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : buf_(capacity) {}

    void push(const T& v) {
        std::lock_guard<std::mutex> lk(m_);
        buf_[head_] = v;
        head_ = (head_ + 1) % buf_.size();
        if (size_ == buf_.size()) {
            tail_ = (tail_ + 1) % buf_.size();
            ++overwritten_;
        } else {
            ++size_;
        }
    }

    // Moves up to `max` oldest records into `out`. Returns how many.
    size_t drain(std::vector<T>& out, size_t max) {
        std::lock_guard<std::mutex> lk(m_);
        size_t n = size_ < max ? size_ : max;
        out.clear();
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            out.push_back(buf_[tail_]);
            tail_ = (tail_ + 1) % buf_.size();
        }
        size_ -= n;
        return n;
    }

    size_t size() const { std::lock_guard<std::mutex> lk(m_); return size_; }
    size_t capacity() const { return buf_.size(); }
    uint64_t overwritten() const { std::lock_guard<std::mutex> lk(m_); return overwritten_; }

private:
    mutable std::mutex m_;
    std::vector<T> buf_;
    size_t head_ = 0, tail_ = 0, size_ = 0;
    uint64_t overwritten_ = 0;
};
