cmake_minimum_required(VERSION 3.25)

cmake_policy(SET CMP0167 NEW)

project(rps-plugin-scanner
    VERSION 0.1.0
    DESCRIPTION "Reliable Plugin Scanner"
    LANGUAGES C CXX
)

# Enforce strictly C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings — applied only to our own targets, not third-party code
add_library(rps_warnings INTERFACE)
if(MSVC)
    target_compile_options(rps_warnings INTERFACE /W4 /WX /permissive-)
else()
    target_compile_options(rps_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

# Force static linking
if(WIN32 AND NOT MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()

# Boost: build from local source tree via add_subdirectory
# Override with: cmake -DBOOST_SOURCE_DIR=/path/to/boost
#            or: export BOOST_SOURCE_DIR=/path/to/boost && cmake -B build
if(DEFINED ENV{BOOST_SOURCE_DIR} AND NOT DEFINED BOOST_SOURCE_DIR)
    set(BOOST_SOURCE_DIR "$ENV{BOOST_SOURCE_DIR}" CACHE PATH "Path to Boost source tree (requires Boost 1.90+)")
else()
    set(BOOST_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/boost" CACHE PATH "Path to Boost source tree (requires Boost 1.90+)")
endif()
if(NOT EXISTS "${BOOST_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Boost CMake source tree not found at: ${BOOST_SOURCE_DIR}\n"
        "Note: The official boost.org tarball does NOT include CMake support.\n"
        "You must clone Boost from GitHub:\n"
        "  git clone https://github.com/boostorg/boost.git /path/to/boost\n"
        "  cd /path/to/boost && git checkout boost-1.90.0\n"
        "  git submodule update --init --recursive\n"
        "Then pass: cmake -DBOOST_SOURCE_DIR=/path/to/boost\n"
    )
endif()
set(BOOST_INCLUDE_LIBRARIES json program_options filesystem process interprocess uuid)
add_subdirectory(${BOOST_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/boost EXCLUDE_FROM_ALL)

# Find SQLite3
# Force finding the static library
find_library(SQLITE3_STATIC_LIBRARY NAMES libsqlite3.a sqlite3.a REQUIRED)
find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h REQUIRED)

add_library(SQLite::SQLite3 UNKNOWN IMPORTED)
set_target_properties(SQLite::SQLite3 PROPERTIES
    IMPORTED_LOCATION "${SQLITE3_STATIC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INCLUDE_DIR}"
)

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
add_subdirectory(libs/rps-ipc)
add_subdirectory(libs/rps-engine)

# Include directories for applications
add_subdirectory(apps/rps-pluginscanorchestrator)
add_subdirectory(apps/rps-pluginscanner)
add_subdirectory(apps/rps-server)
