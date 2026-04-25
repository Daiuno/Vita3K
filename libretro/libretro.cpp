// Vita3K emulator project - libretro core entry points
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// This file holds the 18 mandatory libretro API entry points plus the
// Vita3K-specific retro_vita_* extensions.

#include <libretro.h>

#include "libretro_audio.h"
#include "libretro_core_options.h"
#include "libretro_core_opts.h"
#include "libretro_extensions.h"
#include "libretro_vulkan_context.h"
#include "libretro_input.h"
#include "libretro_internal.h"
#include "libretro_ios_jit.h"
#include "libretro_paths.h"
#include "libretro_stub_sdl.h"

#include "libretro_boot.h"
#include "libretro_game_boot.h"
#include <config/state.h>
#include <emuenv/state.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <renderer/types.h>
#include <renderer/vulkan/libretro_vulkan_api.h>
#include <util/fs.h>
#include <util/types.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// Version / identity
// ---------------------------------------------------------------------------
#define VITA3K_LIBRETRO_VERSION "0.1.0"
#ifndef VITA3K_LIBRETRO_GIT_SHA
#define VITA3K_LIBRETRO_GIT_SHA "unknown"
#endif
static const char *const kCoreName = "Vita3K";
static const char *const kCoreVersion = VITA3K_LIBRETRO_VERSION "-" VITA3K_LIBRETRO_GIT_SHA;
static const char *const kValidExtensions = "vpk|pkg|pup|vita3k";

// ---------------------------------------------------------------------------
// Callback slots (set by the frontend via retro_set_*)
// ---------------------------------------------------------------------------
static retro_environment_t        env_cb        = nullptr;
static retro_video_refresh_t      video_cb      = nullptr;
static retro_audio_sample_t       audio_cb      = nullptr;
static retro_audio_sample_batch_t audio_batch_cb= nullptr;
static retro_input_poll_t         input_poll_cb = nullptr;
static retro_input_state_t        input_state_cb= nullptr;

// Logging -------------------------------------------------------------------
static retro_log_printf_t log_cb = nullptr;

static void fallback_log(enum retro_log_level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define VLOG(level, ...)  do { if (log_cb) log_cb(level, __VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// Global core state (very small for M1)
// ---------------------------------------------------------------------------
namespace {
    std::string g_save_dir;       // from RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY
    std::string g_system_dir;     // from RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY (unused)
    std::string g_content_path;   // argument from retro_load_game
    std::string g_last_error;
    bool        g_game_loaded = false;

}

// Owning pointer to the Vita3K emulator state.  External linkage so the
// Vulkan context bridge (libretro_vulkan_context.cpp) can reach it via
// `extern std::unique_ptr<EmuEnvState> g_emuenv;` without going through
// an accessor.
std::unique_ptr<EmuEnvState> g_emuenv;

static void set_last_error(const char *msg) {
    g_last_error = msg ? msg : "";
}

// ---------------------------------------------------------------------------
// Exposed to install / catalog / future workers via libretro_internal.h.
// ---------------------------------------------------------------------------
namespace vita3k_libretro_core {

std::unique_ptr<EmuEnvState> &emuenv() { return g_emuenv; }

void set_last_error(const char *msg) { ::set_last_error(msg); }
void set_last_error(const std::string &msg) { ::set_last_error(msg.c_str()); }

bool is_booted() {
    return g_emuenv != nullptr;
}

const char *pref_path_utf8() {
    const char *p = libretro_paths_pref();
    return p ? p : "";
}

const char *cache_path_utf8() {
    static std::string s_cache;
    const char *pref = libretro_paths_pref();
    s_cache.clear();
    if (pref && *pref) {
        s_cache = pref;
        if (s_cache.back() != '/')
            s_cache.push_back('/');
        s_cache += "cache";
    }
    return s_cache.c_str();
}

} // namespace vita3k_libretro_core

// ===========================================================================
//                    Standard libretro API (18 entry points)
// ===========================================================================

RETRO_API unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    std::memset(info, 0, sizeof(*info));
    info->library_name     = kCoreName;
    info->library_version  = kCoreVersion;
    info->valid_extensions = kValidExtensions;
    info->need_fullpath    = true;   // Vita3K operates on host paths, not ROM blobs
    info->block_extract    = true;   // We never want the frontend to extract .pkg
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    std::memset(info, 0, sizeof(*info));
    // Vita native resolution: 960x544 @ ~60 Hz.
    info->geometry.base_width   = 960;
    info->geometry.base_height  = 544;
    info->geometry.max_width    = 960 * 4;   // allow up to 4x internal resolution
    info->geometry.max_height   = 544 * 4;
    info->geometry.aspect_ratio = 16.0f / 9.0f;
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = 48000.0;   // SceNgs / SceAudio native
    // Used by Vulkan swapchain when there is no SDL window (HW context).
    renderer::vulkan::libretro::output_extent_set(
        static_cast<uint32_t>(info->geometry.base_width),
        static_cast<uint32_t>(info->geometry.base_height));
}

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

RETRO_API void retro_set_environment(retro_environment_t cb) {
    env_cb = cb;

    // Request logging as early as possible.
    retro_log_callback log = { fallback_log };
    if (env_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        log_cb = log.log;
    else
        log_cb = fallback_log;

    // Advertise that the core can run with no ROM (for homebrew/firmware install).
    bool no_rom = true;
    env_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);

    // Core options v2
    unsigned options_version = 0;
    if (env_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &options_version)
        && options_version >= 2) {
        env_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_us);
    }

    // Serialization quirks: no rewind / run-ahead / netplay (see Code Plan §9.2)
    uint64_t quirks = RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE
                    | RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE
                    | RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE
                    | RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION;
    env_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);

    // Pixel format: even though we render via HW context, RetroArch wants a
    // valid SW format configured.  XRGB8888 matches the GLES3 default FBO.
    enum retro_pixel_format pix_fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pix_fmt);

    // M11.1: take an initial snapshot of the user's option choices.  Must
    // happen AFTER SET_CORE_OPTIONS_V2 so the frontend is willing to
    // answer GET_VARIABLE queries.  Subsequent live updates are observed
    // via libretro_core_opts_check_dirty() in retro_run.
    libretro_core_opts_refresh(env_cb);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)    { video_cb       = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)      { audio_cb       = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    audio_batch_cb = cb;
    libretro_audio_set_batch_cb(cb);
}
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)          {
    input_poll_cb = cb;
    libretro_input_set_poll_cb(cb);
}
RETRO_API void retro_set_input_state(retro_input_state_t cb)        {
    input_state_cb = cb;
    libretro_input_set_state_cb(cb);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

RETRO_API void retro_init(void) {
    VLOG(RETRO_LOG_INFO, "[Vita3K] retro_init (version=%s)\n", kCoreVersion);

    // Cache the save/system directories early; they are the only paths the
    // core is allowed to touch (see Code Plan §12).
    const char *path = nullptr;
    if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &path) && path)
        g_save_dir = path;
    if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &path) && path)
        g_system_dir = path;

    VLOG(RETRO_LOG_INFO, "[Vita3K] save_dir=%s system_dir=%s\n",
         g_save_dir.c_str(), g_system_dir.c_str());

    // Build ux0/pd0/os0/... under <save_dir>/Vita3K/pref and cache the Root.
    libretro_paths_init(g_save_dir.c_str());
    VLOG(RETRO_LOG_INFO, "[Vita3K] pref=%s\n", libretro_paths_pref());

    libretro_stub_sdl_init();

    // Initialise the audio bridge BEFORE we boot the engine so the
    // LibretroAudioAdapter that app::late_init() selects can immediately
    // forward sceAudioOutOutput PCM into the libretro ring buffer.
    libretro_audio_init();

    // Verify the Vita3K Root we just built is well-formed.
    const Root &root = vita3k_libretro::paths_root();
    VLOG(RETRO_LOG_DEBUG, "[Vita3K] root.base=%s root.pref=%s\n",
         root.get_base_path().string().c_str(),
         root.get_pref_path().string().c_str());

    // Instantiate the emulator environment and drive the headless boot
    // (filesystem / imgui context / memory / audio / ngs).  A real HW
    // renderer is still NOT created — that happens later when the
    // frontend supplies a HW context (see libretro_vulkan_context.cpp).
    if (!g_emuenv) {
        g_emuenv = std::make_unique<EmuEnvState>();
        VLOG(RETRO_LOG_DEBUG, "[Vita3K] EmuEnvState constructed\n");

        if (vita3k_libretro::boot(*g_emuenv)) {
            VLOG(RETRO_LOG_INFO, "[Vita3K] headless boot complete\n");
        } else {
            VLOG(RETRO_LOG_ERROR, "[Vita3K] headless boot FAILED\n");
        }
    }
}

RETRO_API void retro_deinit(void) {
    VLOG(RETRO_LOG_INFO, "[Vita3K] retro_deinit\n");
    if (g_emuenv) {
        vita3k_libretro::shutdown(*g_emuenv);
        g_emuenv.reset();
    }
    libretro_vulkan_shutdown();
    libretro_audio_deinit();
    libretro_stub_sdl_deinit();
    g_game_loaded = false;
    g_content_path.clear();
}

RETRO_API void retro_reset(void) {
    VLOG(RETRO_LOG_INFO, "[Vita3K] retro_reset\n");
    // TODO(M6): emuenv.kernel.schedule_reset();
}

RETRO_API void retro_run(void) {
    // M11.1: cheap option-update probe.  When the frontend signals a
    // change we re-snapshot every OPT_*; only a small subset (audio
    // volume, resolution multiplier, surface_sync) is honored mid-run
    // because most options gate engine init.  CPU backend changes
    // require a core re-load.
    if (env_cb && libretro_core_opts_check_dirty(env_cb)) {
        const LibretroCoreOpts &opts = libretro_core_opts_current();
        if (g_emuenv) {
            g_emuenv->cfg.audio_volume                  = opts.audio_volume;
            g_emuenv->cfg.current_config.audio_volume   = opts.audio_volume;
            g_emuenv->cfg.resolution_multiplier         = static_cast<float>(opts.resolution_multiplier);
            g_emuenv->cfg.current_config.resolution_multiplier = static_cast<float>(opts.resolution_multiplier);
            g_emuenv->cfg.disable_surface_sync          = (opts.surface_sync == "fast");
            g_emuenv->cfg.current_config.disable_surface_sync = (opts.surface_sync == "fast");
        }
    }

    // Per libretro contract, input must be polled exactly once per frame
    // before the core reads button state.  We forward to both the legacy
    // callback slot (kept for parity with cores that poll their own SDL
    // eventloop) and the bridge used by ctrl.cpp::retrieve_ctrl_data.
    libretro_input_poll();

    // Drive one guest frame.
    //
    // Vita3K is inherently multithreaded — the Vita kernel runs guest
    // threads on host threads that execute independently of retro_run.
    // retro_run's responsibilities per host frame are:
    //   1. Poll frontend input (done above).
    //   2. Drain the GXM command queue so it is ready for the next display
    //      tick.
    //   3. Composite the current Vita framebuffer into libretro's HW FBO.
    //   4. Inform the frontend a new HW frame is ready.
    //   5. Drain the libretro audio ring (M10) and forward to the
    //      frontend's audio_sample_batch_cb.
    //
    // Until context_reset has fired we only have the frontend's software
    // path to fall back on; send a null frame so RetroArch keeps the UI
    // responsive without painting garbage.  We still drain audio in this
    // case so any samples the engine produced before the HW context came
    // up do not pile up in the ring buffer.
    if (!libretro_vulkan_is_ready()) {
        if (video_cb) video_cb(NULL, 0, 0, 0);
        libretro_audio_drain();
        return;
    }

    // Idempotent: if the frontend signalled HW ready before we could run the
    // deferred run_app from launch_title, complete guest start here (libretro
    // docs: do not rely on ordering between threads; this covers edge cases).
    if (g_emuenv && g_emuenv->renderer) {
        vita3k_libretro::start_guest_if_pending(*g_emuenv);
    }
    {
        static bool hw_iface_registered = false;
        if (!hw_iface_registered && env_cb) {
            const struct retro_hw_render_interface *iface = nullptr;
            if (env_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) && iface
                && iface->interface_type == RETRO_HW_RENDER_INTERFACE_VULKAN) {
                renderer::vulkan::libretro::hw_render_interface_set(
                    reinterpret_cast<const retro_hw_render_interface_vulkan *>(iface));
                hw_iface_registered = true;
            }
        }
    }

    constexpr unsigned kVitaWidth  = 960;
    constexpr unsigned kVitaHeight = 544;

    if (g_emuenv && g_emuenv->renderer && vita3k_libretro::is_title_running()) {
        // Consume whatever GPU commands kernel threads have queued up so
        // far and promote the newest Vita framebuffer into the renderer's
        // "should_display" state machine.
        const bool sd_before = g_emuenv->renderer->should_display;
        // Wall-clock budget while should_display is false: batch.cpp uses the same 500ms cap as
        // desktop main.cpp (one process_batches per frame). Too small a budget returns before
        // notifications/sync are processed; guest threads then wait on semaphores /
        // sceGxmNotificationWait with no forward progress.
        renderer::process_batches(*g_emuenv->renderer,
                                  g_emuenv->renderer->features,
                                  g_emuenv->mem,
                                  g_emuenv->cfg);
        const bool sd_after = g_emuenv->renderer->should_display;

        // Composite the Vita display into the Vulkan swapchain (see
        // renderer/src/vulkan/screen_renderer.cpp).
        const SceFVector2 viewport_pos  = { 0.0f, 0.0f };
        const SceFVector2 viewport_size = {
            static_cast<float>(kVitaWidth),
            static_cast<float>(kVitaHeight)
        };
        g_emuenv->renderer->render_frame(viewport_pos, viewport_size,
                                         g_emuenv->display, g_emuenv->gxm,
                                         g_emuenv->mem);
        g_emuenv->renderer->swap_window(nullptr);

        // Throttled diagnostics – use a local counter that counts retro_run
        // calls rather than emuenv.frame_count (which the guest increments
        // inside _sceDisplaySetFrameBuf).  That way we still see diagnostics
        // during loading when the guest hasn't produced a frame yet.
        {
            static uint64_t retro_run_count = 0;
            ++retro_run_count;
            if ((retro_run_count % 60ULL) == 1ULL && log_cb) {
                const uint32_t q_depth = g_emuenv->renderer
                    ? static_cast<uint32_t>(g_emuenv->renderer->command_buffer_queue.size())
                    : 0;
                log_cb(RETRO_LOG_INFO,
                    "[VITA3K-LR] retro_run host_frame=%llu guest_frame=%llu sd_before=%d sd_after=%d ctx=%s cmdq=%u\n",
                    (unsigned long long)retro_run_count,
                    (unsigned long long)g_emuenv->frame_count,
                    sd_before ? 1 : 0,
                    sd_after ? 1 : 0,
                    (g_emuenv->renderer && g_emuenv->renderer->context ? "set" : "null"),
                    q_depth);
            }
        }
    }

    if (video_cb)
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, kVitaWidth, kVitaHeight, 0);

    // M10: forward whatever PCM the engine pushed into our ring during
    // this host tick to the frontend.  Cheap when empty.
    libretro_audio_drain();
}

// ---------------------------------------------------------------------------
// Controller topology
// ---------------------------------------------------------------------------

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port; (void)device;
    // Single player, fixed RetroPad for now.
}

// ---------------------------------------------------------------------------
// Game loading
// ---------------------------------------------------------------------------

RETRO_API bool retro_load_game(const struct retro_game_info *info) {
    // Negotiate Vulkan HW context + device creation. Must run in retro_load_game
    // (context_reset follows). Idempotent if already registered.
    if (!libretro_vulkan_register_hw_and_negotiation(env_cb)) {
        VLOG(RETRO_LOG_ERROR,
             "[Vita3K] frontend rejected Vulkan HW / negotiation; core cannot render\n");
        set_last_error("Frontend does not support Vulkan HW rendering");
        return false;
    }
    VLOG(RETRO_LOG_INFO, "[Vita3K] Vulkan HW + negotiation registered\n");

    if (!info || !info->path) {
        VLOG(RETRO_LOG_INFO, "[Vita3K] retro_load_game: no content (firmware-install mode)\n");
        g_game_loaded = true;
        return true;
    }

    g_content_path = info->path;
    VLOG(RETRO_LOG_INFO, "[Vita3K] retro_load_game: path=%s\n", g_content_path.c_str());

    // Expected formats:
    //   vita3k://<TITLE_ID>     launch previously-installed game (M9)
    //   /abs/path/to/game.vpk   side-load VPK (use retro_vita_install_vpk first)
    //   /abs/path/to/game.pkg   side-load PKG (use retro_vita_install_pkg first)
    //   /abs/path/to/file.pup   firmware install (use retro_vita_install_firmware)
    if (g_content_path.rfind(VITA3K_LIBRETRO_URI_SCHEME, 0) == 0) {
        if (!g_emuenv) {
            VLOG(RETRO_LOG_ERROR, "[Vita3K] retro_load_game: EmuEnvState missing — retro_init did not complete\n");
            set_last_error("retro_load_game: EmuEnvState missing");
            return false;
        }
        const std::string title_id =
            g_content_path.substr(std::strlen(VITA3K_LIBRETRO_URI_SCHEME));
        if (!vita3k_libretro::launch_title(*g_emuenv, title_id)) {
            VLOG(RETRO_LOG_ERROR, "[Vita3K] retro_load_game: launch_title failed: %s\n",
                 retro_vita_last_error());
            return false;
        }
        VLOG(RETRO_LOG_INFO, "[Vita3K] retro_load_game: launched title_id=%s\n",
             title_id.c_str());
    } else {
        VLOG(RETRO_LOG_WARN,
             "[Vita3K] retro_load_game: path does not use the '%s' scheme;\n"
             "[Vita3K]   install the content first via retro_vita_install_vpk/\n"
             "[Vita3K]   retro_vita_install_pkg, then launch with '%sTITLE_ID'.\n",
             VITA3K_LIBRETRO_URI_SCHEME, VITA3K_LIBRETRO_URI_SCHEME);
    }

    g_game_loaded = true;
    return true;
}

RETRO_API bool retro_load_game_special(unsigned type,
                                       const struct retro_game_info *info,
                                       size_t num) {
    (void)type; (void)info; (void)num;
    return false;
}

RETRO_API void retro_unload_game(void) {
    VLOG(RETRO_LOG_INFO, "[Vita3K] retro_unload_game\n");
    // M10.3: tear the running guest down in-process so the same core
    // can host another retro_load_game("vita3k://...") call without a
    // RetroArch-side reload.  See unload_title()'s docs in
    // libretro_game_boot.h for the (small) set of one-shot leaks that
    // are expected per re-launch.
    if (g_emuenv) {
        if (!vita3k_libretro::unload_title(*g_emuenv)) {
            VLOG(RETRO_LOG_WARN, "[Vita3K] retro_unload_game: unload_title reported partial failure: %s\n",
                 retro_vita_last_error());
        }
    } else {
        vita3k_libretro::mark_title_stopped();
    }
    g_game_loaded = false;
    g_content_path.clear();
}

RETRO_API unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

// ---------------------------------------------------------------------------
// Save states
// ---------------------------------------------------------------------------

RETRO_API size_t retro_serialize_size(void) {
    // M1: report 0 bytes so the frontend disables save-state UI.
    // TODO(M9): compute compressed Vita3K snapshot size.
    return 0;
}

RETRO_API bool retro_serialize(void *data, size_t size) {
    (void)data; (void)size;
    return false;
}

RETRO_API bool retro_unserialize(const void *data, size_t size) {
    (void)data; (void)size;
    return false;
}

// ---------------------------------------------------------------------------
// Cheats
// ---------------------------------------------------------------------------

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index; (void)enabled; (void)code;
}

// ---------------------------------------------------------------------------
// Memory maps (not used - Vita3K manages its own mapped memory)
// ---------------------------------------------------------------------------

RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

// ===========================================================================
//                    Vita3K extensions (retro_vita_*)
//
// install / catalog extensions live in libretro_install.cpp and
// libretro_catalog.cpp.  The ones below are trivial constants or simply
// report the internal state owned by this TU.
// ===========================================================================

RETRO_API const char *retro_vita_last_error(void) {
    return g_last_error.c_str();
}

RETRO_API const char *retro_vita_core_version(void) {
    return kCoreVersion;
}

RETRO_API const char *retro_vita_get_pref_path(void) {
    return vita3k_libretro_core::pref_path_utf8();
}

RETRO_API const char *retro_vita_get_cache_path(void) {
    return vita3k_libretro_core::cache_path_utf8();
}

// ---------------------------------------------------------------------------
// M11.1: JIT status reporting.  Translates the internal vita3k_ios_jit_mode_t
// onto the public VITA3K_JIT_STATUS_* enum so a frontend can dlsym this
// without needing libretro_ios_jit.h.
// ---------------------------------------------------------------------------

RETRO_API int retro_vita_jit_status(void) {
    switch (vita3k_ios_jit_mode()) {
    case VITA3K_JIT_TXM_DUAL_MAPPING: return VITA3K_JIT_STATUS_TXM_DUAL_MAPPING;
    case VITA3K_JIT_LEGACY_WX:        return VITA3K_JIT_STATUS_LEGACY_WX;
    case VITA3K_JIT_PPL_DUAL_MAPPING: return VITA3K_JIT_STATUS_PPL_DUAL_MAPPING;
    case VITA3K_JIT_NONE:
    default:                          return VITA3K_JIT_STATUS_NONE;
    }
}

RETRO_API const char *retro_vita_jit_status_string(void) {
    return vita3k_ios_jit_mode_string();
}
