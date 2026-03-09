#include "FileLoader/IncrementalBuffer.hpp"

#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <iterator> // for std::make_move_iterator
#include <utility> // for std::move
#include <algorithm> // for std::min

FileLoader::IncrementalBuffer::IncrementalBuffer() = default;
FileLoader::IncrementalBuffer::~IncrementalBuffer() = default;

bool FileLoader::IncrementalBuffer::Push(const FileLoader::ByteBuffer& chunk)
{ 
    const std::lock_guard lock(mutex_);
    if (closed_ || cancelled_) return false;
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    size_.store(buffer_.size(), std::memory_order_release);
    cv_.notify_all();

    std::function<void(std::size_t)> cb_copy;
    { const std::lock_guard lock_cb(cb_mutex_); cb_copy = on_push_; }
    if (cb_copy) cb_copy(size_.load());
    return true;
}

bool FileLoader::IncrementalBuffer::Push(FileLoader::ByteBuffer&& chunk)
{
    const std::lock_guard lock(mutex_);
    if (closed_ || cancelled_) return false;
    buffer_.insert(buffer_.end(), std::make_move_iterator(chunk.begin()), std::make_move_iterator(chunk.end()));
    size_.store(buffer_.size(), std::memory_order_release);
    cv_.notify_all();

    std::function<void(std::size_t)> cb_copy;
    { const std::lock_guard lock_cb(cb_mutex_); cb_copy = on_push_; }
    if (cb_copy) cb_copy(size_.load());
    return true;
}

void FileLoader::IncrementalBuffer::SetOnPush(std::function<void(std::size_t)> cb) {
    const std::lock_guard lock(cb_mutex_);
    on_push_ = std::move(cb);
}

FileLoader::ByteSpan FileLoader::IncrementalBuffer::ReadSnapshot() const {
    const std::lock_guard lock(mutex_);
    return FileLoader::ByteSpan(buffer_.data(), buffer_.size());
}

FileLoader::ByteSpan FileLoader::IncrementalBuffer::ReadRange(std::size_t offset, std::size_t len) const {
    const std::lock_guard lock(mutex_);
    if (offset >= buffer_.size()) return {};
    const std::size_t count = std::min(len, buffer_.size() - offset);
    return FileLoader::ByteSpan(buffer_.data() + offset, count);
}

std::size_t FileLoader::IncrementalBuffer::Size() const {
    return size_.load(std::memory_order_acquire);
}

bool FileLoader::IncrementalBuffer::WaitForSize(std::size_t target_size, std::chrono::milliseconds timeout) {
    std::unique_lock lock(cv_mutex_);
    auto pred = [&] { return cancelled_.load() || size_.load() >= target_size || closed_; };
    if (timeout == std::chrono::milliseconds::max()) cv_.wait(lock, pred);
    else if (!cv_.wait_for(lock, timeout, pred)) return false;
    return size_.load() >= target_size;
}

void FileLoader::IncrementalBuffer::Close() {
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

void FileLoader::IncrementalBuffer::Cancel() {
    cancelled_.store(true, std::memory_order_release);
    cv_.notify_all();
}

bool FileLoader::IncrementalBuffer::IsClosed() const {
    const std::lock_guard lock(mutex_);
    return closed_;
}

bool FileLoader::IncrementalBuffer::IsCancelled() const {
    const std::lock_guard lock(mutex_);
    return cancelled_.load();
}