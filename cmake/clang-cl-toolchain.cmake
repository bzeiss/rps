# Clang-CL toolchain — use LLVM tools for compilation, linking, and assembly.
# Requires: LLVM/Clang installed, run from a Developer PowerShell for VS.
#
# This file is chainloaded by vcpkg instead of its default windows.cmake,
# so it must replicate the CRT flag handling that windows.cmake normally does.

cmake_minimum_required(VERSION 3.25)

if(_CLANG_CL_TOOLCHAIN_LOADED)
    return()
endif()
set(_CLANG_CL_TOOLCHAIN_LOADED TRUE)

# ── LLVM tools ───────────────────────────────────────────────────────────────
set(_LLVM_HINTS
    "C:/Program Files/LLVM/bin"
    "$ENV{PROGRAMFILES}/LLVM/bin"
    "$ENV{LLVM_PATH}/bin"
)

find_program(CLANG_CL_EXECUTABLE   clang-cl  HINTS ${_LLVM_HINTS} REQUIRED)
find_program(LLD_LINK_EXECUTABLE   lld-link  HINTS ${_LLVM_HINTS} REQUIRED)
find_program(LLVM_RC_EXECUTABLE    llvm-rc   HINTS ${_LLVM_HINTS} REQUIRED)
find_program(LLVM_MT_EXECUTABLE    llvm-mt   HINTS ${_LLVM_HINTS} REQUIRED)
find_program(LLVM_ML_EXECUTABLE    llvm-ml64 HINTS ${_LLVM_HINTS} REQUIRED)

set(CMAKE_C_COMPILER           "${CLANG_CL_EXECUTABLE}")
set(CMAKE_CXX_COMPILER         "${CLANG_CL_EXECUTABLE}")
set(CMAKE_LINKER                "${LLD_LINK_EXECUTABLE}")
set(CMAKE_RC_COMPILER           "${LLVM_RC_EXECUTABLE}")
set(CMAKE_MT                    "${LLVM_MT_EXECUTABLE}")
set(CMAKE_ASM_MASM_COMPILER     "${LLVM_ML_EXECUTABLE}")

# ── CRT linkage (replicates vcpkg's windows.cmake logic) ────────────────────
# When vcpkg chainloads a custom toolchain, its default windows.cmake is
# bypassed. That file normally bakes /MT or /MD into the per-config compiler
# flags. Without this, CMake's defaults (/MD) are used, causing CRT mismatch
# with vcpkg libraries built via the triplet's VCPKG_CRT_LINKAGE setting.
#
# We use the same approach: set CMAKE_MSVC_RUNTIME_LIBRARY (for modern CMake)
# AND bake /MT into the per-config flags (for older cmake_minimum_required).

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

# Determine CRT flag from VCPKG_CRT_LINKAGE (set by vcpkg) or default to /MT
if(VCPKG_CRT_LINKAGE STREQUAL "dynamic")
    set(_CRT_FLAG "/MD")
elseif(VCPKG_CRT_LINKAGE STREQUAL "static")
    set(_CRT_FLAG "/MT")
else()
    # Default to static when not running under vcpkg
    set(_CRT_FLAG "/MT")
endif()

set(CMAKE_C_FLAGS_RELEASE   "${_CRT_FLAG} /O2 /Oi /Gy /DNDEBUG /Z7 ${VCPKG_C_FLAGS_RELEASE}"   CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "${_CRT_FLAG} /O2 /Oi /Gy /DNDEBUG /Z7 ${VCPKG_CXX_FLAGS_RELEASE}" CACHE STRING "")
set(CMAKE_C_FLAGS_DEBUG     "${_CRT_FLAG}d /Z7 /Ob0 /Od /RTC1 ${VCPKG_C_FLAGS_DEBUG}"            CACHE STRING "")
set(CMAKE_CXX_FLAGS_DEBUG   "${_CRT_FLAG}d /Z7 /Ob0 /Od /RTC1 ${VCPKG_CXX_FLAGS_DEBUG}"          CACHE STRING "")

# Base flags (without /MP since clang-cl doesn't support it)
set(CMAKE_C_FLAGS   " /nologo /DWIN32 /D_WINDOWS /utf-8 ${VCPKG_C_FLAGS}"            CACHE STRING "")
set(CMAKE_CXX_FLAGS " /nologo /DWIN32 /D_WINDOWS /utf-8 /GR /EHsc ${VCPKG_CXX_FLAGS}" CACHE STRING "")

unset(_CRT_FLAG)
