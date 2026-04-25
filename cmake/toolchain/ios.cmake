# Vita3K iOS arm64 toolchain (for the libretro core).
# Derived from PPSSPP's cmake/Toolchains/ios.cmake.
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/ios.cmake -DLIBRETRO=ON ...
#
# Variables exposed:
#   IOS_PLATFORM = OS (default) | SIMULATOR
#   CMAKE_IOS_SDK_ROOT = auto (xcrun) | <custom SDK path>

set(MOBILE_DEVICE ON)
set(USING_GLES3 ON)
set(IPHONEOS_DEPLOYMENT_TARGET 13.0)

set(CMAKE_SYSTEM_NAME      iOS)
set(CMAKE_SYSTEM_VERSION   ${IPHONEOS_DEPLOYMENT_TARGET})
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(IOS                    ON)
set(CMAKE_CROSSCOMPILING   ON)
set(CMAKE_MACOSX_BUNDLE    YES)
# Force our custom iOS Boost build; never pick up host Homebrew/system Boost.
set(VITA3K_FORCE_CUSTOM_BOOST ON CACHE BOOL "" FORCE)

if(NOT DEFINED IOS_PLATFORM)
    set(IOS_PLATFORM "OS")
endif()
set(IOS_PLATFORM ${IOS_PLATFORM} CACHE STRING "Type of iOS platform to build for (OS/SIMULATOR)")

if(IOS_PLATFORM STREQUAL "OS")
    set(IOS_SDK_NAME "iphoneos")
    set(IOS_ARCH "arm64")
elseif(IOS_PLATFORM STREQUAL "SIMULATOR")
    set(IOS_SDK_NAME "iphonesimulator")
    set(IOS_ARCH "arm64")
else()
    message(FATAL_ERROR "Unsupported IOS_PLATFORM value: '${IOS_PLATFORM}'. Use OS or SIMULATOR.")
endif()

if(NOT CMAKE_IOS_SDK_ROOT)
    execute_process(
        COMMAND xcrun --sdk ${IOS_SDK_NAME} --show-sdk-path
        OUTPUT_VARIABLE CMAKE_IOS_SDK_ROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "Toolchain using default iOS SDK: ${CMAKE_IOS_SDK_ROOT}")
endif()
set(CMAKE_IOS_SDK_ROOT ${CMAKE_IOS_SDK_ROOT} CACHE PATH "Location of the selected iOS SDK")

set(CMAKE_OSX_SYSROOT           ${CMAKE_IOS_SDK_ROOT}             CACHE PATH   "iOS sysroot")
set(CMAKE_OSX_ARCHITECTURES     "${IOS_ARCH}"                     CACHE STRING "iOS build architecture")
set(CMAKE_OSX_DEPLOYMENT_TARGET "${IPHONEOS_DEPLOYMENT_TARGET}"   CACHE STRING "iOS deployment target")

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -stdlib=libc++")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")

set(CMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++")
set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET ${IPHONEOS_DEPLOYMENT_TARGET})

add_definitions(-DGL_ETC1_RGB8_OES=0 -U__STRICT_ANSI__)

# ASM flags
set(CMAKE_ASM_FLAGS "" CACHE STRING "" FORCE)
foreach(arch ${IOS_ARCH})
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -arch ${arch}" CACHE STRING "" FORCE)
endforeach()

set(CMAKE_FIND_ROOT_PATH ${CMAKE_IOS_SDK_ROOT} ${CMAKE_PREFIX_PATH} CACHE STRING "iOS find root")
set(CMAKE_FIND_FRAMEWORK FIRST)
set(CMAKE_SYSTEM_FRAMEWORK_PATH
    ${CMAKE_IOS_SDK_ROOT}/System/Library/Frameworks
    ${CMAKE_IOS_SDK_ROOT}/System/Library/PrivateFrameworks
    ${CMAKE_IOS_SDK_ROOT}/Developer/Library/Frameworks
)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
