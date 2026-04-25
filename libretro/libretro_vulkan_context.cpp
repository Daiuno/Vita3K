// Vita3K libretro — Vulkan HW context bridge (RetroArch + MoltenVK).
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "libretro_vulkan_context.h"

#include "libretro_boot.h"
#include "libretro_game_boot.h"
#include "libretro_paths.h"

#include <emuenv/state.h>
#include <io/state.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <renderer/types.h>
#include <renderer/vulkan/libretro_vulkan_api.h>
#include <util/log.h>

#include <libretro_vulkan.h>

#include <memory>

#include <algorithm>
#include <atomic>
#include <cstring>

extern std::unique_ptr<EmuEnvState> g_emuenv;

/* RetroArch invokes these via C function pointers; use C linkage to match retro_vulkan_create_device*_t. */
extern "C" {
static bool vita3k_lr_create_device_v1(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions, unsigned num_required_device_extensions,
    const char **required_device_layers, unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features) {
    return renderer::vulkan::libretro::negotiation_create_device_v1(context, instance, gpu, surface, get_instance_proc_addr,
        required_device_extensions, num_required_device_extensions, required_device_layers, num_required_device_layers, required_features);
}

static bool vita3k_lr_create_device_v2(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr, retro_vulkan_create_device_wrapper_t create_device_wrapper, void *opaque) {
    return renderer::vulkan::libretro::negotiation_create_device_v2(context, instance, gpu, surface, get_instance_proc_addr,
        create_device_wrapper, opaque);
}
}

namespace {

retro_hw_render_callback g_hw_render{};
std::atomic<bool> g_has_context{ false };
std::atomic<bool> g_is_ready{ false };

struct retro_hw_render_context_negotiation_interface_vulkan g_negotiate{};

static VkApplicationInfo g_app_info{
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pNext = nullptr,
    .pApplicationName = "Vita3K",
    .applicationVersion = 0,
    .pEngineName = "Vita3K-libretro",
    .engineVersion = 0,
    .apiVersion = VK_MAKE_VERSION(1, 1, 0),
};

static const VkApplicationInfo *get_application_info_cb(void) {
    return &g_app_info;
}

static void destroy_device_cb(void) {
    renderer::vulkan::libretro::negotiation_destroy_device();
}

void try_init_renderer() {
    if (!g_emuenv) {
        LOG_ERROR("[Vita3K] Vulkan context_reset fired but EmuEnvState is null");
        return;
    }

    EmuEnvState &emuenv = *g_emuenv;
    const Root &root = vita3k_libretro::paths_root();

    // Default host output size for swapchain (matches retro_get_system_av_info until scaling is wired).
    renderer::vulkan::libretro::output_extent_set(960, 544);

    if (!emuenv.renderer) {
        if (!renderer::init(/*window=*/nullptr, emuenv.renderer, renderer::Backend::Vulkan, emuenv.cfg, root)) {
            LOG_ERROR("[Vita3K] renderer::init(Vulkan) failed under libretro");
            return;
        }
        emuenv.renderer->late_init(emuenv.cfg, /*game_id=*/"", emuenv.mem);
        LOG_INFO("[Vita3K] Vulkan renderer initialised via libretro HW context");
    } else {
        LOG_INFO("[Vita3K] libretro context_reset: re-using existing renderer");
    }

    if (emuenv.renderer && !emuenv.io.title_id.empty() && !emuenv.self_name.empty()) {
        emuenv.renderer->set_app(emuenv.io.title_id.c_str(), emuenv.self_name.c_str());
        LOG_INFO("[Vita3K] renderer->set_app post-context_reset");
    }

    // Deferring run_app from retro_load_game: start the guest only after the
    // Vulkan device and renderer exist so retro_run can always drain GXM.
    if (!vita3k_libretro::start_guest_if_pending(emuenv)) {
        LOG_ERROR("[Vita3K] Deferred guest start failed; HW context stays not ready");
        return;
    }

    g_is_ready.store(true, std::memory_order_release);
}

void release_renderer() {
    g_is_ready.store(false, std::memory_order_release);
    if (g_emuenv) {
        g_emuenv->renderer.reset();
        LOG_INFO("[Vita3K] renderer torn down (context_destroy)");
    }
    renderer::vulkan::libretro::negotiation_handles_release();
}

void context_reset_trampoline(void) {
    g_has_context.store(true, std::memory_order_release);
    try_init_renderer();
}

void context_destroy_trampoline(void) {
    release_renderer();
    g_has_context.store(false, std::memory_order_release);
}

} // namespace

extern "C" bool libretro_vulkan_register_hw_and_negotiation(retro_environment_t env_cb) {
    if (!env_cb)
        return false;

    unsigned support_version = 0;
    struct retro_hw_render_context_negotiation_interface support{};
    support.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
    support.interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
    if (env_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &support)) {
        support_version = support.interface_version;
    }

    std::memset(&g_negotiate, 0, sizeof(g_negotiate));
    g_negotiate.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
    g_negotiate.interface_version = std::min<unsigned>(support_version,
        static_cast<unsigned>(RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION));
    if (g_negotiate.interface_version == 0)
        g_negotiate.interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;

    g_negotiate.get_application_info = get_application_info_cb;
    g_negotiate.create_device = vita3k_lr_create_device_v1;
    g_negotiate.destroy_device = destroy_device_cb;
    if (g_negotiate.interface_version >= 2)
        g_negotiate.create_device2 = vita3k_lr_create_device_v2;

    if (!env_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &g_negotiate)) {
        std::memset(&g_negotiate, 0, sizeof(g_negotiate));
        return false;
    }

    std::memset(&g_hw_render, 0, sizeof(g_hw_render));
    g_hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
    g_hw_render.version_major = VK_VERSION_MAJOR(VK_MAKE_VERSION(1, 1, 0));
    g_hw_render.version_minor = VK_VERSION_MINOR(VK_MAKE_VERSION(1, 1, 0));
    g_hw_render.context_reset = context_reset_trampoline;
    g_hw_render.context_destroy = context_destroy_trampoline;
    g_hw_render.depth = true;
    g_hw_render.stencil = true;
    g_hw_render.bottom_left_origin = false;
    g_hw_render.cache_context = false;
    g_hw_render.debug_context = false;

    if (!env_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hw_render)) {
        std::memset(&g_hw_render, 0, sizeof(g_hw_render));
        return false;
    }

    return true;
}

extern "C" bool libretro_vulkan_is_ready(void) {
    return g_is_ready.load(std::memory_order_acquire);
}

extern "C" bool libretro_vulkan_has_context(void) {
    return g_has_context.load(std::memory_order_acquire);
}

extern "C" void libretro_vulkan_shutdown(void) {
    g_has_context.store(false, std::memory_order_release);
    g_is_ready.store(false, std::memory_order_release);
    renderer::vulkan::libretro::negotiation_handles_release();
    std::memset(&g_hw_render, 0, sizeof(g_hw_render));
    std::memset(&g_negotiate, 0, sizeof(g_negotiate));
}
