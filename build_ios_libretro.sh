#!/bin/bash
# Vita3K iOS Libretro Framework Build Script
#
# Derived from ppsspp/build_ios.sh.  Produces
#   build/ios-arm64-libretro/vita3k.libretro.framework
# containing an ad-hoc signed arm64 dylib with install_name rewritten to
# the framework-relative path so RetroArch-iOS can load it directly.
#
# Requirements:
#   - macOS with Xcode command-line tools
#   - CMake 3.22+
#   - An iPhoneOS SDK visible to `xcrun --sdk iphoneos --show-sdk-path`

set -euo pipefail

BUILD_DIR="build/ios-arm64-libretro"
FRAMEWORK_NAME="vita3k.libretro.framework"
TARGET_BINARY="vita3k.libretro"
# libretro-super core-info convention: <corename>_libretro.info, placed
# alongside the framework so a packaging step can copy (framework + info)
# together into RetroArch's cores/ directory.
INFO_NAME="vita3k_libretro.info"

echo "================================================"
echo "  Vita3K iOS Libretro Core Build Script"
echo "================================================"

mkdir -p "$BUILD_DIR"

# -----------------------------------------------------------------------------
# CMake configure.  We invoke the existing top-level Vita3K CMakeLists.txt
# with -DLIBRETRO=ON which will cause it to descend into `libretro/`.
# -----------------------------------------------------------------------------
echo "[1/4] Configuring CMake..."
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/ios.cmake \
    -DLIBRETRO=ON \
    -DBUILD_TESTING=OFF \
    -DVITA3K_BUILD_TOOLS=OFF \
    . -B"$BUILD_DIR"

# -----------------------------------------------------------------------------
# Build.
# -----------------------------------------------------------------------------
echo "[2/4] Compiling vita3k_libretro..."
CPU_COUNT=$(sysctl -n hw.ncpu)
cmake --build "$BUILD_DIR" --target vita3k_libretro --config Release -j"$CPU_COUNT"

# -----------------------------------------------------------------------------
# Locate the produced dylib.
# -----------------------------------------------------------------------------
DYLIB="$BUILD_DIR/lib/vita3k_libretro.dylib"
if [ ! -f "$DYLIB" ]; then
    echo "ERROR: expected output not found at $DYLIB"
    echo "Searching for any produced dylib:"
    find "$BUILD_DIR" -name "*vita3k*.dylib" -o -name "*vita3k*.so" || true
    exit 1
fi

# -----------------------------------------------------------------------------
# Assemble the .framework bundle (iOS flat layout).
# -----------------------------------------------------------------------------
echo "[3/4] Packaging $FRAMEWORK_NAME..."
FRAMEWORK_DIR="$BUILD_DIR/$FRAMEWORK_NAME"
rm -rf "$FRAMEWORK_DIR"
mkdir -p "$FRAMEWORK_DIR"

cp "$DYLIB" "$FRAMEWORK_DIR/$TARGET_BINARY"

install_name_tool -id "@rpath/$FRAMEWORK_NAME/$TARGET_BINARY" "$FRAMEWORK_DIR/$TARGET_BINARY"

# -----------------------------------------------------------------------------
# Bundle static assets inside the framework root.  Vita3K's renderer resolves
# `state.static_assets_path / "shaders-builtin/opengl/render_main.vert"` at
# screen-filter construction; if the file is missing we fail framework init
# with a silent "no CPU, no audio, no video" black screen (M12.7.5 smoke).
# libretro_paths.cpp's runtime dladdr lookup points static_assets_path at
# the framework root, so putting the folder here is sufficient.
# -----------------------------------------------------------------------------
SHADER_SRC="vita3k/shaders-builtin"
if [ -d "$SHADER_SRC" ]; then
    echo "[3.5/5] Bundling shaders-builtin/ into framework..."
    rm -rf "$FRAMEWORK_DIR/shaders-builtin"
    cp -R "$SHADER_SRC" "$FRAMEWORK_DIR/shaders-builtin"
else
    echo "WARNING: $SHADER_SRC not found; renderer will fail to init at runtime."
fi

cat > "$FRAMEWORK_DIR/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>           <string>English</string>
    <key>CFBundleExecutable</key>                  <string>vita3k.libretro</string>
    <key>CFBundleIdentifier</key>                  <string>com.vita3k.libretro</string>
    <key>CFBundleInfoDictionaryVersion</key>       <string>6.0</string>
    <key>CFBundleName</key>                        <string>vita3k.libretro</string>
    <key>CFBundlePackageType</key>                 <string>FMWK</string>
    <key>CFBundleShortVersionString</key>          <string>0.1.0</string>
    <key>CFBundleVersion</key>                     <string>1</string>
    <key>CFBundleSignature</key>                   <string>????</string>
    <key>NSPrincipalClass</key>                    <string></string>
    <key>CFBundleSupportedPlatforms</key>          <array><string>iPhoneOS</string></array>
    <key>MinimumOSVersion</key>                    <string>13.0</string>
</dict>
</plist>
PLIST

echo "[4/5] Ad-hoc signing..."
# Sign the whole bundle (not just the binary) so that the bundled
# shaders-builtin/ resources are recorded in _CodeSignature/CodeResources.
# Otherwise iOS may reject the framework at dlopen / codesign verify time.
codesign --force --deep --sign - "$FRAMEWORK_DIR"

# -----------------------------------------------------------------------------
# Emit libretro core-info file next to the framework.  Format & fields follow
# libretro-super/dist/info/00_example_libretro.info and the ppsspp / azahar
# reference entries.  Placed at $BUILD_DIR/$INFO_NAME so the RetroArch-iOS
# packaging layer can copy (framework + info) together into its cores/ dir.
# -----------------------------------------------------------------------------
echo "[5/5] Generating $INFO_NAME..."
INFO_PATH="$BUILD_DIR/$INFO_NAME"

# Pull the core's self-reported version from libretro.cpp (VITA3K_LIBRETRO_VERSION);
# if absent we fall back to "Git" to match upstream cores.
DISPLAY_VERSION="Git"
if [ -f libretro/libretro.cpp ]; then
    CORE_VER=$(grep -E '^#define VITA3K_LIBRETRO_VERSION' libretro/libretro.cpp \
               | head -1 | sed -E 's/.*"([^"]+)".*/\1/') || true
    if [ -n "${CORE_VER:-}" ]; then
        DISPLAY_VERSION="$CORE_VER"
    fi
fi

cat > "$INFO_PATH" <<INFO
# Software Information
display_name = "Sony - PlayStation Vita (Vita3K)"
authors = "Vita3K Team"
supported_extensions = "vpk|pkg|pup|vita3k"
corename = "Vita3K"
categories = "Emulator"
license = "GPLv2+"
permissions = ""
display_version = "$DISPLAY_VERSION"

# Hardware Information
manufacturer = "Sony"
systemname = "PlayStation Vita"
systemid = "playstation_vita"

# Libretro Features
database = "Sony - PlayStation Vita"
supports_no_game = "false"
savestate = "true"
savestate_features = "serialized"
cheats = "false"
input_descriptors = "true"
memory_descriptors = "false"
libretro_saves = "true"
core_options = "true"
core_options_version = "2.0"
load_subsystem = "false"
hw_render = "true"
required_hw_api = "Vulkan >= 1.1"
needs_fullpath = "true"
disk_control = "false"
is_experimental = "true"

# Firmware / BIOS
firmware_count = 1
firmware0_desc = "PSVUPDAT.PUP (PS Vita firmware, optional)"
firmware0_path = "vita3k/PSVUPDAT.PUP"
firmware0_opt = "true"
notes = "(!) Firmware is installed via the retro_vita_install_firmware extension entry point; the frontend must call it once to make commercial games playable.|(!) Content is installed via retro_vita_install_pkg / retro_vita_install_vpk; the core owns the Vita3K emulator_path (pref_path) directory under the frontend save directory.|(!) Launching a game uses a pseudo-path scheme 'vita3k://<TITLE_ID>' fed to retro_load_game; only a compatible RetroArch-iOS fork currently knows how to generate one.|(!) CPU backend: vita3k_cpu_backend=ir_interpreter works everywhere. dynarmic_jit requires iOS 26+ (TXM with StikDebug, or non-TXM PPL dual mapping). On iOS < 26 leave it on ir_interpreter to avoid std::bad_alloc."

description = "Port of the Vita3K PlayStation Vita emulator to libretro, targeted at a custom RetroArch-iOS front-end. The core uses libretro core-options v2 for CPU/GPU/audio tuning and a family of retro_vita_* extension entry points (install_firmware, install_pkg, install_vpk, list_games, delete_game, get_pref_path, jit_status, ...) for game/system management. Video uses the libretro Vulkan HW context (MoltenVK on iOS). On iOS 26+ JIT is available via TXM (StikDebug required) or PPL dual-mapping; otherwise the PPSSPP-style IR cache-interpreter keeps everything playable."
INFO

# -----------------------------------------------------------------------------
# Verification.
# -----------------------------------------------------------------------------
echo ""
echo "================================================"
echo "SUCCESS: $FRAMEWORK_DIR"
echo "         $INFO_PATH"
echo "================================================"
ls -lh "$FRAMEWORK_DIR/$TARGET_BINARY" "$INFO_PATH"
echo ""
echo "Architecture:"
file   "$FRAMEWORK_DIR/$TARGET_BINARY"
echo ""
echo "Exported retro_* symbols:"
nm -gU "$FRAMEWORK_DIR/$TARGET_BINARY" | grep -E "retro_" | head -20
echo ""
echo "Signature:"
codesign -dv "$FRAMEWORK_DIR/$TARGET_BINARY" 2>&1 | head -5
echo ""
echo "Core info:"
head -10 "$INFO_PATH"
echo ""
echo "Next steps:"
echo "  1. Copy $FRAMEWORK_NAME into your RetroArch-iOS frameworks/ dir"
echo "  2. Copy $INFO_NAME into your RetroArch info/ dir (modules/info on iOS)"
echo "  3. In RetroArch iOS, pick 'Load Core' -> 'Vita3K'"
