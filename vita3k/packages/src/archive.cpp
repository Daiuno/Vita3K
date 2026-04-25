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

// GUI-free implementation of VPK/ZIP archive installation.
// See packages/archive.h for rationale.
//
// This file is a near-verbatim port of the helpers that historically
// lived in `vita3k/interface.cpp` (install_archive, install_archive_content,
// install_contents, install_content, get_archive_contents_path,
// get_contents_path, write_to_buffer, miniz_get_error, set_theme_name,
// set_content_path, is_nonpdrm).  All `gui::*` call-sites have been
// replaced by optional hook invocations (archive::InstallHooks).  The
// SDL/ImGui frontend wires those hooks in interface.cpp; the libretro
// core calls us with `hooks = {}` to get silent auto-overwrite behaviour.

#include <packages/archive.h>

#include <config/state.h>
#include <emuenv/state.h>
#include <io/functions.h>
#include <io/vfs.h>
#include <packages/functions.h>
#include <packages/pkg.h>
#include <packages/sfo.h>
#include <util/log.h>
#include <util/string_utils.h>
#include <util/vector_utils.h>

#include <fmt/format.h>
#include <miniz.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

namespace archive {

namespace {

// --------------------------------------------------------------------
// miniz helpers
// --------------------------------------------------------------------

using ZipPtr = std::shared_ptr<mz_zip_archive>;

void delete_zip(mz_zip_archive *zip) {
    mz_zip_reader_end(zip);
    delete zip;
}

size_t write_to_buffer(void *pOpaque, mz_uint64 file_ofs, const void *pBuf, size_t n) {
    auto *buffer = static_cast<vfs::FileBuffer *>(pOpaque);
    assert(file_ofs == buffer->size());
    const uint8_t *first = static_cast<const uint8_t *>(pBuf);
    const uint8_t *last = first + n;
    buffer->insert(buffer->end(), first, last);
    return n;
}

const char *miniz_get_error(const ZipPtr &zip) {
    return mz_zip_get_error_string(mz_zip_get_last_error(zip.get()));
}

// --------------------------------------------------------------------
// theme.xml title extraction
// Mirrors gui::get_theme_title_from_buffer(), duplicated here to
// avoid pulling gui/ into libpackages.
// --------------------------------------------------------------------
std::string parse_theme_title(const vfs::FileBuffer &buffer) {
    pugi::xml_document doc;
    if (doc.load_buffer(buffer.data(), buffer.size()))
        return doc.child("theme")
            .child("InfomationProperty")
            .child("m_title")
            .child("m_default")
            .text()
            .as_string();
    return "Internal error";
}

void set_theme_name(EmuEnvState &emuenv, vfs::FileBuffer &buf) {
    emuenv.app_info.app_title = parse_theme_title(buf);
    emuenv.app_info.app_title_id = string_utils::remove_special_chars(emuenv.app_info.app_title);
    const auto nospace = std::remove_if(emuenv.app_info.app_title_id.begin(),
        emuenv.app_info.app_title_id.end(),
        [](unsigned char c) { return std::isspace(c); });
    emuenv.app_info.app_title_id.erase(nospace, emuenv.app_info.app_title_id.end());
    emuenv.app_info.app_category = "theme";
    emuenv.app_info.app_content_id = emuenv.app_info.app_title_id;
    emuenv.app_info.app_title += " (Theme)";
}

// --------------------------------------------------------------------
// NoNpDrm post-install decrypt
// --------------------------------------------------------------------
bool is_nonpdrm(EmuEnvState &emuenv, const fs::path &output_path) {
    const auto app_license_path{ emuenv.pref_path / "ux0/license"
        / emuenv.app_info.app_title_id
        / fmt::format("{}.rif", emuenv.app_info.app_content_id) };
    const auto is_patch_found_app_license =
        (emuenv.app_info.app_category == "gp") && fs::exists(app_license_path);

    if (fs::exists(output_path / "sce_sys/package/work.bin") || is_patch_found_app_license) {
        fs::path licpath = is_patch_found_app_license
            ? app_license_path
            : output_path / "sce_sys/package/work.bin";
        LOG_INFO("Decrypt layer: {}", output_path);
        if (!decrypt_install_nonpdrm(emuenv, licpath, output_path)) {
            LOG_ERROR("NoNpDrm installation failed, deleting data!");
            fs::remove_all(output_path);
            return false;
        }
        return true;
    }
    return false;
}

// --------------------------------------------------------------------
// Per-category destination rewriter
// --------------------------------------------------------------------
bool set_content_path(EmuEnvState &emuenv, bool is_theme, fs::path &dest_path) {
    const auto app_path = dest_path / "app" / emuenv.app_info.app_title_id;

    if (emuenv.app_info.app_category == "ac") {
        if (is_theme) {
            dest_path /= fs::path("theme") / emuenv.app_info.app_content_id;
            emuenv.app_info.app_title += " (Theme)";
        } else {
            emuenv.app_info.app_content_id = emuenv.app_info.app_content_id.substr(20);
            dest_path /= fs::path("addcont") / emuenv.app_info.app_title_id
                / emuenv.app_info.app_content_id;
            emuenv.app_info.app_title += " (DLC)";
        }
    } else if (emuenv.app_info.app_category.find("gp") != std::string::npos) {
        if (!fs::exists(app_path) || fs::is_empty(app_path)) {
            LOG_ERROR("Install app before patch");
            return false;
        }
        dest_path /= fs::path("patch") / emuenv.app_info.app_title_id;
        emuenv.app_info.app_title += " (Patch)";
    } else {
        dest_path = app_path;
        emuenv.app_info.app_title += " (App)";
    }
    return true;
}

// --------------------------------------------------------------------
// Single-content install from an opened ZIP.
// --------------------------------------------------------------------
bool install_archive_content(EmuEnvState &emuenv, const ZipPtr &zip,
    const std::string &content_path,
    const ProgressCallback &progress_callback,
    const InstallHooks &hooks) {
    const std::string sfo_path = "sce_sys/param.sfo";
    const std::string theme_path = "theme.xml";
    vfs::FileBuffer buffer, theme;

    const bool is_theme = mz_zip_reader_extract_file_to_callback(
        zip.get(), (content_path + theme_path).c_str(), &write_to_buffer, &theme, 0);

    auto output_path{ emuenv.pref_path / "ux0" };
    if (mz_zip_reader_extract_file_to_callback(
            zip.get(), (content_path + sfo_path).c_str(), &write_to_buffer, &buffer, 0)) {
        sfo::get_param_info(emuenv.app_info, buffer, emuenv.cfg.sys_lang);
        if (!set_content_path(emuenv, is_theme, output_path))
            return false;
    } else if (is_theme) {
        set_theme_name(emuenv, theme);
        output_path /= fs::path("theme") / emuenv.app_info.app_content_id;
    } else {
        LOG_CRITICAL("miniz error: {} extracting file: {}", miniz_get_error(zip), sfo_path);
        return false;
    }

    // Destination already exists -> ask the caller what to do.  Unset
    // hook means "always overwrite" (headless / libretro behaviour).
    const bool created = fs::create_directories(output_path);
    if (!created) {
        bool overwrite = true;
        if (hooks.on_reinstall_prompt)
            overwrite = hooks.on_reinstall_prompt(emuenv);
        if (!overwrite) {
            LOG_INFO("{} already installed, keeping existing copy",
                emuenv.app_info.app_title_id);
            return true;
        }
        fs::remove_all(output_path);
        fs::create_directories(output_path);
    }

    float file_progress = 0.f;
    float decrypt_progress = 0.f;
    const auto update_progress = [&]() {
        if (progress_callback)
            progress_callback({ {}, {}, { file_progress * 0.7f + decrypt_progress * 0.3f } });
    };

    const mz_uint num_files = mz_zip_reader_get_num_files(zip.get());
    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(zip.get(), i, &file_stat))
            continue;

        const std::string m_filename = file_stat.m_filename;
        if (m_filename.find(content_path) == std::string::npos)
            continue;

        file_progress = static_cast<float>(i) / num_files * 100.0f;
        update_progress();

        const std::string replace_filename = m_filename.substr(content_path.size());
        const fs::path file_output = (output_path / fs_utils::utf8_to_path(replace_filename)).generic_path();
        if (mz_zip_reader_is_file_a_directory(zip.get(), i)) {
            fs::create_directories(file_output);
        } else {
            fs::create_directories(file_output.parent_path());
            LOG_INFO("Extracting {}", file_output);
            mz_zip_reader_extract_to_file(zip.get(), i,
                fs_utils::path_to_utf8(file_output).c_str(), 0);
        }
    }

    if (fs::exists(output_path / "sce_sys/package/")
        && emuenv.app_info.app_title_id.starts_with("PCS")) {
        update_progress();
        if (is_nonpdrm(emuenv, output_path))
            decrypt_progress = 100.f;
        else
            return false;
    }

    if (!copy_path(output_path, emuenv.pref_path,
            emuenv.app_info.app_title_id, emuenv.app_info.app_category))
        return false;

    update_progress();

    LOG_INFO("{} [{}] installed successfully!",
        emuenv.app_info.app_title, emuenv.app_info.app_title_id);

    if (hooks.on_item_installed)
        hooks.on_item_installed(emuenv, emuenv.app_info.app_category);

    return true;
}

// --------------------------------------------------------------------
// VPK/ZIP top-level scan
// --------------------------------------------------------------------
std::vector<std::string> get_archive_contents_path(const ZipPtr &zip) {
    const mz_uint num_files = mz_zip_reader_get_num_files(zip.get());
    std::vector<std::string> content_path;
    const std::string sfo_path = "sce_sys/param.sfo";
    const std::string theme_path = "theme.xml";

    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(zip.get(), i, &file_stat))
            continue;

        std::string m_filename = std::string(file_stat.m_filename);
        if (m_filename.find("sce_module/steroid.suprx") != std::string::npos) {
            LOG_CRITICAL("A Vitamin dump was detected, aborting installation...");
            content_path.clear();
            break;
        }

        const bool is_content =
            (m_filename.find(sfo_path) != std::string::npos)
            || (m_filename.find(theme_path) != std::string::npos);
        if (is_content) {
            const auto content_type = (m_filename.find(sfo_path) != std::string::npos)
                ? sfo_path
                : theme_path;
            m_filename.erase(m_filename.find(content_type));
            vector_utils::push_if_not_exists(content_path, m_filename);
        }
    }
    return content_path;
}

// --------------------------------------------------------------------
// Filesystem-based single-content install (used by install_contents).
// --------------------------------------------------------------------
std::vector<fs::path> get_contents_path(const fs::path &path) {
    std::vector<fs::path> contents_path;
    for (const auto &p : fs::recursive_directory_iterator(path)) {
        const auto filename = p.path().filename();
        const bool is_content = (filename == "param.sfo") || (filename == "theme.xml");
        if (is_content) {
            const auto parent_path = p.path().parent_path();
            const auto content_path = (filename == "param.sfo")
                ? parent_path.parent_path()
                : parent_path;
            vector_utils::push_if_not_exists(contents_path, content_path);
        }
    }
    return contents_path;
}

bool install_content(EmuEnvState &emuenv, const fs::path &content_path,
    const InstallHooks &hooks) {
    const auto sfo_path{ content_path / "sce_sys/param.sfo" };
    const auto theme_path{ content_path / "theme.xml" };
    vfs::FileBuffer buffer;

    const bool is_theme = fs::exists(theme_path);
    auto dst_path{ emuenv.pref_path / "ux0" };

    if (fs_utils::read_data(sfo_path, buffer)) {
        sfo::get_param_info(emuenv.app_info, buffer, emuenv.cfg.sys_lang);
        if (!set_content_path(emuenv, is_theme, dst_path))
            return false;
        if (fs::exists(dst_path)) {
            // install_contents historically wiped unconditionally here.
            // Preserve that behaviour: hooks only participate in archive
            // (VPK) installs where the user can be asked interactively.
            fs::remove_all(dst_path);
        }
    } else if (fs_utils::read_data(theme_path, buffer)) {
        set_theme_name(emuenv, buffer);
        dst_path /= fs::path("theme") / fs_utils::utf8_to_path(emuenv.app_info.app_title_id);
    } else {
        LOG_ERROR("Param.sfo file is missing in path {}", sfo_path);
        return false;
    }

    if (!copy_directories(content_path, dst_path)) {
        LOG_ERROR("Failed to copy directory to: {}", dst_path);
        return false;
    }

    if (fs::exists(dst_path / "sce_sys/package/") && !is_nonpdrm(emuenv, dst_path))
        return false;

    if (!copy_path(dst_path, emuenv.pref_path,
            emuenv.app_info.app_title_id, emuenv.app_info.app_category))
        return false;

    LOG_INFO("{} [{}] installed successfully!",
        emuenv.app_info.app_title, emuenv.app_info.app_title_id);

    if (hooks.on_item_installed)
        hooks.on_item_installed(emuenv, emuenv.app_info.app_category);

    return true;
}

} // namespace

// ====================================================================
// Public API
// ====================================================================

std::vector<ContentInfo> install_archive(EmuEnvState &emuenv,
    const fs::path &archive_path,
    const ProgressCallback &progress_callback,
    const InstallHooks &hooks) {
    FILE *vpk_fp = FOPEN(archive_path.c_str(), "rb");
    if (!vpk_fp) {
        LOG_CRITICAL("Failed to load archive file in path: {}",
            fs_utils::path_to_utf8(archive_path));
        return {};
    }

    const ZipPtr zip(new mz_zip_archive, delete_zip);
    std::memset(zip.get(), 0, sizeof(*zip));

    if (!mz_zip_reader_init_cfile(zip.get(), vpk_fp, 0, 0)) {
        LOG_CRITICAL("miniz error reading archive: {}", miniz_get_error(zip));
        fclose(vpk_fp);
        return {};
    }

    const auto content_path = get_archive_contents_path(zip);
    if (content_path.empty()) {
        fclose(vpk_fp);
        return {};
    }

    const float count = static_cast<float>(content_path.size());
    float current = 0.f;
    const auto update_progress = [&]() {
        if (progress_callback)
            progress_callback({ count, current, {} });
    };
    update_progress();

    std::vector<ContentInfo> content_installed;
    for (const auto &path : content_path) {
        current++;
        update_progress();
        const bool state = install_archive_content(emuenv, zip, path,
            progress_callback, hooks);
        content_installed.push_back({
            emuenv.app_info.app_title,
            emuenv.app_info.app_title_id,
            emuenv.app_info.app_category,
            emuenv.app_info.app_content_id,
            path,
            state,
        });
    }

    fclose(vpk_fp);
    return content_installed;
}

uint32_t install_contents(EmuEnvState &emuenv, const fs::path &path,
    const InstallHooks &hooks) {
    const auto src_path = get_contents_path(path);
    LOG_WARN_IF(src_path.empty(),
        "No found any content compatible on this path: {}", path);

    uint32_t installed = 0;
    for (const auto &src : src_path) {
        if (install_content(emuenv, src, hooks))
            ++installed;
    }

    if (installed) {
        if (hooks.on_batch_complete)
            hooks.on_batch_complete(emuenv, installed);
        LOG_INFO("Successfully installed {} content!", installed);
    }

    return installed;
}

} // namespace archive
