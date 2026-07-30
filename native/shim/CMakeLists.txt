# Linux build of the C-ABI shim -> libCameraOnScreen.Shim.so (issue #27 Phase 2).
# Windows keeps shim.vcxproj (deliberately outside the .sln); this file is Linux-only.
# Maxine (Phase 4): COS_VFX_SDK_DIR / COS_AR_SDK_DIR env point at the Linux SDK-CORE trees
# (include/ + features/*/include + lib/; e.g. ~/dev/VideoFX-linux/VideoFX). Unset -> the
# CI-safe passthrough stubs, exactly as before. COS_FRUC_SDK_DIR points at the Optical Flow
# SDK root (e.g. ~/dev/Optical_Flow_SDK_5.0.7) and enables FRUC frame interpolation; the
# CUDA driver API is dlsym'd from libcuda.so.1 at runtime, so no CUDA toolkit is needed.
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
  matte_ops.cpp
  seg_onnx.cpp
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

# FRUC (frame interpolation, Optical Flow SDK — separate product, DesignWorks license).
set(_fruc "$ENV{COS_FRUC_SDK_DIR}")
if(_fruc AND EXISTS "${_fruc}/NvOFFRUC/Interface/NvOFFRUC.h")
  target_compile_definitions(shim PRIVATE COS_HAS_FRUC)
  target_include_directories(shim SYSTEM PRIVATE "${_fruc}/NvOFFRUC/Interface")
  message(STATUS "FRUC: ENABLED (${_fruc})")
else()
  message(STATUS "FRUC: stub (COS_FRUC_SDK_DIR unset or not an Optical Flow SDK root)")
endif()

add_executable(v4l2_probe smoke/v4l2_probe.cpp)
target_compile_options(v4l2_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v4l2_probe PRIVATE shim)

# ONNX green-screen smoke (issue #24): compiles the engine directly (shim symbols are
# hidden). Needs COS_SEG_RUNTIME_DIR (ORT lib + model) at run time, not build time.
add_executable(seg_probe smoke/seg_probe.cpp seg_onnx.cpp matte_ops.cpp paths.cpp)
target_compile_options(seg_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(seg_probe PRIVATE ${CMAKE_DL_LIBS})
