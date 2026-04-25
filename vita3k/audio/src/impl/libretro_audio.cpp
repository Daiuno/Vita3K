// Vita3K emulator project - libretro audio adapter
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef LIBRETRO

#include "audio/impl/libretro_audio.h"

#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// Shared sink pointer.  Producers (guest audio threads) load it with
// memory_order_acquire; the libretro core registers/clears it from the
// frontend's main thread with memory_order_release.  Acquire/release pair
// is sufficient — we never need full sequential consistency here.
std::atomic<LibretroAudioSinkFn> g_sink{ nullptr };

// Output target: libretro canonical "interleaved S16 stereo @ 48 kHz".
constexpr int kOutChannels = 2;
constexpr int kOutFreq     = 48000;

// Hard cap on the temporary stereo buffer used per audio_output() call.
// 8192 frames is 170 ms at 48 kHz — well above any realistic single-port
// burst (Vita SceAudio MAIN slices are typically 256/512/1024 samples).
constexpr std::size_t kMaxConvertFramesPerCall = 8192;

void emit(const int16_t *frames, std::size_t num_frames) {
    if (num_frames == 0)
        return;
    LibretroAudioSinkFn sink = g_sink.load(std::memory_order_acquire);
    if (!sink)
        return;
    sink(frames, num_frames);
}

// Interleave + clamp helper for the saturating add when applying volume.
inline int16_t clamp_to_s16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

} // namespace

extern "C" void audio_libretro_set_sink(LibretroAudioSinkFn sink) {
    g_sink.store(sink, std::memory_order_release);
}

LibretroAudioAdapter::LibretroAudioAdapter(AudioState &audio_state)
    : AudioAdapter(audio_state) {}

bool LibretroAudioAdapter::init() {
    LOG_INFO("LibretroAudioAdapter: ready (output=48 kHz stereo S16)");
    return true;
}

AudioOutPortPtr LibretroAudioAdapter::open_port(int nb_channels, int freq, int nb_sample) {
    auto port = std::make_shared<AudioOutPort>();
    port->volume           = 1.0f;
    port->len_bytes        = nb_sample * nb_channels * static_cast<int>(sizeof(int16_t));
    // Same throttling formula as the SDL backend so AudioState::audio_output
    // can keep pacing the guest sceAudioOutOutput caller.
    port->len_microseconds = (static_cast<uint64_t>(nb_sample) * 1'000'000ULL)
                             / static_cast<uint64_t>(std::max(1, freq));
    return port;
}

void LibretroAudioAdapter::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (!buffer || out_port.len_bytes <= 0)
        return;

    // Quick exit — no sink registered yet (e.g. during retro_init's audio
    // boot before libretro_audio_init() ran).  Drop silently; the guest
    // thread is still throttled by AudioState::audio_output's sleep.
    if (!g_sink.load(std::memory_order_acquire))
        return;

    // Decode port format from the AudioOutPort fields populated by the
    // SceAudio module (channels = mode==MONO?1:2, freq, len = nb_sample).
    const int   src_channels = (out_port.mode == 1 /* SCE_AUDIO_OUT_MODE_MONO */) ? 1 : 2;
    const int   src_freq     = out_port.freq > 0 ? out_port.freq : kOutFreq;
    const int   src_frames   = out_port.len;        // already "frames", i.e. samples-per-channel
    if (src_frames <= 0)
        return;

    const int16_t *src = static_cast<const int16_t *>(buffer);
    const float    vol = std::clamp(out_port.volume, 0.0f, 4.0f); // allow gentle gain

    // Fast path A: source already matches output (48 kHz stereo).
    if (src_channels == 2 && src_freq == kOutFreq && std::abs(vol - 1.0f) < 1e-4f) {
        emit(src, static_cast<std::size_t>(src_frames));
        return;
    }

    // Fast path B: 48 kHz stereo with non-unity volume — single-pass scale.
    if (src_channels == 2 && src_freq == kOutFreq) {
        std::vector<int16_t> tmp(static_cast<std::size_t>(src_frames) * 2);
        for (int i = 0; i < src_frames * 2; ++i) {
            tmp[i] = clamp_to_s16(static_cast<int32_t>(src[i] * vol));
        }
        emit(tmp.data(), static_cast<std::size_t>(src_frames));
        return;
    }

    // General path: mono->stereo + linear resampling to kOutFreq.
    //
    // out_frames = round(src_frames * kOutFreq / src_freq)
    const double  ratio_in_per_out = static_cast<double>(src_freq) / static_cast<double>(kOutFreq);
    std::size_t   out_frames       = static_cast<std::size_t>(
        (static_cast<double>(src_frames) * kOutFreq) / static_cast<double>(src_freq) + 0.5);
    if (out_frames == 0)
        return;
    out_frames = std::min(out_frames, kMaxConvertFramesPerCall);

    std::vector<int16_t> tmp(out_frames * 2);

    auto fetch = [src, src_channels, src_frames](int frame_idx, int16_t *l, int16_t *r) {
        if (frame_idx < 0)            frame_idx = 0;
        if (frame_idx >= src_frames)  frame_idx = src_frames - 1;
        if (src_channels == 1) {
            const int16_t s = src[frame_idx];
            *l = s; *r = s;
        } else {
            *l = src[frame_idx * 2 + 0];
            *r = src[frame_idx * 2 + 1];
        }
    };

    for (std::size_t i = 0; i < out_frames; ++i) {
        const double  src_pos    = static_cast<double>(i) * ratio_in_per_out;
        const int     idx0       = static_cast<int>(src_pos);
        const int     idx1       = idx0 + 1;
        const double  frac       = src_pos - static_cast<double>(idx0);

        int16_t l0, r0, l1, r1;
        fetch(idx0, &l0, &r0);
        fetch(idx1, &l1, &r1);

        const double l = (l0 * (1.0 - frac) + l1 * frac) * vol;
        const double r = (r0 * (1.0 - frac) + r1 * frac) * vol;

        tmp[i * 2 + 0] = clamp_to_s16(static_cast<int32_t>(l));
        tmp[i * 2 + 1] = clamp_to_s16(static_cast<int32_t>(r));
    }
    emit(tmp.data(), out_frames);
}

void LibretroAudioAdapter::set_volume(AudioOutPort &out_port, float volume) {
    // AudioState::set_volume passes us `volume * global_volume`; we apply
    // it inside audio_output() where the sample data is.  Just stash it.
    out_port.volume = volume;
}

void LibretroAudioAdapter::switch_state(const bool /*pause*/) {
    // Pause/resume is owned by the libretro frontend (RetroArch's UI).
    // The adapter has no per-device handle to pause, and silencing the
    // sink would cut off pause-menu music in many games.  No-op.
}

int LibretroAudioAdapter::get_rest_sample(AudioOutPort &out_port) {
    // Returned by sceAudioOutGetRestSample().  We do not maintain a
    // per-port host queue depth here — the libretro core's ring buffer
    // is opaque to us — so report 0 to tell the guest "host is keeping
    // up", matching the behaviour the SDL backend reports right after
    // a fresh submission.  Refining this requires plumbing the libretro
    // core's queue depth back through audio_libretro_*; deferred to M11.
    (void)out_port;
    return 0;
}

#endif // LIBRETRO
