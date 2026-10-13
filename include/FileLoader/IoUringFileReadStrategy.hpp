#ifndef FILE_LOADER_IOURINGFILEREADSTRATEGY_HPP
#define FILE_LOADER_IOURINGFILEREADSTRATEGY_HPP

#include "IFileReadStrategy.hpp"
#include "IOQueue.hpp"
#include "DynamicThreadPool.hpp"
#include "IncrementalBuffer.hpp"

#include <deque>
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

#if defined(__linux__)
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

namespace FileLoader
{

#if defined(__linux__)
class IoUringFileReadStrategy final : public IFileReadStrategy
{
public:
    struct Config
    {
        std::size_t io_threads = 1;                    // one ring per thread
        std::size_t submission_depth = 32;             // max chunks in flight per ring
        std::size_t pipeline_depth = 4;                // max in-flight chunks per file
        std::size_t full_chunk_bytes = 512 * 1024;     // ungated Full-read chunks
        std::size_t stream_chunk_bytes = 64 * 1024;    // Streaming + rate-gated chunks
        std::size_t post_threads = 2;
    };

    /// Cheap one-time probe: can this kernel create and map an io_uring ring?
    static bool IsAvailable();

    IoUringFileReadStrategy();
    explicit IoUringFileReadStrategy(Config cfg);
    ~IoUringFileReadStrategy() override;

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
    class UringRing;

    void WorkerLoop();

    Config config_;
    std::unique_ptr<IOQueue> io_queue_;
    std::vector<std::jthread> io_workers_;
    std::unique_ptr<DynamicThreadPool> post_pool_;
};

#else

// This strategy is Linux-only; other targets use DefaultFileReadStrategy.

#endif // __linux__

} // namespace FileLoader

#endif // FILE_LOADER_IOURINGFILEREADSTRATEGY_HPP
