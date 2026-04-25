# Vita3K Libretro Core
#
# This target wraps the Vita3K emulator engine as a Libretro dynamic library.
# It is built on top of the existing Vita3K subsystem targets (`app`, `cpu`,
# `renderer`, `io`, ...) and exposes the 18 mandatory libretro API functions
# plus Vita-specific extensions (see libretro_extensions.h).
#
# Output:
#   - vita3k_libretro.dylib / .so / .dll
#   On iOS this dylib is further packaged as `vita3k.libretro.framework`
#   by build_ios_libretro.sh at the repository root.

cmake_minimum_required(VERSION 3.22)

set(LIBRETRO_CORE_SOURCES
    libretro.cpp
    libretro_paths.cpp
    libretro_boot.cpp
    libretro_core_opts.cpp
    libretro_game_boot.cpp
    libretro_stub_sdl.cpp
    libretro_vulkan_context.cpp
    libretro_install.cpp
    libretro_catalog.cpp
    libretro_audio.cpp
    libretro_input.cpp
    libretro_camera.cpp
    libretro_savestate.cpp
    # Re-use the standalone frontend's `load_app` + `run_app` (and their
    # static helpers).  interface.cpp is *only* built into the vita3k
    # executable in normal builds, which is skipped in LIBRETRO mode.
    # We pull it in directly here so the libretro TU can call into it.
    # handle_events and friends remain unreferenced (and get dead-code
    # stripped by the linker with -fvisibility=hidden + LTO); what we
    # need is load_app() + run_app() only.
    ${CMAKE_CURRENT_SOURCE_DIR}/../vita3k/interface.cpp
)

set(LIBRETRO_CORE_HEADERS
    libretro_extensions.h
    libretro_core_options.h
    libretro_core_options_intl.h
    libretro_core_opts.h
    libretro_paths.h
    libretro_boot.h
    libretro_game_boot.h
    libretro_stub_sdl.h
    libretro_vulkan_context.h
    libretro_input.h
    libretro_internal.h
    libretro_ios_jit.h
    libretro_audio.h
)

add_library(vita3k_libretro MODULE
    ${LIBRETRO_CORE_SOURCES}
    ${LIBRETRO_CORE_HEADERS}
)

# ---------------------------------------------------------------------------
# iOS-specific sources (JIT dual-mapping + camera permission bridge)
# ---------------------------------------------------------------------------
if(APPLE AND IOS)
    # Plain C++: oaknut/code_block.hpp pulls os/log.h inside namespace oaknut;
    # compiling as .mm (ObjC++) breaks the iOS SDK headers.
    target_sources(vita3k_libretro PRIVATE libretro_ios_jit.cpp)
    target_include_directories(vita3k_libretro PRIVATE
        ${CMAKE_SOURCE_DIR}/external/dynarmic/externals/oaknut/include)
    target_link_libraries(vita3k_libretro PRIVATE
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
    )
    if(TARGET merry::oaknut)
        target_link_libraries(vita3k_libretro PRIVATE merry::oaknut)
    endif()
endif()

# ---------------------------------------------------------------------------
# libretro-common (cloned from Daiuno/libretro-common master into
# ${CMAKE_CURRENT_SOURCE_DIR}/libretro-common).
# ---------------------------------------------------------------------------
target_include_directories(vita3k_libretro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libretro-common/include
    ${CMAKE_SOURCE_DIR}/vita3k/renderer/include
    ${CMAKE_SOURCE_DIR}/external/miniz
)

# ---------------------------------------------------------------------------
# Compile flags / definitions
# ---------------------------------------------------------------------------
target_compile_features(vita3k_libretro PRIVATE cxx_std_20)

target_compile_definitions(vita3k_libretro PRIVATE
    VITA3K_HEADLESS_SDL=1
)

# Hide non-exported symbols on ELF/Mach-O to keep the library lean.
if(NOT MSVC)
    target_compile_options(vita3k_libretro PRIVATE
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )
endif()

# Apple exported symbols whitelist
if(APPLE)
    set(LIBRETRO_EXPORTS_DEF "${CMAKE_CURRENT_SOURCE_DIR}/libretro.osx.def")
    if(EXISTS "${LIBRETRO_EXPORTS_DEF}")
        set_target_properties(vita3k_libretro PROPERTIES
            LINK_FLAGS "-Wl,-exported_symbols_list,${LIBRETRO_EXPORTS_DEF}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Link against the Vita3K subsystem libraries.
# ---------------------------------------------------------------------------
set(VITA3K_ENGINE_LIBS
    app audio ctrl gui dialog compat gdbstub host_dialog lang
    emuenv config cpu display gxm io kernel mem
    modules motion net ngs np packages patch regmgr
    renderer rtc shader shaders-builtin threads touch util camera
)

foreach(lib ${VITA3K_ENGINE_LIBS})
    if(TARGET ${lib})
        target_link_libraries(vita3k_libretro PRIVATE ${lib})
    endif()
endforeach()
if(TARGET ffmpeg)
    target_link_libraries(vita3k_libretro PRIVATE ffmpeg)
endif()

# ---------------------------------------------------------------------------
# Output naming:  vita3k_libretro.<ext>  (no `lib` prefix, libretro convention)
# ---------------------------------------------------------------------------
set_target_properties(vita3k_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "vita3k_libretro"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

if(APPLE)
    set_target_properties(vita3k_libretro PROPERTIES SUFFIX ".dylib")
elseif(WIN32)
    set_target_properties(vita3k_libretro PROPERTIES SUFFIX ".dll")
else()
    set_target_properties(vita3k_libretro PROPERTIES SUFFIX ".so")
endif()

message(STATUS "vita3k_libretro: target configured (iOS=${IOS}, APPLE=${APPLE})")
