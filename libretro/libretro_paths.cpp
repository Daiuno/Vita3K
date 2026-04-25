// Vita3K emulator project - libretro <-> Vita3K path mapping
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Maps RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY into a Vita3K `pref_path` and
// materializes the guest-visible mount points (ux0, ur0, os0, pd0, ...).
//
// Layout, rooted at <save_dir>/Vita3K/:
//   base/                    shared/static assets shipped with the core
//   pref/                    Vita3K "user prefs" root  (== pref_path)
//     ux0/{app,user/00/savedata,addcont,temp,patch,pspemu}
//     ur0/{user/00/savedata,shell}
//     os0/                   firmware-installed modules
//     pd0/                   vsh / system
//     sa0/                   sce_sys_conf (firmware)
//     vs0/                   app browser etc.
//   cache/                   shader cache, thumbnails...
//   logs/{shaderlog,texturelog}
//   config/                  config.yml, custom_configs/
//   patch/                   game-specific patches
//
// Everything is created lazily; if the directories already exist no work
// happens on subsequent calls.

#include "libretro_paths.h"

#include <util/fs.h>
#include <util/log.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

#if defined(__APPLE__)
#    include <dlfcn.h>
#endif

namespace vita3k_libretro {

namespace {

    std::once_flag g_init_once;
    Root           g_root;

    void ensure_directory(const fs::path &p) {
        boost::system::error_code ec;
        fs::create_directories(p, ec);
        // Ignore errors here; the frontend save dir may already exist or be
        // read-only.  We log them from libretro.cpp when the tree is unusable.
        (void)ec;
    }

#if defined(__APPLE__)
    // Resolve the full path of the framework bundle we are running from.
    // Returns an empty string on failure; the caller falls back to the
    // base path (pref root) which won't have shaders-builtin/ but at
    // least won't crash.
    fs::path resolve_framework_root() {
        Dl_info info{};
        // Pass the address of any symbol defined in this TU.
        if (!dladdr(reinterpret_cast<const void *>(&resolve_framework_root), &info) || !info.dli_fname) {
            return {};
        }
        // dli_fname -> ".../vita3k.libretro.framework/vita3k.libretro"
        // framework root -> ".../vita3k.libretro.framework"
        return fs::path(info.dli_fname).parent_path();
    }
#else
    fs::path resolve_framework_root() { return {}; }
#endif

    void populate_vita_tree(const fs::path &pref) {
        // Core Vita partitions mapped by SceIO.
        static const std::array<const char *, 6> kPartitions = {
            "ux0", "ur0", "os0", "pd0", "sa0", "vs0"
        };
        for (auto *p : kPartitions) {
            ensure_directory(pref / p);
        }
        // ux0 sub-folders that Vita3K's IO layer expects to exist up-front.
        ensure_directory(pref / "ux0" / "app");
        ensure_directory(pref / "ux0" / "addcont");
        ensure_directory(pref / "ux0" / "patch");
        ensure_directory(pref / "ux0" / "temp");
        ensure_directory(pref / "ux0" / "user" / "00" / "savedata");
        ensure_directory(pref / "ux0" / "pspemu");
        // ur0 sub-folders
        ensure_directory(pref / "ur0" / "user" / "00" / "savedata");
        ensure_directory(pref / "ur0" / "shell");
    }

} // namespace

const Root &paths_init(const char *save_dir) {
    std::call_once(g_init_once, [save_dir]() {
        // Resolve the root of our sandbox.  If the frontend failed to provide
        // a save directory we fall back to the current working directory so
        // the core still functions (mostly useful when sideloaded for tests).
        const std::string save_root = (save_dir && *save_dir) ? save_dir : ".";
        const fs::path base  = fs::path(save_root);
        const fs::path pref  = base / "pref";
        const fs::path cache = base / "cache";
        const fs::path logs  = base / "logs";
        const fs::path conf  = base / "config";
        const fs::path patch = base / "patch";

        ensure_directory(base);
        ensure_directory(pref);
        ensure_directory(cache);
        ensure_directory(logs / "shaderlog");
        ensure_directory(logs / "texturelog");
        ensure_directory(conf);
        ensure_directory(conf / "custom_config");
        ensure_directory(patch);
        populate_vita_tree(pref);

        g_root.set_base_path(base);
        g_root.set_pref_path(pref);
        g_root.set_cache_path(cache);
        g_root.set_log_path(logs);
        g_root.set_config_path(conf);
        g_root.set_patch_path(patch);
        g_root.set_shared_path(base);            // libretro ships shared assets inside the framework

        // M12.7.6: static assets (shaders-builtin/, possibly lang/ later)
        // live inside the framework bundle, not in the save dir.  Locate
        // the framework via dladdr at runtime; if that fails for any
        // reason (non-Apple, static link, ...) fall back to `base` so
        // at least the emulator can still attempt to run (the renderer
        // will then fail with a clear "Couldn't open shader" message).
        const fs::path framework_root = resolve_framework_root();
        boost::system::error_code fr_ec;
        if (!framework_root.empty() && fs::exists(framework_root / "shaders-builtin", fr_ec)) {
            g_root.set_static_assets_path(framework_root);
        } else {
            g_root.set_static_assets_path(base);
        }

        // Initialise spdlog before any engine LOG_* (e.g. [VITA3K-LR] in renderer).
        // On Apple libretro builds, logging::init also registers an os_log sink so
        // messages show in Console.app (stdout alone is often invisible when the
        // core is loaded inside RetroArch / StikDebug without Xcode attached).
        if (logging::init(g_root, false) != Success) {
            // Continue: boot may still work; user loses file + unified log until fixed.
        }
        // Vita3K already writes engine logs under <save>/logs/vita3k.log; libretro
        // does not load config.yml, so spdlog would otherwise stay at its default.
        // Force TRACE so LOG_TRACE / LOG_TRACE_IF match Config::log_level default.
        logging::set_level(spdlog::level::trace);
    });
    return g_root;
}

const Root &paths_root() {
    return g_root;
}

fs::path paths_pref() {
    return g_root.get_pref_path();
}

fs::path paths_base() {
    return g_root.get_base_path();
}

} // namespace vita3k_libretro

// ---------------------------------------------------------------------------
// C shims
// ---------------------------------------------------------------------------

extern "C" void libretro_paths_init(const char *save_dir) {
    (void)vita3k_libretro::paths_init(save_dir);
}

extern "C" const char *libretro_paths_pref(void) {
    static std::string cached;
    cached = vita3k_libretro::paths_pref().string();
    return cached.c_str();
}
