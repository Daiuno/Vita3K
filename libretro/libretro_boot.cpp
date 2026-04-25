// Vita3K libretro core - boot entry point
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Equivalent of main() in the standalone Vita3K executable, but completely
// head-less: no SDL window, no ImGui main loop, no argument parsing.
//
// Order of operations:
//   1. Pull the Root built by libretro_paths_init() (RETRO_ENVIRONMENT_
//      GET_SAVE_DIRECTORY).
//   2. Produce a minimal Config tuned for the libretro Vulkan path.
//   3. Call app::init()        — filesystem/io/imgui context (SDL window
//                                creation is compiled out under LIBRETRO).
//   4. Call app::late_init()   — memory/audio/ngs (renderer->late_init is
//                                compiled out under LIBRETRO).
//
// After boot() returns true the EmuEnvState can receive a VPK / PKG via
// retro_load_game and we can start spinning retro_run.  The HW renderer is
// still NOT live — wired by libretro_vulkan_context.cpp (context_reset).

#include "libretro_boot.h"

#include "libretro_core_opts.h"
#include "libretro_ios_jit.h"
#include "libretro_paths.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <app/functions.h>
#include <config/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <modules/module_parent.h>
#include <util/fs.h>
#include <util/log.h>

#include <atomic>

namespace vita3k_libretro {

namespace {

    std::atomic<bool> g_booted{ false };

    void populate_config(EmuEnvState &emuenv) {
        // Most of Config's members have sensible defaults (see
        // config/state.h + CONFIG_LIST macro).  We only override the ones
        // whose defaults assume a desktop build with a host window.
        auto &cfg = emuenv.cfg;

        // pref_path must be an UTF-8 std::string; Root already owns the
        // canonical fs::path version.
        cfg.set_pref_path(paths_pref());
        cfg.config_path = paths_base() / "config";

        // Persist nothing to disk implicitly — the libretro core re-reads
        // its state from core options on every run.
        cfg.overwrite_config = false;
        cfg.load_config      = false;
        cfg.fullscreen       = false;
        cfg.console          = false;
        cfg.load_app_list    = false;

        cfg.backend_renderer = "Vulkan";

        // Route SceAudio PCM through the libretro audio bridge instead
        // of cubeb / SDL3.  AudioState::set_backend understands this
        // value only when the engine was compiled with LIBRETRO
        // (see vita3k/audio/src/audio.cpp).
        cfg.audio_backend = "Libretro";

        // ----------------- M11.1: pull libretro core options -----------------
        // Snapshot is refreshed once in retro_set_environment() and again
        // every retro_run() that observes RETRO_ENVIRONMENT_GET_VARIABLE_
        // UPDATE.  Here we apply the snapshot to the engine's Config.
        const LibretroCoreOpts &opts = libretro_core_opts_current();

        // CPU backend selection (M11.2 + M12):
        //   "dynarmic_jit"          -> DynarmicCPU (JIT, all_safe_optimizations)
        //   "ir_interpreter"        -> IRInterpreterCPU (M12 PPSSPP-style IR
        //                                                cache-interpreter)
        //   "fallback_interpreter"  -> DynarmicCPU with cpu_opt=false
        //                             (no optimizations, step-mode M11.3)
        //
        // M12.7.1 hotfix: we **strictly obey** the user's `vita3k_cpu_backend`
        // value.  There is no runtime JIT probe, no auto-downgrade to
        // ir_interpreter when allocation might fail.  Rationale: the probe
        // had false-positives on some iOS versions (mmap(MAP_JIT) succeeding
        // as a leaked page, then the real alloc failing at 16 KiB) and
        // disobeying an explicit user choice was more confusing than useful.
        //
        // If the user picks dynarmic_jit on a host that can't allocate JIT
        // memory, the CodeBlock ctor inside Dynarmic will throw bad_alloc.
        // On iOS 26+ TXM, StikDebug must be attached for the first TXM region.
        const std::string &effective_backend = opts.cpu_backend;
        const bool wanted_jit = (effective_backend == "dynarmic_jit");
#if defined(__APPLE__) && TARGET_OS_IPHONE
        if (wanted_jit) {
            // Advisory only -- does not gate the backend choice.
            LOG_INFO("Vita3K libretro: dynarmic_jit selected on iOS; "
                     "host mode classification: {} (available={}). "
                     "If this fails to JIT, switch to ir_interpreter in "
                     "core options.",
                     vita3k_ios_jit_mode_string(),
                     vita3k_ios_jit_available() ? "yes" : "no");
        }
#endif
        cfg.cpu_backend        = effective_backend;
        cfg.jit_block_size_kib = opts.jit_block_size_kib;
        // cpu_opt still governs Dynarmic-internal optimizations when the
        // selected backend is Dynarmic. The IR interpreter ignores it.
        cfg.cpu_opt                    = wanted_jit;
        cfg.current_config.cpu_opt     = wanted_jit;
        // Propagate backend choice into KernelState so init_cpu() can branch
        // on it when creating per-thread CPU instances (see cpu.cpp::init_cpu).
        emuenv.kernel.cpu_backend = effective_backend;

        // Audio
        if (opts.audio_backend == "null")
            cfg.audio_backend = "SDL"; // engine "SDL" path sinks to /dev/null in stub
        cfg.audio_volume                  = opts.audio_volume;
        cfg.current_config.audio_volume   = opts.audio_volume;

        // Video — resolution multiplier and AF feed CurrentConfig directly.
        cfg.resolution_multiplier              = static_cast<float>(opts.resolution_multiplier);
        cfg.current_config.resolution_multiplier = static_cast<float>(opts.resolution_multiplier);
        cfg.anisotropic_filtering              = opts.anisotropic_filter;
        cfg.current_config.anisotropic_filtering = opts.anisotropic_filter;
        cfg.shader_cache                       = opts.shader_cache;
        cfg.current_config.disable_surface_sync = (opts.surface_sync == "fast");
        cfg.disable_surface_sync               = (opts.surface_sync == "fast");

        LOG_INFO("Vita3K libretro: cpu_backend={} cpu_opt={} jit_block_size_kib={} "
                 "res_mult={} surface_sync={}",
                 cfg.cpu_backend, cfg.cpu_opt, cfg.jit_block_size_kib,
                 opts.resolution_multiplier, opts.surface_sync);
    }

} // namespace

bool boot(EmuEnvState &emuenv) {
    if (g_booted.load(std::memory_order_acquire))
        return true;

    populate_config(emuenv);

    const Root &root = paths_root();

    // Config is move-assigned inside app::init, so keep a local copy of
    // our minimal values and let the engine take ownership.
    Config bootstrap;
    bootstrap = std::move(emuenv.cfg); // transfers the settings we just set
    // NOTE: after the move, emuenv.cfg is in a valid-but-unspecified state;
    // app::init will overwrite it with a fresh move-assigned value.

    if (!app::init(emuenv, bootstrap, root)) {
        LOG_ERROR("Vita3K libretro: app::init failed");
        return false;
    }
    LOG_INFO("Vita3K libretro: app::init OK (pref={})", emuenv.pref_path.string());

    if (!app::late_init(emuenv)) {
        LOG_ERROR("Vita3K libretro: app::late_init failed");
        return false;
    }
    LOG_INFO("Vita3K libretro: app::late_init OK (memory/audio/ngs ready)");

    // M12.7.5: mirror main.cpp:304 -- run all LIBRARY_INIT() callbacks so
    // that SysmemState, FiberState, AudiodecState, SharedFbState, ...
    // register themselves into emuenv.kernel.obj_store.  Without this
    // `obj_store.get<SysmemState>()` returns nullptr and the very first
    // sceKernelAllocMemBlock guest call (from SceLibc's module_start)
    // crashes with pthread_mutex_lock(NULL) on `state->mutex.lock()`.
    init_libraries(emuenv);
    LOG_INFO("Vita3K libretro: init_libraries OK (LIBRARY_INIT callbacks ran)");

    g_booted.store(true, std::memory_order_release);
    return true;
}

void shutdown(EmuEnvState &emuenv) {
    if (!g_booted.exchange(false, std::memory_order_acq_rel))
        return;

    // app::destroy is ImGui/discord heavy and expects a valid ImGui_State;
    // in the libretro core we tear down piecewise with the help of
    // EmuEnvState's unique_ptr members.  Just log that we are done here —
    // the actual deallocation runs when the EmuEnvState dtor fires.
    LOG_INFO("Vita3K libretro: boot shutdown (pref={})", emuenv.pref_path.string());
}

} // namespace vita3k_libretro
