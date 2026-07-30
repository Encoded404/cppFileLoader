#ifndef FILE_LOADER_DYNAMIC_THREAD_POOL_HPP
#define FILE_LOADER_DYNAMIC_THREAD_POOL_HPP

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

namespace FileLoader
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
            const std::scoped_lock lock(mutex_);
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

} // namespace FileLoader

#endif // FILE_LOADER_DYNAMIC_THREAD_POOL_HPP
