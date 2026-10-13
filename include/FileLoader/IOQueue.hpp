#ifndef FILE_LOADER_IO_QUEUE_HPP
#define FILE_LOADER_IO_QUEUE_HPP

#include "IFileReadStrategy.hpp"
#include "IncrementalBuffer.hpp"
#include "Types.hpp"

#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <optional>
#include <filesystem>
#include <memory>
#include <fstream>
#include <future>
#include <cstdint>
#include <cstddef>

namespace FileLoader
{

struct IORequest
{
    enum class Type { Full, Streaming };

    Type type;
    std::filesystem::path path;
    int priority = 0;
    std::chrono::steady_clock::time_point ready_at = std::chrono::steady_clock::time_point::min();
    std::uint64_t bytes_read = 0;
    std::uint64_t size_hint = 0;

    CancellationToken cancel;
    ReadRateControl rate;

    // Full mode state
    std::promise<std::shared_ptr<ByteBuffer>> full_promise;
    std::shared_ptr<ByteBuffer> partial_buffer;

    // Streaming mode state
    std::promise<void> stream_promise;
    std::shared_ptr<IncrementalBuffer> stream_output;

    // I/O state — stream stays open between rate-limited re-queues
    std::unique_ptr<std::ifstream> ifs;

    // Strategy bookkeeping, owned by the IFileReadStrategy implementation that
    // queued the request. DefaultFileReadStrategy ignores both fields; the
    // native async strategies (io_uring / overlapped IOCP) use them to carry
    // an open handle and read position across rate-limited re-queues.
    std::intptr_t os_handle = -1;      // fd (Linux) / HANDLE (Windows); -1 = not opened
    std::uint64_t next_offset = 0;     // offset of the next chunk to read
};

class IOQueue
{
public:
    IOQueue() = default;
    ~IOQueue();

    IOQueue(const IOQueue&) = delete;
    IOQueue& operator=(const IOQueue&) = delete;
    IOQueue(IOQueue&&) = delete;
    IOQueue& operator=(IOQueue&&) = delete;

    void Push(IORequest request);

    // Returns std::nullopt if Stop() was called and queue is drained.
    std::optional<IORequest> Pop();

    // Non-blocking variant: returns the highest-priority ready request, or
    // std::nullopt when the queue is empty, the front item's ready_at is in
    // the future, or Stop() was called. Used by workers that hold work in
    // flight and must never stall the pipeline behind a delayed request.
    std::optional<IORequest> TryPop();

    void Stop();

    std::size_t Size() const;
    bool IsEmpty() const;

    // Discard a cancelled request — resolves its promise with partial/empty data.
    static void DiscardCancelled(IORequest&& req);

private:
    struct Compare
    {
        bool operator()(const IORequest& a, const IORequest& b) const noexcept;
    };

    mutable std::mutex mutex_;
    std::deque<IORequest> heap_;
    std::condition_variable cv_;
    std::atomic<bool> stopped_{false};
};

} // namespace FileLoader

#endif // FILE_LOADER_IO_QUEUE_HPP
