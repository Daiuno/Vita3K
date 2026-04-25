// Vita3K emulator project - libretro core options runtime reader
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reads the OPT_* keys defined in libretro_core_options.h via the
// frontend's RETRO_ENVIRONMENT_GET_VARIABLE callback and exposes them
// as a typed snapshot.  The snapshot is consumed at boot time
// (libretro_boot.cpp::populate_config) and refreshed every retro_run
// frame when the frontend signals RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE.
//
// Keep this header free of libretro.h so it can be included from any TU
// (including engine-internal code that doesn't otherwise know about
// libretro).  Implementation in libretro_core_opts.cpp.

#pragma once

#include <libretro.h>

#include <string>

/// Snapshot of every OPT_* defined in libretro_core_options.h.  All fields
/// have safe defaults that match the option_defs_us[] table.
struct LibretroCoreOpts {
    // CPU
    std::string cpu_backend;        ///< "dynarmic_jit" | "ir_interpreter" | "fallback_interpreter"
    int         jit_block_size_kib; ///< 64 / 128 / 256 / 512

    // Video
    int         resolution_multiplier; ///< 1..4
    int         anisotropic_filter;    ///< 1/2/4/8/16
    bool        shader_cache;
    std::string surface_sync;       ///< "fast" | "accurate"

    // Audio
    std::string audio_backend;      ///< "libretro" | "null"
    int         audio_volume;       ///< percent

    // Input
    std::string rear_touch_mode;    ///< "disabled" | "right_stick" | "l2r2" | "touch_screen"
    bool        motion_support;

    // Camera
    bool        camera_support;
    std::string camera_device;      ///< "back" | "front"

    // Save state
    bool        fast_savestates;
    int         savestate_compression; ///< zstd level
};

// These three are C++ only because the snapshot type is std::string-bearing.
// They have internal-C++ linkage; they are NOT exported from the dylib.

/// Pull every OPT_* via @env_cb.  Must be called AFTER
/// RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2.  Sets the global snapshot.
void libretro_core_opts_refresh(retro_environment_t env_cb);

/// Read-only access to the latest snapshot.  Always returns a valid
/// reference (initialised to defaults at static-init time).
const LibretroCoreOpts &libretro_core_opts_current(void);

/// Returns true if the frontend signalled GET_VARIABLE_UPDATE since the
/// last call.  Safe to poll once per retro_run.
bool libretro_core_opts_check_dirty(retro_environment_t env_cb);
