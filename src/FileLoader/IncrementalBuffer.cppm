module;

#ifdef CPPFILELOADER_USE_STD_MODULE
// GMF is intentionally empty — import std; in module purview
#else
#include <vector>
#include <span>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <iterator>
#include <utility>
#include <algorithm>
#endif

export module FileLoader.IncrementalBuffer;

#ifdef CPPFILELOADER_USE_STD_MODULE
import std;
#endif

import FileLoader.Types;

export namespace FileLoader
{
    class IncrementalBuffer : public std::enable_shared_from_this<IncrementalBuffer> {
    public:
        IncrementalBuffer() = default;
        ~IncrementalBuffer();

        // push data
        bool Push(const ByteBuffer& chunk);
        bool Push(ByteBuffer&& chunk);

        // register on-push callback
        void SetOnPush(std::function<void(std::size_t)> cb);

        // read data
        ByteSpan ReadSnapshot() const;
        ByteSpan ReadRange(std::size_t offset, std::size_t len) const;
        std::size_t Size() const;

        // wait for at least target_size bytes (or closed/cancelled)
        bool WaitForSize(std::size_t target_size, std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

        // control
        void Close();
        void Cancel();
        bool IsClosed() const;
        bool IsCancelled() const;

    private:
        mutable std::mutex mutex_;
        ByteBuffer buffer_;
        std::atomic<std::size_t> size_{0};
        bool closed_ {false};
        std::atomic<bool> cancelled_ {false};

        mutable std::mutex cv_mutex_;
        std::condition_variable cv_;

        std::function<void(std::size_t)> on_push_;
        std::mutex cb_mutex_;
    };

    // --- Implementations ---

    IncrementalBuffer::~IncrementalBuffer() = default;

    bool IncrementalBuffer::Push(const ByteBuffer& chunk)
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || cancelled_) return false;
        buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
        size_.store(buffer_.size(), std::memory_order_release);
        cv_.notify_all();

        std::function<void(std::size_t)> cb_copy;
        { const std::lock_guard lock_cb(cb_mutex_); cb_copy = on_push_; }
        if (cb_copy) cb_copy(size_.load());
        return true;
    }

    bool IncrementalBuffer::Push(ByteBuffer&& chunk)
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || cancelled_) return false;
        buffer_.insert(buffer_.end(),
                       std::make_move_iterator(chunk.begin()),
                       std::make_move_iterator(chunk.end()));
        size_.store(buffer_.size(), std::memory_order_release);
        cv_.notify_all();

        std::function<void(std::size_t)> cb_copy;
        { const std::lock_guard lock_cb(cb_mutex_); cb_copy = on_push_; }
        if (cb_copy) cb_copy(size_.load());
        return true;
    }

    void IncrementalBuffer::SetOnPush(std::function<void(std::size_t)> cb) {
        const std::lock_guard lock(cb_mutex_);
        on_push_ = std::move(cb);
    }

    ByteSpan IncrementalBuffer::ReadSnapshot() const {
        const std::lock_guard lock(mutex_);
        return ByteSpan(buffer_.data(), buffer_.size());
    }

    ByteSpan IncrementalBuffer::ReadRange(std::size_t offset, std::size_t len) const {
        const std::lock_guard lock(mutex_);
        if (offset >= buffer_.size()) return {};
        const std::size_t count = std::min(len, buffer_.size() - offset);
        return ByteSpan(buffer_.data() + offset, count);
    }

    std::size_t IncrementalBuffer::Size() const {
        return size_.load(std::memory_order_acquire);
    }

    bool IncrementalBuffer::WaitForSize(std::size_t target_size, std::chrono::milliseconds timeout) {
        std::unique_lock lock(cv_mutex_);
        auto pred = [&] { return cancelled_.load() || size_.load() >= target_size || closed_; };
        if (timeout == std::chrono::milliseconds::max()) cv_.wait(lock, pred);
        else if (!cv_.wait_for(lock, timeout, pred)) return false;
        return size_.load() >= target_size;
    }

    void IncrementalBuffer::Close() {
        {
            const std::lock_guard lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    void IncrementalBuffer::Cancel() {
        cancelled_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

    bool IncrementalBuffer::IsClosed() const {
        const std::lock_guard lock(mutex_);
        return closed_;
    }

    bool IncrementalBuffer::IsCancelled() const {
        const std::lock_guard lock(mutex_);
        return cancelled_.load();
    }
} // namespace FileLoader
