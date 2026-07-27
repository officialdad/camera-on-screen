# Linux build of the C-ABI shim -> libCameraOnScreen.Shim.so (issue #27 Phase 2).
# Windows keeps shim.vcxproj (deliberately outside the .sln); this file is Linux-only.
# Maxine (Phase 4): COS_VFX_SDK_DIR / COS_AR_SDK_DIR env point at the Linux SDK-CORE trees
# (include/ + features/*/include + lib/; e.g. ~/dev/VideoFX-linux/VideoFX). Unset -> the
# CI-safe passthrough stubs, exactly as before. COS_HAS_FRUC stays undefined on Linux
# (Optical Flow SDK has no non-interactive Linux download; follow-up issue).
cmake_minimum_required(VERSION 3.20)
project(CameraOnScreenShim CXX)

if(NOT UNIX OR APPLE)
  message(FATAL_ERROR "This CMake build targets Linux only; build shim.vcxproj on Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(shim SHARED
  shim.cpp
  capture_v4l2.cpp
  aigs.cpp
  eyecontact.cpp
  superres.cpp
  fruc.cpp
  paths.cpp
  vfx_paths.cpp
)
set_target_properties(shim PROPERTIES
  OUTPUT_NAME "CameraOnScreen.Shim"   # -> libCameraOnScreen.Shim.so
  CXX_VISIBILITY_PRESET hidden
)
target_compile_options(shim PRIVATE -Wall -Wextra -Werror)
target_link_libraries(shim PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# VFX (green screen). Requires the greenscreen feature header, so a core tree without the
# feature installed stays stub instead of failing mid-build.
set(_vfx "$ENV{COS_VFX_SDK_DIR}")
if(_vfx AND EXISTS "${_vfx}/include/nvVideoEffects.h"
        AND EXISTS "${_vfx}/features/nvvfxgreenscreen/include/nvVFXGreenScreen.h")
  target_compile_definitions(shim PRIVATE COS_HAS_MAXINE)
  target_include_directories(shim SYSTEM PRIVATE
    "${_vfx}/include" "${_vfx}/features/nvvfxgreenscreen/include")
  set(_maxine ON)
  message(STATUS "Maxine VFX: ENABLED (${_vfx})")
else()
  message(STATUS "Maxine VFX: stub (COS_VFX_SDK_DIR unset or not a VFX SDK-core tree)")
endif()

# AR (eye contact / gaze). VFX include dirs stay FIRST so nvCVImage.h comes from VFX 1.2 —
# the co-versioned pick (the shared NVCVImage runtime also comes from VFX, first-load-wins).
set(_ar "$ENV{COS_AR_SDK_DIR}")
if(_ar AND EXISTS "${_ar}/include/nvAR.h")
  target_compile_definitions(shim PRIVATE COS_HAS_MAXINE_AR)
  target_include_directories(shim SYSTEM PRIVATE "${_ar}/include")
  set(_maxine ON)
  message(STATUS "Maxine AR: ENABLED (${_ar})")
else()
  message(STATUS "Maxine AR: stub (COS_AR_SDK_DIR unset or not an AR SDK-core tree)")
endif()

if(_maxine)
  target_sources(shim PRIVATE maxine_linux.cpp maxine_proxy_linux.cpp)
endif()

add_executable(v4l2_probe smoke/v4l2_probe.cpp)
target_compile_options(v4l2_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v4l2_probe PRIVATE shim)
