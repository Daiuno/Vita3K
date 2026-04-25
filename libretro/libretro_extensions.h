// Vita3K emulator project - libretro core extensions
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Vita3K-specific retro_vita_* API.  These functions are **not** part of the
// standard libretro API.  A custom RetroArch-iOS frontend (and only that
// frontend) may resolve them via `dlsym` against the loaded core.  If the
// frontend is stock RetroArch, none of these functions are ever called.
//
// All functions are C ABI and return POD types so they can be consumed from
// both Swift and Objective-C code in the iOS frontend.

#pragma once

#include <libretro.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 1. Firmware (PSVita .PUP)
// ---------------------------------------------------------------------------

typedef void (*retro_vita_progress_cb)(float fraction);

/**
 * Install the official PSVita firmware (PUP file).  Blocking; typical run
 * time is 30–60 seconds.  The PUP file will be consumed entirely; on success
 * the firmware modules are written under <save_dir>/Vita3K/os0/ and pd0/.
 *
 * @param pup_path  Absolute host filesystem path of the PUP.
 * @param progress  Optional progress callback (may be NULL).
 * @return  1 on success, 0 on failure.  Call retro_vita_last_error() to
 *          obtain a human-readable reason.
 */
RETRO_API int retro_vita_install_firmware(const char *pup_path,
                                          retro_vita_progress_cb progress);

/**
 * Returns the currently-installed firmware version string (e.g. "3.60"), or
 * NULL if no firmware is installed.  The returned buffer is owned by the
 * core and valid until the next call to retro_vita_install_firmware().
 */
RETRO_API const char *retro_vita_get_firmware_version(void);

// ---------------------------------------------------------------------------
// 2. Package / VPK installation
// ---------------------------------------------------------------------------

struct VitaInstallResult {
    int         success;          // 1 on success, 0 on failure
    const char *title;            // PARAM.SFO TITLE (UTF-8)
    const char *title_id;         // e.g. "PCSG00001"
    const char *content_id;       // e.g. "EP9000-PCSG00001_00-XXXXXX"
    const char *install_path;     // Absolute path to ux0:/app/<TITLE_ID>
    const void *icon_data;        // ICON0.PNG raw bytes (may be NULL)
    int         icon_size;        // icon_data length in bytes
    int         icon_width;       // Decoded width (0 if not decoded)
    int         icon_height;      // Decoded height (0 if not decoded)
    const char *error_message;    // NULL on success
};

/**
 * Install a PlayStation Store .pkg using the provided zRIF license.
 *
 * @param pkg_path  Absolute path to the .pkg file.
 * @param zrif      NULL-terminated zRIF string (game license).  Required.
 * @param progress  Optional progress callback (may be NULL).
 * @return  Pointer to a VitaInstallResult owned by the core.  Valid until the
 *          next call to retro_vita_install_pkg() / retro_vita_install_vpk().
 */
RETRO_API const struct VitaInstallResult *
retro_vita_install_pkg(const char *pkg_path, const char *zrif,
                       retro_vita_progress_cb progress);

/**
 * Install a VPK homebrew archive.  No zRIF required.
 */
RETRO_API const struct VitaInstallResult *
retro_vita_install_vpk(const char *vpk_path, retro_vita_progress_cb progress);

// ---------------------------------------------------------------------------
// 3. Game catalogue
// ---------------------------------------------------------------------------

struct VitaGameEntry {
    const char *title;            // Localised title from PARAM.SFO
    const char *title_id;         // e.g. "PCSG00001"
    const char *version;          // e.g. "01.00"
    const char *category;         // "gd" (gamedata), "gp" (patch), "ac" (addcont)
    const char *install_path;     // Absolute path to ux0:/app/<TITLE_ID>
    uint64_t    install_time;     // Unix timestamp
    uint64_t    play_time_seconds;
    const void *icon_data;        // ICON0.PNG raw bytes (may be NULL)
    int         icon_size;
};

/**
 * Return the list of installed games.  The list is owned by the core and
 * valid until the next call to retro_vita_list_games() or the next
 * successful install.
 *
 * @param out_count  Receives the number of entries.
 * @return  Pointer to an array of VitaGameEntry of length *out_count.
 */
RETRO_API const struct VitaGameEntry *retro_vita_list_games(int *out_count);

/**
 * Remove a game and all of its save data.
 * @return 1 on success, 0 on failure.
 */
RETRO_API int retro_vita_uninstall_game(const char *title_id);

// ---------------------------------------------------------------------------
// 4. Miscellaneous
// ---------------------------------------------------------------------------

/**
 * Return the last error message set by any retro_vita_* function, or an
 * empty string if the last call succeeded.
 */
RETRO_API const char *retro_vita_last_error(void);

/**
 * Return the core version string, e.g. "vita3k-libretro 0.1.0 (abcdef1)".
 */
RETRO_API const char *retro_vita_core_version(void);

/**
 * UTF-8 absolute path of the Vita3K pref-root (the parent of ux0/ur0/
 * os0/pd0/sa0/vs0).  Typically "<save_dir>/Vita3K/".  Empty until the
 * core has finished retro_init.  Owned by the core; valid for the
 * lifetime of the core instance.
 */
RETRO_API const char *retro_vita_get_pref_path(void);

/**
 * UTF-8 absolute path of the Vita3K cache root, typically
 * "<save_dir>/Vita3K/cache".  Suitable for temporary extracted package
 * contents / shader cache artefacts.
 */
RETRO_API const char *retro_vita_get_cache_path(void);

/**
 * Stable JIT status enum returned by retro_vita_jit_status().  Mirrors
 * vita3k_ios_jit_mode_t but kept duplicated here so the iOS frontend
 * doesn't need to include any other header to interpret the value.
 */
enum {
    VITA3K_JIT_STATUS_NONE              = 0, ///< Interpreter only / non-iOS
    VITA3K_JIT_STATUS_LEGACY_WX         = 1, ///< iOS <26 Legacy W^X (oaknut)
    VITA3K_JIT_STATUS_TXM_DUAL_MAPPING  = 2, ///< iOS 26+ TXM + StikDebug
    VITA3K_JIT_STATUS_PPL_DUAL_MAPPING  = 3  ///< iOS 26+ PPL dual map (non-TXM)
};

/**
 * Returns one of the VITA3K_JIT_STATUS_* values describing the active
 * (or, on hosts without iOS, theoretically-available) CPU recompiler
 * mode.  Calling this is cheap; the result is cached after the first
 * call.  Suitable for showing in a frontend "About" dialog.
 */
RETRO_API int retro_vita_jit_status(void);

/**
 * Human-readable counterpart to retro_vita_jit_status().  Owned by the
 * core; never NULL.
 */
RETRO_API const char *retro_vita_jit_status_string(void);

// ---------------------------------------------------------------------------
// 5. Virtual launch URI scheme
//
// retro_load_game() accepts a path of the form "vita3k://<TITLE_ID>".  The
// frontend obtains <TITLE_ID> from retro_vita_list_games() and passes it
// back via the standard RETRO_ENVIRONMENT_… flow.
// ---------------------------------------------------------------------------
#define VITA3K_LIBRETRO_URI_SCHEME "vita3k://"

#ifdef __cplusplus
} /* extern "C" */
#endif
