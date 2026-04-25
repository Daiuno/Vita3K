// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// Shared VMA createImage logging + exception context for LIBRETRO / MoltenVK.

#pragma once

#ifdef LIBRETRO

#include <vkutil/vkutil.h>

namespace vkutil {

struct LibretroVmaCreateImageExtras {
    vk::Format fmt = vk::Format::eUndefined;
    unsigned base_fmt_hex = 0;
    uint32_t raw_gxm_w = 0;
    uint32_t raw_gxm_h = 0;
    uint32_t max_dim = 0;
    bool has_texture_context = false;
};

uint32_t next_vkutil_vma_seq();

void libretro_vma_create_image(
    vma::Allocator &allocator,
    const vk::ImageCreateInfo &image_info,
    const char *tag,
    uint32_t seq,
    vk::Image &out_image,
    vma::Allocation &out_alloc,
    const LibretroVmaCreateImageExtras *extras = nullptr);

} // namespace vkutil

#endif // LIBRETRO
