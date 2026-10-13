#ifndef FILE_LOADER_OVERLAPPEDFILEREADSTRATEGY_HPP
#define FILE_LOADER_OVERLAPPEDFILEREADSTRATEGY_HPP

#include "IFileReadStrategy.hpp"
#include "IOQueue.hpp"
#include "DynamicThreadPool.hpp"
#include "IncrementalBuffer.hpp"

#include <list>
#include <map>
#include <vector>
#include <memory>
#include <future>
#include <functional>
#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <filesystem>
#include <cstdint>
#include <cstddef>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace FileLoader
{

#if defined(_WIN32)
class OverlappedFileReadStrategy final : public IFileReadStrategy
{
public:
    struct Config
    {
        std::size_t io_threads = 1;                    // one completion port per thread
        std::size_t submission_depth = 32;             // max chunks in flight per port
        std::size_t pipeline_depth = 4;                // max in-flight chunks per file
        std::size_t full_chunk_bytes = 512 * 1024;     // ungated Full-read chunks
        std::size_t stream_chunk_bytes = 64 * 1024;    // Streaming + rate-gated chunks
        std::size_t post_threads = 2;
    };

    /// Always available on Windows (IOCP is a Win32 core facility).
    static bool IsAvailable();

    OverlappedFileReadStrategy();
    explicit OverlappedFileReadStrategy(Config cfg);
    ~OverlappedFileReadStrategy() override;

    void Post(WorkItem work) override;

    std::future<std::shared_ptr<ByteBuffer>> ReadFull(
        const std::filesystem::path& path,
        std::uint64_t size_hint,
        CancellationToken cancel,
        ReadRateControl rate) override;

    std::future<void> ReadStreaming(
        const std::filesystem::path& path,
        CancellationToken cancel,
        std::shared_ptr<IncrementalBuffer> output,
        ReadRateControl rate) override;

private:
    struct ChunkSlot;
    struct ActiveRequest;
    class OverlappedPort;

    void WorkerLoop();

    Config config_;
    std::unique_ptr<IOQueue> io_queue_;
    std::vector<std::jthread> io_workers_;
    std::unique_ptr<DynamicThreadPool> post_pool_;
};


#else

// This strategy is Windows-only; other targets use DefaultFileReadStrategy.

#endif // _WIN32

} // namespace FileLoader

#endif // FILE_LOADER_OVERLAPPEDFILEREADSTRATEGY_HPP
