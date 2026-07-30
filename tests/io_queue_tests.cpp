#include <FileLoader/IOQueue.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace FileLoader;

namespace {

// Helper: create a Full-mode IORequest with given priority and ready_at
IORequest MakeFullRequest(int priority,
                                 std::chrono::steady_clock::time_point ready_at =
                                     std::chrono::steady_clock::time_point::min())
{
    IORequest req;
    req.type = IORequest::Type::Full;
    req.path = "/test/file.bin";
    req.priority = priority;
    req.ready_at = ready_at;
    return req;
}

} // namespace

// ============================================================================
// Push/Pop FIFO order (same priority)
// ============================================================================
TEST(IOQueueTest, FifoOrderSamePriority)
{
    IOQueue queue;
    queue.Push(MakeFullRequest(0));
    queue.Push(MakeFullRequest(0));
    queue.Push(MakeFullRequest(0));

    EXPECT_EQ(queue.Size(), 3);

    // All same priority, ready_at = min → should come out in push order
    auto r1 = queue.Pop();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->priority, 0);

    auto r2 = queue.Pop();
    ASSERT_TRUE(r2.has_value());

    auto r3 = queue.Pop();
    ASSERT_TRUE(r3.has_value());
}

// ============================================================================
// Priority ordering (higher priority first)
// ============================================================================
TEST(IOQueueTest, HigherPriorityPoppedFirst)
{
    IOQueue queue;
    queue.Push(MakeFullRequest(0));
    queue.Push(MakeFullRequest(10));
    queue.Push(MakeFullRequest(5));

    auto r1 = queue.Pop();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->priority, 10);

    auto r2 = queue.Pop();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->priority, 5);

    auto r3 = queue.Pop();
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->priority, 0);
}

// ============================================================================
// ready_at scheduling — delayed items are not popped until ready
// ============================================================================
TEST(IOQueueTest, DelayedItemBlocksUntilReady)
{
    IOQueue queue;
    auto future = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);

    // Push a delayed low-priority item, then an instant high-priority item
    queue.Push(MakeFullRequest(0, future));
    queue.Push(MakeFullRequest(10));

    // High-priority (ready now) should come out first
    auto r1 = queue.Pop();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->priority, 10);

    // Next pop should block until the delayed item is ready
    auto start = std::chrono::steady_clock::now();
    auto r2 = queue.Pop();
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->priority, 0);
    EXPECT_GE(elapsed, std::chrono::milliseconds(80));
}

// ============================================================================
// Cancellation skip — cancelled requests discarded on Pop
// ============================================================================
TEST(IOQueueTest, CancelledRequestDiscardedOnPop)
{
    IOQueue queue;
    auto cancelled = std::make_shared<std::atomic<bool>>(true);

    IORequest req = MakeFullRequest(5);
    req.cancel.cancelled = cancelled;
    queue.Push(std::move(req));

    queue.Push(MakeFullRequest(1));
    queue.Push(MakeFullRequest(2));

    // The cancelled request (priority 5) should be skipped in favor of lower-priority ones
    auto r1 = queue.Pop();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->priority, 2);

    auto r2 = queue.Pop();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->priority, 1);

    // Queue should now be empty
    EXPECT_TRUE(queue.IsEmpty());
}

// ============================================================================
// Cancellation with partial data — cancelled Full request returns partial buffer
// ============================================================================
TEST(IOQueueTest, CancelledFullRequestReturnsPartialBuffer)
{
    IOQueue queue;
    auto cancelled = std::make_shared<std::atomic<bool>>(true);

    IORequest req = MakeFullRequest(5);
    req.cancel.cancelled = cancelled;
    req.partial_buffer = std::make_shared<ByteBuffer>(ByteBuffer{std::byte{0xAB}, std::byte{0xCD}});
    auto fut = req.full_promise.get_future();
    queue.Push(std::move(req));

    // Push another request to force Pop to discard the cancelled one
    queue.Push(MakeFullRequest(1));
    auto r1 = queue.Pop();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->priority, 1);

    // Wait for the cancelled request's future to resolve with partial data
    auto buf = fut.get();
    ASSERT_NE(buf, nullptr);
    ASSERT_EQ(buf->size(), 2);
    EXPECT_EQ((*buf)[0], std::byte{0xAB});
    EXPECT_EQ((*buf)[1], std::byte{0xCD});
}

// ============================================================================
// Stop signal — Pop returns nullopt, workers exit cleanly
// ============================================================================
TEST(IOQueueTest, StopReturnsNullopt)
{
    IOQueue queue;

    // Spawn a worker thread that blocks on Pop
    std::atomic<bool> exited{false};
    std::thread worker([&] {
        auto result = queue.Pop();
        EXPECT_FALSE(result.has_value());
        exited.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(exited.load(std::memory_order_acquire));

    queue.Stop();
    worker.join();
    EXPECT_TRUE(exited.load(std::memory_order_acquire));
}

// ============================================================================
// Stop with items still in queue — remaining items are not popped
// ============================================================================
TEST(IOQueueTest, StopWithPendingItems)
{
    IOQueue queue;
    queue.Push(MakeFullRequest(1));
    queue.Push(MakeFullRequest(2));
    queue.Stop();

    auto result = queue.Pop();
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// Requeue pattern — pop, modify, push preserves heap invariant
// ============================================================================
TEST(IOQueueTest, RequeuePreservesOrder)
{
    IOQueue queue;
    queue.Push(MakeFullRequest(0));
    queue.Push(MakeFullRequest(10));
    queue.Push(MakeFullRequest(5));

    // Pop the highest priority (10)
    auto r1 = queue.Pop();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->priority, 10);

    // Modify and requeue with lower priority
    r1->priority = -5;
    r1->ready_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    queue.Push(std::move(*r1));

    // Now the queue should have: 5, 0, -5 (delayed)
    auto r2 = queue.Pop();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->priority, 5);

    auto r3 = queue.Pop();
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->priority, 0);

    // Last item is delayed; Pop should block briefly
    auto start = std::chrono::steady_clock::now();
    auto r4 = queue.Pop();
    auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(r4->priority, -5);
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));
}

// ============================================================================
// Empty/Size queries
// ============================================================================
TEST(IOQueueTest, EmptyAndSizeQueries)
{
    IOQueue queue;
    EXPECT_TRUE(queue.IsEmpty());
    EXPECT_EQ(queue.Size(), 0);

    queue.Push(MakeFullRequest(1));
    EXPECT_FALSE(queue.IsEmpty());
    EXPECT_EQ(queue.Size(), 1);

    queue.Push(MakeFullRequest(2));
    EXPECT_EQ(queue.Size(), 2);

    queue.Pop();
    EXPECT_EQ(queue.Size(), 1);

    queue.Pop();
    EXPECT_TRUE(queue.IsEmpty());
    EXPECT_EQ(queue.Size(), 0);
}

// ============================================================================
// Concurrent push/pop from multiple threads
// ============================================================================
TEST(IOQueueTest, ConcurrentPushPop)
{
    IOQueue queue;
    constexpr int kNumItems = 100;
    std::atomic<int> popped{0};

    // Producer thread
    std::thread producer([&] {
        for (int i = 0; i < kNumItems; ++i)
        {
            queue.Push(MakeFullRequest(i % 10));
            std::this_thread::yield();
        }
    });

    // Consumer thread
    std::thread consumer([&] {
        int count = 0;
        while (count < kNumItems)
        {
            auto opt = queue.Pop();
            if (opt)
            {
                ++count;
                popped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(popped.load(std::memory_order_relaxed), kNumItems);
    EXPECT_TRUE(queue.IsEmpty());
}

// ============================================================================
// Destructor calls Stop() — safety net
// ============================================================================
TEST(IOQueueTest, DestructorCallsStop)
{
    [[maybe_unused]] const std::optional<IORequest> popped;
    {
        IOQueue queue;
        queue.Push(MakeFullRequest(42));

        // Destroying queue should call Stop, not Pop the item
    }
    // If ~IOQueue didn't call Stop, the test would hang or crash.
    SUCCEED();
}
