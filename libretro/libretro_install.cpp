// Vita3K emulator project - libretro install extensions
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Implements the retro_vita_* install/uninstall helpers defined in
// libretro_extensions.h.  Bridges directly to Vita3K's existing
// install_pup() / install_pkg() / install_archive() so all the heavy
// lifting (PUP segment decryption, PKG AES-CTR, zRIF parsing, VPK/ZIP
// extraction, PARAM.SFO scraping, license_manager bookkeeping) stays
// in the engine — libretro_install.cpp is 100% translation layer.
//
// Threading: libretro spec only permits retro_* calls from the frontend
// core thread, so we do not lock anything here.  The install functions
// are synchronous; the frontend should display a busy dialog while they
// run.
//
// Error handling: every failure path routes through
// vita3k_libretro_core::set_last_error() so the frontend can query via
// retro_vita_last_error().

#include "libretro_extensions.h"

#include "libretro_internal.h"

#include <emuenv/state.h>
#include <packages/archive.h>
#include <packages/functions.h>
#include <packages/pkg.h>
#include <packages/sfo.h>
#include <util/fs.h>
#include <util/log.h>

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Result cache — the retro_vita_install_* functions return a pointer to
// this TU-owned struct whose backing strings live in the scratch below.
// ---------------------------------------------------------------------------
namespace {

struct ResultCache {
    VitaInstallResult header{};
    std::string       title;
    std::string       title_id;
    std::string       content_id;
    std::string       install_path;
    std::string       error;
    std::vector<unsigned char> icon_bytes;

    void clear() {
        header = VitaInstallResult{};
        title.clear();
        title_id.clear();
        content_id.clear();
        install_path.clear();
        error.clear();
        icon_bytes.clear();
    }

    // After filling the std::string fields, point the C-string pointers.
    void publish() {
        header.title         = title.empty()        ? nullptr : title.c_str();
        header.title_id      = title_id.empty()     ? nullptr : title_id.c_str();
        header.content_id    = content_id.empty()   ? nullptr : content_id.c_str();
        header.install_path  = install_path.empty() ? nullptr : install_path.c_str();
        header.error_message = error.empty()        ? nullptr : error.c_str();
        if (!icon_bytes.empty()) {
            header.icon_data = icon_bytes.data();
            header.icon_size = static_cast<int>(icon_bytes.size());
        } else {
            header.icon_data = nullptr;
            header.icon_size = 0;
        }
        // icon_width / icon_height are left at 0 — the frontend decodes
        // PNG with its own stack (UIKit / stb_image / libpng).
        header.icon_width  = 0;
        header.icon_height = 0;
    }
};

ResultCache g_last_pkg_result;
ResultCache g_last_vpk_result;
std::string g_last_firmware_version;

// Read up to 1 MB of an ICON0.PNG into bytes, returns empty on any error.
std::vector<unsigned char> read_icon_blob(const fs::path &icon_path) {
    std::vector<unsigned char> bytes;
    // boost::filesystem::path is not implicitly convertible to std::filesystem::path
    // (which is what libc++ std::ifstream wants), so route through UTF-8.
    std::ifstream f(fs_utils::path_to_utf8(icon_path), std::ios::binary);
    if (!f.is_open())
        return bytes;
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<std::streamsize>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (sz <= 0 || sz > (1 << 20))
        return bytes;
    bytes.resize(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char *>(bytes.data()), sz))
        bytes.clear();
    return bytes;
}

// Populate ResultCache from EmuEnvState's last-populated app_info plus
// the pref_path / title_id path the installer wrote to.
void fill_result_from_emuenv(ResultCache &r, EmuEnvState &env) {
    r.title       = env.app_info.app_title;
    r.title_id    = env.app_info.app_title_id;
    r.content_id  = env.app_info.app_content_id;
    if (!r.title_id.empty()) {
        const fs::path p = env.pref_path / "ux0" / "app" / r.title_id;
        r.install_path = fs_utils::path_to_utf8(p);
        r.icon_bytes = read_icon_blob(p / "sce_sys" / "icon0.png");
    }
    r.header.success = 1;
    r.publish();
}

void fill_result_error(ResultCache &r, const std::string &msg) {
    r.header.success = 0;
    r.error = msg;
    r.publish();
}

} // namespace

// ---------------------------------------------------------------------------
// Firmware (.PUP)
// ---------------------------------------------------------------------------

RETRO_API int retro_vita_install_firmware(const char *pup_path,
                                          retro_vita_progress_cb progress) {
    auto &emuenv_ptr = vita3k_libretro_core::emuenv();
    if (!emuenv_ptr) {
        vita3k_libretro_core::set_last_error(
            "install_firmware: EmuEnvState not initialised (did retro_init succeed?)");
        return 0;
    }
    if (!pup_path || !*pup_path) {
        vita3k_libretro_core::set_last_error("install_firmware: NULL pup path");
        return 0;
    }

    fs::path pup{ pup_path };
    if (!fs::exists(pup)) {
        vita3k_libretro_core::set_last_error(
            std::string{ "install_firmware: file not found: " } + pup_path);
        return 0;
    }

    std::function<void(uint32_t)> cb;
    if (progress) {
        cb = [progress](uint32_t p) { progress(p / 100.0f); };
    }

    const std::string err = install_pup(emuenv_ptr->pref_path, pup, cb);
    if (!err.empty()) {
        vita3k_libretro_core::set_last_error(
            std::string{ "install_pup: " } + err);
        return 0;
    }

    LOG_INFO("[Vita3K] firmware installed into {}",
             fs_utils::path_to_utf8(emuenv_ptr->pref_path / "os0"));
    vita3k_libretro_core::set_last_error(nullptr);
    return 1;
}

RETRO_API const char *retro_vita_get_firmware_version(void) {
    // Vita3K stores the firmware version in os0:/kd/registry.db0 / version.xml,
    // but Vita3K itself does not expose a helper.  Cheap heuristic: look for
    // os0:/kd/SceSblUpdateMgr.skprx and — if present — trust the installer.
    // We return the placeholder "installed" so the frontend can at least
    // render a "firmware: yes" label.  A proper parse ships with M8+.
    auto &emuenv_ptr = vita3k_libretro_core::emuenv();
    if (!emuenv_ptr)
        return nullptr;

    const fs::path probe = emuenv_ptr->pref_path / "os0" / "kd";
    if (!fs::exists(probe) || fs::is_empty(probe)) {
        g_last_firmware_version.clear();
        return nullptr;
    }
    if (g_last_firmware_version.empty())
        g_last_firmware_version = "installed";
    return g_last_firmware_version.c_str();
}

// ---------------------------------------------------------------------------
// PKG
// ---------------------------------------------------------------------------

RETRO_API const struct VitaInstallResult *
retro_vita_install_pkg(const char *pkg_path, const char *zrif,
                       retro_vita_progress_cb progress) {
    auto &r = g_last_pkg_result;
    r.clear();

    auto &emuenv_ptr = vita3k_libretro_core::emuenv();
    if (!emuenv_ptr) {
        fill_result_error(r,
            "install_pkg: EmuEnvState not initialised");
        vita3k_libretro_core::set_last_error(r.error);
        return &r.header;
    }
    if (!pkg_path || !*pkg_path) {
        fill_result_error(r, "install_pkg: NULL pkg path");
        vita3k_libretro_core::set_last_error(r.error);
        return &r.header;
    }
    fs::path pkg{ pkg_path };
    if (!fs::exists(pkg)) {
        fill_result_error(r,
            std::string{ "install_pkg: file not found: " } + pkg_path);
        vita3k_libretro_core::set_last_error(r.error);
        return &r.header;
    }

    // install_pkg unconditionally invokes `progress_callback(0)` so we
    // must always pass a non-empty std::function.
    std::function<void(float)> cb = [progress](float f) {
        if (progress) progress(f);
    };

    std::string zrif_s = zrif ? zrif : "";
    const bool ok = install_pkg(pkg, *emuenv_ptr, zrif_s, cb);
    if (!ok) {
        fill_result_error(r,
            "install_pkg failed (check zRIF, pkg integrity, or stderr logs)");
        vita3k_libretro_core::set_last_error(r.error);
        return &r.header;
    }

    fill_result_from_emuenv(r, *emuenv_ptr);
    vita3k_libretro_core::set_last_error(nullptr);
    return &r.header;
}

// ---------------------------------------------------------------------------
// VPK / ZIP
// ---------------------------------------------------------------------------

RETRO_API const struct VitaInstallResult *
retro_vita_install_vpk(const char *vpk_path, retro_vita_progress_cb progress) {
    auto &r = g_last_vpk_result;
    r.clear();

    auto &emuenv_ptr = vita3k_libretro_core::emuenv();
    if (!emuenv_ptr) {
        fill_result_error(r, "install_vpk: EmuEnvState not initialised");
        vita3k_libretro_core::set_last_error(r.error);
        return &r.header;
    }
    if (!vpk_path || !*vpk_path) {
        fill_result_error(r, "install_vpk: NULL vpk path");
        vita3k_libretro_core::set_last_error(r.error);
        return &r.header;
    }
    fs::path vpk{ vpk_path };
    if (!fs::exists(vpk)) {
        fill_result_error(r,
            std::string{ "install_vpk: file not found: " } + vpk_path);
        vita3k_libretro_core::set_last_error(r.error);
        return &r.header;
    }

    // archive::install_archive() flattens ArchiveContents{count, current,
    // progress} into a single float 0..1 for the libretro callback.  The
    // header reports per-item progress; install_archive_content reports
    // intra-item byte progress (0..100).  Combining gives a smooth bar
    // across multi-content VPKs.
    archive::ProgressCallback cb;
    if (progress) {
        cb = [progress](archive::ArchiveContents pc) {
            if (pc.progress.has_value()) {
                // Intra-item progress: 0..100 -> 0..1.
                progress(pc.progress.value() / 100.0f);
            } else if (pc.count.has_value() && pc.current.has_value() && pc.count.value() > 0.f) {
                progress(pc.current.value() / pc.count.value());
            }
        };
    }

    // Empty hooks -> silent auto-overwrite (no reinstall prompt, no
    // apps-cache flush).  The libretro frontend refreshes its catalog
    // by re-calling retro_vita_list_games() after install returns.
    const std::vector<archive::ContentInfo> results =
        archive::install_archive(*emuenv_ptr, vpk, cb, /*hooks=*/{});

    if (results.empty()) {
        fill_result_error(r,
            "install_archive returned no contents "
            "(check .vpk integrity and LOG output)");
        vita3k_libretro_core::set_last_error(r.error);
        return &r.header;
    }

    // Prefer the first successfully-installed gd entry (= the real app)
    // over any ancillary patch/dlc/theme bundles in the same VPK.
    const archive::ContentInfo *selected = nullptr;
    for (const auto &ci : results) {
        if (!ci.state)
            continue;
        if (ci.category.find("gd") != std::string::npos) {
            selected = &ci;
            break;
        }
        if (!selected)
            selected = &ci;
    }
    if (!selected) {
        fill_result_error(r, "install_archive: all contents failed to install");
        vita3k_libretro_core::set_last_error(r.error);
        return &r.header;
    }

    r.title      = selected->title;
    r.title_id   = selected->title_id;
    r.content_id = selected->content_id;
    if (!r.title_id.empty()) {
        const fs::path app_dir = emuenv_ptr->pref_path / "ux0" / "app" / r.title_id;
        if (fs::exists(app_dir)) {
            r.install_path = fs_utils::path_to_utf8(app_dir);
            r.icon_bytes = read_icon_blob(app_dir / "sce_sys" / "icon0.png");
        }
    }
    r.publish();

    LOG_INFO("[Vita3K] VPK installed: {} [{}] ({} contents, {} succeeded)",
        r.title, r.title_id, results.size(),
        std::count_if(results.begin(), results.end(),
            [](const archive::ContentInfo &c) { return c.state; }));

    vita3k_libretro_core::set_last_error(nullptr);
    return &r.header;
}

// ---------------------------------------------------------------------------
// Uninstall
// ---------------------------------------------------------------------------

RETRO_API int retro_vita_uninstall_game(const char *title_id) {
    auto &emuenv_ptr = vita3k_libretro_core::emuenv();
    if (!emuenv_ptr) {
        vita3k_libretro_core::set_last_error(
            "uninstall_game: EmuEnvState not initialised");
        return 0;
    }
    if (!title_id || !*title_id) {
        vita3k_libretro_core::set_last_error("uninstall_game: NULL title_id");
        return 0;
    }

    // Wipe ux0:/app/<ID>, ur0:/user/<hash>/<ID>, ux0:/patch/<ID>,
    // ux0:/addcont/<ID>, ux0:/license/<ID>, ux0:/savedata/<ID>.
    // Vita3K's content manager does the same walk; we replicate it in
    // a stand-alone form because content_manager::delete_app() lives in
    // gui/ which is not linked against this TU's dependency tree.
    const fs::path &pref = emuenv_ptr->pref_path;
    const std::string tid{ title_id };
    const fs::path targets[] = {
        pref / "ux0" / "app"       / tid,
        pref / "ux0" / "patch"     / tid,
        pref / "ux0" / "addcont"   / tid,
        pref / "ux0" / "license"   / tid,
        pref / "ux0" / "savedata"  / tid,
    };
    boost::system::error_code ec;
    bool wiped = false;
    for (const auto &t : targets) {
        if (fs::exists(t)) {
            fs::remove_all(t, ec);
            if (!ec) {
                wiped = true;
            } else {
                vita3k_libretro_core::set_last_error(
                    std::string{ "uninstall_game: remove_all failed on " }
                    + fs_utils::path_to_utf8(t)
                    + ": " + ec.message());
                return 0;
            }
        }
    }
    if (!wiped) {
        vita3k_libretro_core::set_last_error(
            std::string{ "uninstall_game: no files found for title_id=" } + tid);
        return 0;
    }
    vita3k_libretro_core::set_last_error(nullptr);
    LOG_INFO("[Vita3K] uninstalled title_id={}", tid);
    return 1;
}
