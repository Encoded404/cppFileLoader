module;

#ifdef CPPFILELOADER_USE_STD_MODULE
#else
#include <future>
#include <memory>
#include <functional>
#include <filesystem>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <algorithm>
#include <iterator>
#include <utility>
#include <thread>
#endif

export module FileLoader.DefaultFileReadStrategy;

#ifdef CPPFILELOADER_USE_STD_MODULE
import std;
#endif

import FileLoader.Types;
import FileLoader.IncrementalBuffer;
import FileLoader.IFileReadStrategy;
import FileLoader.DynamicThreadPool;
import FileLoader.IOQueue;

export namespace FileLoader
{

class DefaultFileReadStrategy : public IFileReadStrategy
{
public:
    struct Config
    {
        std::size_t io_threads = 4;
        std::size_t post_threads = 2;
    };

    DefaultFileReadStrategy();
    explicit DefaultFileReadStrategy(Config cfg);
    ~DefaultFileReadStrategy() override;

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
    void WorkerLoop();

    Config config_;
    std::unique_ptr<IOQueue> io_queue_;
    std::vector<std::jthread> io_workers_;
    std::unique_ptr<DynamicThreadPool> post_pool_;
};

// --- Implementations ---

constexpr std::size_t kChunkSize = static_cast<std::size_t>(64) * 1024;

bool IsCancelled(const IORequest& req)
{
    return req.cancel.cancelled &&
           req.cancel.cancelled->load(std::memory_order_acquire);
}

DefaultFileReadStrategy::DefaultFileReadStrategy()
    : config_{}
    , io_queue_{std::make_unique<IOQueue>()}
{
    for (std::size_t i = 0; i < config_.io_threads; ++i)
        io_workers_.emplace_back([this] { WorkerLoop(); });

    DynamicThreadPool::Config pc;
    pc.min_threads = config_.post_threads;
    pc.max_threads = config_.post_threads;
    post_pool_ = std::make_unique<DynamicThreadPool>(pc);
}

DefaultFileReadStrategy::DefaultFileReadStrategy(Config cfg)
    : config_{cfg}
    , io_queue_{std::make_unique<IOQueue>()}
{
    for (std::size_t i = 0; i < config_.io_threads; ++i)
        io_workers_.emplace_back([this] { WorkerLoop(); });

    DynamicThreadPool::Config pc;
    pc.min_threads = config_.post_threads;
    pc.max_threads = config_.post_threads;
    post_pool_ = std::make_unique<DynamicThreadPool>(pc);
}

DefaultFileReadStrategy::~DefaultFileReadStrategy()
{
    io_queue_->Stop();
}

void DefaultFileReadStrategy::Post(WorkItem work)
{
    post_pool_->Post(std::move(work));
}

std::future<std::shared_ptr<ByteBuffer>> DefaultFileReadStrategy::ReadFull(
    const std::filesystem::path& path,
    std::uint64_t size_hint,
    CancellationToken cancel,
    ReadRateControl rate)
{
    IORequest req;
    req.type = IORequest::Type::Full;
    req.path = path;
    req.size_hint = size_hint;
    req.cancel = std::move(cancel);
    req.rate = std::move(rate);
    auto fut = req.full_promise.get_future();

    io_queue_->Push(std::move(req));
    return fut;
}

std::future<void> DefaultFileReadStrategy::ReadStreaming(
    const std::filesystem::path& path,
    CancellationToken cancel,
    std::shared_ptr<IncrementalBuffer> output,
    ReadRateControl rate)
{
    IORequest req;
    req.type = IORequest::Type::Streaming;
    req.path = path;
    req.cancel = std::move(cancel);
    req.rate = std::move(rate);
    req.stream_output = std::move(output);
    auto fut = req.stream_promise.get_future();

    io_queue_->Push(std::move(req));
    return fut;
}

void DefaultFileReadStrategy::WorkerLoop()
{
    while (true)
    {
        auto opt = io_queue_->Pop();
        if (!opt)
            return;

        IORequest req = std::move(*opt);

        // Check cancellation before opening file
        if (IsCancelled(req))
        {
            IOQueue::DiscardCancelled(std::move(req));
            continue;
        }

        try
        {
            if (!req.ifs)
            {
                req.ifs = std::make_unique<std::ifstream>(req.path, std::ios::binary);
                if (!(*req.ifs))
                {
                    throw std::system_error(errno, std::generic_category(),
                                            "Failed to open file: " + req.path.string());
                }
            }

            // Non-rate-limited reads stay in this tight loop to avoid re-queue overhead
            while (true)
            {
                ByteBuffer chunk(kChunkSize);
                req.ifs->read(reinterpret_cast<char*>(chunk.data()),
                              static_cast<std::streamsize>(kChunkSize));
                std::streamsize read = req.ifs->gcount();

                if (read <= 0)
                {
                    // EOF — resolve success
                    if (req.type == IORequest::Type::Full)
                    {
                        if (!req.partial_buffer)
                            req.partial_buffer = std::make_shared<ByteBuffer>();
                        req.full_promise.set_value(std::move(req.partial_buffer));
                    }
                    else
                    {
                        req.stream_output->Close();
                        req.stream_promise.set_value();
                    }
                    break;
                }

                chunk.resize(static_cast<std::size_t>(read));

                // Append data
                if (req.type == IORequest::Type::Full)
                {
                    if (!req.partial_buffer)
                    {
                        req.partial_buffer = std::make_shared<ByteBuffer>();
                        req.partial_buffer->reserve(
                            static_cast<std::size_t>(req.size_hint ? req.size_hint : kChunkSize * 4));
                    }
                    req.partial_buffer->insert(req.partial_buffer->end(),
                        std::make_move_iterator(chunk.begin()),
                        std::make_move_iterator(chunk.end()));
                }
                else
                {
                    if (!req.stream_output->Push(std::move(chunk)))
                    {
                        // Consumer cancelled the stream — resolve cleanly
                        req.stream_promise.set_value();
                        break;
                    }
                }

                // Check cancellation mid-read
                if (IsCancelled(req))
                {
                    // Return partial data (Full) or close stream (Streaming)
                    if (req.type == IORequest::Type::Full)
                        req.full_promise.set_value(std::move(req.partial_buffer));
                    else
                    {
                        req.stream_output->Close();
                        req.stream_promise.set_value();
                    }
                    break;
                }

                // Rate limit check
                std::uint64_t rate_val = 0;
                if (req.rate.bytes_per_sec)
                    rate_val = req.rate.bytes_per_sec->load(std::memory_order_relaxed);

                if (rate_val > 0)
                {
                    // Rate-limited: read one chunk, yield back to queue with delay
                    req.bytes_read += static_cast<std::uint64_t>(read);
                    auto delay = std::chrono::duration<double>(
                        static_cast<double>(read) / static_cast<double>(rate_val));
                    req.ready_at = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
                    io_queue_->Push(std::move(req));
                    break;
                }
                // No rate limit: continue reading on this thread
            }
        }
        catch (...)
        {
            auto eptr = std::current_exception();
            if (req.type == IORequest::Type::Full)
            {
                if (req.partial_buffer && !req.partial_buffer->empty())
                    req.full_promise.set_value(std::move(req.partial_buffer));
                else
                    req.full_promise.set_exception(eptr);
            }
            else
            {
                if (req.stream_output)
                    req.stream_output->Cancel();
                req.stream_promise.set_exception(eptr);
            }
        }
    }
}

} // namespace FileLoader
