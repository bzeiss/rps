cmake_minimum_required(VERSION 3.25)

project(rps-plugin-scanner
    VERSION 0.1.0
    DESCRIPTION "Reliable Plugin Scanner"
    LANGUAGES CXX
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

# Find Boost
# Required components: json, process, interprocess (header only mostly, but let's be safe), program_options, filesystem
find_package(Boost 1.81 REQUIRED COMPONENTS json program_options filesystem)

# Find SQLite3
find_package(SQLite3 REQUIRED)

# Include directories for libraries
add_subdirectory(libs/rps-core)
add_subdirectory(libs/rps-ipc)

# Include directories for applications
add_subdirectory(apps/rps-pluginscanorchestrator)
add_subdirectory(apps/rps-pluginscanner)
