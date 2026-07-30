#include <FileLoader/DynamicThreadPool.hpp>

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>

using namespace FileLoader;

TEST(DynamicThreadPool, EnqueueAndRetrieveResult)
{
    DynamicThreadPool pool({.min_threads = 1, .max_threads = 4, .idle_timeout = std::chrono::seconds(30)});
    auto fut = pool.Enqueue([] { return 42; });
    EXPECT_EQ(fut.get(), 42);
}

TEST(DynamicThreadPool, MultipleEnqueuesComplete)
{
    DynamicThreadPool pool({.min_threads = 2, .max_threads = 4, .idle_timeout = std::chrono::seconds(30)});
    constexpr int N = 100;
    std::vector<std::future<int>> futures;
    futures.reserve(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
    {
        futures.push_back(pool.Enqueue([i] { return i * i; }));
    }
    for (int i = 0; i < N; ++i)
    {
        EXPECT_EQ(futures[i].get(), i * i);
    }
}

TEST(DynamicThreadPool, ScaleUpThreadCountIncreases)
{
    DynamicThreadPool pool({.min_threads = 1, .max_threads = 8, .idle_timeout = std::chrono::seconds(30)});
    std::atomic<bool> go{false};
    std::vector<std::future<void>> futures;
    futures.reserve(8);

    // Submit enough tasks that block, forcing scale-up
    for (int i = 0; i < 8; ++i)
    {
        futures.push_back(pool.Enqueue([&go] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }));
    }

    // Give threads time to spawn and start processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_GT(pool.ThreadCount(), 1);

    go.store(true, std::memory_order_release);
    for (auto& f : futures) f.get();
}

TEST(DynamicThreadPool, ScaleDownThreadsExit)
{
    DynamicThreadPool pool({.min_threads = 1, .max_threads = 8, .idle_timeout = std::chrono::milliseconds(50)});

    // Submit work to cause scale-up
    std::atomic<bool> go{false};
    std::vector<std::future<void>> futures;
    futures.reserve(8);
    for (int i = 0; i < 8; ++i)
    {
        futures.push_back(pool.Enqueue([&go] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GT(pool.ThreadCount(), 1);

    go.store(true, std::memory_order_release);
    for (auto& f : futures) f.get();

    // Wait for idle timeout + scale-down
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // After scale-down, only min_threads should remain
    EXPECT_EQ(pool.ThreadCount(), 1);
}

TEST(DynamicThreadPool, RespectsMaxThreads)
{
    DynamicThreadPool pool({.min_threads = 1, .max_threads = 4, .idle_timeout = std::chrono::seconds(30)});
    std::atomic<bool> go{false};
    std::vector<std::future<void>> futures;
    futures.reserve(16);

    for (int i = 0; i < 16; ++i)
    {
        futures.push_back(pool.Enqueue([&go] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_LE(pool.ThreadCount(), 4);

    go.store(true, std::memory_order_release);
    for (auto& f : futures) f.get();
}

TEST(DynamicThreadPool, PostFireAndForget)
{
    DynamicThreadPool pool({.min_threads = 1, .max_threads = 4, .idle_timeout = std::chrono::seconds(30)});
    std::atomic<int> counter{0};

    for (int i = 0; i < 100; ++i)
    {
        pool.Post([&counter] { counter.fetch_add(1); });
    }

    // Wait for all posts to complete
    // Post returns void, so we signal via a sentinel Enqueue
    auto fut = pool.Enqueue([&counter] {
        return counter.load();
    });
    fut.wait();
    EXPECT_EQ(fut.get(), 100);
}

TEST(DynamicThreadPool, ExceptionPropagation)
{
    DynamicThreadPool pool({.min_threads = 1, .max_threads = 4, .idle_timeout = std::chrono::seconds(30)});
    auto fut = pool.Enqueue([]() -> int { throw std::runtime_error("test error"); });
    EXPECT_THROW(fut.get(), std::runtime_error);
}

TEST(DynamicThreadPool, DestructorJoinsCleanly)
{
    // This test passes if the destructor returns without deadlocking
    {
        DynamicThreadPool pool({.min_threads = 2, .max_threads = 4, .idle_timeout = std::chrono::seconds(30)});
        pool.Post([] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
        pool.Post([] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
        // Destructor runs here
    }
    SUCCEED();
}

TEST(DynamicThreadPool, PendingCount)
{
    DynamicThreadPool pool({.min_threads = 1, .max_threads = 1, .idle_timeout = std::chrono::seconds(30)});
    std::atomic<bool> go{false};

    auto fut = pool.Enqueue([&go] {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return 0;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Submit more tasks while the only thread is busy
    pool.Post([] {});
    pool.Post([] {});

    EXPECT_GE(pool.PendingCount(), 2);

    go.store(true, std::memory_order_release);
    fut.get();
}
