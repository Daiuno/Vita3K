set(AUDIO_SOURCES
    src/audio.cpp
    src/impl/sdl_audio.cpp)

# cubeb's CoreAudio backend uses macOS-only AudioHardware APIs.  Skip it
# entirely on iOS; the SDL backend is already enough for the libretro core.
if(NOT IOS)
    list(APPEND AUDIO_SOURCES src/impl/cubeb_audio.cpp)
endif()

# Libretro PCM sink — only compiled when the libretro core build is
# active (top-level CMake injects LIBRETRO=1 in that mode).
if(LIBRETRO)
    list(APPEND AUDIO_SOURCES src/impl/libretro_audio.cpp)
endif()

add_library(audio STATIC ${AUDIO_SOURCES})

target_include_directories(audio PUBLIC include)
if(IOS)
    target_link_libraries(audio PRIVATE util SDL3::SDL3)
    target_compile_definitions(audio PRIVATE VITA3K_NO_CUBEB=1)
else()
    target_link_libraries(audio PRIVATE util cubeb SDL3::SDL3)
endif()
