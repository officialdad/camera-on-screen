# Linux build of the C-ABI shim -> libCameraOnScreen.Shim.so (issue #27 Phase 2).
# Windows keeps shim.vcxproj (deliberately outside the .sln); this file is Linux-only.
# Effects compile as passthrough stubs until the Maxine Linux runtimes land (Phase 4):
# COS_HAS_MAXINE / COS_HAS_MAXINE_AR / COS_HAS_FRUC are intentionally not defined here yet.
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
)
set_target_properties(shim PROPERTIES
  OUTPUT_NAME "CameraOnScreen.Shim"   # -> libCameraOnScreen.Shim.so
  CXX_VISIBILITY_PRESET hidden
)
target_compile_options(shim PRIVATE -Wall -Wextra -Werror)
target_link_libraries(shim PRIVATE Threads::Threads)

add_executable(v4l2_probe smoke/v4l2_probe.cpp)
target_compile_options(v4l2_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v4l2_probe PRIVATE shim)
