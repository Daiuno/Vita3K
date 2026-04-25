// Vita3K emulator project - libretro game-boot bridge
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Launches a previously-installed Vita title from the libretro core.
//
// Replaces the app-selector GUI flow used by the stock desktop front-end:
// instead of `gui::get_app_index(gui, app_path)` giving us title/version/
// content-id, we read the values straight from
//   <pref>/ux0/app/<TITLE_ID>/sce_sys/param.sfo
// and fill the same EmuEnvState::io/app_info fields `load_app_impl()`
// expects.  Everything after that (kernel init, module loading, guest
// main thread creation, vblank sync thread) is reused verbatim via
// `load_app()` immediately, and defers `run_app()` until retro_run sees that
// the libretro frontend has created the Vulkan device
// (RETRO_HW_CONTEXT_RESET -> libretro_vulkan_context::try_init_renderer).
// This matches the libretro contract: hardware resources are only used after
// the HW context exists, and heavy guest work is kept out of context_reset.
//
// Called from retro_load_game when the content path starts with
// VITA3K_LIBRETRO_URI_SCHEME ("vita3k://").

#pragma once

#include "libretro_paths.h"

#include <string>

struct EmuEnvState;

namespace vita3k_libretro {

// Launches `title_id` (9-char Vita serial, e.g. "PCSG00042").  Returns
// true on success; on failure the caller should forward
// retro_vita_last_error() to the frontend.  Loads modules via `load_app`;
// `run_app` (guest main thread) is deferred until `start_guest_if_pending`
// runs from retro_run after the Vulkan context is ready.
bool launch_title(EmuEnvState &emuenv, const std::string &title_id);

// Runs the deferred `run_app` when `load_app` succeeded but the guest has
// not started yet.  Idempotent — safe to call every frame.  Returns false
// if `run_app` fails (caller should not treat the HW context as usable).
bool start_guest_if_pending(EmuEnvState &emuenv);

// True after load_app succeeded but before the deferred run_app has been
// consumed by retro_run.  Used only for diagnostics / frontend ordering
// checks.
bool has_pending_guest_start();

// True once `start_guest_if_pending` has completed successfully and a guest
// main thread is live.  retro_run() gates frame work on this flag.
bool is_title_running();

// Stops marking the title as running.  Does NOT tear down the guest
// main thread — only flips the `is_title_running` flag.  For a real
// tear-down call `unload_title()` instead.
void mark_title_stopped();

// Tears down the currently-running guest in-process while leaving the
// host EmuEnvState (libretro HW context, audio adapter, configured
// pref_path, ...) intact, so that a subsequent `launch_title()` can
// re-use the same core process for a different game.
//
// Order of operations:
//   1. mark_title_stopped()     — retro_run stops driving the engine
//   2. drain + abort the GXM display queue, exit_delete its host thread
//   3. exit_delete_all_threads() and **wait** until kernel.threads is
//      empty (or a timeout fires)
//   4. signal display.abort, join the vblank sync thread, then reset
//      display.abort back to false for the next game
//   5. stop_module() + unload_module() every loaded module in reverse
//      load order (mirrors `unload_sys_module` semantics)
//   6. clear IO open-file / dir-entry tables
//   7. clear AudioState::out_ports under audio.mutex (the libretro
//      audio adapter is preserved so its sink registration survives)
//   8. reset DisplayState/GxmState bookkeeping (frame counters, sync
//      objects, predicted-frame cache, memory_mapped_regions, ...)
//   9. drop the renderer command queue
//
// **Known limitations** — see Code Plan §M10.3 "Pitfalls":
//   • `KernelState::init` is *not* re-runnable cleanly: a second call
//     replaces `exclusive_monitor` without freeing the previous one
//     (one-shot leak per re-launch).
//   • `init_exported_vars` uses `unordered_map::emplace`, so any NIDs
//     already present from the first run keep their old addresses.
//   • A handful of static caches inside the engine (notably the GXM
//     `client_vtable`) point at guest memory that is freed by
//     `unload_self`; the next launch re-allocates fresh blocks but
//     the cache pointer is stale until `sceGxmInitialize` runs again.
//
// Returns true if every step succeeded; false if a partial failure
// happened (e.g. a guest thread did not exit within the timeout).  Even
// on partial failure the function is best-effort: as many resources as
// possible are released and `g_title_running` is left at false.
bool unload_title(EmuEnvState &emuenv);

} // namespace vita3k_libretro
