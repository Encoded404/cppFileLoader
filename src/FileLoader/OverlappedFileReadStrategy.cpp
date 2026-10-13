#include "FileLoader/OverlappedFileReadStrategy.hpp"
#include <logging/logging.hpp>

#include <list>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <filesystem>
#include <memory>
#include <fstream>
#include <future>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <string>
#include <thread>
#include <vector>
#include <system_error>
#include <functional>

#include <string>

namespace FileLoader
{

#if defined(_WIN32)

/// One read submitted via ReadFile, not yet consumed. The OVERLAPPED lives
/// inside the slot and its address is the completion's identity.
struct OverlappedFileReadStrategy::ChunkSlot
{
    ActiveRequest* req = nullptr;
    std::uint64_t offset = 0;
    std::size_t claimed = 0;
    int res = 0;                       // bytes read, or -(GetLastError())
    bool done = false;
    std::shared_ptr<ByteBuffer> buffer;
    OVERLAPPED ov{};                   // Offset/OffsetHigh carry the file offset
    std::shared_ptr<std::atomic_bool> delivered =
        std::make_shared<std::atomic_bool>(false);
};

/// A request currently owned by one worker visit.
struct OverlappedFileReadStrategy::ActiveRequest
{
    IORequest req;
    std::uint64_t submit_offset = 0;
    std::uint64_t frontier = 0;
    std::map<std::uint64_t, ChunkSlot*> pending;
    std::chrono::steady_clock::time_point next_ready{};
    bool resolved = false;
    bool returned = false;
};

/// Minimal thread-confined completion port wrapper.
class OverlappedFileReadStrategy::OverlappedPort
{
public:
    OverlappedPort() = default;
    ~OverlappedPort() { Teardown(); }

    OverlappedPort(const OverlappedPort&) = delete;
    OverlappedPort& operator=(const OverlappedPort&) = delete;

    [[nodiscard]] bool Init(unsigned /*entries*/)
    {
        Teardown();
        port_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        return port_ != nullptr;
    }

    /// Associate an overlapped handle with this port. Returns false on failure
    /// (completions would then be unroutable and must not be queued).
    bool Bind(HANDLE file)
    {
        return ::CreateIoCompletionPort(file, port_, 0, 0) != nullptr;
    }

    [[nodiscard]] bool Valid() const noexcept { return port_ != nullptr; }
    [[nodiscard]] HANDLE Port() const noexcept { return port_; }

    static ChunkSlot* OvpToSlot(OVERLAPPED* ovp) noexcept
    {
        return reinterpret_cast<ChunkSlot*>(
            reinterpret_cast<std::uint8_t*>(ovp) - offsetof(ChunkSlot, ov));
    }

private:
    void Teardown() noexcept
    {
        if (port_)
            ::CloseHandle(port_);
        port_ = nullptr;
    }

    HANDLE port_ = nullptr;
};

namespace
{

bool OverlappedIsCancelled(const IORequest& req)
{
    return req.cancel.cancelled &&
           req.cancel.cancelled->load(std::memory_order_acquire);
}

/// Close a request's handle (if the native strategy opened one) and discard
/// the cancelled request exactly like IOQueue::DiscardCancelled.
void OverlappedDiscardCancelledNative(IORequest& req)
{
    if (req.os_handle != -1)
    {
        ::CloseHandle(reinterpret_cast<HANDLE>(req.os_handle));
        req.os_handle = -1;
    }
    IOQueue::DiscardCancelled(std::move(req));
}

HANDLE OpenForRead(const std::filesystem::path& path, DWORD& err) noexcept
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        err = ::GetLastError();
    return h;
}

} // namespace

bool OverlappedFileReadStrategy::IsAvailable()
{
    return true;
}

OverlappedFileReadStrategy::OverlappedFileReadStrategy()
    : OverlappedFileReadStrategy(Config{})
{
}

OverlappedFileReadStrategy::OverlappedFileReadStrategy(Config cfg)
    : config_(cfg)
    , io_queue_(std::make_unique<IOQueue>())
{
    for (std::size_t i = 0; i < config_.io_threads; ++i)
        io_workers_.emplace_back([this] { WorkerLoop(); });

    DynamicThreadPool::Config pc;
    pc.min_threads = config_.post_threads;
    pc.max_threads = config_.post_threads;
    post_pool_ = std::make_unique<DynamicThreadPool>(pc);
}

OverlappedFileReadStrategy::~OverlappedFileReadStrategy()
{
    // Workers exit on their own: queue stop makes Pop return nullopt, and
    // in-flight reads drain through their (guaranteed) completion packets
    // before the loop reaches the stopped Pop.
    io_queue_->Stop();
}

void OverlappedFileReadStrategy::Post(WorkItem work)
{
    post_pool_->Post(std::move(work));
}

std::future<std::shared_ptr<ByteBuffer>> OverlappedFileReadStrategy::ReadFull(
    const std::filesystem::path& path,
    std::uint64_t size_hint,
    CancellationToken cancel,
    ReadRateControl rate)
{
#if LOGIFACE_ENABLE_LOGGING
    std::error_code ec;
    const auto fsize = std::filesystem::file_size(path, ec);
    if (ec)
    {
        LOGIFACE_LOG(warn, std::string("FileLoader: unable to check file size for '") +
                     path.string() + "': " + ec.message());
    }
    else if (fsize < 16ULL * 1024)
    {
        LOGIFACE_LOG(info, std::string("FileLoader: '") + path.string() +
                     "' is under 16 KB - consider synchronous read");
    }
#endif

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

std::future<void> OverlappedFileReadStrategy::ReadStreaming(
    const std::filesystem::path& path,
    CancellationToken cancel,
    std::shared_ptr<IncrementalBuffer> output,
    ReadRateControl rate)
{
#if LOGIFACE_ENABLE_LOGGING
    std::error_code ec;
    const auto fsize = std::filesystem::file_size(path, ec);
    if (ec)
    {
        LOGIFACE_LOG(warn, std::string("FileLoader: unable to check file size for '") +
                     path.string() + "': " + ec.message());
    }
    else if (fsize < 16ULL * 1024)
    {
        LOGIFACE_LOG(info, std::string("FileLoader: '") + path.string() +
                     "' is under 16 KB - consider synchronous read");
    }
#endif

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

void OverlappedFileReadStrategy::WorkerLoop()
{
    OverlappedPort port;
    if (!port.Init(static_cast<unsigned>(config_.submission_depth)))
    {
        LOGIFACE_LOG(warn,
            "FileLoader: IOCP creation failed in worker; strategy thread exiting");
        return;
    }

    std::list<ActiveRequest> active;
    std::size_t expected_packets = 0;

    const auto finish = [](ActiveRequest& a) {
        if (a.req.type == IORequest::Type::Full)
        {
            if (!a.req.partial_buffer)
                a.req.partial_buffer = std::make_shared<ByteBuffer>();
            a.req.full_promise.set_value(std::move(a.req.partial_buffer));
        }
        else
        {
            a.req.stream_output->Close();
            a.req.stream_promise.set_value();
        }
        a.resolved = true;
    };


    const auto close_handle = [](ActiveRequest& a) {
        if (a.req.os_handle != -1)
        {
            ::CloseHandle(reinterpret_cast<HANDLE>(a.req.os_handle));
            a.req.os_handle = -1;
        }
    };

    /// Mark a slot consumed exactly once. The caller handles packet accounting
    /// (expected_packets) — only packet deliveries decrement it.
    const auto claim_slot = [](ChunkSlot* slot, int res) -> bool {
        if (slot->delivered->exchange(true, std::memory_order_acq_rel))
            return false; // duplicate delivery — already claimed inline or by packet
        slot->res = res;
        slot->done = true;
        return true;
    };

    while (true)
    {
        // ---- frontier processing: append in offset order + decision ladder --
        for (auto it = active.begin(); it != active.end();)
        {
            ActiveRequest& a = *it;

            // Zombie bookkeeping: resolved/returned requests drain their
            // in-flight slots without touching promises.
            if (a.resolved || a.returned)
            {
                for (auto pit = a.pending.begin(); pit != a.pending.end();)
                {
                    if (pit->second->done)
                    {
                        delete pit->second;
                        pit = a.pending.erase(pit);
                    }
                    else
                    {
                        ++pit;
                    }
                }
                if (a.pending.empty())
                {
                    if (a.resolved)
                        close_handle(a); // returned requests keep the handle open
                    it = active.erase(it);
                }
                else
                {
                    ++it;
                }
                continue;
            }

            while (!a.pending.empty() && !a.resolved)
            {
                auto begin = a.pending.begin();
                if (!begin->second->done || begin->first != a.frontier)
                    break;

                ChunkSlot* slot = begin->second;
                const int res = slot->res;
                const std::size_t claimed = slot->claimed;
                auto buffer = std::move(slot->buffer);
                delete slot;
                a.pending.erase(begin);

                if (res <= 0)
                {
                    if (res < 0)
                    {
                        LOGIFACE_LOG(warn,
                            std::string("FileLoader: overlapped read failed at offset ")
                            + std::to_string(begin->first) + " of '"
                            + a.req.path.string()
                            + "' — resolving as EOF (Default parity)");
                    }
                    finish(a);
                    break;
                }

                buffer->resize(static_cast<std::size_t>(res));

                if (a.req.type == IORequest::Type::Full)
                {
                    if (!a.req.partial_buffer)
                    {
                        a.req.partial_buffer = std::make_shared<ByteBuffer>();
                        a.req.partial_buffer->reserve(
                            static_cast<std::size_t>(
                                a.req.size_hint ? a.req.size_hint
                                                : config_.stream_chunk_bytes * 4));
                    }
                    a.req.partial_buffer->insert(
                        a.req.partial_buffer->end(),
                        std::make_move_iterator(buffer->begin()),
                        std::make_move_iterator(buffer->end()));
                }
                else
                {
                    if (!a.req.stream_output->Push(std::move(*buffer)))
                    {
                        finish(a);
                        break;
                    }
                }
                a.req.bytes_read += static_cast<std::uint64_t>(res);
                a.frontier += claimed;

                if (OverlappedIsCancelled(a.req))
                {
                    finish(a);
                    break;
                }

                std::uint64_t rate_val = 0;
                if (a.req.rate.bytes_per_sec)
                    rate_val = a.req.rate.bytes_per_sec->load(std::memory_order_relaxed);
                if (rate_val > 0)
                {
                    const auto delay = std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(
                            static_cast<double>(res) /
                            static_cast<double>(rate_val)));
                    a.next_ready = std::chrono::steady_clock::now() + delay;
                    a.req.next_offset = a.frontier;
                    a.req.ready_at = a.next_ready;
                    io_queue_->Push(std::move(a.req));
                    a.returned = true;
                    break;
                }

                if (res < static_cast<int>(claimed))
                {
                    // short read → end of file
                    finish(a);
                    break;
                }
            }

            if (a.resolved || a.returned)
            {
                if (a.pending.empty() && a.resolved)
                {
                    close_handle(a);
                    it = active.erase(it);
                }
                else if (a.pending.empty() && a.returned)
                {
                    it = active.erase(it);
                }
                else
                {
                    ++it;
                }
                continue;
            }

            ++it;
        }

        // ---- admit phase ------------------------------------------------------
        {
            const bool can_admit =
                expected_packets < config_.submission_depth &&
                active.size() < config_.submission_depth;
            if (can_admit)
            {
                std::optional<IORequest> next;
                if (active.empty() && expected_packets == 0)
                {
                    next = io_queue_->Pop();
                    if (!next)
                        return;
                }
                else
                {
                    next = io_queue_->TryPop();
                }

                if (next)
                {
                    if (OverlappedIsCancelled(*next))
                    {
                        OverlappedDiscardCancelledNative(*next);
                    }
                    else
                    {
                        if (next->os_handle == -1)
                        {
                            DWORD open_err = 0;
                            const HANDLE h = OpenForRead(next->path, open_err);
                            if (h == INVALID_HANDLE_VALUE)
                            {
                                auto eptr = std::make_exception_ptr(std::system_error(
                                    static_cast<int>(open_err), std::system_category(),
                                    "Failed to open file: " + next->path.string()));
                                next->full_promise.set_exception(eptr);
                                next->stream_promise.set_exception(std::move(eptr));
                            }
                            else
                            {
                                next->os_handle =
                                    static_cast<std::intptr_t>(reinterpret_cast<std::intptr_t>(h));
                                port.Bind(h);
                            }
                        }
                        if (next->os_handle != -1)
                        {
                            ActiveRequest a;
                            a.req = std::move(*next);
                            a.submit_offset = a.req.next_offset;
                            a.frontier = a.req.next_offset;
                            a.next_ready =
                                (a.req.ready_at ==
                                 std::chrono::steady_clock::time_point::min())
                                    ? std::chrono::steady_clock::time_point{}
                                    : a.req.ready_at;
                            active.push_back(std::move(a));
                        }
                    }
                }
            }
        }

        // ---- submit phase -------------------------------------------------------
        for (auto& a : active)
        {
            if (a.resolved || a.returned || a.req.os_handle == -1)
                continue;

            bool rate_limited = false;
            if (a.req.rate.bytes_per_sec)
                rate_limited =
                    a.req.rate.bytes_per_sec->load(std::memory_order_relaxed) > 0;

            const std::size_t max_pending =
                rate_limited ? 1 : config_.pipeline_depth;
            const std::size_t chunk_bytes =
                rate_limited
                    ? config_.stream_chunk_bytes
                    : (a.req.type == IORequest::Type::Full
                           ? config_.full_chunk_bytes
                           : config_.stream_chunk_bytes);

            while (!a.resolved && !a.returned &&
                   a.pending.size() < max_pending &&
                   expected_packets < config_.submission_depth)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now < a.next_ready)
                    break;

                auto buffer = std::make_shared<ByteBuffer>(chunk_bytes);
                auto* slot = new ChunkSlot();
                slot->req = &a;
                slot->offset = a.submit_offset;
                slot->claimed = chunk_bytes;
                slot->buffer = std::move(buffer);

                const HANDLE file = reinterpret_cast<HANDLE>(a.req.os_handle);
                OVERLAPPED& ov = slot->ov;
                std::memset(&ov, 0, sizeof(ov));
                ov.Offset = static_cast<DWORD>(a.submit_offset & 0xFFFFFFFFull);
                ov.OffsetHigh = static_cast<DWORD>(a.submit_offset >> 32);

                const BOOL ok = ::ReadFile(file, slot->buffer->data(),
                                           static_cast<DWORD>(chunk_bytes),
                                           nullptr, &ov);
                if (ok)
                {
                    // Synchronous completion; the packet is still posted for
                    // port-bound handles — never process inline (double-count
                    // protection lives in claim_slot).
                    ++expected_packets;
                    a.pending[a.submit_offset] = slot;
                    a.submit_offset += chunk_bytes;
                    continue;
                }

                const DWORD err = ::GetLastError();
                if (err == ERROR_IO_PENDING)
                {
                    ++expected_packets;
                    a.pending[a.submit_offset] = slot;
                    a.submit_offset += chunk_bytes;
                    continue;
                }

                // EOF and hard failures complete synchronously without a
                // packet (ERROR_HANDLE_EOF is documented not to post one).
                // Claim the slot inline (a surprise duplicate is ignored by the
                // delivery guard) and keep it in the pending map — the frontier
                // ladder resolves the request as EOF, and the zombie drain
                // frees the slot. Never delete here: if the OS posts a packet
                // anyway it would reference freed memory.
                claim_slot(slot, err == ERROR_HANDLE_EOF
                                     ? 0
                                     : -static_cast<int>(err));
                a.pending[a.submit_offset] = slot;
                a.submit_offset += chunk_bytes;
                break;
            }
        }

        // ---- gate phase ------------------------------------------------------------
        for (auto it = active.begin(); it != active.end();)
        {
            ActiveRequest& a = *it;
            if (!a.resolved && !a.returned && a.pending.empty() &&
                std::chrono::steady_clock::now() < a.next_ready)
            {
                a.req.next_offset = a.frontier;
                a.req.ready_at = a.next_ready;
                io_queue_->Push(std::move(a.req));
                a.returned = true;
            }
            if (a.returned && a.pending.empty())
            {
                it = active.erase(it); // handle travels with the request
            }
            else
            {
                ++it;
            }
        }

        // ---- wait phase --------------------------------------------------------------
        if (expected_packets > 0)
        {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* ovp = nullptr;
            const BOOL ok = ::GetQueuedCompletionStatus(
                port.Port(), &bytes, &key, &ovp, INFINITE);
            if (!ok)
            {
                const DWORD err = ::GetLastError();
                if (ovp == nullptr)
                    break; // no packet and no slot — port failure; exit worker
                auto* slot = OverlappedPort::OvpToSlot(ovp);
                if (slot->delivered->exchange(true, std::memory_order_acq_rel))
                    break; // already claimed inline; nothing to count
                slot->res = -static_cast<int>(err);
                slot->done = true;
                --expected_packets;
                LOGIFACE_LOG(warn,
                    std::string("FileLoader: overlapped read failed: Win32 error ")
                    + std::to_string(err) + " — resolving as EOF (Default parity)");
            }
            else if (ovp != nullptr)
            {
                auto* slot = OverlappedPort::OvpToSlot(ovp);
                if (slot->delivered->exchange(true, std::memory_order_acq_rel))
                    continue; // already claimed inline — duplicate packet
                slot->res = static_cast<int>(bytes);
                slot->done = true;
                --expected_packets;
            }
            else
            {
                break; // null-overlap packet — unexpected wake; exit worker
            }
        }
        // expected_packets == 0 → loop; the admit phase (Pop) and rate gates
        // handle all waiting.
    }
}

} // namespace FileLoader

#endif // _WIN32
