/// Parity suite for the native async read strategies
/// (IoUringFileReadStrategy on Linux, OverlappedFileReadStrategy on Windows)
/// against DefaultFileReadStrategy. Every strategy maker registered below runs
/// the exact same test body: byte-exact Full reads, streaming, cancellation
/// with partial-data resolution, rate-limited pacing incl. mid-flight rate
/// changes, and the rate-gate re-queue path that carries the open handle
/// across IOQueue round-trips.

#include <FileLoader/IFileReadStrategy.hpp>
#include <FileLoader/DefaultFileReadStrategy.hpp>

#if defined(__linux__)
#include <FileLoader/IoUringFileReadStrategy.hpp>
#endif
#if defined(_WIN32)
#include <FileLoader/OverlappedFileReadStrategy.hpp>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace FileLoader;

namespace
{

void WriteDeterministicFile(const std::filesystem::path& path, std::size_t size)
{
    std::ofstream ofs(path, std::ios::binary);
    for (std::size_t i = 0; i < size; ++i)
    {
        const char byte = static_cast<char>(0x20 + (i * 31) % 0x5F);
        ofs.write(&byte, 1);
    }
}

std::string ExpectedContent(std::size_t size)
{
    std::string s;
    s.reserve(size);
    for (std::size_t i = 0; i < size; ++i)
        s.push_back(static_cast<char>(0x20 + (i * 31) % 0x5F));
    return s;
}

std::unique_ptr<IFileReadStrategy> MakeDefaultStrategy()
{
    return std::make_unique<DefaultFileReadStrategy>(
        DefaultFileReadStrategy::Config{.io_threads = 2, .post_threads = 2});
}

std::vector<std::pair<std::string,
                      std::function<std::unique_ptr<IFileReadStrategy>()>>>
StrategyMakers()
{
    std::vector<std::pair<std::string,
                          std::function<std::unique_ptr<IFileReadStrategy>()>>>
        makers;
    makers.emplace_back("default", &MakeDefaultStrategy);
#if defined(__linux__)
    makers.emplace_back("io_uring", []() -> std::unique_ptr<IFileReadStrategy> {
        if (!IoUringFileReadStrategy::IsAvailable())
            return nullptr;
        return std::make_unique<IoUringFileReadStrategy>(
            IoUringFileReadStrategy::Config{.io_threads = 2,
                                            .submission_depth = 16,
                                            .pipeline_depth = 4,
                                            .full_chunk_bytes = 256 * 1024,
                                            .stream_chunk_bytes = 64 * 1024,
                                            .post_threads = 2});
    });
#endif
#if defined(_WIN32)
    makers.emplace_back("overlapped", []() -> std::unique_ptr<IFileReadStrategy> {
        return std::make_unique<OverlappedFileReadStrategy>(
            OverlappedFileReadStrategy::Config{.io_threads = 2,
                                               .submission_depth = 16,
                                               .pipeline_depth = 4,
                                               .full_chunk_bytes = 256 * 1024,
                                               .stream_chunk_bytes = 64 * 1024,
                                               .post_threads = 2});
    });
#endif
    return makers;
}

ReadRateControl MakeRate(std::uint64_t bytes_per_sec)
{
    return ReadRateControl{
        std::make_shared<std::atomic<std::uint64_t>>(bytes_per_sec)};
}

using StrategyParam = std::pair<
    std::string, std::function<std::unique_ptr<IFileReadStrategy>()>>;

class AsyncStrategyParityTest : public ::testing::TestWithParam<StrategyParam>
{
protected:
    void SetUp() override
    {
        test_dir_ = std::filesystem::temp_directory_path() /
                    "cppFileLoader_async_strategy_test";
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

} // namespace

TEST_P(AsyncStrategyParityTest, ReadFullExactBytes)
{
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP() << name << " unavailable on this platform";

    const std::vector<std::size_t> sizes = {
        1, 4097, 64 * 1024, 64 * 1024 + 1, 1024 * 1024 + 13};

    for (const std::size_t size : sizes)
    {
        SCOPED_TRACE(testing::Message() << name << " size=" << size);
        const auto path = TestPath("exact_" + std::to_string(size) + ".bin");
        WriteDeterministicFile(path, size);
        const std::string expected = ExpectedContent(size);

        auto fut = strategy->ReadFull(path, 0, CancellationToken{}, ReadRateControl{});
        auto buf = fut.get();

        ASSERT_NE(buf, nullptr);
        ASSERT_EQ(buf->size(), expected.size());
        EXPECT_EQ(0, std::memcmp(buf->data(), expected.data(), size));
    }

    // Empty file resolves to an empty (non-null) buffer
    const auto empty = TestPath("empty.bin");
    { std::ofstream ofs(empty, std::ios::binary); }
    auto fut = strategy->ReadFull(empty, 0, CancellationToken{}, ReadRateControl{});
    auto buf = fut.get();
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(buf->size(), 0u);
}

TEST_P(AsyncStrategyParityTest, ReadFullSizeHintDoesNotAlterContent)
{
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP();

    const auto path = TestPath("hint.bin");
    WriteDeterministicFile(path, 3 * 1024 * 1024 + 7);
    const std::string expected = ExpectedContent(3 * 1024 * 1024 + 7);

    auto fut = strategy->ReadFull(path, 1 << 20, CancellationToken{}, ReadRateControl{});
    auto buf = fut.get();
    ASSERT_NE(buf, nullptr);
    ASSERT_EQ(buf->size(), expected.size());
    EXPECT_EQ(0, std::memcmp(buf->data(), expected.data(), expected.size()));
}

TEST_P(AsyncStrategyParityTest, MissingFileThrowsOpenError)
{
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP();

    const auto path = TestPath("does_not_exist.bin");
    std::filesystem::remove(path);

    auto fut = strategy->ReadFull(path, 0, CancellationToken{}, ReadRateControl{});
    try
    {
        (void)fut.get();
        FAIL() << "expected std::system_error";
    }
    catch (const std::system_error& e)
    {
        EXPECT_NE(std::string(e.what()).find("Failed to open file"), std::string::npos);
    }
}

TEST_P(AsyncStrategyParityTest, CancelBeforeIssueResolvesEmpty)
{
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP();

    const auto path = TestPath("pre_cancelled.bin");
    WriteDeterministicFile(path, 128 * 1024);

    CancellationToken cancel{std::make_shared<const std::atomic<bool>>(true)};
    auto fut = strategy->ReadFull(path, 0, cancel, ReadRateControl{});
    auto buf = fut.get();
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(buf->size(), 0u);
}

TEST_P(AsyncStrategyParityTest, CancelMidReadReturnsPartialPrefix)
{
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP();

    constexpr std::size_t kSize = 4 * 1024 * 1024;
    const auto path = TestPath("mid_cancel.bin");
    WriteDeterministicFile(path, kSize);
    const std::string expected = ExpectedContent(kSize);

    auto token = std::make_shared<std::atomic<bool>>(false);
    auto fut = strategy->ReadFull(path, 0, CancellationToken{token},
                                  MakeRate(1 * 1024 * 1024));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    token->store(true);

    auto buf = fut.get(); // must resolve, never hang
    ASSERT_NE(buf, nullptr);
    ASSERT_GT(buf->size(), 0u);
    ASSERT_LT(buf->size(), kSize); // did not run to completion
    // The delivered prefix must be byte-exact
    EXPECT_EQ(0, std::memcmp(buf->data(), expected.data(), buf->size()));
}

TEST_P(AsyncStrategyParityTest, StreamingDeliversExactBytes)
{
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP();

    constexpr std::size_t kSize = 300 * 1024;
    const auto path = TestPath("stream.bin");
    WriteDeterministicFile(path, kSize);
    const std::string expected = ExpectedContent(kSize);

    auto stream_out = std::make_shared<IncrementalBuffer>();
    auto fut = strategy->ReadStreaming(path, CancellationToken{}, stream_out,
                                       ReadRateControl{});
    fut.get(); // future<void> resolves cleanly (throws on error)
    EXPECT_TRUE(stream_out->IsClosed());
    EXPECT_EQ(stream_out->Size(), kSize);
    const auto snapshot = stream_out->ReadSnapshot();
    EXPECT_EQ(0, std::memcmp(snapshot.data(), expected.data(), kSize));
}

TEST_P(AsyncStrategyParityTest, StreamingConsumerCancelResolves)
{
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP();

    const auto path = TestPath("stream_cancel.bin");
    WriteDeterministicFile(path, 2 * 1024 * 1024);

    auto stream_out = std::make_shared<IncrementalBuffer>();
    auto fut = strategy->ReadStreaming(path, CancellationToken{}, stream_out,
                                       ReadRateControl{});

    stream_out->Cancel();
    fut.get(); // resolves cleanly even though the consumer cancelled
}

TEST_P(AsyncStrategyParityTest, RateLimitPacingIsHonored)
{
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP();

    constexpr std::size_t kSize = 256 * 1024;
    const auto path = TestPath("paced.bin");
    WriteDeterministicFile(path, kSize);

    const auto start = std::chrono::steady_clock::now();
    auto fut = strategy->ReadFull(path, 0, CancellationToken{}, MakeRate(128 * 1024));
    auto buf = fut.get();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_NE(buf, nullptr);
    ASSERT_EQ(buf->size(), kSize);
    // 256 KiB at 128 KiB/s ≈ 2 s
    EXPECT_GE(elapsed, std::chrono::milliseconds(1500));
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST_P(AsyncStrategyParityTest, RateLimitChangeMidFlight)
{
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP();

    constexpr std::size_t kSize = 256 * 1024;
    const auto path = TestPath("mid_rate.bin");
    WriteDeterministicFile(path, kSize);

    auto rate = MakeRate(64 * 1024); // would take ~4 s if never raised
    const auto start = std::chrono::steady_clock::now();
    auto fut = strategy->ReadFull(path, 0, CancellationToken{}, rate);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    rate.bytes_per_sec->store(0); // unlimited from here on
    auto buf = fut.get();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_NE(buf, nullptr);
    ASSERT_EQ(buf->size(), kSize);
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST_P(AsyncStrategyParityTest, RateGatedRequeueCarriesOpenHandle)
{
    // Exercises the gate path where a rate-limited request is re-entered into
    // the IOQueue between chunks multiple times: handle + offset bookkeeping
    // must survive each round trip and the final buffer must be byte-exact.
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP();

    for (int rep = 0; rep < 4; ++rep)
    {
        SCOPED_TRACE(testing::Message() << name << " rep=" << rep);
        const auto path = TestPath("requeued_" + std::to_string(rep) + ".bin");
        WriteDeterministicFile(path, 192 * 1024); // 3 chunks at 64 KiB
        const std::string expected = ExpectedContent(192 * 1024);

        auto fut = strategy->ReadFull(path, 0, CancellationToken{},
                                      MakeRate(128 * 1024));
        auto buf = fut.get();
        ASSERT_NE(buf, nullptr);
        ASSERT_EQ(buf->size(), expected.size());
        EXPECT_EQ(0, std::memcmp(buf->data(), expected.data(), expected.size()));
        // three gated round trips ≈ 1.5 s of pacing
    }
}

TEST_P(AsyncStrategyParityTest, ConcurrentSoak)
{
    const auto& [name, make] = GetParam();
    auto strategy = make();
    if (!strategy)
        GTEST_SKIP();

    constexpr std::size_t kFiles = 8;
    constexpr std::size_t kSize = 256 * 1024;
    const std::string expected = ExpectedContent(kSize);

    std::vector<std::future<std::shared_ptr<ByteBuffer>>> futs;
    for (std::size_t i = 0; i < kFiles; ++i)
    {
        const auto path = TestPath("soak_" + std::to_string(i) + ".bin");
        WriteDeterministicFile(path, kSize);
        futs.push_back(strategy->ReadFull(path, kSize, CancellationToken{},
                                          ReadRateControl{}));
    }

    for (std::size_t i = 0; i < kFiles; ++i)
    {
        auto buf = futs[i].get();
        ASSERT_NE(buf, nullptr);
        ASSERT_EQ(buf->size(), kSize) << "file " << i;
        EXPECT_EQ(0, std::memcmp(buf->data(), expected.data(), kSize)) << "file " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Strategies, AsyncStrategyParityTest, testing::ValuesIn(StrategyMakers()),
    [](const testing::TestParamInfo<AsyncStrategyParityTest::ParamType>& info) {
        return info.param.first;
    });