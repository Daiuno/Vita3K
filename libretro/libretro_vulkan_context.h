// Vita3K libretro — Vulkan HW context (negotiation + lifecycle).
#pragma once

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

bool libretro_vulkan_register_hw_and_negotiation(retro_environment_t env_cb);

bool libretro_vulkan_is_ready(void);

bool libretro_vulkan_has_context(void);

void libretro_vulkan_shutdown(void);

#ifdef __cplusplus
}
#endif
