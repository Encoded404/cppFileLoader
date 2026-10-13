module;

#include <logging/logging_macros.hpp>

#ifndef CPPFILELOADER_USE_LOGIFACE_MODULE
#include <logging/logging.hpp>
#endif

#if defined(__linux__)
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

#ifdef CPPFILELOADER_USE_STD_MODULE
#else
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
#include <iterator>
#include <exception>
#include <utility>
#include <string>
#include <thread>
#include <vector>
#include <system_error>
#include <functional>
#endif

#ifndef CPPFILELOADER_USE_LOGIFACE_MODULE
#include <logging/logging.hpp>
#endif

export module FileLoader.IoUringFileReadStrategy;

#ifdef CPPFILELOADER_USE_STD_MODULE
import std;
#endif

#ifdef CPPFILELOADER_USE_LOGIFACE_MODULE
import logiface;
#endif

import FileLoader.Types;
import FileLoader.IncrementalBuffer;
import FileLoader.IFileReadStrategy;
import FileLoader.IOQueue;
import FileLoader.DynamicThreadPool;

export namespace FileLoader
{

#if defined(__linux__)

/// Read strategy that issues file reads through Linux io_uring, via raw
/// syscalls (no liburing dependency). One ring per IO-worker thread; chunks
/// are pipelined per request (up to Config.pipeline_depth in flight per file)
/// so even a single worker keeps real queue depth in flight for bulk
/// sequential loads.
///
/// IOQueue semantics are preserved exactly: priority heap ordering, ready_at
/// gating, rate-limited chunk pacing (rate-limited loads read one chunk at a
/// time and re-enter the queue between chunks, exactly like
/// DefaultFileReadStrategy), and cancellation with partial-data resolution at
/// the current chunk boundary.
///
/// Kernel requirement: io_uring with IORING_OP_READ (Linux ≥ 5.6 in
/// practice; probe with IsAvailable()). The FileManager platform factory
/// falls back to DefaultFileReadStrategy when the probe fails.
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

// --- Implementations ---------------------------------------------------------

/// A chunk submitted to the ring that has not been consumed at its offset yet.
struct IoUringFileReadStrategy::ChunkSlot
{
    ActiveRequest* req = nullptr;          // owning request (list-stable pointer)
    std::uint64_t offset = 0;
    std::size_t claimed = 0;               // bytes requested from the kernel
    int res = 0;                           // kernel result: bytes read, or -errno
    bool done = false;                     // CQE consumed by reap
    std::shared_ptr<ByteBuffer> buffer;
};

/// A request currently owned by one worker visit.
struct IoUringFileReadStrategy::ActiveRequest
{
    IORequest req;
    std::uint64_t submit_offset = 0;       // offset of the next chunk to submit
    std::uint64_t frontier = 0;            // offset of the next chunk to append
    std::map<std::uint64_t, ChunkSlot*> pending;  // offset -> slot, ordered
    std::chrono::steady_clock::time_point next_ready{};
    bool resolved = false;                 // promise settled (zombie until drained)
    bool returned = false;                 // handed back to the IOQueue
};

/// Minimal thread-confined io_uring wrapper (raw syscalls, no liburing).
class IoUringFileReadStrategy::UringRing
{
public:
    UringRing() = default;
    ~UringRing() { Teardown(); }

    UringRing(const UringRing&) = delete;
    UringRing& operator=(const UringRing&) = delete;

    [[nodiscard]] bool Init(unsigned entries)
    {
        Teardown();

        io_uring_params params{};
        const int fd = static_cast<int>(
            ::syscall(__NR_io_uring_setup, entries, &params));
        if (fd < 0)
            return false;
        ring_fd_ = fd;
        entries_ = entries;

        const std::size_t sq_size =
            params.sq_off.array + entries * sizeof(std::uint32_t);
        const std::size_t cq_size =
            params.cq_off.cqes + entries * sizeof(io_uring_cqe);
        const std::size_t sqes_size = entries * sizeof(io_uring_sqe);

        sq_ring_ = static_cast<std::uint8_t*>(::mmap(
            nullptr, sq_size, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING));
        cq_ring_ = static_cast<std::uint8_t*>(::mmap(
            nullptr, cq_size, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING));
        sqes_ = static_cast<io_uring_sqe*>(::mmap(
            nullptr, sqes_size, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES));

        if (sq_ring_ == MAP_FAILED || cq_ring_ == MAP_FAILED || sqes_ == MAP_FAILED)
        {
            Teardown();
            return false;
        }
        sq_ring_size_ = sq_size;
        cq_ring_size_ = cq_size;

        sq_head_ = Field<std::atomic<std::uint32_t>>(sq_ring_, params.sq_off.head);
        sq_tail_ = Field<std::atomic<std::uint32_t>>(sq_ring_, params.sq_off.tail);
        sq_mask_ = Field<std::atomic<std::uint32_t>>(sq_ring_, params.sq_off.ring_mask);
        sq_array_ = reinterpret_cast<std::uint32_t*>(sq_ring_ + params.sq_off.array);
        cq_head_ = Field<std::atomic<std::uint32_t>>(cq_ring_, params.cq_off.head);
        cq_tail_ = Field<std::atomic<std::uint32_t>>(cq_ring_, params.cq_off.tail);
        cq_mask_ = Field<std::atomic<std::uint32_t>>(cq_ring_, params.cq_off.ring_mask);
        cqes_ = reinterpret_cast<io_uring_cqe*>(cq_ring_ + params.cq_off.cqes);
        return true;
    }

    [[nodiscard]] bool Valid() const noexcept { return ring_fd_ >= 0; }
    [[nodiscard]] bool HasRoom() const noexcept { return inflight_ < entries_; }
    [[nodiscard]] unsigned InFlight() const noexcept { return inflight_; }

    /// Queue a read SQE. Returns false when the ring is full or invalid.
    bool SubmitRead(int fd, std::uint64_t offset, void* buffer,
                    std::uint32_t len, std::uint64_t user_data) noexcept
    {
        if (ring_fd_ < 0 || inflight_ >= entries_)
            return false;

        const std::uint32_t tail = sq_tail_->load(std::memory_order_relaxed);
        const std::uint32_t idx = tail & sq_mask_->load(std::memory_order_relaxed);

        io_uring_sqe* sqe = &sqes_[idx];
        std::memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_READ;
        sqe->fd = fd;
        sqe->off = offset;
        sqe->addr = reinterpret_cast<std::uintptr_t>(buffer);
        sqe->len = len;
        sqe->user_data = user_data;

        sq_array_[idx] = idx;
        sq_tail_->store(tail + 1, std::memory_order_release);
        ++inflight_;
        ++unflushed_; // the kernel sees the new SQE on the next enter(to_submit)
        return true;
    }

    /// Flush all queued SQEs and block until at least `min_complete`
    /// completions are consumable. Returns false on kernel error; EINTR is
    /// retried internally (the unflushed SQEs are retried with it).
    bool Wait(std::uint32_t min_complete)
    {
        while (true)
        {
            const std::uint32_t to_submit = std::exchange(unflushed_, 0u);
            if (to_submit == 0 && inflight_ == 0)
                return true;
            const long rc = ::syscall(__NR_io_uring_enter, ring_fd_,
                                      to_submit, min_complete, IORING_ENTER_GETEVENTS,
                                      nullptr, 0);
            if (rc < 0 && errno == EINTR)
                unflushed_ += to_submit; // re-submit on retry
            if (rc >= 0)
                return true;
            if (errno == EINTR)
                continue;
            return false;
        }
    }

    /// Drain all available CQEs, invoking on_cqe(user_data, res) per entry.
    /// `res` is the byte count, or -errno on kernel-reported failure.
    template <typename Fn>
    void Reap(Fn&& on_cqe)
    {
        for (;;)
        {
            const std::uint32_t head = cq_head_->load(std::memory_order_acquire);
            const std::uint32_t tail = cq_tail_->load(std::memory_order_acquire);
            if (head == tail)
                break;

            const io_uring_cqe* cqe =
                &cqes_[head & cq_mask_->load(std::memory_order_relaxed)];
            on_cqe(cqe->user_data, cqe->res);
            cq_head_->store(head + 1, std::memory_order_release);
            --inflight_;
        }
    }

private:
    template <typename T>
    static T* Field(std::uint8_t* base, std::uint32_t offset) noexcept
    {
        return reinterpret_cast<T*>(base + offset);
    }

    void Teardown() noexcept
    {
        if (sq_ring_)
            ::munmap(sq_ring_, sq_ring_size_);
        if (cq_ring_)
            ::munmap(cq_ring_, cq_ring_size_);
        if (sqes_)
            ::munmap(sqes_, static_cast<std::size_t>(entries_) * sizeof(io_uring_sqe));
        sq_ring_ = nullptr;
        cq_ring_ = nullptr;
        sqes_ = nullptr;
        sq_ring_size_ = 0;
        cq_ring_size_ = 0;
        entries_ = 0;
        inflight_ = 0;
        if (ring_fd_ >= 0)
            ::close(ring_fd_);
        ring_fd_ = -1;
    }

    int ring_fd_ = -1;
    unsigned entries_ = 0;
    unsigned inflight_ = 0;
    std::uint32_t unflushed_ = 0;

    std::uint8_t* sq_ring_ = nullptr;
    std::uint8_t* cq_ring_ = nullptr;
    io_uring_sqe* sqes_ = nullptr;
    std::size_t sq_ring_size_ = 0;
    std::size_t cq_ring_size_ = 0;

    std::atomic<std::uint32_t>* sq_head_ = nullptr;
    std::atomic<std::uint32_t>* sq_tail_ = nullptr;
    std::atomic<std::uint32_t>* sq_mask_ = nullptr;
    std::atomic<std::uint32_t>* cq_head_ = nullptr;
    std::atomic<std::uint32_t>* cq_tail_ = nullptr;
    std::atomic<std::uint32_t>* cq_mask_ = nullptr;
    std::uint32_t* sq_array_ = nullptr;
    io_uring_cqe* cqes_ = nullptr;
};


// (module-linkage helpers; non-exported inside exported namespace)

bool UringIsCancelled(const IORequest& req)
{
    return req.cancel.cancelled &&
           req.cancel.cancelled->load(std::memory_order_acquire);
}

unsigned RoundUpPow2(unsigned v) noexcept
{
    unsigned p = 8;
    while (p < v)
        p *= 2;
    return p;
}

/// Close a request's handle (if the native strategy opened one) and discard
/// the cancelled request exactly like IOQueue::DiscardCancelled.
void UringDiscardCancelledNative(IORequest& req)
{
    if (req.os_handle >= 0)
    {
        ::close(static_cast<int>(req.os_handle));
        req.os_handle = -1;
    }
    IOQueue::DiscardCancelled(std::move(req));
}


bool IoUringFileReadStrategy::IsAvailable()
{
    static const bool available = []() noexcept {
        UringRing probe;
        return probe.Init(8);
    }();
    return available;
}

IoUringFileReadStrategy::IoUringFileReadStrategy()
    : IoUringFileReadStrategy(Config{})
{
}

IoUringFileReadStrategy::IoUringFileReadStrategy(Config cfg)
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

IoUringFileReadStrategy::~IoUringFileReadStrategy()
{
    io_queue_->Stop();
}

void IoUringFileReadStrategy::Post(WorkItem work)
{
    post_pool_->Post(std::move(work));
}

std::future<std::shared_ptr<ByteBuffer>> IoUringFileReadStrategy::ReadFull(
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

std::future<void> IoUringFileReadStrategy::ReadStreaming(
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


void IoUringFileReadStrategy::WorkerLoop()
{
    UringRing ring;
    const unsigned entries = RoundUpPow2(static_cast<unsigned>(
        config_.submission_depth + config_.pipeline_depth + 8));
    if (!ring.Init(entries))
    {
        LOGIFACE_LOG(warn,
            "FileLoader: io_uring ring setup failed in worker; strategy thread exiting");
        return;
    }

    std::list<ActiveRequest> active;

    /// Resolve a request's promise the way DefaultFileReadStrategy does:
    /// Full → value (partial-if-present, else empty — exceptions only for
    /// open failure), Streaming → clean close of the stream.
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

    /// Fail an active request with an exception, preserving partial data for
    /// Full reads (mirrors the DefaultFileReadStrategy catch block).
    const auto fail = [](ActiveRequest& a, const std::exception_ptr& eptr) {
        if (a.req.type == IORequest::Type::Full)
        {
            if (a.req.partial_buffer && !a.req.partial_buffer->empty())
                a.req.full_promise.set_value(std::move(a.req.partial_buffer));
            else
                a.req.full_promise.set_exception(eptr);
        }
        else
        {
            if (a.req.stream_output)
                a.req.stream_output->Cancel();
            a.req.stream_promise.set_exception(eptr);
        }
        a.resolved = true;
    };

    /// Close the request handle once its slot list has fully drained.
    const auto close_handle = [](ActiveRequest& a) {
        if (a.req.os_handle >= 0)
        {
            ::close(static_cast<int>(a.req.os_handle));
            a.req.os_handle = -1;
        }
    };

    while (true)
    {
        // ---- reap: mark all delivered slots done -----------------------------
        ring.Reap([](std::uint64_t user_data, int res) {
            auto* slot = reinterpret_cast<ChunkSlot*>(user_data);
            slot->res = res;
            slot->done = true;
        });

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

            bool gated = false;
            while (!a.pending.empty() && !a.resolved)
            {
                auto begin = a.pending.begin();
                if (!begin->second->done || begin->first != a.frontier)
                    break; // oldest chunk not delivered yet (or gap — no overlap)

                ChunkSlot* slot = begin->second;
                const int res = slot->res;
                const std::size_t claimed = slot->claimed;
                auto buffer = std::move(slot->buffer);
                delete slot;
                a.pending.erase(begin);

                if (res <= 0)
                {
                    // res == 0 → EOF; res < 0 → kernel read error.
                    // DefaultFileReadStrategy maps both to a clean resolve
                    // (its ifstream path does not throw on read errors).
                    if (res < 0)
                    {
                        LOGIFACE_LOG(warn,
                            std::string("FileLoader: io_uring read failed at offset ")
                            + std::to_string(begin->first) + " of '"
                            + a.req.path.string()
                            + "' — resolving as EOF (Default parity)");
                    }
                    finish(a);
                    break;
                }

                buffer->resize(static_cast<std::size_t>(res));

                // Append the chunk
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
                        // Consumer cancelled/closed the stream — resolve cleanly
                        finish(a);
                        break;
                    }
                }
                a.req.bytes_read += static_cast<std::uint64_t>(res);
                a.frontier += claimed;

                // Cancellation mid-read → partial-data resolution (parity)
                if (UringIsCancelled(a.req))
                {
                    finish(a);
                    break;
                }

                // Rate gate: one chunk at a time, then hand the request back to
                // the IOQueue with a delay — DefaultFileReadStrategy behavior.
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

                // EOF: a regular-file short read means end of file
                if (res < static_cast<int>(claimed))
                {
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
                    it = active.erase(it); // handle travels with the request
                }
                else
                {
                    ++it; // fully drained on a later reap pass
                }
                continue;
            }

            if (gated)
            {
                ++it;
                continue;
            }

            ++it;
        }

        // ---- admit phase -----------------------------------------------------
        {
            const bool can_admit =
                ring.HasRoom() &&
                active.size() < config_.submission_depth;
            if (can_admit)
            {
                std::optional<IORequest> next;
                if (active.empty() && ring.InFlight() == 0)
                {
                    next = io_queue_->Pop(); // blocking; handles ready_at + arrivals
                    if (!next)
                        return; // queue stopped and drained
                }
                else
                {
                    next = io_queue_->TryPop();
                }

                if (next)
                {
                    if (UringIsCancelled(*next))
                    {
                        UringDiscardCancelledNative(*next);
                    }
                    else
                    {
                        if (next->os_handle < 0)
                        {
                            next->os_handle = ::open(next->path.c_str(), O_RDONLY | O_CLOEXEC);
                            if (next->os_handle < 0)
                            {
                                const int ec = errno;
                                auto eptr = std::make_exception_ptr(
                                    std::system_error(
                                        ec, std::generic_category(),
                                        "Failed to open file: " + next->path.string()));
                                next->full_promise.set_exception(eptr);
                                next->stream_promise.set_exception(std::move(eptr));
                            }
                        }
                        if (next->os_handle >= 0)
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

        // ---- submit phase ------------------------------------------------------
        for (auto& a : active)
        {
            if (a.resolved || a.returned || a.req.os_handle < 0)
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
                   a.pending.size() < max_pending && ring.HasRoom())
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

                if (!ring.SubmitRead(static_cast<int>(a.req.os_handle),
                                     slot->offset, slot->buffer->data(),
                                     static_cast<std::uint32_t>(slot->claimed),
                                     reinterpret_cast<std::uint64_t>(slot)))
                {
                    delete slot;
                    break;
                }
                a.pending[a.submit_offset] = slot;
                a.submit_offset += chunk_bytes;
            }
        }

        // ---- wait phase ----------------------------------------------------------
        if (ring.InFlight() > 0)
        {
            if (!ring.Wait(1))
            {
                // Ring failure surfaced from the kernel side: fail loudly.
                const auto eptr = std::make_exception_ptr(std::system_error(
                    EIO, std::generic_category(), "io_uring ring failure"));
                for (auto& a : active)
                {
                    for (auto& entry : a.pending)
                    {
                        delete entry.second;
                    }
                    a.pending.clear();
                    if (!a.resolved && !a.returned)
                        fail(a, eptr);
                }
                return;
            }
        }
        // InFlight() == 0 → loop; the admit phase (Pop) and rate gates handle
        // all waiting.
    }
}

} // namespace FileLoader

#else

// This strategy is Linux-only; other targets use DefaultFileReadStrategy.

#endif // __linux__
