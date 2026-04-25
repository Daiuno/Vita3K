// Vita3K emulator project - libretro core options
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Core options v2 definitions for the Vita3K libretro core.  The layout
// follows the convention used by beetle-psx / ppsspp so the frontend menu
// groups them by category.

#pragma once

#include <libretro.h>
#include "libretro_core_options_intl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Option keys (shared between this header and libretro.cpp)
#define OPT_CPU_BACKEND          "vita3k_cpu_backend"
#define OPT_JIT_BLOCK_SIZE       "vita3k_jit_block_size"
#define OPT_RESOLUTION_MULT      "vita3k_resolution_multiplier"
#define OPT_ANISOTROPIC          "vita3k_anisotropic_filter"
#define OPT_SHADER_CACHE         "vita3k_shader_cache"
#define OPT_SURFACE_SYNC         "vita3k_surface_sync"
#define OPT_AUDIO_BACKEND        "vita3k_audio_backend"
#define OPT_AUDIO_VOLUME         "vita3k_audio_volume"
#define OPT_REAR_TOUCH_MODE      "vita3k_rear_touch_mode"
#define OPT_MOTION_SUPPORT       "vita3k_motion_support"
#define OPT_CAMERA_SUPPORT       "vita3k_camera_support"
#define OPT_CAMERA_DEVICE        "vita3k_camera_device"
#define OPT_FAST_SAVESTATES      "vita3k_fast_savestates"
#define OPT_SAVESTATE_COMPRESSION "vita3k_savestate_compression"

// ---------------------------------------------------------------------------
// English definitions (other locales come from libretro_core_options_intl.h)
// ---------------------------------------------------------------------------
static struct retro_core_option_v2_category option_cats_us[] = {
    { "cpu",      "CPU",            "Processor emulation backend."            },
    { "video",    "Video",          "Renderer settings."                     },
    { "audio",    "Audio",          "Audio backend and mixer."               },
    { "input",    "Input",          "Gamepad, touch and motion."             },
    { "camera",   "Camera",         "Camera passthrough for SceCamera games." },
    { "savestate","Save States",    "Serialization options."                 },
    { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition option_defs_us[] = {
    // --- CPU -----------------------------------------------------------------
    {
        OPT_CPU_BACKEND,
        "CPU Backend",
        "Backend",
        "Interpreter is always available.  JIT requires iOS 26+ (TXM) or a "
        "jailbroken device; on macOS/Linux it works out of the box.",
        NULL, "cpu",
        {
            { "ir_interpreter", "IR Interpreter (recommended)" },
            { "dynarmic_jit",   "Dynarmic JIT" },
            { "fallback_interpreter", "Fallback Interpreter (slow, debug)" },
            { NULL, NULL },
        },
        "ir_interpreter",
    },
    {
        OPT_JIT_BLOCK_SIZE,
        "JIT Block Size",
        "Block size",
        "Size of a single JIT code page in KiB.  Smaller pages use less "
        "memory but increase JIT cache misses.",
        NULL, "cpu",
        {
            { "64",   "64 KiB" },
            { "128",  "128 KiB" },
            { "256",  "256 KiB (default)" },
            { "512",  "512 KiB" },
            { NULL, NULL },
        },
        "256",
    },

    // --- Video ---------------------------------------------------------------
    {
        OPT_RESOLUTION_MULT,
        "Internal Resolution Multiplier",
        "Resolution",
        "Upscale Vita's 960×544 framebuffer.  2x doubles the resolution in "
        "both axes; higher values stress the GPU significantly.",
        NULL, "video",
        {
            { "1", "1x (960x544)" },
            { "2", "2x (1920x1088)" },
            { "3", "3x (2880x1632)" },
            { "4", "4x (3840x2176)" },
            { NULL, NULL },
        },
        "1",
    },
    {
        OPT_ANISOTROPIC,
        "Anisotropic Filtering",
        "Anisotropic",
        "Improves texture sharpness at oblique viewing angles.",
        NULL, "video",
        {
            { "1",  "Off" },
            { "2",  "2x" },
            { "4",  "4x" },
            { "8",  "8x" },
            { "16", "16x" },
            { NULL, NULL },
        },
        "1",
    },
    {
        OPT_SHADER_CACHE,
        "Shader Cache",
        "Cache",
        "Persist compiled GLSL shaders across sessions.",
        NULL, "video",
        { { "enabled", "Enabled" }, { "disabled", "Disabled" }, { NULL, NULL } },
        "enabled",
    },
    {
        OPT_SURFACE_SYNC,
        "Surface Sync",
        "Surface Sync",
        "Strict CPU↔GPU memory synchronization.  Fixes graphical glitches in "
        "some games at a performance cost.",
        NULL, "video",
        { { "fast", "Fast" }, { "accurate", "Accurate" }, { NULL, NULL } },
        "fast",
    },

    // --- Audio ---------------------------------------------------------------
    {
        OPT_AUDIO_BACKEND,
        "Audio Backend",
        "Backend",
        "Libretro uses the frontend's audio device; the stub/null option is "
        "for debugging only.",
        NULL, "audio",
        { { "libretro", "Libretro (default)" }, { "null", "Null (silent)" }, { NULL, NULL } },
        "libretro",
    },
    {
        OPT_AUDIO_VOLUME,
        "Audio Volume",
        "Volume",
        "Global output volume as a percentage.",
        NULL, "audio",
        {
            { "50",  "50%" }, { "75", "75%" }, { "100", "100%" },
            { "125", "125%" }, { "150", "150%" },
            { NULL, NULL },
        },
        "100",
    },

    // --- Input ---------------------------------------------------------------
    {
        OPT_REAR_TOUCH_MODE,
        "Rear Touchpad Mapping",
        "Rear Touchpad",
        "How to expose the Vita's rear touchpad on a regular gamepad.",
        NULL, "input",
        {
            { "disabled",     "Disabled" },
            { "right_stick",  "Right Stick (click for tap)" },
            { "l2r2",         "L2/R2 buttons (top/bottom)" },
            { "touch_screen", "Screen touch (top half = rear)" },
            { NULL, NULL },
        },
        "right_stick",
    },
    {
        OPT_MOTION_SUPPORT,
        "Motion Controls",
        "Motion",
        "Forward RETRO_SENSOR_ACCELEROMETER / GYROSCOPE samples to the guest.",
        NULL, "input",
        { { "enabled", "Enabled" }, { "disabled", "Disabled" }, { NULL, NULL } },
        "disabled",
    },

    // --- Camera --------------------------------------------------------------
    {
        OPT_CAMERA_SUPPORT,
        "Camera Support",
        "Camera",
        "Expose the frontend's camera to the guest.  Requires camera "
        "permission at the OS level.  Disabled by default to avoid "
        "triggering a permission prompt for games that never use it.",
        NULL, "camera",
        { { "disabled", "Disabled" }, { "enabled", "Enabled" }, { NULL, NULL } },
        "disabled",
    },
    {
        OPT_CAMERA_DEVICE,
        "Camera Device",
        "Device",
        "Which Vita camera the physical camera should appear as.",
        NULL, "camera",
        {
            { "back",  "Back (recommended)" },
            { "front", "Front" },
            { NULL, NULL },
        },
        "back",
    },

    // --- Save State ----------------------------------------------------------
    {
        OPT_FAST_SAVESTATES,
        "Fast Save States",
        "Fast",
        "Omit the Vita GPU command buffer snapshot.  Saves are smaller and "
        "faster but cannot be used across graphically-dependent moments.",
        NULL, "savestate",
        { { "disabled", "Disabled" }, { "enabled", "Enabled" }, { NULL, NULL } },
        "disabled",
    },
    {
        OPT_SAVESTATE_COMPRESSION,
        "Save State Compression",
        "Compression",
        "zstd level.  Higher values shrink files but stall the main thread "
        "longer on save.",
        NULL, "savestate",
        {
            { "1",  "Level 1 (fastest)" },
            { "3",  "Level 3 (default)" },
            { "9",  "Level 9" },
            { "19", "Level 19 (slow, smallest)" },
            { NULL, NULL },
        },
        "3",
    },

    { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};

static struct retro_core_options_v2 options_us = {
    option_cats_us,
    option_defs_us,
};

#ifdef __cplusplus
} /* extern "C" */
#endif
