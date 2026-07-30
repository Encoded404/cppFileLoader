#ifndef FILE_LOADER_MOCK_FILE_READ_STRATEGY_HPP
#define FILE_LOADER_MOCK_FILE_READ_STRATEGY_HPP

#include "IFileReadStrategy.hpp"

#include <map>
#include <stdexcept>

namespace FileLoader
{

class MockFileReadStrategy : public IFileReadStrategy
{
public:
    void SetFileContent(const std::filesystem::path& path, ByteBuffer content)
    {
        files_[path] = std::move(content);
    }

    void SetFileError(const std::filesystem::path& path, std::exception_ptr error)
    {
        errors_[path] = std::move(error);
    }

    void Post(WorkItem work) override
    {
        work();
    }

    std::future<std::shared_ptr<ByteBuffer>> ReadFull(
        const std::filesystem::path& path,
        std::uint64_t /*size_hint*/,
        CancellationToken /*cancel*/,
        ReadRateControl /*rate*/) override
    {
        std::promise<std::shared_ptr<ByteBuffer>> prom;
        auto fut = prom.get_future();

        auto err_it = errors_.find(path);
        if (err_it != errors_.end())
        {
            prom.set_exception(err_it->second);
            return fut;
        }

        auto it = files_.find(path);
        if (it != files_.end())
        {
            prom.set_value(std::make_shared<ByteBuffer>(it->second));
        }
        else
        {
            prom.set_exception(std::make_exception_ptr(
                std::runtime_error("MockFileReadStrategy: file not found: " + path.string())));
        }

        return fut;
    }

    std::future<void> ReadStreaming(
        const std::filesystem::path& path,
        CancellationToken /*cancel*/,
        std::shared_ptr<IncrementalBuffer> output,
        ReadRateControl /*rate*/) override
    {
        std::promise<void> prom;
        auto fut = prom.get_future();

        auto err_it = errors_.find(path);
        if (err_it != errors_.end())
        {
            prom.set_exception(err_it->second);
            return fut;
        }

        auto it = files_.find(path);
        if (it != files_.end())
        {
            output->Push(it->second);
            output->Close();
            prom.set_value();
        }
        else
        {
            prom.set_exception(std::make_exception_ptr(
                std::runtime_error("MockFileReadStrategy: file not found: " + path.string())));
        }

        return fut;
    }

private:
    std::map<std::filesystem::path, ByteBuffer> files_;
    std::map<std::filesystem::path, std::exception_ptr> errors_;
};

} // namespace FileLoader

#endif // FILE_LOADER_MOCK_FILE_READ_STRATEGY_HPP
