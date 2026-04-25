// Vita3K emulator project - SDL3 stub for libretro
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Vita3K's engine uses SDL3 internally for windowing, audio, joystick and
// camera.  Under libretro those responsibilities are owned by the frontend
// (RetroArch), so we neutralise the SDL-facing pieces inside Vita3K and
// route the interesting signals through libretro callbacks instead.
//
// For M3 we only toggle lifecycle; real SDL replacement symbols land in M4
// alongside the engine boot work.

#include "libretro_stub_sdl.h"

#include <atomic>

namespace {
    std::atomic<bool> g_initialized{false};
}

extern "C" void libretro_stub_sdl_init(void) {
    // Idempotent — retro_init is re-entrant on some frontends when the core
    // is loaded twice in a session.
    g_initialized.store(true, std::memory_order_release);
}

extern "C" void libretro_stub_sdl_deinit(void) {
    g_initialized.store(false, std::memory_order_release);
}

extern "C" int libretro_stub_sdl_is_initialized(void) {
    return g_initialized.load(std::memory_order_acquire) ? 1 : 0;
}
