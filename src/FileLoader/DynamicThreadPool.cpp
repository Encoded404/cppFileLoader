#include "FileLoader/DynamicThreadPool.hpp"

#include <utility>

namespace FileLoader
{

DynamicThreadPool::DynamicThreadPool(Config cfg)
    : config_(cfg)
{
    workers_.reserve(config_.min_threads);
    for (std::size_t i = 0; i < config_.min_threads; ++i)
    {
        thread_count_.fetch_add(1, std::memory_order_relaxed);
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

DynamicThreadPool::~DynamicThreadPool()
{
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
    workers_.clear();
}

void DynamicThreadPool::Post(std::function<void()> work)
{
    {
        const std::scoped_lock lock(mutex_);
        tasks_.push(std::move(work));
    }
    cv_.notify_one();
    MaybeSpawnWorker();
}

std::size_t DynamicThreadPool::PendingCount() const
{
    const std::scoped_lock lock(mutex_);
    return tasks_.size();
}

void DynamicThreadPool::MaybeSpawnWorker()
{
    if (idle_count_.load(std::memory_order_relaxed) > 0)
        return;
    if (thread_count_.load(std::memory_order_relaxed) >= config_.max_threads)
        return;

    const std::scoped_lock lock(spawn_mutex_);

    if (idle_count_.load(std::memory_order_relaxed) > 0)
        return;
    if (thread_count_.load(std::memory_order_relaxed) >= config_.max_threads)
        return;

    thread_count_.fetch_add(1, std::memory_order_relaxed);
    workers_.emplace_back([this] { WorkerLoop(); });
}

void DynamicThreadPool::WorkerLoop()
{
    while (true)
    {
        std::function<void()> task;
        bool timed_out = false;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            idle_count_.fetch_add(1, std::memory_order_relaxed);

            if (tasks_.empty() && !stop_.load(std::memory_order_acquire))
            {
                auto result = cv_.wait_for(lock, config_.idle_timeout);
                timed_out = (result == std::cv_status::timeout);
            }

            idle_count_.fetch_sub(1, std::memory_order_relaxed);

            if (timed_out)
            {
                auto current = thread_count_.load(std::memory_order_relaxed);
                while (current > config_.min_threads)
                {
                    if (thread_count_.compare_exchange_weak(current, current - 1,
                            std::memory_order_release, std::memory_order_relaxed))
                    {
                        return;
                    }
                }
                continue;
            }

            if (!tasks_.empty())
            {
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            else if (stop_.load(std::memory_order_acquire))
            {
                thread_count_.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
        }

        if (task)
        {
            task();
        }
    }
}

} // namespace FileLoader
