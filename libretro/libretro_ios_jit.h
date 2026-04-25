// Vita3K emulator project — iOS JIT status API for libretro / frontends
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// JIT memory allocation is implemented in oaknut::CodeBlock (see
// external/dynarmic/externals/oaknut/include/oaknut/code_block.hpp).
// This header only describes runtime classification for diagnostics.

#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * True on iPhone/iPad builds: oaknut provides Legacy (< iOS 26), PPL (26+ non-TXM),
 * or TXM (26+ TXM) JIT. False on non-iOS libretro hosts.
 */
bool vita3k_ios_jit_available(void);

/**
 * True when the firmware indicates Trusted Execution Monitor (TXM) — StikDebug
 * TXM protocol applies on iOS 26+.
 */
bool vita3k_ios_jit_is_txm_device(void);

/**
 * True after the first successful TXM `brk #0xf00d` handshake in this process
 * (see oaknut::CodeBlock). Always false on non-TXM paths.
 */
bool vita3k_ios_jit_stikdebug_detached(void);

/**
 * Stable enum for retro_vita_jit_status() and host UI.
 */
typedef enum {
    VITA3K_JIT_NONE = 0, ///< Unused on real iOS; reserved for non-iOS stubs
    VITA3K_JIT_LEGACY_WX = 1, ///< iOS < 26: mmap + mprotect W^X
    VITA3K_JIT_TXM_DUAL_MAPPING = 2, ///< iOS 26+ TXM: mach_make_memory_entry + vm_remap + brk
    VITA3K_JIT_PPL_DUAL_MAPPING = 3, ///< iOS 26+ non-TXM: mmap RX + vm_remap RW (PPL)
} vita3k_ios_jit_mode_t;

vita3k_ios_jit_mode_t vita3k_ios_jit_mode(void);

/** English, ASCII; owned by the core. */
const char *vita3k_ios_jit_mode_string(void);

#ifdef __cplusplus
}
#endif
