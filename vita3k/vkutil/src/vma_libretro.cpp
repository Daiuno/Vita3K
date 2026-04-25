// Vita3K emulator project
// Copyright (C) 2026 Vita3K team

#ifdef LIBRETRO

#include <vkutil/vma_libretro.h>

#include <util/log.h>

#include <atomic>
#include <cstdio>
#include <fmt/format.h>
#include <tuple>

#include <spdlog/spdlog.h>

#if defined(__APPLE__)
#include <os/log.h>
#endif

namespace vkutil {

namespace {
std::atomic<uint32_t> g_vkutil_vma_seq{0};

// RetroArch / embedded frontends often merge a different log stream than vita3k.log; Metal may
// abort before spdlog sinks flush. Mirror prelude lines to stderr + Unified Logging on Apple.
void emit_vma_diagnostic_line(const char *line) {
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
#if defined(__APPLE__)
    static os_log_t os_vma = os_log_create("com.vita3k.libretro", "VMA");
    os_log_with_type(os_vma, OS_LOG_TYPE_ERROR, "%{public}s", line);
#endif
}

void flush_spdlog() {
    if (auto lg = spdlog::default_logger())
        lg->flush();
}
} // namespace

uint32_t next_vkutil_vma_seq() {
    return ++g_vkutil_vma_seq;
}

void libretro_vma_create_image(
    vma::Allocator &allocator,
    const vk::ImageCreateInfo &image_info,
    const char *tag,
    const uint32_t seq,
    vk::Image &out_image,
    vma::Allocation &out_alloc,
    const LibretroVmaCreateImageExtras *extras) {
    const bool log_prelude = (seq <= 25u || (seq % 64u) == 0u);
    if (log_prelude) {
        std::string line;
        if (extras && extras->has_texture_context) {
            const vk::Format fmt_for_log = (extras->fmt != vk::Format::eUndefined) ? extras->fmt : image_info.format;
            line = fmt::format(
                "[VITA3K-LR] VMA createImage prelude #{} [{}] extent={}x{}x{} mipLevels={} arrayLayers={} samples={} fmt={} tiling=optimal base_fmt=0x{:x} raw_gxm_wh=({},{}) max_dim={}",
                seq, tag,
                image_info.extent.width, image_info.extent.height, image_info.extent.depth,
                image_info.mipLevels, image_info.arrayLayers,
                static_cast<uint32_t>(image_info.samples),
                vk::to_string(fmt_for_log),
                extras->base_fmt_hex,
                extras->raw_gxm_w, extras->raw_gxm_h,
                extras->max_dim);
        } else {
            line = fmt::format(
                "[VITA3K-LR] VMA createImage prelude #{} [{}] extent={}x{}x{} mipLevels={} arrayLayers={} samples={} fmt={} flags={} usage={}",
                seq, tag,
                image_info.extent.width, image_info.extent.height, image_info.extent.depth,
                image_info.mipLevels, image_info.arrayLayers,
                static_cast<uint32_t>(image_info.samples),
                vk::to_string(image_info.format),
                vk::to_string(image_info.flags),
                vk::to_string(image_info.usage));
        }
        // ERROR level so a user-configured minimum of "errors only" still records preludes.
        LOG_ERROR("{}", line);
        emit_vma_diagnostic_line(line.c_str());
        flush_spdlog();
    }
    try {
        std::tie(out_image, out_alloc) = allocator.createImage(image_info, vma_auto_alloc);
    } catch (const std::exception &e) {
        LOG_ERROR("[VITA3K-LR] VMA createImage FAILED [{}] seq={}: {}", tag, seq, e.what());
        if (extras && extras->has_texture_context) {
            const vk::Format fmt_for_log = (extras->fmt != vk::Format::eUndefined) ? extras->fmt : image_info.format;
            LOG_ERROR("[VITA3K-LR]  extent={}x{}x{} mipLevels={} arrayLayers={} fmt={} base_fmt=0x{:x} raw_gxm_wh=({},{}) max_dim={}",
                image_info.extent.width, image_info.extent.height, image_info.extent.depth,
                image_info.mipLevels, image_info.arrayLayers,
                vk::to_string(fmt_for_log),
                extras->base_fmt_hex,
                extras->raw_gxm_w, extras->raw_gxm_h,
                extras->max_dim);
        } else {
            LOG_ERROR("[VITA3K-LR]  extent={}x{}x{} mipLevels={} arrayLayers={} fmt={} flags={} usage={}",
                image_info.extent.width, image_info.extent.height, image_info.extent.depth,
                image_info.mipLevels, image_info.arrayLayers,
                vk::to_string(image_info.format),
                vk::to_string(image_info.flags),
                vk::to_string(image_info.usage));
        }
        flush_spdlog();
        throw;
    }
}

} // namespace vkutil

#endif // LIBRETRO
