#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <type_traits>

class buffer_pool_buf
{
public:
    bool ready_capture() const
    {
        return released;
    }

    bool ready_encode() const
    {
        return available;
    }

    std::atomic<bool> released{true}; // if the buffer can be used to store new pending frames
    std::atomic<bool> available{false}; // if the buffer can be used to feed the encoder
};

// Fixed-size, double-buffered pool. Capture and encode alternate between the
// buffers; the capture side waits for the encoder to release a buffer instead
// of growing the pool. This keeps the capture -> encode latency bounded and
// avoids a queue of stale frames.
template <class T, int N>
class buffer_pool
{
public:
    static_assert(std::is_base_of<buffer_pool_buf, T>::value, "T must be subclass of buffer_pool_buf");
    static_assert(N >= 2, "buffer_pool needs at least two buffers");

    buffer_pool()
    {
        for (size_t i = 0; i < N; ++i) {
            bufs[i] = new T;
        }
    }

    ~buffer_pool()
    {
        for (size_t i = 0; i < N; ++i) {
            delete bufs[i];
        }
    }

    size_t size() const
    {
        return N;
    }

    const T* at(size_t i) const
    {
        return bufs[i];
    }

    // Only the capture thread calls capture()/next_capture(); only the encode
    // thread calls encode()/next_encode(). The indices are therefore owned by
    // their respective thread, and no lock is needed here.
    T& capture()
    {
        return *bufs[capture_idx];
    }

    T& encode()
    {
        return *bufs[encode_idx];
    }

    // Wait until the current capture buffer is free. Returns true if the
    // buffer is ready, false on timeout or when stop is set.
    bool wait_for_capture(const std::atomic<bool>& stop)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return stop.load(std::memory_order_relaxed) ||
                bufs[capture_idx]->ready_capture();
        });
        return !stop.load(std::memory_order_relaxed) &&
            bufs[capture_idx]->ready_capture();
    }

    // Wait until the current encode buffer contains a frame. Returns true if
    // the buffer is ready, false on timeout or when stop is set.
    bool wait_for_encode(const std::atomic<bool>& stop)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return stop.load(std::memory_order_relaxed) ||
                bufs[encode_idx]->ready_encode();
        });
        return !stop.load(std::memory_order_relaxed) &&
            bufs[encode_idx]->ready_encode();
    }

    // Signal that the current capture buffer has been successfully obtained
    // from the compositor and select the next buffer to capture in.
    T& next_capture()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            bufs[capture_idx]->released = false;
            bufs[capture_idx]->available = true;
            capture_idx = (capture_idx + 1) % N;
        }
        cv.notify_all();
        return *bufs[capture_idx];
    }

    // Signal that the encode buffer has been submitted for encoding
    // and select the next buffer for encoding.
    T& next_encode()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            bufs[encode_idx]->available = false;
            bufs[encode_idx]->released = true;
            encode_idx = (encode_idx + 1) % N;
        }
        cv.notify_all();
        return *bufs[encode_idx];
    }

    // Wake any thread blocked in wait_for_capture()/wait_for_encode().
    void notify_all()
    {
        cv.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::array<T*, N> bufs;
    size_t capture_idx = 0;
    size_t encode_idx = 0;
};
