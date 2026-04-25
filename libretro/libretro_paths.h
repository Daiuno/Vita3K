// Vita3K emulator project - libretro <-> Vita3K path mapping (header)
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Builds a Vita3K-shaped directory tree underneath the libretro
// frontend-provided save directory and exposes the resulting Root.

#pragma once

#ifdef __cplusplus

#include <util/fs.h>

namespace vita3k_libretro {

// Initialize the Vita3K file hierarchy under `save_dir` and return the
// configured Root.  Safe to call multiple times (idempotent).
const Root &paths_init(const char *save_dir);

// Returns the Root previously built by paths_init(); fields are empty
// strings if paths_init() has not been called yet.
const Root &paths_root();

// Convenience helpers returning host paths.
fs::path paths_pref();
fs::path paths_base();

} // namespace vita3k_libretro

extern "C" {
#endif

// C shims (used from libretro.cpp so pure-C translation units can also
// access the pref path if ever needed).
void        libretro_paths_init(const char *save_dir);
const char *libretro_paths_pref(void);

#ifdef __cplusplus
} // extern "C"
#endif
