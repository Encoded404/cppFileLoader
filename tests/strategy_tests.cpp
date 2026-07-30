#include <FileLoader/DefaultFileReadStrategy.hpp>
#include <FileLoader/IFileReadStrategy.hpp>

#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstddef>

using namespace FileLoader;

namespace {

void WriteTestFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

class DefaultFileReadStrategyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        test_dir_ = std::filesystem::temp_directory_path() / "cppFileLoader_strategy_test";
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override
    {
        std::filesystem::remove_all(test_dir_);
    }

    [[nodiscard]] std::filesystem::path TestPath(const std::string& name) const
    {
        return test_dir_ / name;
    }

    std::filesystem::path test_dir_;
};

TEST_F(DefaultFileReadStrategyTest, ReadFullReadsCompleteFile)
{
    DefaultFileReadStrategy strategy(DefaultFileReadStrategy::Config{.io_threads = 2, .post_threads = 2});
    const std::string expected = "Hello, World! This is a test file with some content.";
    WriteTestFile(TestPath("test.bin"), expected);

    auto fut = strategy.ReadFull(
        TestPath("test.bin"),
        0,
        CancellationToken{},
        ReadRateControl{});
    auto buf = fut.get();

    ASSERT_TRUE(buf != nullptr);
    EXPECT_EQ(buf->size(), expected.size());
    const std::string actual(reinterpret_cast<const char*>(buf->data()), buf->size());
    EXPECT_EQ(actual, expected);
}

TEST_F(DefaultFileReadStrategyTest, ReadFullEmptyFile)
{
    DefaultFileReadStrategy strategy(DefaultFileReadStrategy::Config{.io_threads = 2, .post_threads = 2});
    WriteTestFile(TestPath("empty.bin"), "");

    auto fut = strategy.ReadFull(
        TestPath("empty.bin"),
        0,
        CancellationToken{},
        ReadRateControl{});
    auto buf = fut.get();

    ASSERT_TRUE(buf != nullptr);
    EXPECT_EQ(buf->size(), 0);
}

TEST_F(DefaultFileReadStrategyTest, ReadFullThrowsOnMissingFile)
{
    DefaultFileReadStrategy strategy(DefaultFileReadStrategy::Config{.io_threads = 2, .post_threads = 2});

    auto fut = strategy.ReadFull(
        TestPath("nonexistent.bin"),
        0,
        CancellationToken{},
        ReadRateControl{});
    EXPECT_THROW(fut.get(), std::system_error);
}

TEST_F(DefaultFileReadStrategyTest, ReadFullWithCancellation)
{
    DefaultFileReadStrategy strategy(DefaultFileReadStrategy::Config{.io_threads = 2, .post_threads = 2});

    // Write a larger file so cancellation has time to take effect
    const std::string content(1024UL * 1024, 'A'); // 1MB
    WriteTestFile(TestPath("large.bin"), content);

    auto cancelled = std::make_shared<std::atomic<bool>>(false);

    auto fut = strategy.ReadFull(
        TestPath("large.bin"),
        0,
        CancellationToken{cancelled},
        ReadRateControl{});

    // Cancel almost immediately
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cancelled->store(true, std::memory_order_release);

    auto buf = fut.get();
    ASSERT_TRUE(buf != nullptr);
    // Buffer should be incomplete (cancelled mid-read)
    EXPECT_LT(buf->size(), content.size());
}

TEST_F(DefaultFileReadStrategyTest, ReadFullWithRateLimit)
{
    DefaultFileReadStrategy strategy(DefaultFileReadStrategy::Config{.io_threads = 2, .post_threads = 2});

    const std::string content(128UL * 1024, 'B'); // 128KB
    WriteTestFile(TestPath("rate.bin"), content);

    auto rate = std::make_shared<std::atomic<std::uint64_t>>(64 * 1024); // 64KB/s

    auto start = std::chrono::steady_clock::now();
    auto fut = strategy.ReadFull(
        TestPath("rate.bin"),
        0,
        CancellationToken{},
        ReadRateControl{rate});
    auto buf = fut.get();
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    ASSERT_TRUE(buf != nullptr);
    EXPECT_EQ(buf->size(), content.size());
    // 128KB at 64KB/s should take ~2 seconds (allow generous tolerance)
    EXPECT_GT(elapsed, 0.5);
}

TEST_F(DefaultFileReadStrategyTest, PostExecutesWork)
{
    DefaultFileReadStrategy strategy(DefaultFileReadStrategy::Config{.io_threads = 2, .post_threads = 2});
    std::atomic<int> counter{0};

    strategy.Post([&counter] { counter.store(42); });

    // Use ReadFull as a fence to wait for the post to complete
    WriteTestFile(TestPath("fence.bin"), "x");
    auto fut = strategy.ReadFull(TestPath("fence.bin"), 0, CancellationToken{}, ReadRateControl{});
    fut.wait();

    EXPECT_EQ(counter.load(), 42);
}

TEST_F(DefaultFileReadStrategyTest, ReadStreamingPushesChunks)
{
    DefaultFileReadStrategy strategy(DefaultFileReadStrategy::Config{.io_threads = 2, .post_threads = 2});
    const std::string content = "Streaming test data that spans multiple chunks hopefully!";
    WriteTestFile(TestPath("stream.bin"), content);

    auto stream = std::make_shared<IncrementalBuffer>();

    auto fut = strategy.ReadStreaming(
        TestPath("stream.bin"),
        CancellationToken{},
        stream,
        ReadRateControl{});
    fut.get(); // Wait for streaming to complete

    EXPECT_EQ(stream->Size(), content.size());

    auto snapshot = stream->ReadSnapshot();
    const std::string actual(reinterpret_cast<const char*>(snapshot.data()), snapshot.size());
    EXPECT_EQ(actual, content);
}

TEST_F(DefaultFileReadStrategyTest, ReadStreamingCancelled)
{
    DefaultFileReadStrategy strategy(DefaultFileReadStrategy::Config{.io_threads = 2, .post_threads = 2});
    const std::string content(1024UL * 1024, 'C'); // 1MB
    WriteTestFile(TestPath("stream_large.bin"), content);

    auto stream = std::make_shared<IncrementalBuffer>();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);

    auto fut = strategy.ReadStreaming(
        TestPath("stream_large.bin"),
        CancellationToken{cancelled},
        stream,
        ReadRateControl{});

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cancelled->store(true, std::memory_order_release);

    fut.get(); // Should complete (possibly with partial data)

    EXPECT_LT(stream->Size(), content.size());
}
