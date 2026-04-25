// Vita3K emulator project — iOS JIT classification (thin layer over oaknut::CodeBlock)
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Compiled as plain C++ (.cpp), not Objective-C++ (.mm): oaknut's code_block.hpp
// includes <os/log.h> inside namespace oaknut, which breaks under ObjC++ with
// "Objective-C declarations may only appear in global scope".
//
// Actual dual-mapping / Legacy W^X allocation lives in
// external/dynarmic/externals/oaknut/include/oaknut/code_block.hpp.

#include "libretro_ios_jit.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if TARGET_OS_IPHONE

#include <oaknut/code_block.hpp>

namespace {

bool g_mode_cached = false;
vita3k_ios_jit_mode_t g_mode = VITA3K_JIT_NONE;

} // namespace

extern "C" bool vita3k_ios_jit_available(void) {
    return true;
}

extern "C" bool vita3k_ios_jit_is_txm_device(void) {
    return oaknut::CodeBlockDeviceHasTXM();
}

extern "C" vita3k_ios_jit_mode_t vita3k_ios_jit_mode(void) {
    if (g_mode_cached)
        return g_mode;
    g_mode_cached = true;

    if (!oaknut::CodeBlockIsIOS26OrLater()) {
        g_mode = VITA3K_JIT_LEGACY_WX;
    } else if (oaknut::CodeBlockDeviceHasTXM()) {
        g_mode = VITA3K_JIT_TXM_DUAL_MAPPING;
    } else {
        g_mode = VITA3K_JIT_PPL_DUAL_MAPPING;
    }
    return g_mode;
}

extern "C" const char *vita3k_ios_jit_mode_string(void) {
    switch (vita3k_ios_jit_mode()) {
    case VITA3K_JIT_TXM_DUAL_MAPPING:
        return "TXM dual-mapping (iOS 26+, StikDebug brk #0xf00d)";
    case VITA3K_JIT_PPL_DUAL_MAPPING:
        return "PPL dual-mapping (iOS 26+, mmap RX + vm_remap RW)";
    case VITA3K_JIT_LEGACY_WX:
        return "Legacy W^X (iOS < 26, mmap + mprotect)";
    case VITA3K_JIT_NONE:
    default:
        return "Unknown";
    }
}

extern "C" bool vita3k_ios_jit_stikdebug_detached(void) {
    return oaknut::detail::g_vita3k_txm_handshake_done.load(std::memory_order_acquire);
}

#else /* !TARGET_OS_IPHONE */

extern "C" bool vita3k_ios_jit_available(void) {
    return false;
}
extern "C" bool vita3k_ios_jit_is_txm_device(void) {
    return false;
}
extern "C" vita3k_ios_jit_mode_t vita3k_ios_jit_mode(void) {
    return VITA3K_JIT_NONE;
}
extern "C" const char *vita3k_ios_jit_mode_string(void) {
    return "Non-iOS host";
}
extern "C" bool vita3k_ios_jit_stikdebug_detached(void) {
    return false;
}

#endif
