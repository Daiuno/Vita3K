// Vita3K libretro core: stub BGM player.
//
// The full bgm_player.cpp uses cubeb directly for low-latency playback of the
// LiveArea AT9 background music.  cubeb's CoreAudio backend doesn't compile on
// iOS, so when building the libretro core we replace the player with a no-op
// implementation.  The libretro frontend (RetroArch) handles all audio output
// itself anyway, and the in-emulator LiveArea overlay is not surfaced when the
// core is loaded by an external frontend.

#include "private.h"

#include <gui/functions.h>

namespace gui {

void init_bgm_player(const float /*vol*/) {}

void destroy_bgm_player() {}

bool init_bgm_streaming(uint8_t * /*at9_data*/, uint32_t /*size*/) {
    return false;
}

bool init_bgm(GuiState & /*gui*/, EmuEnvState & /*emuenv*/) {
    return false;
}

void set_bgm_volume(const float /*vol*/) {}

void stop_bgm() {}

void switch_bgm_state(const bool /*pause*/) {}

} // namespace gui
