// Vita3K emulator project - libretro audio adapter
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// LibretroAudioAdapter reroutes Vita3K's per-port PCM submissions away
// from cubeb/SDL3 and into a registered sink function that the libretro
// core drains on every retro_run().
//
// The adapter is selected at boot time when AudioState::set_backend() is
// called with the literal "Libretro" (only allowed when the build defines
// LIBRETRO).  The libretro front-end registers a sink callback via
// the C function `audio_libretro_set_sink()` declared at the bottom of
// this header before app::late_init() runs (i.e. before any Vita port
// can be opened).
//
// Output format: 48 kHz, stereo, signed 16-bit interleaved — the libretro
// canonical format.  The adapter does:
//   * mono   -> stereo expansion (sample duplication)
//   * non-48 kHz -> linear interpolation to 48 kHz
//   * volume application (per-port `volume * global_volume` already
//     reduced by AudioState::set_volume())
//
// Multi-port mixing limitation: this v1 implementation simply *appends*
// each port's converted PCM into the sink rather than additively mixing
// concurrent ports.  Real Vita games drive the host audio almost
// exclusively through SceNgs (which mixes internally and pushes a
// single MAIN port), so this is acceptable for the first iteration.
// True multi-port mixing is tracked under M11.

#pragma once

#ifdef LIBRETRO

#include <audio/state.h>

#include <cstddef>
#include <cstdint>

class LibretroAudioAdapter : public AudioAdapter {
public:
    explicit LibretroAudioAdapter(AudioState &audio_state);
    ~LibretroAudioAdapter() override = default;

    bool init() override;
    AudioOutPortPtr open_port(int nb_channels, int freq, int nb_sample) override;
    void audio_output(AudioOutPort &out_port, const void *buffer) override;
    void set_volume(AudioOutPort &out_port, float volume) override;
    void switch_state(const bool pause) override;
    int  get_rest_sample(AudioOutPort &out_port) override;
};

extern "C" {

// PCM sink signature.  Receives interleaved stereo S16 frames at 48 kHz.
// `frames` is the number of *frames* (one frame = 2 samples = 4 bytes).
//
// The sink is called from arbitrary guest threads (the same thread that
// invoked sceAudioOutOutput).  Implementations must be thread-safe.
typedef void (*LibretroAudioSinkFn)(const int16_t *interleaved_stereo_s16,
                                    std::size_t frames);

// Register the sink that LibretroAudioAdapter forwards PCM to.  Pass
// NULL to deregister (any subsequent submissions are dropped silently
// instead of crashing).  Idempotent.
void audio_libretro_set_sink(LibretroAudioSinkFn sink);

} // extern "C"

#endif // LIBRETRO
