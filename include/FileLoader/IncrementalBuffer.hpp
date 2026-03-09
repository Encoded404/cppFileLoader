#ifndef INCREMENTAL_BUFFER_HPP
#define INCREMENTAL_BUFFER_HPP

#include <vector>
#include <span>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <cstddef>
#include <chrono>

namespace FileLoader
{
    using Byte = std::byte;
    using ByteBuffer = std::vector<Byte>;
    using ByteSpan = std::span<const Byte>;
    
    class IncrementalBuffer : public std::enable_shared_from_this<IncrementalBuffer> {
    public:
        IncrementalBuffer();
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
} // namespace FileLoader

#endif // INCREMENTAL_BUFFER_HPP