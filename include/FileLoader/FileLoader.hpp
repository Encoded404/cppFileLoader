#ifndef VULKAN_ENGINE_FILE_LOADER_HPP
#define VULKAN_ENGINE_FILE_LOADER_HPP

#include "IncrementalBuffer.hpp"
#include <future>
#include <memory>
#include <thread>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <system_error>

namespace FileLoader
{

struct FileLoadInfo
{
    std::filesystem::path path;
    std::uint64_t initial_read_rate_bytes_per_sec = 0;
    std::uint64_t size_hint_bytes = 0;
    int priority = 0;
    std::shared_ptr<void> user_context;
};

enum class AssemblyMode {
    Stream,    // assemble_from_stream
    FullBuffer // assemble_from_full_buffer
};

template<typename T, AssemblyMode Mode = AssemblyMode::Stream>
struct IAssembler {
    virtual ~IAssembler() = default;

    virtual std::future<std::shared_ptr<T>> AssembleFromStream(std::shared_ptr<IncrementalBuffer> stream) {
        auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
        prom->set_exception(std::make_exception_ptr(std::runtime_error("AssembleFromStream not implemented")));
        (void)stream; // avoid unused-parameter warnings in base
        return prom->get_future();
    }

    virtual std::future<std::shared_ptr<T>> AssembleFromFullBuffer(std::shared_ptr<ByteBuffer> buffer) {
        auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
        prom->set_exception(std::make_exception_ptr(std::runtime_error("AssembleFromFullBuffer not implemented")));
        (void)buffer;
        return prom->get_future();
    }

    std::future<std::shared_ptr<T>> AssembleFromFuture(std::future<std::shared_ptr<ByteBuffer>> fut) {
        auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
        std::thread([this, p = prom, f = std::move(fut)]() mutable {
            try {
                auto buf = f.get();
                auto nested = this->AssembleFromFullBuffer(buf);
                p->set_value(nested.get());
            } catch (...) { p->set_exception(std::current_exception()); }
        }).detach();
        return prom->get_future();
    }
};

template<typename T>
class LoadHandle {
public:
    LoadHandle() = default;

    void SetReadRateLimit(std::uint64_t bytes_per_sec) {
        if (!state_) return;
        state_->read_rate_bytes_per_sec.store(bytes_per_sec, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t GetReadRateLimit() const {
        return state_ ? state_->read_rate_bytes_per_sec.load() : 0;
    }

    void Cancel() {
        if (!state_) return;
        state_->cancelled.store(true, std::memory_order_release);
        if (state_->stream) state_->stream->Cancel();
    }

    [[nodiscard]] bool IsCancelled() const {
        return state_ && state_->cancelled.load(std::memory_order_acquire); 
    }
    
    std::shared_future<std::shared_ptr<T>> GetFuture() const {
        return state_ ? state_->result_future : std::shared_future<std::shared_ptr<T>>{};
    }

private:
    template<typename, AssemblyMode> friend class FileManager;
    struct State {
        std::atomic<std::uint64_t> read_rate_bytes_per_sec {0};
        std::atomic<bool> cancelled {false};
        std::shared_ptr<IncrementalBuffer> stream;
        std::shared_future<std::shared_ptr<T>> result_future;
    };
    std::shared_ptr<State> state_;
    explicit LoadHandle(std::shared_ptr<State> s) : state_(std::move(s)) {}
};

class FileManager {
public:
    FileManager() = default;

    // Stream assembly version
    template<typename T>
    LoadHandle<T> LoadFile(const FileLoadInfo& info, std::shared_ptr<IAssembler<T, AssemblyMode::Stream>> assembler) {
        using State = typename LoadHandle<T>::State;
        auto state = std::make_shared<State>();
        state->read_rate_bytes_per_sec.store(info.initial_read_rate_bytes_per_sec);
        state->cancelled.store(false);
        state->stream = std::make_shared<IncrementalBuffer>();

        auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
        state->result_future = prom->get_future().share();

        std::thread([info, state, prom, assembler]() {
            try {
                std::ifstream ifs(info.path, std::ios::binary);
                if (!ifs) throw std::system_error(errno, std::generic_category(), "Failed to open file");

                constexpr std::size_t chunk_size = static_cast<std::size_t>(64) * 1024;
                ByteBuffer chunk;
                chunk.reserve(chunk_size);

                auto assemble_fut = assembler->AssembleFromStream(state->stream);

                while (!state->cancelled.load(std::memory_order_acquire)) {
                    chunk.clear();
                    chunk.resize(chunk_size);
                    ifs.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk_size));
                    const std::streamsize read = ifs.gcount();

                    if (read <= 0) break;
                    chunk.resize(read);

                    if (!state->stream->push(std::move(chunk))) break;

                    // Recreate chunk after it was moved-from to avoid use-after-move
                    chunk = ByteBuffer();
                    chunk.reserve(chunk_size);

                    const auto rate = state->read_rate_bytes_per_sec.load(std::memory_order_relaxed);
                    if (rate > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::duration<double>(static_cast<double>(read) / static_cast<double>(rate))
                        );
                    }
                }

                state->stream->close();
                auto result = assemble_fut.get();
                prom->set_value(result);
            } catch (...) {
                state->stream->Cancel();
                prom->set_exception(std::current_exception());
            }
        }).detach();

        return LoadHandle<T>(state);
    }

    // Full buffer assembly version
    template<typename T>
    LoadHandle<T> LoadFile(const FileLoadInfo& info, std::shared_ptr<IAssembler<T, AssemblyMode::FullBuffer>> assembler) {
        using State = typename LoadHandle<T>::State;
        auto state = std::make_shared<State>();
        state->read_rate_bytes_per_sec.store(info.initial_read_rate_bytes_per_sec);
        state->cancelled.store(false);

        auto buffer_prom = std::make_shared<std::promise<std::shared_ptr<ByteBuffer>>>();
        auto buffer_fut = buffer_prom->get_future();
        
        auto assemble_fut = assembler->AssembleFromFuture(std::move(buffer_fut));
        state->result_future = assemble_fut.share();

        std::thread([info, state, buffer_prom, assembler]() {
            try {
                std::ifstream ifs(info.path, std::ios::binary);
                if (!ifs) throw std::system_error(errno, std::generic_category(), "Failed to open file");

                constexpr std::size_t chunk_size = static_cast<std::size_t>(64) * 1024;
                auto full_buf = std::make_shared<ByteBuffer>();
                full_buf->reserve(static_cast<std::size_t>(info.size_hint_bytes ? info.size_hint_bytes : chunk_size * 4));

                ByteBuffer chunk;
                chunk.reserve(chunk_size);

                while (!state->cancelled.load(std::memory_order_acquire)) {
                    chunk.clear();
                    chunk.resize(chunk_size);
                    ifs.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk_size));
                    const std::streamsize read = ifs.gcount();

                    if (read <= 0) break;
                    chunk.resize(read);

                    full_buf->insert(full_buf->end(), chunk.begin(), chunk.end());

                    const std::uint64_t rate = state->read_rate_bytes_per_sec.load(std::memory_order_relaxed);

                    if (rate > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::duration<double>(static_cast<double>(read) / static_cast<double>(rate))
                        );
                    }

                    // Recreate chunk after potential moves in other code paths
                    chunk = ByteBuffer();
                    chunk.reserve(chunk_size);
                }

                buffer_prom->set_value(full_buf);
            } catch (...) {
                buffer_prom->set_exception(std::current_exception());
            }
        }).detach();

        return LoadHandle<T>(state);
    }
};

} // namespace FileLoader

#endif // VULKAN_ENGINE_FILE_LOADER_HPP