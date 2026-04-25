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

#pragma once

#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#elif defined(__APPLE__)
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_SPACESHIP_OPERATOR
#include <vulkan/vulkan.hpp>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#endif

#include <vk_mem_alloc.hpp>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <util/fs.h>

#ifdef LIBRETRO
#include <algorithm>
#include <cstdint>
#include <vulkan/vulkan_format_traits.hpp>
#endif

namespace vkutil {

#ifdef LIBRETRO
// Libretro / MoltenVK: 0 / UINT32_MAX (often signed -1 or garbage) must not reach vkCreateImage.
inline uint32_t clamp_image_extent_component(uint32_t dim, uint32_t max_image_dimension_2d) {
    if (dim == 0 || dim == UINT32_MAX)
        return 1u;
    // Guest sometimes leaves 0xFFFFFFFE etc.; treat as invalid.
    if (dim >= UINT32_MAX - 1u)
        return 1u;

    return std::min(dim, max_image_dimension_2d);
}

// MoltenVK: compressed optimal images must use block-aligned extents (shared by texture cache + vkutil::Image).
inline void clamp_image_extent_to_block_grid(uint32_t &width, uint32_t &height, vk::Format fmt, uint32_t max_dim) {
    const auto be = vk::blockExtent(fmt);
    const uint32_t bx = std::max(1u, static_cast<uint32_t>(be[0]));
    const uint32_t by = std::max(1u, static_cast<uint32_t>(be[1]));
    if (bx <= 1 && by <= 1)
        return;

    auto round_up = [](uint32_t v, uint32_t a) { return (v + a - 1u) / a * a; };
    auto round_down = [](uint32_t v, uint32_t a) { return (v / a) * a; };

    uint32_t w = round_up(width, bx);
    uint32_t h = round_up(height, by);
    if (w > max_dim)
        w = round_down(max_dim, bx);
    if (h > max_dim)
        h = round_down(max_dim, by);
    if (w == 0 || w < bx)
        w = std::min(bx, max_dim);
    if (h == 0 || h < by)
        h = std::min(by, max_dim);
    width = std::max(1u, w);
    height = std::max(1u, h);
}
#endif // LIBRETRO

static constexpr vk::ImageSubresourceRange color_subresource_range = {
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1
};

static constexpr vk::ImageSubresourceRange ds_subresource_range = {
    .aspectMask = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1
};

static constexpr vk::ImageSubresourceRange d_subresource_range = {
    .aspectMask = vk::ImageAspectFlagBits::eDepth,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1
};

static constexpr vk::ImageSubresourceLayers color_subresource_layer = {
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .mipLevel = 0,
    .baseArrayLayer = 0,
    .layerCount = 1
};

static constexpr vk::ComponentMapping default_comp_mapping = {
    vk::ComponentSwizzle::eIdentity,
    vk::ComponentSwizzle::eIdentity,
    vk::ComponentSwizzle::eIdentity,
    vk::ComponentSwizzle::eIdentity
};

static constexpr vk::ComponentMapping rgba_mapping = {
    vk::ComponentSwizzle::eR,
    vk::ComponentSwizzle::eG,
    vk::ComponentSwizzle::eB,
    vk::ComponentSwizzle::eA
};

static constexpr vk::ColorComponentFlags default_color_mask = vk::ColorComponentFlagBits::eR
    | vk::ColorComponentFlagBits::eG
    | vk::ColorComponentFlagBits::eB
    | vk::ColorComponentFlagBits::eA;

static constexpr vma::AllocationCreateInfo vma_auto_alloc = {
    .usage = vma::MemoryUsage::eAuto
};

static constexpr vma::AllocationCreateInfo vma_mapped_alloc = {
    .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped,
    .usage = vma::MemoryUsage::eAuto,
    .preferredFlags = vk::MemoryPropertyFlagBits::eHostCoherent
};

static constexpr vma::AllocationCreateInfo vma_host_visible = {
    .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite,
    .usage = vma::MemoryUsage::eAuto,
    .requiredFlags = vk::MemoryPropertyFlagBits::eHostVisible,
};

vk::CommandBuffer create_single_time_command(vk::Device device, vk::CommandPool cmd_pool);
void end_single_time_command(vk::Device device, vk::Queue queue, vk::CommandPool cmd_pool, vk::CommandBuffer cmd_buffer);

vk::ShaderModule load_shader(vk::Device device, const fs::path &shader_path);
vk::ShaderModule load_shader(vk::Device device, const void *data, const uint32_t size);

void copy_buffer(vk::Device device, vk::CommandPool cmd_pool, vk::Queue queue, vk::Buffer src, vk::Buffer dst, vk::DeviceSize size);

enum struct ImageLayout {
    Undefined,
    TransferSrc,
    TransferDst,
    ColorAttachment,
    DepthStencilAttachment,
    ColorAttachmentReadWrite,
    SampledImage,
    StorageImage,
    DepthStencilReadOnly
};
void transition_image_layout(vk::CommandBuffer cmd_buffer, vk::Image image, ImageLayout src_layout, ImageLayout dst_layout, const vk::ImageSubresourceRange &range = color_subresource_range);
// transition image layout assuming you don't care about the former image content
void transition_image_layout_discard(vk::CommandBuffer cmd_buffer, vk::Image image, ImageLayout src_layout, ImageLayout dst_layout, const vk::ImageSubresourceRange &range = color_subresource_range);
// Return the vulkan layout associated with ImageLayout
vk::ImageLayout get_underlying_layout(ImageLayout layout);

// given the swizzle of the color surface (which was written as rgba because vulkan doesn't support swizzle on framebuffers)
// and the swizzle of the texture that's reading from the color surface
// return the swizzle the texture should to make it like the color actually had a swizzle applied to it
vk::ComponentMapping color_to_texture_swizzle(const vk::ComponentMapping &swizzle_color, const vk::ComponentMapping &swizzle_texture);

} // namespace vkutil
