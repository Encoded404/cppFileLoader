module;

#ifdef CPPFILELOADER_USE_STD_MODULE
#else
#include <future>
#include <memory>
#include <thread>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdint>
#include <cstddef>
#endif

export module FileLoader;

#ifdef CPPFILELOADER_USE_STD_MODULE
import std;
#endif

export import FileLoader.Types;
export import FileLoader.IncrementalBuffer;
export import FileLoader.IFileReadStrategy;

import FileLoader.DefaultFileReadStrategy;
import FileLoader.DynamicThreadPool;

export namespace FileLoader
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
struct IAssembler : std::enable_shared_from_this<IAssembler<T, Mode>> {
    virtual ~IAssembler() = default;

    virtual std::future<std::shared_ptr<T>> AssembleFromStream(std::shared_ptr<IncrementalBuffer> stream) {
        auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
        prom->set_exception(std::make_exception_ptr(std::runtime_error("AssembleFromStream not implemented")));
        (void)stream;
        return prom->get_future();
    }

    virtual std::future<std::shared_ptr<T>> AssembleFromFullBuffer(std::shared_ptr<ByteBuffer> buffer) {
        auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
        prom->set_exception(std::make_exception_ptr(std::runtime_error("AssembleFromFullBuffer not implemented")));
        (void)buffer;
        return prom->get_future();
    }
};

template<typename T>
class LoadHandle {
public:
    LoadHandle() = default;

    void SetReadRateLimit(std::uint64_t bytes_per_sec) {
        if (!state_) return;
        state_->read_rate_bytes_per_sec->store(bytes_per_sec, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t GetReadRateLimit() const {
        return state_ ? state_->read_rate_bytes_per_sec->load() : 0;
    }

    void Cancel() {
        if (!state_) return;
        state_->cancelled->store(true, std::memory_order_release);
        if (state_->stream) state_->stream->Cancel();
    }

    [[nodiscard]] bool IsCancelled() const {
        return state_ && state_->cancelled->load(std::memory_order_acquire);
    }

    std::shared_future<std::shared_ptr<T>> GetFuture() const {
        return state_ ? state_->result_future : std::shared_future<std::shared_ptr<T>>{};
    }

private:
    friend class FileManager;
    struct State {
        std::shared_ptr<std::atomic<std::uint64_t>> read_rate_bytes_per_sec
            = std::make_shared<std::atomic<std::uint64_t>>(0);
        std::shared_ptr<std::atomic<bool>> cancelled
            = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<IncrementalBuffer> stream;
        std::shared_future<std::shared_ptr<T>> result_future;
    };
    std::shared_ptr<State> state_;
    explicit LoadHandle(std::shared_ptr<State> s) : state_(std::move(s)) {}
};

class FileManager {
public:
    using CpuWork = std::function<void()>;
    using CpuScheduler = std::function<void(CpuWork)>;

    FileManager()
        : strategy_(std::make_shared<DefaultFileReadStrategy>())
        , cpu_pool_(std::make_shared<DynamicThreadPool>([]() {
            DynamicThreadPool::Config cfg;
            const auto hw = std::thread::hardware_concurrency();
            cfg.max_threads = (hw > 1) ? static_cast<std::size_t>(hw) - 1 : 1;
            cfg.min_threads = 1;
            cfg.idle_timeout = std::chrono::seconds(30);
            return cfg;
        }()))
        , cpu_scheduler_([pool = cpu_pool_](CpuWork work) { pool->Post(std::move(work)); })
    {}

    explicit FileManager(std::shared_ptr<IFileReadStrategy> strategy,
                         CpuScheduler cpu_scheduler)
        : strategy_(std::move(strategy))
        , cpu_scheduler_(std::move(cpu_scheduler))
    {}

    explicit FileManager(std::shared_ptr<IFileReadStrategy> strategy)
        : strategy_(std::move(strategy))
        , cpu_pool_(std::make_shared<DynamicThreadPool>([]() {
            DynamicThreadPool::Config cfg;
            const auto hw = std::thread::hardware_concurrency();
            cfg.max_threads = (hw > 1) ? static_cast<std::size_t>(hw) - 1 : 1;
            cfg.min_threads = 1;
            cfg.idle_timeout = std::chrono::seconds(30);
            return cfg;
        }()))
        , cpu_scheduler_([pool = cpu_pool_](CpuWork work) { pool->Post(std::move(work)); })
    {}

    template<typename T>
    LoadHandle<T> LoadFile(const FileLoadInfo& info, std::shared_ptr<IAssembler<T, AssemblyMode::Stream>> assembler) {
        using State = typename LoadHandle<T>::State;
        auto state = std::make_shared<State>();
        state->read_rate_bytes_per_sec->store(info.initial_read_rate_bytes_per_sec);
        state->stream = std::make_shared<IncrementalBuffer>();

        auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
        state->result_future = prom->get_future().share();

        auto assemble_fut = assembler->AssembleFromStream(state->stream);
        auto read_fut = strategy_->ReadStreaming(info.path,
                                                  CancellationToken{state->cancelled},
                                                  state->stream,
                                                  ReadRateControl{state->read_rate_bytes_per_sec});

        auto read_fut_ptr = std::make_shared<decltype(read_fut)>(std::move(read_fut));
        auto assemble_fut_ptr = std::make_shared<decltype(assemble_fut)>(std::move(assemble_fut));
        strategy_->Post([prom, stream = state->stream,
                         read_fut_ptr, assemble_fut_ptr]() mutable {
            try {
                read_fut_ptr->get();
                stream->Close();
                auto result = assemble_fut_ptr->get();
                prom->set_value(result);
            } catch (...) {
                auto eptr = std::current_exception();
                stream->Cancel();
                prom->set_exception(eptr);
            }
        });

        return LoadHandle<T>(state);
    }

    template<typename T>
    LoadHandle<T> LoadFile(const FileLoadInfo& info, std::shared_ptr<IAssembler<T, AssemblyMode::FullBuffer>> assembler) {
        using State = typename LoadHandle<T>::State;
        auto state = std::make_shared<State>();
        state->read_rate_bytes_per_sec->store(info.initial_read_rate_bytes_per_sec);

        auto buffer_fut = strategy_->ReadFull(info.path, info.size_hint_bytes,
                                               CancellationToken{state->cancelled},
                                               ReadRateControl{state->read_rate_bytes_per_sec});
        auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
        state->result_future = prom->get_future().share();

        auto buffer_fut_ptr = std::make_shared<decltype(buffer_fut)>(std::move(buffer_fut));
        strategy_->Post([buffer_fut_ptr, assembler = std::move(assembler), prom,
                         cpu = cpu_scheduler_]() mutable {
            try {
                auto buf = buffer_fut_ptr->get();
                cpu([buf = std::move(buf), assembler = std::move(assembler), prom]() {
                    try {
                        prom->set_value(assembler->AssembleFromFullBuffer(buf).get());
                    } catch (...) {
                        prom->set_exception(std::current_exception());
                    }
                });
            } catch (...) {
                prom->set_exception(std::current_exception());
            }
        });

        return LoadHandle<T>(state);
    }

private:
    std::shared_ptr<IFileReadStrategy> strategy_;
    std::shared_ptr<DynamicThreadPool> cpu_pool_;
    CpuScheduler cpu_scheduler_;
};

} // namespace FileLoader
