// Vita3K emulator project - libretro audio sink
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "libretro_audio.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <audio/impl/libretro_audio.h>

namespace {

// Output canon — must agree with LibretroAudioAdapter and with the
// timing.sample_rate value advertised in retro_get_system_av_info.
constexpr std::size_t kFrameBytes  = 2 /*ch*/ * sizeof(int16_t);

// Ring capacity in *frames* (interleaved stereo S16, so one frame is
// 4 bytes).  16384 frames = ~340 ms at 48 kHz, big enough to absorb
// a couple of dropped retro_run ticks (~6 frames worth) without
// wraparound, but small enough that a stalled drain does not balloon
// the working set (16 384 * 4 B = 64 KiB).
constexpr std::size_t kRingFrames  = 16384;

// We use a classic mutex-protected ring rather than lock-free SPSC
// because the producer side is multi-threaded (any guest thread can
// be the one running sceAudioOutOutput) and uncontended mutexes on
// modern arm64 are a few nanoseconds — negligible next to the 20+ ms
// of host audio frame budget.
struct RingBuffer {
    std::mutex             mu;
    std::vector<int16_t>   data;        // size = kRingFrames * 2
    std::size_t            head = 0;    // read cursor (in samples, NOT frames)
    std::size_t            tail = 0;    // write cursor (in samples)
    std::size_t            level = 0;   // queued samples (head + level == tail mod cap)
    std::atomic<uint64_t>  drops{ 0 };  // overrun stat (frames discarded)
};

RingBuffer g_ring;

inline std::size_t ring_capacity_samples() { return kRingFrames * 2; }

void ring_init() {
    std::lock_guard<std::mutex> lock(g_ring.mu);
    g_ring.data.assign(ring_capacity_samples(), 0);
    g_ring.head  = 0;
    g_ring.tail  = 0;
    g_ring.level = 0;
}

void ring_clear() {
    std::lock_guard<std::mutex> lock(g_ring.mu);
    g_ring.head  = 0;
    g_ring.tail  = 0;
    g_ring.level = 0;
}

// Producer side (called from guest audio threads via the registered sink).
void ring_push(const int16_t *interleaved_stereo, std::size_t frames) {
    if (frames == 0)
        return;

    std::lock_guard<std::mutex> lock(g_ring.mu);
    if (g_ring.data.empty())
        return; // not initialised; treat as no sink

    const std::size_t cap   = ring_capacity_samples();
    const std::size_t want  = frames * 2;            // samples to write
    const std::size_t avail = cap - g_ring.level;    // samples free

    if (want > avail) {
        // Overrun: drop oldest enough to fit the newest data.  Newer
        // samples are perceptually more important and we never want to
        // crash or block the guest thread here.
        const std::size_t need_to_drop = want - avail;
        g_ring.head   = (g_ring.head + need_to_drop) % cap;
        g_ring.level -= need_to_drop;
        g_ring.drops.fetch_add(need_to_drop / 2, std::memory_order_relaxed);
    }

    // Two-segment copy to handle wrap.
    const std::size_t first = std::min(want, cap - g_ring.tail);
    std::memcpy(g_ring.data.data() + g_ring.tail, interleaved_stereo,
                first * sizeof(int16_t));
    if (want > first) {
        std::memcpy(g_ring.data.data(),
                    interleaved_stereo + first,
                    (want - first) * sizeof(int16_t));
    }
    g_ring.tail   = (g_ring.tail + want) % cap;
    g_ring.level += want;
}

// Consumer side (called only from retro_run).
std::size_t ring_pop(int16_t *out, std::size_t max_frames) {
    std::lock_guard<std::mutex> lock(g_ring.mu);
    if (g_ring.data.empty() || g_ring.level == 0)
        return 0;

    const std::size_t cap        = ring_capacity_samples();
    const std::size_t avail_frm  = g_ring.level / 2;
    const std::size_t take_frm   = std::min(max_frames, avail_frm);
    const std::size_t take_smp   = take_frm * 2;

    const std::size_t first = std::min(take_smp, cap - g_ring.head);
    std::memcpy(out, g_ring.data.data() + g_ring.head, first * sizeof(int16_t));
    if (take_smp > first) {
        std::memcpy(out + first, g_ring.data.data(),
                    (take_smp - first) * sizeof(int16_t));
    }
    g_ring.head   = (g_ring.head + take_smp) % cap;
    g_ring.level -= take_smp;
    return take_frm;
}

// Stable, non-atomic batch callback owned by libretro.cpp.  Stored under
// a separate atomic so the producer thread (via the sink) does not need
// to take the ring mutex just to check whether forwarding is worthwhile.
std::atomic<retro_audio_sample_batch_t> g_batch_cb{ nullptr };

// The sink we register with the audio module is just a thin trampoline
// that pushes into the ring buffer.  Producer code (guest thread) does
// not need to know about libretro types.
void sink_push(const int16_t *frames, std::size_t num_frames) {
    if (!g_batch_cb.load(std::memory_order_acquire))
        return; // nothing to do; let the ring stay empty
    ring_push(frames, num_frames);
}

std::atomic<bool> g_initialised{ false };

} // namespace

extern "C" void libretro_audio_set_batch_cb(retro_audio_sample_batch_t cb) {
    g_batch_cb.store(cb, std::memory_order_release);
}

extern "C" void libretro_audio_init(void) {
    if (g_initialised.exchange(true, std::memory_order_acq_rel))
        return; // already initialised
    ring_init();
    // Plug ourselves into the engine-side adapter.  After this call any
    // sceAudioOutOutput from the guest will land in our ring buffer.
    audio_libretro_set_sink(&sink_push);
}

extern "C" void libretro_audio_deinit(void) {
    if (!g_initialised.exchange(false, std::memory_order_acq_rel))
        return;
    audio_libretro_set_sink(nullptr);
    ring_clear();
    g_batch_cb.store(nullptr, std::memory_order_release);
}

extern "C" void libretro_audio_drain(void) {
    retro_audio_sample_batch_t cb = g_batch_cb.load(std::memory_order_acquire);
    if (!cb)
        return;

    // Drain in moderately sized chunks.  Keeping the working buffer
    // small ensures we stay on stack and never call the frontend with
    // an unbounded batch (some frontends limit to 4096 frames per call).
    constexpr std::size_t kChunkFrames = 1024;
    int16_t scratch[kChunkFrames * 2];

    for (;;) {
        const std::size_t got = ring_pop(scratch, kChunkFrames);
        if (got == 0)
            break;
        // retro_audio_sample_batch_t signature returns "frames consumed";
        // libretro contract says the frontend always consumes everything,
        // so we ignore the return value (cores like ppsspp do the same).
        cb(scratch, got);
        if (got < kChunkFrames)
            break; // buffer drained
    }
}
