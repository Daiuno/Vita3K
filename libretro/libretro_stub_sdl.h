// Vita3K emulator project - SDL3 stub header
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Declares the handful of entry points our SDL3 replacement exposes to the
// rest of the libretro core.  The actual body is in libretro_stub_sdl.cpp.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void libretro_stub_sdl_init(void);
void libretro_stub_sdl_deinit(void);
int  libretro_stub_sdl_is_initialized(void);

#ifdef __cplusplus
}
#endif
