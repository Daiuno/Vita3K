// Vita3K emulator project - libretro input bridge
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Replaces the SDL keyboard / gamepad polls inside retrieve_ctrl_data()
// with a libretro input_state_cb() query.  The standalone Vita3K frontend
// calls SDL_GetKeyboardState()/SDL_GetGamepadButton() every time the guest
// issues sceCtrlPeekBuffer*/sceCtrlReadBuffer*; we keep that call site and
// just redirect the physical-input read through libretro under
// LIBRETRO.
//
// Lifecycle:
//   retro_set_input_poll()  -> libretro_input_set_poll_cb(cb)
//   retro_set_input_state() -> libretro_input_set_state_cb(cb)
//   retro_run() -> libretro_input_poll()          (once per host frame)
//   sceCtrlRead* (guest) -> retrieve_ctrl_data() ->
//                           libretro_input_fill_ctrl(port, buttons, lx, ly, rx, ry)
//
// Only RETRO_DEVICE_JOYPAD + RETRO_DEVICE_ANALOG are wired in M9.
// Touch and motion sensors are stubbed out (all zeros) for now and will
// be added alongside a libretro pointer/lightgun mapping in M9.5.

#pragma once

#include <libretro.h>

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Registration (called from libretro.cpp's retro_set_input_* hooks).
void libretro_input_set_poll_cb(retro_input_poll_t cb);
void libretro_input_set_state_cb(retro_input_state_t cb);

// Called from retro_run() exactly once per host frame — runs the frontend's
// input poll so subsequent libretro_input_fill_ctrl() calls return fresh
// values.  Safe to call when no callbacks are wired (becomes a no-op).
void libretro_input_poll(void);

// Populates a Vita-format SceCtrlData snapshot from the frontend's
// cached JOYPAD + ANALOG state.  `port` is a 1-based Vita port id (1..4);
// we map port 1 to the frontend's port 0 and additional ports to 1..3 so
// pstv_mode works transparently.  `buttons` is OR-accumulated (caller
// starts from 0), axes are written as uint8 with 0x80 center.
void libretro_input_fill_ctrl(int port,
                              uint32_t *buttons,
                              uint8_t  *lx,
                              uint8_t  *ly,
                              uint8_t  *rx,
                              uint8_t  *ry);

#ifdef __cplusplus
} // extern "C"
#endif
