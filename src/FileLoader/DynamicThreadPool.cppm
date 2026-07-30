module;

#ifdef CPPFILELOADER_USE_STD_MODULE
#else
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <utility>
#endif

export module FileLoader.DynamicThreadPool;

#ifdef CPPFILELOADER_USE_STD_MODULE
import std;
#endif

export namespace FileLoader
{

class DynamicThreadPool
{
public:
    struct Config
    {
        std::size_t min_threads = 4;
        std::size_t max_threads = 64;
        std::chrono::milliseconds idle_timeout = std::chrono::seconds(30);
    };

    explicit DynamicThreadPool(Config cfg);
    ~DynamicThreadPool();

    DynamicThreadPool(const DynamicThreadPool&) = delete;
    DynamicThreadPool& operator=(const DynamicThreadPool&) = delete;
    DynamicThreadPool(DynamicThreadPool&&) = delete;
    DynamicThreadPool& operator=(DynamicThreadPool&&) = delete;

    template<typename F>
    auto Enqueue(F&& fn) -> std::future<decltype(fn())>
    {
        using R = decltype(fn());
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        MaybeSpawnWorker();
        return future;
    }

    void Post(std::function<void()> work);

    std::size_t ThreadCount() const { return thread_count_.load(std::memory_order_relaxed); }
    std::size_t IdleCount() const { return idle_count_.load(std::memory_order_relaxed); }
    std::size_t PendingCount() const;

private:
    void MaybeSpawnWorker();
    void WorkerLoop();

    Config config_;
    mutable std::mutex mutex_;
    std::queue<std::function<void()>> tasks_;
    std::condition_variable cv_;
    std::atomic<std::size_t> thread_count_{0};
    std::atomic<std::size_t> idle_count_{0};
    std::atomic<bool> stop_{false};
    std::vector<std::jthread> workers_;
    std::mutex spawn_mutex_;
};

// --- Implementations ---

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
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(work));
    }
    cv_.notify_one();
    MaybeSpawnWorker();
}

std::size_t DynamicThreadPool::PendingCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void DynamicThreadPool::MaybeSpawnWorker()
{
    if (idle_count_.load(std::memory_order_relaxed) > 0)
        return;
    if (thread_count_.load(std::memory_order_relaxed) >= config_.max_threads)
        return;

    std::lock_guard<std::mutex> lock(spawn_mutex_);

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
