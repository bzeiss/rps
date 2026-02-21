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

# Enable modern warnings
if(MSVC)
    add_compile_options(/W4 /WX /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Force static linking
if(WIN32 AND NOT MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()

set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME ON)

# Find Boost
# Required components: json, process, interprocess (header only mostly, but let's be safe), program_options, filesystem
find_package(Boost 1.81 REQUIRED COMPONENTS json program_options filesystem)

# Find SQLite3
# Force finding the static library
find_library(SQLITE3_STATIC_LIBRARY NAMES libsqlite3.a sqlite3.a REQUIRED)
find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h REQUIRED)

add_library(SQLite::SQLite3 UNKNOWN IMPORTED)
set_target_properties(SQLite::SQLite3 PROPERTIES
    IMPORTED_LOCATION "${SQLITE3_STATIC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INCLUDE_DIR}"
)

# Include directories for libraries
add_subdirectory(libs/rps-core)
add_subdirectory(libs/rps-ipc)

# Include directories for applications
add_subdirectory(apps/rps-pluginscanorchestrator)
add_subdirectory(apps/rps-pluginscanner)
