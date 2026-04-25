// Vita3K emulator project - libretro camera bridge
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Forwards frames from the libretro frontend's camera interface into
// Vita3K's SceCamera backend.  The frontend takes care of requesting
// OS-level camera permission.  M7 will implement this.

#include <libretro.h>

extern "C" void libretro_camera_register(retro_environment_t env_cb) {
    (void)env_cb;
    // TODO(M7): fill struct retro_camera_callback and call
    //           RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE.
}

extern "C" void libretro_camera_start(void) {
    // TODO(M7): callback.start()
}

extern "C" void libretro_camera_stop(void) {
    // TODO(M7): callback.stop()
}
