// Vita3K — Libretro Vulkan HW context negotiation (RetroArch + MoltenVK).
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <utility>

#include <vulkan/vulkan_core.h>

#ifdef LIBRETRO
#include <libretro_vulkan.h>
#endif

namespace renderer::vulkan {

struct VKState;

/// Payload filled by a successful retro_vulkan negotiation callback before VKState::create().
struct LibretroNegotiatedVulkan {
    /// Required for Vulkan-HPP: RetroArch provides this in negotiation; SDL's
    /// SDL_Vulkan_GetVkGetInstanceProcAddr() is often NULL without an SDL Vulkan window.
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    VkInstance instance{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    VkSurfaceKHR surface{};
    uint32_t general_family_index{};
    uint32_t transfer_family_index{};
    bool valid = false;
};

namespace libretro {

LibretroNegotiatedVulkan negotiated_handles_get();

/// Host output size for swapchain when there is no SDL window (HW Vulkan). Mirrors
/// retro_get_system_av_info geometry / Vita defaults (960x544) unless overridden.
void output_extent_set(uint32_t width, uint32_t height);
std::pair<uint32_t, uint32_t> output_extent_get();

#ifdef LIBRETRO
bool negotiation_create_device_v1(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions, unsigned num_required_device_extensions,
    const char **required_device_layers, unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features);

bool negotiation_create_device_v2(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr, retro_vulkan_create_device_wrapper_t create_device_wrapper, void *opaque);

/// Called by RetroArch when tearing down the negotiated device. Must NOT clear handles
/// needed later by context_reset / bootstrap — only release truly auxiliary resources.
void negotiation_destroy_device();

/// Clear negotiated instance/device/surface handles (call from context_destroy / retro_deinit).
void negotiation_handles_release();

/// RetroArch fills \c retro_hw_render_interface_vulkan after HW context is ready; the core must use
/// \c set_image (not a second swapchain on the negotiated surface).
void hw_render_interface_set(const retro_hw_render_interface_vulkan *iface);
const retro_hw_render_interface_vulkan *hw_render_interface_get();
#endif

} // namespace libretro

} // namespace renderer::vulkan
