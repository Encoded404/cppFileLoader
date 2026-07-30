#ifndef FILE_LOADER_DEFAULT_FILE_READ_STRATEGY_HPP
#define FILE_LOADER_DEFAULT_FILE_READ_STRATEGY_HPP

#include "IFileReadStrategy.hpp"
#include "DynamicThreadPool.hpp"
#include "IOQueue.hpp"

#include <memory>
#include <vector>
#include <thread>
#include <cstddef>

namespace FileLoader
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

} // namespace FileLoader

#endif // FILE_LOADER_DEFAULT_FILE_READ_STRATEGY_HPP
