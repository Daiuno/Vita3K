// Vita3K — Libretro Vulkan device negotiation.
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <renderer/vulkan/libretro_vulkan_api.h>

#ifdef LIBRETRO

#include <util/log.h>

#include <cstring>
#include <vector>

namespace renderer::vulkan::libretro {

static LibretroNegotiatedVulkan g_stored;
static const retro_hw_render_interface_vulkan *g_hw_render_iface{};

LibretroNegotiatedVulkan negotiated_handles_get() {
    return g_stored;
}

static uint32_t g_output_extent_w = 960;
static uint32_t g_output_extent_h = 544;

void output_extent_set(uint32_t width, uint32_t height) {
    if (width != 0 && height != 0) {
        g_output_extent_w = width;
        g_output_extent_h = height;
    }
}

std::pair<uint32_t, uint32_t> output_extent_get() {
    return { g_output_extent_w, g_output_extent_h };
}

namespace {

static void add_unique(std::vector<const char *> &list, const char *value) {
    if (!value)
        return;
    for (const char *n : list) {
        if (!std::strcmp(n, value))
            return;
    }
    list.push_back(value);
}

static bool pick_queue_family(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice phys, VkSurfaceKHR surf, uint32_t &out_family) {
    auto vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto vkGetPhysicalDeviceSurfaceSupportKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        gipa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
    if (!vkGetPhysicalDeviceQueueFamilyProperties || !vkGetPhysicalDeviceSurfaceSupportKHR)
        return false;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, props.data());

    for (uint32_t i = 0; i < count; i++) {
        const auto &q = props[i];
        if (!(q.queueFlags & VK_QUEUE_GRAPHICS_BIT))
            continue;
        if (!(q.queueFlags & VK_QUEUE_TRANSFER_BIT))
            continue;
        VkBool32 surf_ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surf, &surf_ok);
        if (surf_ok) {
            out_family = i;
            return true;
        }
    }
    return false;
}

static void merge_vita_baseline_features(const VkPhysicalDeviceFeatures &gpu, VkPhysicalDeviceFeatures *out) {
    out->depthClamp = gpu.depthClamp;
    out->fillModeNonSolid = gpu.fillModeNonSolid;
    out->wideLines = gpu.wideLines;
    out->samplerAnisotropy = gpu.samplerAnisotropy;
    out->occlusionQueryPrecise = gpu.occlusionQueryPrecise;
    out->fragmentStoresAndAtomics = gpu.fragmentStoresAndAtomics;
    out->shaderStorageImageExtendedFormats = gpu.shaderStorageImageExtendedFormats;
    out->shaderInt16 = gpu.shaderInt16;
}

static bool negotiation_impl(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr, const VkPhysicalDeviceFeatures *frontend_required,
    const char **req_ext, unsigned num_req_ext, const char **req_layers, unsigned num_req_layers,
    retro_vulkan_create_device_wrapper_t create_wrapper, void *wrapper_opaque) {
    if (!context || !get_instance_proc_addr || !surface)
        return false;

    g_stored = {};

    auto vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        get_instance_proc_addr(instance, "vkEnumerateDeviceExtensionProperties"));
    auto vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(get_instance_proc_addr(instance, "vkCreateDevice"));
    auto vkGetPhysicalDeviceFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
        get_instance_proc_addr(instance, "vkGetPhysicalDeviceFeatures"));
    if (!vkEnumerateDeviceExtensionProperties || !vkCreateDevice || !vkGetPhysicalDeviceFeatures) {
        LOG_ERROR("libretro vulkan: missing core device functions");
        return false;
    }

    uint32_t general_family = 0;
    if (!pick_queue_family(get_instance_proc_addr, instance, gpu, surface, general_family)) {
        LOG_ERROR("libretro vulkan: no suitable queue family");
        return false;
    }

    float priority = 1.f;
    VkDeviceQueueCreateInfo qinfo{};
    qinfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qinfo.queueFamilyIndex = general_family;
    qinfo.queueCount = 1;
    qinfo.pQueuePriorities = &priority;

    static const char *vita3k_base_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
#if defined(__APPLE__)
        "VK_KHR_portability_subset",
#endif
    };

    std::vector<const char *> ext_names;
    for (unsigned i = 0; i < num_req_ext; ++i)
        add_unique(ext_names, req_ext[i]);
    for (const char *e : vita3k_base_extensions)
        add_unique(ext_names, e);

    std::vector<const char *> layer_names;
    for (unsigned i = 0; i < num_req_layers; ++i)
        add_unique(layer_names, req_layers[i]);

    uint32_t avail_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &avail_count, nullptr);
    std::vector<VkExtensionProperties> avail(avail_count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &avail_count, avail.data());
    std::vector<std::string> avail_set;
    for (const auto &p : avail)
        avail_set.emplace_back(p.extensionName);

    std::vector<const char *> ext_filtered;
    for (const char *req : ext_names) {
        bool ok = false;
        for (const auto &name : avail_set) {
            if (name == req) {
                ok = true;
                break;
            }
        }
        if (ok)
            ext_filtered.push_back(req);
        else
            LOG_WARN("libretro vulkan: device lacks extension '{}'", req);
    }

    bool has_swapchain = false;
    for (const char *e : ext_filtered) {
        if (!std::strcmp(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            has_swapchain = true;
    }
    if (!has_swapchain) {
        LOG_ERROR("libretro vulkan: VK_KHR_swapchain unavailable");
        return false;
    }

    VkPhysicalDeviceFeatures gpu_feats{};
    vkGetPhysicalDeviceFeatures(gpu, &gpu_feats);

    VkPhysicalDeviceFeatures enabled{};
    merge_vita_baseline_features(gpu_feats, &enabled);
    if (frontend_required) {
        for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++) {
            if (reinterpret_cast<const VkBool32 *>(frontend_required)[i])
                reinterpret_cast<VkBool32 *>(&enabled)[i] = VK_TRUE;
        }
    }

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qinfo;
    dci.enabledExtensionCount = static_cast<uint32_t>(ext_filtered.size());
    dci.ppEnabledExtensionNames = ext_filtered.data();
    dci.enabledLayerCount = static_cast<uint32_t>(layer_names.size());
    dci.ppEnabledLayerNames = layer_names.empty() ? nullptr : layer_names.data();
    dci.pEnabledFeatures = &enabled;

    VkDevice device = VK_NULL_HANDLE;
    VkResult cr;
    if (create_wrapper) {
        device = create_wrapper(gpu, wrapper_opaque, &dci);
        cr = device ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    } else {
        cr = vkCreateDevice(gpu, &dci, nullptr, &device);
    }

    if (cr != VK_SUCCESS || device == VK_NULL_HANDLE) {
        LOG_ERROR("libretro vulkan: vkCreateDevice failed ({})", static_cast<int>(cr));
        return false;
    }

    auto vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(get_instance_proc_addr(instance, "vkGetDeviceQueue"));
    if (!vkGetDeviceQueue) {
        LOG_ERROR("libretro vulkan: vkGetDeviceQueue missing");
        return false;
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, general_family, 0, &queue);

    context->gpu = gpu;
    context->device = device;
    context->queue = queue;
    context->queue_family_index = general_family;
    context->presentation_queue = queue;
    context->presentation_queue_family_index = general_family;

    g_stored.get_instance_proc_addr = get_instance_proc_addr;
    g_stored.instance = instance;
    g_stored.physical_device = gpu;
    g_stored.device = device;
    g_stored.surface = surface;
    g_stored.general_family_index = general_family;
    g_stored.transfer_family_index = general_family;
    g_stored.valid = true;

    LOG_INFO("libretro vulkan: negotiated device (queue family {})", general_family);
    return true;
}

} // namespace

bool negotiation_create_device_v1(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions, unsigned num_required_device_extensions,
    const char **required_device_layers, unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features) {
    VkPhysicalDeviceFeatures req{};
    if (required_features)
        req = *required_features;
    return negotiation_impl(context, instance, gpu, surface, get_instance_proc_addr, &req, required_device_extensions,
        num_required_device_extensions, required_device_layers, num_required_device_layers, nullptr, nullptr);
}

bool negotiation_create_device_v2(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr, retro_vulkan_create_device_wrapper_t create_device_wrapper, void *opaque) {
    if (!create_device_wrapper) {
        LOG_ERROR("libretro vulkan: create_device_wrapper is null");
        return false;
    }
    return negotiation_impl(context, instance, gpu, surface, get_instance_proc_addr, nullptr, nullptr, 0, nullptr, 0, create_device_wrapper, opaque);
}

void negotiation_destroy_device() {
    // RetroArch may invoke this before context_reset. Clearing g_stored here would leave
    // bootstrap with valid=false or stale pointers after create_device — do not clear.
    LOG_INFO("[VITA3K-LR] negotiation_destroy_device (retaining handles for context_reset)");
}

void negotiation_handles_release() {
    g_stored = {};
    g_hw_render_iface = nullptr;
    LOG_INFO("[VITA3K-LR] negotiation_handles_release");
}

void hw_render_interface_set(const retro_hw_render_interface_vulkan *iface) {
    g_hw_render_iface = iface;
}

const retro_hw_render_interface_vulkan *hw_render_interface_get() {
    return g_hw_render_iface;
}

} // namespace renderer::vulkan::libretro

#else // !LIBRETRO

namespace renderer::vulkan::libretro {

LibretroNegotiatedVulkan negotiated_handles_get() {
    return {};
}

void output_extent_set(uint32_t, uint32_t) {
}

std::pair<uint32_t, uint32_t> output_extent_get() {
    return { 960, 544 };
}

} // namespace renderer::vulkan::libretro

#endif
