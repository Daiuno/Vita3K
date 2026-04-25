// Vita3K libretro core - boot entry point
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Drives the Vita3K engine "boot" (equivalent of main() -> app::init +
// app::late_init) from a headless libretro context.  All host-window /
// ImGui / renderer bits are deferred until a real HW context is supplied
// by the frontend via retro_hw_render_callback (see M6+).

#pragma once

struct EmuEnvState;

namespace vita3k_libretro {

// Boots the engine *without* creating an SDL window.  Requires
// libretro_paths_init() to have run first.  Returns true when the
// emuenv is ready to load a VPK/PKG.  Safe to call multiple times —
// subsequent invocations are no-ops.
bool boot(EmuEnvState &emuenv);

// Tears down anything `boot()` created.  Must be called before the
// EmuEnvState unique_ptr is reset.
void shutdown(EmuEnvState &emuenv);

} // namespace vita3k_libretro
