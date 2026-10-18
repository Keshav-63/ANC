// Phase 3 test suite: ring buffer correctness.
//
// - FIFO ordering and wrap-around
// - full/empty short writes and short reads
// - SPSC integrity under concurrent producer/consumer threads
// - monotonic counters used by the latency estimator

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/ring_buffer.h"
#include "test_framework.h"

ANCTEST(ring_buffer_basic_fifo_and_wrap) {
    anc::SpscRingBuffer<int> ring(8);
    ctx.expect(ring.capacity() == 8, "capacity rounds to power of two");

    // Fill exactly to capacity.
    int in[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    ctx.expect(ring.write(in, 8) == 8, "full write accepted");
    ctx.expect(ring.availableToWrite() == 0, "no room when full");
    ctx.expect(ring.write(in, 1) == 0, "write into a full ring is refused");

    // Drain half, write more (forces wrap-around), drain all.
    int out[8] = {0};
    ctx.expect(ring.read(out, 4) == 4, "partial read");
    for (int i = 0; i < 4; ++i) ctx.expect(out[i] == i, "FIFO order (first half)");

    int more[4] = {100, 101, 102, 103};
    ctx.expect(ring.write(more, 4) == 4, "write after wrap-around");
    ctx.expect(ring.read(out, 8) == 8, "full drain");
    for (int i = 0; i < 4; ++i) ctx.expect(out[i] == 4 + i, "FIFO order (wrapped part 1)");
    for (int i = 0; i < 4; ++i) ctx.expect(out[4 + i] == 100 + i, "FIFO order (wrapped part 2)");

    // Empty ring behaviours.
    ctx.expect(ring.read(out, 1) == 0, "read from an empty ring returns 0");
    ctx.expect(ring.availableToRead() == 0, "availableToRead is 0");
    ctx.expect(ring.written() == 12 && ring.readTotal() == 12, "monotonic counters agree");
}

ANCTEST(ring_buffer_spsc_stress) {
    // Producer writes a monotonic sequence; consumer verifies integrity.
    constexpr size_t kTotal = 2'000'000;
    anc::SpscRingBuffer<uint32_t> ring(1024);

    std::atomic<bool> producerDone{false};
    std::atomic<uint64_t> overflows{0};

    std::thread producer([&] {
        uint32_t next = 0;
        while (next < kTotal) {
            const uint32_t chunk = 1 + (next % 97);
            uint32_t block[128];
            const uint32_t n = chunk < 128 ? chunk : 127;
            for (uint32_t i = 0; i < n && next + i < kTotal; ++i) block[i] = next + i;
            const uint32_t count = (next + n <= kTotal) ? n : static_cast<uint32_t>(kTotal - next);
            const size_t written = ring.write(block, count);
            if (written < count) overflows.fetch_add(count - written);
            next += static_cast<uint32_t>(written);
        }
        producerDone.store(true);
    });

    uint64_t received = 0;
    bool corrupted = false;
    std::thread consumer([&] {
        uint32_t out[256];
        while (received < kTotal || !producerDone.load()) {
            const size_t n = ring.read(out, 256);
            for (size_t i = 0; i < n; ++i) {
                if (out[i] != static_cast<uint32_t>(received + i)) corrupted = true;
            }
            received += n;
            if (n == 0 && producerDone.load() && ring.availableToRead() == 0) break;
        }
        // Final drain.
        size_t n;
        while ((n = ring.read(out, 256)) > 0) {
            for (size_t i = 0; i < n; ++i) {
                if (out[i] != static_cast<uint32_t>(received + i)) corrupted = true;
            }
            received += n;
        }
    });

    producer.join();
    consumer.join();

    ctx.expect(!corrupted, "no data corruption or reordering over 2M samples");
    ctx.expect(received == kTotal, "all samples received");
    ctx.expect(overflows.load() == 0, "producer-gated writes never overflowed");
}

ANCTEST(ring_buffer_float_audio_pattern) {
    // Audio-like usage: fixed block writes with jittered reads.
    constexpr size_t kFrames = 480'000;
    anc::SpscRingBuffer<float> ring(16384);
    std::atomic<uint64_t> dropped{0};
    std::atomic<bool> producerDone{false};

    std::thread producer([&] {
        size_t written = 0;
        float block[512];
        while (written < kFrames) {
            const size_t n = std::min<size_t>(240, kFrames - written);
            for (size_t i = 0; i < n; ++i) block[i] = static_cast<float>(written + i);
            const size_t ok = ring.write(block, n);
            if (ok < n) dropped.fetch_add(n - ok);
            written += ok;
        }
        producerDone.store(true);
    });

    std::thread consumer([&] {
        std::vector<float> out(1024);
        uint64_t expected = 0;
        bool ok = true;
        while (!producerDone.load() || ring.availableToRead() > 0) {
            const size_t n = ring.read(out.data(), 480);
            for (size_t i = 0; i < n; ++i) {
                if (out[i] != static_cast<float>(expected + i)) ok = false;
            }
            expected += n;
        }
        ctx.expect(ok, "float stream integrity");
        ctx.expect(expected == kFrames - dropped.load(), "read count matches written count");
    });

    producer.join();
    consumer.join();
    ctx.expect(dropped.load() == 0, "no frames dropped with a 16384-frame ring");
}
