// Vita3K emulator project - libretro game catalog extension
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Implements retro_vita_list_games() — walks <pref>/ux0/app/<TITLE_ID>,
// parses each sce_sys/param.sfo via Vita3K's sfo helpers, and returns a
// contiguous array of VitaGameEntry pointing into a TU-owned scratch.
//
// Design:
//  - Cache is rebuilt on every call (cheap: ~100 apps × tens of ms).
//  - Returned char* / void* pointers are valid until the next call to
//    retro_vita_list_games().  This matches the contract documented in
//    libretro_extensions.h.
//  - No icon decoding in the core: icon_data/icon_size point at the raw
//    PNG bytes; the frontend decodes.

#include "libretro_extensions.h"

#include "libretro_internal.h"

#include <config/state.h>
#include <emuenv/state.h>
#include <packages/sfo.h>
#include <util/fs.h>
#include <util/log.h>

#include <boost/algorithm/string.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct EntryStorage {
    std::string title;
    std::string title_id;
    std::string version;
    std::string category;
    std::string install_path;
    uint64_t    install_time      = 0;
    uint64_t    play_time_seconds = 0;
    std::vector<unsigned char> icon;
};

// Backing store — keeps std::string references alive while the public
// VitaGameEntry array is valid.
std::vector<EntryStorage> g_storage;
std::vector<VitaGameEntry> g_entries;

// Read up to 1 MB (icon0.png is usually ~50 KB).  Return empty on error.
std::vector<unsigned char> read_icon_blob(const fs::path &icon_path) {
    std::vector<unsigned char> bytes;
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

// Read sce_sys/param.sfo into SfoAppInfo.  Returns false if the file is
// missing or malformed.
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

uint64_t fs_last_write_epoch(const fs::path &p) {
    boost::system::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec)
        return 0;
    return static_cast<uint64_t>(t);
}

void scan_one_root(const fs::path &root,
                   const char *category_override,
                   int sys_lang) {
    boost::system::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec))
        return;

    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path &app_dir = it->path();
        if (!fs::is_directory(app_dir, ec))
            continue;

        const fs::path sfo = app_dir / "sce_sys" / "param.sfo";
        sfo::SfoAppInfo info;
        if (!read_param_sfo(sfo, info, sys_lang))
            continue;

        // Trim whitespace / strip newlines from PARAM.SFO strings.
        boost::trim(info.app_title);
        boost::trim(info.app_title_id);
        boost::trim(info.app_version);
        boost::trim(info.app_category);

        if (info.app_title_id.empty()) {
            // Fall back to the directory name (which equals the TITLE_ID
            // for every canonical Vita layout).
            info.app_title_id = app_dir.filename().string();
        }

        EntryStorage e;
        e.title        = info.app_title;
        e.title_id     = info.app_title_id;
        e.version      = info.app_version;
        e.category     = category_override ? category_override : info.app_category;
        e.install_path = fs_utils::path_to_utf8(app_dir);
        e.install_time = fs_last_write_epoch(app_dir);
        e.play_time_seconds = 0; // TODO(M9): stitch to time_tracker::playtime_for(app_title_id)
        e.icon         = read_icon_blob(app_dir / "sce_sys" / "icon0.png");

        g_storage.push_back(std::move(e));
    }
}

void rebuild_entry_view() {
    g_entries.clear();
    g_entries.reserve(g_storage.size());
    for (const auto &e : g_storage) {
        VitaGameEntry v{};
        v.title        = e.title.empty()        ? nullptr : e.title.c_str();
        v.title_id     = e.title_id.empty()     ? nullptr : e.title_id.c_str();
        v.version      = e.version.empty()      ? nullptr : e.version.c_str();
        v.category     = e.category.empty()     ? nullptr : e.category.c_str();
        v.install_path = e.install_path.empty() ? nullptr : e.install_path.c_str();
        v.install_time = e.install_time;
        v.play_time_seconds = e.play_time_seconds;
        v.icon_data = e.icon.empty() ? nullptr : e.icon.data();
        v.icon_size = static_cast<int>(e.icon.size());
        g_entries.push_back(v);
    }
}

} // namespace

RETRO_API const struct VitaGameEntry *retro_vita_list_games(int *out_count) {
    if (out_count) *out_count = 0;
    g_storage.clear();
    g_entries.clear();

    auto &emuenv_ptr = vita3k_libretro_core::emuenv();
    if (!emuenv_ptr) {
        vita3k_libretro_core::set_last_error(
            "list_games: EmuEnvState not initialised");
        return nullptr;
    }

    const fs::path &pref = emuenv_ptr->pref_path;
    const int lang = emuenv_ptr->cfg.sys_lang;

    // Primary: installed games.
    scan_one_root(pref / "ux0" / "app",   /*override=*/"gd", lang);
    // Patches (the PARAM.SFO carries the real category so do not override).
    scan_one_root(pref / "ux0" / "patch", /*override=*/"gp", lang);
    // DLC is stored per-content-id under ux0/addcont/<TITLE_ID>/<CID>, so
    // the directory structure differs; we surface TITLE_ID-level entries
    // only.  Frontend can follow up with per-app DLC queries in M9.

    rebuild_entry_view();

    if (out_count) *out_count = static_cast<int>(g_entries.size());
    vita3k_libretro_core::set_last_error(nullptr);

    LOG_INFO("[Vita3K] retro_vita_list_games: {} entries under {}",
             g_entries.size(), fs_utils::path_to_utf8(pref));

    return g_entries.empty() ? nullptr : g_entries.data();
}
