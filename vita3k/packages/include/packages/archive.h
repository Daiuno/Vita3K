// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// GUI-free archive (VPK/ZIP) installation API.
//
// Until M8 `install_archive` / `install_contents` lived in
// `vita3k/interface.cpp` and were deeply coupled to `gui::*`,
// `SDL_Window`, `imgui_impl_sdl`, and `handle_events` (re-install
// confirmation dialog, notice refresh, apps-cache save).  That made
// them unusable from the libretro core and any other future non-GUI
// caller (CI smoke tests, headless content-scan tools, etc.).
//
// This header exposes the same behaviour as a pure engine helper
// living in libpackages.  GUI integration is provided by an optional
// `InstallHooks` struct of std::functions; the stock interface.cpp
// wrappers populate those hooks so existing SDL-frontend behaviour
// remains bit-identical.

#pragma once

#include <util/fs.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct EmuEnvState;

namespace archive {

struct ArchiveContents {
    std::optional<float> count;
    std::optional<float> current;
    std::optional<float> progress;
};

struct ContentInfo {
    std::string title;
    std::string title_id;
    std::string category;
    std::string content_id;
    std::string path;
    bool state = false;
};

using ProgressCallback = std::function<void(ArchiveContents)>;

// Optional behaviour customisation.  Unset members are treated as
// "headless default": always overwrite, no notifications, no cache
// flushes.  The SDL/ImGui frontend wires these up in interface.cpp to
// reproduce the original user-facing flow.
struct InstallHooks {
    // Invoked when the target install directory already exists.
    // Return true to wipe the destination and reinstall, false to skip
    // this single item (counts as "not installed").
    // Default (unset) → overwrite.
    std::function<bool(EmuEnvState &)> on_reinstall_prompt;

    // Invoked after a single piece of content has been copied into
    // place.  Receives the raw Vita category string from
    // PARAM.SFO ("gd"/"gp"/"ac"/"theme"/...) so the caller can decide
    // what, if any, UI refresh to run.
    std::function<void(EmuEnvState &, const std::string &category)> on_item_installed;

    // Invoked exactly once at the end of `install_contents()` when at
    // least one content was installed.  Typical GUI use: flush the
    // apps cache to disk.
    std::function<void(EmuEnvState &, uint32_t installed_count)> on_batch_complete;
};

// Unpack a .vpk / .zip archive (a bundle that may contain one or more
// Vita content trees).  Returns per-content result metadata.
std::vector<ContentInfo> install_archive(EmuEnvState &emuenv,
                                         const fs::path &archive_path,
                                         const ProgressCallback &progress_callback = nullptr,
                                         const InstallHooks &hooks = {});

// Scan a directory for Vita content trees (identified by a
// `sce_sys/param.sfo` or a `theme.xml`) and install each of them into
// the `ux0/` mount.  Returns the number of content trees installed.
uint32_t install_contents(EmuEnvState &emuenv,
                          const fs::path &path,
                          const InstallHooks &hooks = {});

} // namespace archive
