module;

#include <logging/logging_macros.hpp>

#ifdef CPPFILELOADER_USE_STD_MODULE
#else
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <optional>
#include <filesystem>
#include <memory>
#include <fstream>
#include <future>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <system_error>
#include <stdexcept>
#endif

#ifndef CPPFILELOADER_USE_LOGIFACE_MODULE
#include <logging/logging.hpp>
#endif

export module FileLoader.IOQueue;

#ifdef CPPFILELOADER_USE_STD_MODULE
import std;
#endif

#ifdef CPPFILELOADER_USE_LOGIFACE_MODULE
import logiface;
#endif

import FileLoader.Types;
import FileLoader.IFileReadStrategy;
import FileLoader.IncrementalBuffer;

export namespace FileLoader
{

struct IORequest
{
    enum class Type { Full, Streaming };

    Type type;
    std::filesystem::path path;
    int priority = 0;
    std::chrono::steady_clock::time_point ready_at = std::chrono::steady_clock::time_point::min();
    std::uint64_t bytes_read = 0;
    std::uint64_t size_hint = 0;

    CancellationToken cancel;
    ReadRateControl rate;

    // Full mode state
    std::promise<std::shared_ptr<ByteBuffer>> full_promise;
    std::shared_ptr<ByteBuffer> partial_buffer;

    // Streaming mode state
    std::promise<void> stream_promise;
    std::shared_ptr<IncrementalBuffer> stream_output;

    // I/O state — stream stays open between rate-limited re-queues
    std::unique_ptr<std::ifstream> ifs;
};

class IOQueue
{
public:
    IOQueue() = default;
    ~IOQueue();

    IOQueue(const IOQueue&) = delete;
    IOQueue& operator=(const IOQueue&) = delete;
    IOQueue(IOQueue&&) = delete;
    IOQueue& operator=(IOQueue&&) = delete;

    void Push(IORequest request);

    // Returns std::nullopt if Stop() was called and queue is drained.
    std::optional<IORequest> Pop();

    void Stop();

    std::size_t Size() const;
    bool IsEmpty() const;

    // Discard a cancelled request — resolves its promise with partial/empty data.
    static void DiscardCancelled(IORequest&& req);

private:
    struct Compare
    {
        bool operator()(const IORequest& a, const IORequest& b) const noexcept;
    };

    mutable std::mutex mutex_;
    std::deque<IORequest> heap_;
    std::condition_variable cv_;
    std::atomic<bool> stopped_{false};
};

// --- Implementations ---

IOQueue::~IOQueue()
{
    Stop();
}

void IOQueue::Push(IORequest request)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        heap_.push_back(std::move(request));
        std::push_heap(heap_.begin(), heap_.end(), Compare{});
    }
    cv_.notify_one();
}

std::optional<IORequest> IOQueue::Pop()
{
    std::unique_lock<std::mutex> lock(mutex_);

    while (!stopped_.load(std::memory_order_acquire))
    {
        if (heap_.empty())
        {
            cv_.wait(lock);
            continue;
        }

        IORequest& front = heap_.front();

        // Discard cancelled requests without ever touching the file
        if (front.cancel.cancelled &&
            front.cancel.cancelled->load(std::memory_order_acquire))
        {
            std::pop_heap(heap_.begin(), heap_.end(), Compare{});
            IORequest discarded = std::move(heap_.back());
            heap_.pop_back();
            DiscardCancelled(std::move(discarded));
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (front.ready_at <= now)
        {
            std::pop_heap(heap_.begin(), heap_.end(), Compare{});
            IORequest req = std::move(heap_.back());
            heap_.pop_back();
            return req;
        }

        // All items at the front are delayed — block until earliest becomes ready
        auto delay = front.ready_at - now;
        if (delay > std::chrono::steady_clock::duration::zero())
            cv_.wait_for(lock, delay);
        else
            cv_.wait_for(lock, std::chrono::milliseconds(0));
    }

    return std::nullopt;
}

void IOQueue::Stop()
{
    stopped_.store(true, std::memory_order_release);
    cv_.notify_all();
}

std::size_t IOQueue::Size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return heap_.size();
}

bool IOQueue::IsEmpty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return heap_.empty();
}

bool IOQueue::Compare::operator()(const IORequest& a, const IORequest& b) const noexcept
{
    // Higher priority first; ties broken by earlier ready_at
    if (a.priority != b.priority)
        return a.priority < b.priority;
    return a.ready_at > b.ready_at;
}

void IOQueue::DiscardCancelled(IORequest&& req)
{
    try
    {
        if (req.type == IORequest::Type::Full)
        {
            if (req.partial_buffer)
                req.full_promise.set_value(std::move(req.partial_buffer));
            else
                req.full_promise.set_value(std::make_shared<ByteBuffer>());
        }
        else
        {
            if (req.stream_output)
                req.stream_output->Close();
            req.stream_promise.set_value();
        }
    }
    catch (const std::future_error& e)
    {
        LOGIFACE_LOG(warn, std::string("IOQueue: DiscardCancelled hit already-satisfied promise: ") + e.what());
    }
}

} // namespace FileLoader
