cmake_minimum_required(VERSION 3.25)

cmake_policy(SET CMP0167 NEW)

project(rps-plugin-scanner
    VERSION 0.2.1
    DESCRIPTION "RPS"
    LANGUAGES C CXX
)

# x64-windows* vcpkg triplets produce MSVC-ABI binaries. Using GNU-targeting
# clang++ (e.g. MSYS2 clang64) with those libraries causes linker failures
# such as missing LIBCMTD/OLDNAMES import libs.
if(WIN32
   AND VCPKG_TARGET_TRIPLET MATCHES "^x64-windows"
   AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    message(FATAL_ERROR
        "Incompatible toolchain combination detected:\n"
        "  compiler: ${CMAKE_CXX_COMPILER}\n"
        "  triplet:  ${VCPKG_TARGET_TRIPLET}\n\n"
        "The selected vcpkg triplet uses MSVC ABI libraries, but this clang++ "
        "targets GNU/MinGW. Use one of:\n"
        "  1) clang-cl or cl.exe with -DVCPKG_TARGET_TRIPLET=x64-windows-static\n"
        "  2) clang++ with a MinGW triplet (for example x64-mingw-static).")
endif()

# Enforce strictly C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Force static MSVC runtime to match static vcpkg dependencies (e.g. gRPC)
# We auto-enable this if we detect a static vcpkg triplet
if(VCPKG_TARGET_TRIPLET MATCHES "-static$" AND NOT DEFINED RPS_MSVC_STATIC_RUNTIME)
    set(RPS_MSVC_STATIC_RUNTIME ON CACHE BOOL "Use static MSVC runtime (/MT)" FORCE)
else()
    option(RPS_MSVC_STATIC_RUNTIME "Use static MSVC runtime (/MT)" OFF)
endif()

if(WIN32
   AND RPS_MSVC_STATIC_RUNTIME
   AND (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Resolve LNK4098 by ignoring the runtime we're not using for each config.
    # Keep this only for cl.exe-style drivers; clang++ (MSVC target) does not
    # accept raw '/NODEFAULTLIB:...' in CMAKE_EXE_LINKER_FLAGS.
    if(MSVC)
        set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /NODEFAULTLIB:LIBCMTD")
        set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} /NODEFAULTLIB:LIBCMT")
    endif()
endif()

# Disable Boost Auto-Linking on Windows (CMake handles libraries explicitly)
add_definitions(-DBOOST_ALL_NO_LIB)

# Strict warnings — applied only to our own targets, not third-party code
add_library(rps_warnings INTERFACE)
if(MSVC)
    target_compile_options(rps_warnings INTERFACE /W4 /WX /permissive-)
else()
    target_compile_options(rps_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Boost.WinAPI/Interprocess uses nested anonymous types — suppress Clang warning
        target_compile_options(rps_warnings INTERFACE -Wno-nested-anon-types)
    endif()
endif()

if(WIN32)
    target_compile_definitions(rps_warnings INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# Force static linking
if(WIN32 AND NOT MSVC AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()

# Boost: managed entirely by vcpkg
find_package(Boost REQUIRED COMPONENTS json program_options filesystem process interprocess uuid dll date_time crc)

# Find SQLite3
# Prefer the static library; fall back to shared via find_package
find_library(SQLITE3_STATIC_LIBRARY NAMES libsqlite3.a sqlite3.a)
if(SQLITE3_STATIC_LIBRARY)
    find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h REQUIRED)
    add_library(SQLite::SQLite3 UNKNOWN IMPORTED)
    set_target_properties(SQLite::SQLite3 PROPERTIES
        IMPORTED_LOCATION "${SQLITE3_STATIC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INCLUDE_DIR}"
    )
else()
    find_package(SQLite3 REQUIRED)
endif()

# Optional: VST2.4 plugin scanning (requires the proprietary VST2 SDK)
# Enable with: cmake -DRPS_ENABLE_VST2=ON
option(RPS_ENABLE_VST2 "Enable VST2.4 plugin scanning (requires VST2 SDK)" OFF)
if(RPS_ENABLE_VST2)
    set(RPS_VST2_SDK_PATH "${CMAKE_SOURCE_DIR}/libs/rps-core/include/rps/core/vstsdk2.4" CACHE PATH "Path to VST2.4 SDK root")
    if(NOT EXISTS "${RPS_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffect.h")
        message(FATAL_ERROR
            "VST2.4 SDK not found at: ${RPS_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffect.h\n"
            "Place the VST2.4 SDK in libs/rps-core/include/rps/core/vstsdk2.4/\n"
            "or set -DRPS_VST2_SDK_PATH=/path/to/vstsdk2.4"
        )
    endif()
    message(STATUS "VST2.4 support ENABLED (SDK: ${RPS_VST2_SDK_PATH})")
endif()

# Include directories for libraries
add_subdirectory(libs/rps-core)
add_subdirectory(libs/rps-proto)
add_subdirectory(libs/rps-ipc)
add_subdirectory(libs/rps-audio)
add_subdirectory(libs/rps-host)
add_subdirectory(libs/rps-engine)
add_subdirectory(libs/rps-gui)

# Include directories for applications
add_subdirectory(apps/rps-standalone)
add_subdirectory(apps/rps-pluginscanner)
add_subdirectory(apps/rps-server)
add_subdirectory(apps/rps-vstscannermaster)
add_subdirectory(apps/rps-pluginhost)

# Example clients
add_subdirectory(examples/cpp)

