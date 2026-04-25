// Vita3K emulator project - libretro core internal shared state
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tiny header only exposes what the install / catalog / future modules
// need from libretro.cpp's TU.  Keeps global state concentrated without
// pulling the full libretro.h into every worker file.

#pragma once

#include <memory>
#include <string>

struct EmuEnvState;

namespace vita3k_libretro_core {

// Owned by libretro.cpp.  May be null before retro_init / after retro_deinit.
extern std::unique_ptr<EmuEnvState> &emuenv();

// Writes `msg` into the ring read by retro_vita_last_error().  Pass NULL
// to clear.  Thread-safety: the libretro spec only permits these
// functions to be called from the frontend's core thread, so a bare
// std::string is sufficient.
void set_last_error(const char *msg);
void set_last_error(const std::string &msg);

// True once retro_init finished successfully and EmuEnvState was booted.
bool is_booted();

// Absolute UTF-8 path to the Vita3K pref-root (e.g.
// "<SAVE_DIR>/Vita3K/").  Points into libretro_paths.cpp's cache.
const char *pref_path_utf8();
const char *cache_path_utf8();

} // namespace vita3k_libretro_core
