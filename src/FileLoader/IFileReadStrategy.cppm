module;

#ifdef CPPFILELOADER_USE_STD_MODULE
#else
#include <future>
#include <memory>
#include <functional>
#include <filesystem>
#include <atomic>
#include <cstdint>
#endif

export module FileLoader.IFileReadStrategy;

#ifdef CPPFILELOADER_USE_STD_MODULE
import std;
#endif

import FileLoader.Types;
import FileLoader.IncrementalBuffer;

export namespace FileLoader
{

struct CancellationToken
{
    std::shared_ptr<const std::atomic<bool>> cancelled;
};

struct ReadRateControl
{
    std::shared_ptr<std::atomic<std::uint64_t>> bytes_per_sec;
};

class IFileReadStrategy
{
public:
    virtual ~IFileReadStrategy() = default;

    using WorkItem = std::function<void()>;

    virtual void Post(WorkItem work) = 0;

    virtual std::future<std::shared_ptr<ByteBuffer>> ReadFull(
        const std::filesystem::path& path,
        std::uint64_t size_hint,
        CancellationToken cancel,
        ReadRateControl rate) = 0;

    virtual std::future<void> ReadStreaming(
        const std::filesystem::path& path,
        CancellationToken cancel,
        std::shared_ptr<IncrementalBuffer> output,
        ReadRateControl rate) = 0;
};

} // namespace FileLoader
