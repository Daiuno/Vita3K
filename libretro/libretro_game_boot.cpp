// Vita3K emulator project - libretro game-boot bridge
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "libretro_game_boot.h"

#include "libretro_internal.h"

#include <audio/state.h>
#include <config/state.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <gxm/state.h>
#include <interface.h>
#include <io/functions.h>
#include <io/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <modules/module_parent.h>
#include <packages/license.h>
#include <packages/sfo.h>
#include <renderer/state.h>
#include <util/fs.h>
#include <util/log.h>

#include <boost/algorithm/string.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vita3k_libretro {

namespace {

std::atomic<bool> g_title_running{ false };
std::mutex g_launch_mutex;
// Set after load_app succeeds; consumed by start_guest_if_pending after Vulkan is live.
std::optional<int32_t> g_pending_main_module_id;

static bool has_pending_launch() {
    const std::lock_guard<std::mutex> lk(g_launch_mutex);
    return g_pending_main_module_id.has_value();
}

bool read_param_sfo(const fs::path &sfo_path, sfo::SfoAppInfo &out, int sys_lang) {
    std::ifstream f(fs_utils::path_to_utf8(sfo_path), std::ios::binary);
    if (!f.is_open())
        return false;
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<std::streamsize>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (sz <= 0 || sz > (1 << 20))
        return false;
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char *>(buf.data()), sz))
        return false;
    try {
        sfo::get_param_info(out, buf, sys_lang);
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace

bool launch_title(EmuEnvState &emuenv, const std::string &title_id) {
    if (title_id.empty()) {
        vita3k_libretro_core::set_last_error("launch_title: empty TITLE_ID");
        return false;
    }
    if (g_title_running.load(std::memory_order_acquire) || has_pending_launch()) {
        // A previous title is still live, or load_app completed without a
        // matching run_app yet.  Tear it down first so the rest of this
        // function can reuse the same EmuEnvState.  Note the well-known
        // caveats listed in unload_title()'s docs (one exclusive_monitor
        // leak per re-launch, stale exports, ...).
        LOG_WARN("[Vita3K] launch_title: re-using core process for '{}'; unloading previous title", title_id);
        if (!unload_title(emuenv)) {
            vita3k_libretro_core::set_last_error(
                "launch_title: previous title still running; unload_title failed");
            return false;
        }
    }

    const fs::path app_dir = emuenv.pref_path / "ux0" / "app" / fs_utils::utf8_to_path(title_id);
    boost::system::error_code ec;
    if (!fs::exists(app_dir, ec) || !fs::is_directory(app_dir, ec)) {
        vita3k_libretro_core::set_last_error(
            std::string{ "launch_title: app not installed: " } + title_id);
        return false;
    }

    // Populate the fields that load_app_impl expects.  The stock flow
    // pulls these from gui.app_selector.user_apps[] (populated at home-
    // screen scan time); here we read param.sfo directly.
    sfo::SfoAppInfo info;
    const fs::path sfo_path = app_dir / "sce_sys" / "param.sfo";
    if (!read_param_sfo(sfo_path, info, emuenv.cfg.sys_lang)) {
        vita3k_libretro_core::set_last_error(
            std::string{ "launch_title: failed to parse param.sfo for " } + title_id);
        return false;
    }

    boost::trim(info.app_title);
    boost::trim(info.app_title_id);
    boost::trim(info.app_version);
    boost::trim(info.app_category);
    boost::trim(info.app_content_id);
    boost::trim(info.app_short_title);
    boost::trim(info.app_savedata);
    boost::trim(info.app_addcont);

    // app_path = directory name; falls back to title_id argument if
    // param.sfo is missing one.
    emuenv.io.app_path  = title_id;
    emuenv.io.title_id  = info.app_title_id.empty() ? title_id : info.app_title_id;
    emuenv.io.content_id = info.app_content_id;
    emuenv.io.addcont = info.app_addcont;
    // Must match desktop main.cpp (gui::get_app_index → APP_INDEX->savedata).
    // init_savedata_app_path() only mkdirs; it does NOT set io.savedata. Leaving
    // savedata empty makes device_paths.savedata0 "user//savedata/" and breaks
    // savedata0: → wrong host path (missing .../user/00/savedata/<title>/...).
    emuenv.io.savedata = !info.app_savedata.empty() ? info.app_savedata : emuenv.io.title_id;
    if (emuenv.io.user_id.empty()) {
        emuenv.io.user_id = !emuenv.cfg.user_id.empty() ? emuenv.cfg.user_id : std::string("00");
    }

    emuenv.app_info.app_version     = info.app_version;
    emuenv.app_info.app_category    = info.app_category;
    emuenv.app_info.app_content_id  = info.app_content_id;
    emuenv.app_info.app_title       = info.app_title;
    emuenv.app_info.app_title_id    = emuenv.io.title_id;
    emuenv.app_info.app_short_title = info.app_short_title.empty()
                                        ? info.app_title
                                        : info.app_short_title;

    emuenv.current_app_title = info.app_title;

    // Best-effort license wiring.  For most homebrew/gd apps this is a
    // no-op; for .rif-gated content it ensures NoNpDrm tables are set.
    try {
        get_license(emuenv, emuenv.io.title_id, emuenv.io.content_id);
    } catch (...) {
        LOG_WARN("[Vita3K] launch_title: get_license threw; continuing without DRM");
    }

    int32_t main_module_id = -1;
    if (load_app(main_module_id, emuenv) != Success) {
        vita3k_libretro_core::set_last_error(
            std::string{ "launch_title: load_app failed for " } + title_id);
        return false;
    }

    {
        const std::lock_guard<std::mutex> lk(g_launch_mutex);
        g_pending_main_module_id = main_module_id;
    }

    // M12.7.9: mirror main.cpp:503 -- bind the renderer's shader cache /
    // shader log paths to this title when the renderer already exists.
    // Otherwise libretro_vulkan_context::try_init_renderer calls set_app
    // after Vulkan comes up, then retro_run consumes the pending run_app.
    if (emuenv.renderer && !emuenv.io.title_id.empty() && !emuenv.self_name.empty()) {
        emuenv.renderer->set_app(emuenv.io.title_id.c_str(), emuenv.self_name.c_str());
        LOG_INFO("[Vita3K] renderer->set_app('{}','{}') bound shader cache paths",
                 emuenv.io.title_id, emuenv.self_name);
    }

    LOG_INFO("[Vita3K] load_app finished for '{}' [{}] v{} category={} — deferring run_app until Vulkan HW context is ready",
        emuenv.current_app_title, emuenv.io.title_id,
        emuenv.app_info.app_version, emuenv.app_info.app_category);

    vita3k_libretro_core::set_last_error(nullptr);
    return true;
}

bool start_guest_if_pending(EmuEnvState &emuenv) {
    int32_t main_module_id = -1;
    {
        const std::lock_guard<std::mutex> lk(g_launch_mutex);
        if (!g_pending_main_module_id.has_value())
            return true;
        main_module_id = *g_pending_main_module_id;
    }

    LOG_INFO("[Vita3K] Deferred run_app starting for module id {}", main_module_id);
    if (run_app(emuenv, main_module_id) != Success) {
        LOG_ERROR("[Vita3K] Deferred run_app failed for module id {}", main_module_id);
        {
            const std::lock_guard<std::mutex> lk(g_launch_mutex);
            g_pending_main_module_id.reset();
        }
        vita3k_libretro_core::set_last_error("run_app failed after Vulkan context was ready");
        unload_title(emuenv);
        return false;
    }

    {
        const std::lock_guard<std::mutex> lk(g_launch_mutex);
        g_pending_main_module_id.reset();
    }
    g_title_running.store(true, std::memory_order_release);
    LOG_INFO("[Vita3K] Deferred run_app completed; guest is running");
    return true;
}

bool has_pending_guest_start() {
    return has_pending_launch();
}

bool is_title_running() {
    return g_title_running.load(std::memory_order_acquire);
}

void mark_title_stopped() {
    g_title_running.store(false, std::memory_order_release);
}

namespace {

// Wait until `kernel.threads` is empty, polling at ~5 ms intervals.
// Returns true if the map drained, false on timeout.  The host-side
// SDL thread that hosts each guest thread removes itself from the map
// inside `thread_function` (kernel.cpp:74) once `run_loop()` returns,
// so a non-empty map means at least one guest is still executing CPU
// frames or sleeping in a wait primitive that did not honour our
// `exit_delete` request yet.
bool wait_for_threads_to_exit(KernelState &kernel,
                              std::chrono::milliseconds budget) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + budget;

    while (true) {
        std::size_t remaining;
        {
            const std::lock_guard<std::mutex> lk(kernel.mutex);
            remaining = kernel.threads.size();
        }
        if (remaining == 0)
            return true;
        if (clock::now() >= deadline) {
            LOG_WARN("[Vita3K] unload_title: {} guest thread(s) still alive after {} ms",
                     remaining, budget.count());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Stop + unload every module currently present in `kernel.loaded_modules`,
// in reverse load order (mirrors `unload_sys_module` in module_parent.cpp:
// 369-384).  Modules whose `module_stop` hangs (rare for vita SELFs at
// this stage of tear-down because their owning threads have already been
// `exit_delete`'d) are skipped: the stop_module path runs the entry on a
// **fresh** thread it creates internally, which means it may itself spin
// up new guest threads.  We accept that cost — leaking stop_modules is
// preferable to leaving the import jumptable dangling.
void unload_all_modules(EmuEnvState &emuenv) {
    std::vector<SceUID> uids;
    {
        const std::lock_guard<std::mutex> lk(emuenv.kernel.mutex);
        uids.reserve(emuenv.kernel.loaded_modules.size());
        for (const auto &[uid, _] : emuenv.kernel.loaded_modules)
            uids.push_back(uid);
    }
    std::sort(uids.begin(), uids.end(), std::greater<SceUID>{});

    for (SceUID uid : uids) {
        // Re-fetch the shared_ptr each iteration; a previous unload may
        // have cascaded and dropped this entry already.
        SceKernelModulePtr module;
        {
            const std::lock_guard<std::mutex> lk(emuenv.kernel.mutex);
            auto it = emuenv.kernel.loaded_modules.find(uid);
            if (it == emuenv.kernel.loaded_modules.end())
                continue;
            module = it->second;
        }
        if (!module)
            continue;
        // Best-effort module_stop entry — ignore errors, we are tearing
        // down regardless.
        try {
            stop_module(emuenv, module->info);
        } catch (const std::exception &e) {
            LOG_WARN("[Vita3K] unload_title: stop_module({}) threw: {}",
                     module->info.module_name, e.what());
        }
        const int rc = unload_module(emuenv, uid);
        if (rc < 0) {
            LOG_WARN("[Vita3K] unload_title: unload_module({} '{}') -> {}",
                     uid, module->info.module_name, rc);
        }
    }

    // Drop any stragglers that survived the cascade above.  Without this
    // step a re-launch would re-find them via lock_and_find and short-
    // circuit the new SELF load.
    const std::lock_guard<std::mutex> lk(emuenv.kernel.mutex);
    emuenv.kernel.loaded_modules.clear();
    emuenv.kernel.loaded_sysmodules.clear();
    emuenv.kernel.loaded_internal_sysmodules.clear();
}

void reset_display_state(DisplayState &display) {
    {
        const std::lock_guard<std::mutex> lk(display.display_info_mutex);
        display.next_rendered_frame = DisplayFrameInfo{};
    }
    display.sce_frame = DisplayFrameInfo{};
    display.vblank_count.store(0);
    display.last_setframe_vblank_count.store(0);
    display.vblank_wait_infos.clear();
    display.vblank_callbacks.clear();
    display.predicted_frames.clear();
    display.predicted_frame_position = static_cast<uint32_t>(-1);
    display.predicted_cycles_seen = 0;
    display.predicting.store(false);
    display.current_sync_object.store(0);
    // abort flag is cleared explicitly *after* the vblank thread has
    // joined, otherwise a torn-down thread could see false too early.
}

void reset_gxm_state(GxmState &gxm) {
    gxm.params = SceGxmInitializeParams{};
    gxm.display_queue_thread = 0;
    gxm.global_timestamp.store(1);
    gxm.last_display_global = 0;
    gxm.notification_region = Ptr<uint32_t>{};
    {
        const std::lock_guard<std::mutex> lk(gxm.callback_lock);
        gxm.memory_mapped_regions.clear();
    }
    gxm.display_queue.reset();
}

void reset_io_state(IOState &io) {
    io.tty_files.clear();
    io.std_files.clear();
    io.dir_entries.clear();
    io.cachemap.clear();
    {
        const std::lock_guard<std::mutex> lk(io.overlay_mutex);
        io.overlays.clear();
        io.next_overlay_id = 1;
    }
    io.next_fd = 0;
    // device_paths / title_id / app_path are re-populated by
    // launch_title() before calling load_app_impl, so leave them alone.
}

void clear_audio_ports(AudioState &audio) {
    // Match `AudioState::set_backend` (audio.cpp:46) — drop ports
    // before anything else so guest sceAudioOut* handles become
    // invalid in lock-step with the kernel's UID drop.
    const std::lock_guard<std::mutex> lk(audio.mutex);
    audio.out_ports.clear();
    audio.next_port_id = 1;
}

} // namespace

bool unload_title(EmuEnvState &emuenv) {
    bool pending_launch = false;
    {
        const std::lock_guard<std::mutex> lk(g_launch_mutex);
        pending_launch = g_pending_main_module_id.has_value();
    }

    if (!g_title_running.load(std::memory_order_acquire) && !pending_launch) {
        const std::lock_guard<std::mutex> lk(emuenv.kernel.mutex);
        if (emuenv.kernel.loaded_modules.empty() && emuenv.kernel.threads.empty())
            return true;
    }

    LOG_INFO("[Vita3K] unload_title: tearing down '{}' [{}]",
             emuenv.current_app_title, emuenv.io.title_id);

    // 1. Block retro_run() from advancing engine frames.  Subsequent
    //    libretro_audio_drain() calls remain safe because our PCM ring
    //    is independent of EmuEnvState lifecycle.
    g_title_running.store(false, std::memory_order_release);
    {
        const std::lock_guard<std::mutex> lk(g_launch_mutex);
        g_pending_main_module_id.reset();
    }

    // 2. GXM display queue: wait for in-flight callbacks, then abort
    //    so the detached display_entry_thread (SceGxm.cpp:895) breaks
    //    out of its `top()` wait.  Then exit_delete the guest-side
    //    SceGxmDisplayQueue thread that we registered in
    //    sceGxmInitialize.  Mirror order is taken from sceGxmTerminate
    //    (SceGxm.cpp:4676-4681).
    emuenv.gxm.display_queue.wait_empty();
    emuenv.gxm.display_queue.abort();
    if (emuenv.gxm.display_queue_thread) {
        if (auto t = emuenv.kernel.get_thread(emuenv.gxm.display_queue_thread))
            t->exit_delete();
    }

    // 3. Stop every guest thread.  exit_delete_all_threads only
    //    *signals* — the host SDL threads still need a tick to drain
    //    their run_loop and erase themselves from kernel.threads.
    emuenv.kernel.exit_delete_all_threads();
    if (emuenv.renderer)
        emuenv.renderer->notification_ready.notify_all();

    const bool threads_drained = wait_for_threads_to_exit(
        emuenv.kernel, std::chrono::milliseconds(3000));

    // 4. vblank thread.  It loops on `display.abort` (display.cpp:40);
    //    flip the flag, wake any waiters, join, drop the unique_ptr,
    //    then reset the flag so start_sync_thread on the next launch
    //    does not exit immediately.
    emuenv.display.abort.store(true);
    if (emuenv.renderer)
        emuenv.renderer->notification_ready.notify_all();
    if (emuenv.display.vblank_thread) {
        try {
            if (emuenv.display.vblank_thread->joinable())
                emuenv.display.vblank_thread->join();
        } catch (const std::exception &e) {
            LOG_WARN("[Vita3K] unload_title: vblank join threw: {}", e.what());
        }
        emuenv.display.vblank_thread.reset();
    }
    emuenv.display.abort.store(false);

    // 5. Modules.  Done *after* threads stop so module_stop hooks
    //    cannot race the kernel state we are about to clear.
    unload_all_modules(emuenv);

    // 6. IO open handles.
    reset_io_state(emuenv.io);

    // 7. Audio ports — keep the LibretroAudioAdapter alive (its sink
    //    registration in libretro_audio.cpp survives across loads).
    clear_audio_ports(emuenv.audio);

    // 8. Engine bookkeeping that must be zeroed for a clean re-launch.
    reset_display_state(emuenv.display);
    reset_gxm_state(emuenv.gxm);

    emuenv.frame_count = 0;
    emuenv.fps         = 0;
    emuenv.ms_per_frame = 0;
    emuenv.sdl_ticks   = 0;

    emuenv.io.title_id.clear();
    emuenv.io.app_path.clear();
    emuenv.io.content_id.clear();
    emuenv.io.savedata.clear();
    emuenv.io.addcont.clear();
    emuenv.app_info  = {};
    emuenv.self_path.clear();
    emuenv.self_name.clear();
    emuenv.current_app_title.clear();

    // 9. Renderer command queue — drop anything the GXM thread queued
    //    after its callback already ran.  The HW context itself stays
    //    alive (the libretro front-end owns the GL context).
    if (emuenv.renderer)
        emuenv.renderer->command_buffer_queue.reset();

    if (!threads_drained) {
        vita3k_libretro_core::set_last_error(
            "unload_title: timed out waiting for guest threads to exit");
        return false;
    }
    vita3k_libretro_core::set_last_error(nullptr);
    return true;
}

} // namespace vita3k_libretro
