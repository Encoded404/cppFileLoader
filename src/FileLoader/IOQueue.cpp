#include "FileLoader/IOQueue.hpp"
#include <logging/logging.hpp>

#include <algorithm>
#include <system_error>
#include <future>

namespace FileLoader
{

IOQueue::~IOQueue()
{
    Stop();
}

void IOQueue::Push(IORequest request)
{
    {
        const std::scoped_lock lock(mutex_);
        heap_.push_back(std::move(request));
        std::ranges::push_heap(heap_, Compare{});
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

        const IORequest& front = heap_.front();

        // Discard cancelled requests without ever touching the file
        if (front.cancel.cancelled &&
            front.cancel.cancelled->load(std::memory_order_acquire))
        {
            std::ranges::pop_heap(heap_, Compare{});
            IORequest discarded = std::move(heap_.back());
            heap_.pop_back();
            DiscardCancelled(std::move(discarded));
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (front.ready_at <= now)
        {
            std::ranges::pop_heap(heap_, Compare{});
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
    const std::scoped_lock lock(mutex_);
    return heap_.size();
}

bool IOQueue::IsEmpty() const
{
    const std::scoped_lock lock(mutex_);
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
