// Vita3K emulator project - libretro audio sink
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// libretro-side half of the audio bridge.  Pairs with
// vita3k/audio/include/audio/impl/libretro_audio.h on the engine side.
//
// Lifecycle:
//   retro_set_audio_sample_batch(cb)  ->  libretro_audio_set_batch_cb(cb)
//   retro_init()                       ->  libretro_audio_init() (registers
//                                          ring-buffer sink with audio module)
//   guest sceAudioOutOutput            ->  LibretroAudioAdapter::audio_output
//                                          ->  ring_push (this TU)
//   retro_run()                        ->  libretro_audio_drain()
//                                          ->  audio_batch_cb(samples, frames)
//   retro_deinit()                     ->  libretro_audio_deinit() (clears sink,
//                                          drops queued samples)
//
// Format: interleaved S16 stereo @ 48 kHz throughout.

#pragma once

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wires up the ring-buffer sink with the audio module.  Safe to call
// multiple times; idempotent.  Must run before the engine creates any
// SceAudio port (i.e. before the first retro_load_game succeeds), but
// is itself synchronous.
void libretro_audio_init(void);

// Releases the sink registration with the audio module and discards any
// queued samples.  Called from retro_deinit().
void libretro_audio_deinit(void);

// Stores the libretro frontend's batch sample callback.  Called by
// retro_set_audio_sample_batch.
void libretro_audio_set_batch_cb(retro_audio_sample_batch_t cb);

// Drains the internal ring buffer in chunks and forwards them to the
// frontend's audio_sample_batch_cb.  Must be called exactly once per
// retro_run() invocation, on the same thread as retro_run.  Safe to
// call when no callback is wired (becomes a no-op).
void libretro_audio_drain(void);

#ifdef __cplusplus
} // extern "C"
#endif
