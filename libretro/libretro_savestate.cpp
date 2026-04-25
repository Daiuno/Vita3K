// Vita3K emulator project - libretro save-state compression
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// zstd-compressed Vita3K snapshots.  Target: <50 MB compressed for most
// games.  M7 will implement this.

#include <cstddef>

extern "C" size_t libretro_savestate_size(void) {
    // TODO(M7): probe emuenv for a tight upper bound.
    return 0;
}

extern "C" int libretro_savestate_serialize(void *data, size_t size) {
    (void)data; (void)size;
    return 0;
}

extern "C" int libretro_savestate_unserialize(const void *data, size_t size) {
    (void)data; (void)size;
    return 0;
}
