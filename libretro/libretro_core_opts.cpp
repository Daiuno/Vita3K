// Vita3K emulator project - libretro core options runtime reader
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// See libretro_core_opts.h for the design rationale.

#include "libretro_core_opts.h"
#include "libretro_core_options.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

LibretroCoreOpts g_opts = {
    // cpu_backend default matches the first entry of option_defs_us[OPT_CPU_BACKEND]
    // in libretro_core_options.h ("ir_interpreter").  RetroArch's first-launch
    // behaviour is to NOT call env(GET_VARIABLE) until it has materialised the
    // user's option set, so this fallback controls what the core picks before
    // any user interaction.  JIT must be an explicit opt-in because it
    // depends on StikDebug / device capabilities we cannot auto-discover.
    /* cpu_backend           */ "ir_interpreter",
    /* jit_block_size_kib    */ 256,
    /* resolution_multiplier */ 1,
    /* anisotropic_filter    */ 1,
    /* shader_cache          */ true,
    /* surface_sync          */ "fast",
    /* audio_backend         */ "libretro",
    /* audio_volume          */ 100,
    /* rear_touch_mode       */ "right_stick",
    /* motion_support        */ false,
    /* camera_support        */ false,
    /* camera_device         */ "back",
    /* fast_savestates       */ false,
    /* savestate_compression */ 3,
};

// Helper: returns the string value of @key or @fallback if the frontend
// has no opinion.  Never returns null.
const char *get_str(retro_environment_t env_cb, const char *key, const char *fallback) {
    if (!env_cb)
        return fallback;
    retro_variable var{ key, nullptr };
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && *var.value)
        return var.value;
    return fallback;
}

int get_int(retro_environment_t env_cb, const char *key, int fallback) {
    const char *s = get_str(env_cb, key, nullptr);
    if (!s) return fallback;
    char *end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s) return fallback;
    return static_cast<int>(v);
}

bool get_bool_enabled(retro_environment_t env_cb, const char *key, bool fallback) {
    const char *s = get_str(env_cb, key, nullptr);
    if (!s) return fallback;
    if (!std::strcmp(s, "enabled"))  return true;
    if (!std::strcmp(s, "disabled")) return false;
    return fallback;
}

} // namespace

void libretro_core_opts_refresh(retro_environment_t env_cb) {
    g_opts.cpu_backend           = get_str (env_cb, OPT_CPU_BACKEND,          "ir_interpreter");
    g_opts.jit_block_size_kib    = get_int (env_cb, OPT_JIT_BLOCK_SIZE,       256);
    g_opts.resolution_multiplier = get_int (env_cb, OPT_RESOLUTION_MULT,      1);
    g_opts.anisotropic_filter    = get_int (env_cb, OPT_ANISOTROPIC,          1);
    g_opts.shader_cache          = get_bool_enabled(env_cb, OPT_SHADER_CACHE, true);
    g_opts.surface_sync          = get_str (env_cb, OPT_SURFACE_SYNC,         "fast");
    g_opts.audio_backend         = get_str (env_cb, OPT_AUDIO_BACKEND,        "libretro");
    g_opts.audio_volume          = get_int (env_cb, OPT_AUDIO_VOLUME,         100);
    g_opts.rear_touch_mode       = get_str (env_cb, OPT_REAR_TOUCH_MODE,      "right_stick");
    g_opts.motion_support        = get_bool_enabled(env_cb, OPT_MOTION_SUPPORT, false);
    g_opts.camera_support        = get_bool_enabled(env_cb, OPT_CAMERA_SUPPORT, false);
    g_opts.camera_device         = get_str (env_cb, OPT_CAMERA_DEVICE,        "back");
    g_opts.fast_savestates       = get_bool_enabled(env_cb, OPT_FAST_SAVESTATES, false);
    g_opts.savestate_compression = get_int (env_cb, OPT_SAVESTATE_COMPRESSION, 3);
}

const LibretroCoreOpts &libretro_core_opts_current(void) {
    return g_opts;
}

bool libretro_core_opts_check_dirty(retro_environment_t env_cb) {
    if (!env_cb)
        return false;
    bool dirty = false;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &dirty) && dirty) {
        libretro_core_opts_refresh(env_cb);
        return true;
    }
    return false;
}
