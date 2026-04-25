// Vita3K emulator project - libretro input bridge
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "libretro_input.h"

#include <algorithm>
#include <cstdint>

// Mirror SCE_CTRL_* bit values to avoid pulling the whole Vita PSP2 SDK
// header into this TU.  These match
//   vita3k/modules/SceCtrl/SceCtrl.h / psp2/ctrl.h
// bit-for-bit.
static constexpr uint32_t kSceCtrlSelect   = 0x00000001;
static constexpr uint32_t kSceCtrlL3       = 0x00000002;
static constexpr uint32_t kSceCtrlR3       = 0x00000004;
static constexpr uint32_t kSceCtrlStart    = 0x00000008;
static constexpr uint32_t kSceCtrlUp       = 0x00000010;
static constexpr uint32_t kSceCtrlRight    = 0x00000020;
static constexpr uint32_t kSceCtrlDown     = 0x00000040;
static constexpr uint32_t kSceCtrlLeft     = 0x00000080;
static constexpr uint32_t kSceCtrlLTrigger = 0x00000100;  // L2
static constexpr uint32_t kSceCtrlRTrigger = 0x00000200;  // R2
static constexpr uint32_t kSceCtrlL1       = 0x00000400;
static constexpr uint32_t kSceCtrlR1       = 0x00000800;
static constexpr uint32_t kSceCtrlTriangle = 0x00001000;
static constexpr uint32_t kSceCtrlCircle   = 0x00002000;
static constexpr uint32_t kSceCtrlCross    = 0x00004000;
static constexpr uint32_t kSceCtrlSquare   = 0x00008000;

namespace {
retro_input_poll_t  s_poll_cb  = nullptr;
retro_input_state_t s_state_cb = nullptr;

// Vita 1-based port -> libretro 0-based port.
unsigned retro_port_from_vita(int vita_port) {
    if (vita_port <= 1)
        return 0;
    // vita_port 2..4 -> retro 1..3 in pstv_mode
    return static_cast<unsigned>(vita_port - 1);
}

bool btn(unsigned port, unsigned id) {
    return s_state_cb ? s_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id) != 0 : false;
}

uint8_t axis_to_byte(int16_t v) {
    // libretro analog range: -0x8000..0x7FFF; Vita: 0..255 with 0x80 center.
    const int32_t unsigned_axis = static_cast<int32_t>(v) + 0x8000; // 0..0xFFFF
    const int32_t mapped = unsigned_axis * 255 / 0xFFFF;
    return static_cast<uint8_t>(std::clamp(mapped, 0, 255));
}

int16_t axis_raw(unsigned port, unsigned index, unsigned id) {
    return s_state_cb ? s_state_cb(port, RETRO_DEVICE_ANALOG, index, id) : 0;
}

} // namespace

extern "C" {

void libretro_input_set_poll_cb(retro_input_poll_t cb) {
    s_poll_cb = cb;
}

void libretro_input_set_state_cb(retro_input_state_t cb) {
    s_state_cb = cb;
}

void libretro_input_poll(void) {
    if (s_poll_cb)
        s_poll_cb();
}

void libretro_input_fill_ctrl(int port,
                              uint32_t *buttons,
                              uint8_t  *lx,
                              uint8_t  *ly,
                              uint8_t  *rx,
                              uint8_t  *ry) {
    if (lx) *lx = 0x80;
    if (ly) *ly = 0x80;
    if (rx) *rx = 0x80;
    if (ry) *ry = 0x80;
    if (!s_state_cb)
        return;

    const unsigned p = retro_port_from_vita(port);
    uint32_t out = buttons ? *buttons : 0;

    if (btn(p, RETRO_DEVICE_ID_JOYPAD_UP))     out |= kSceCtrlUp;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_DOWN))   out |= kSceCtrlDown;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_LEFT))   out |= kSceCtrlLeft;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_RIGHT))  out |= kSceCtrlRight;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_START))  out |= kSceCtrlStart;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_SELECT)) out |= kSceCtrlSelect;
    // RetroPad face → Vita face.  RetroPad A = east, B = south, X = north,
    // Y = west; PS controller: circle = east, cross = south, triangle =
    // north, square = west.  Match the canonical RetroArch Sony mapping.
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_A))      out |= kSceCtrlCircle;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_B))      out |= kSceCtrlCross;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_X))      out |= kSceCtrlTriangle;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_Y))      out |= kSceCtrlSquare;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_L))      out |= kSceCtrlL1;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_R))      out |= kSceCtrlR1;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_L2))     out |= kSceCtrlLTrigger;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_R2))     out |= kSceCtrlRTrigger;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_L3))     out |= kSceCtrlL3;
    if (btn(p, RETRO_DEVICE_ID_JOYPAD_R3))     out |= kSceCtrlR3;

    if (buttons)
        *buttons = out;

    if (lx) *lx = axis_to_byte(axis_raw(p, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X));
    if (ly) *ly = axis_to_byte(axis_raw(p, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y));
    if (rx) *rx = axis_to_byte(axis_raw(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X));
    if (ry) *ry = axis_to_byte(axis_raw(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));
}

} // extern "C"
