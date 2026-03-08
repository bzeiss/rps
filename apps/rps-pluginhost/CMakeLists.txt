set(VST3_SDK_ROOT "${CMAKE_SOURCE_DIR}/libs/rps-core/include/rps/core/vst3sdk")

# ---- Unified plugin host: all formats in one binary ----
add_executable(rps-pluginhost
    src/main.cpp
    # VST3 format host sources
    ${CMAKE_SOURCE_DIR}/apps/rps-pluginhost-vst3/src/Vst3GuiHost.cpp
    # CLAP format host sources
    ${CMAKE_SOURCE_DIR}/apps/rps-pluginhost-clap/src/ClapGuiHost.cpp
)

target_include_directories(rps-pluginhost PRIVATE
    src
    # VST3 host headers and SDK
    ${CMAKE_SOURCE_DIR}/apps/rps-pluginhost-vst3/src
    ${VST3_SDK_ROOT}
    # CLAP host headers and SDK
    ${CMAKE_SOURCE_DIR}/apps/rps-pluginhost-clap/src
    ${CMAKE_SOURCE_DIR}/libs/rps-core/include/rps/core/clap/include
)

# Suppress warnings from VST3 SDK headers
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(rps-pluginhost PRIVATE
        -Wno-unnecessary-virtual-specifier
        -Wno-pragma-pack
    )
endif()

target_link_libraries(rps-pluginhost
    PRIVATE
        rps-gui
        rps-core
        rps-ipc
        rps_warnings
        vst3-hosting-helpers
        Boost::program_options
        Boost::json
        Boost::filesystem
)

if(WIN32)
    target_link_libraries(rps-pluginhost PRIVATE ole32)
    # 8MB stack for JUCE-based plugins
    target_link_options(rps-pluginhost PRIVATE /STACK:8388608)
elseif(APPLE)
    target_link_libraries(rps-pluginhost PRIVATE dl)
else()
    target_link_libraries(rps-pluginhost PRIVATE dl)
endif()

# Copy to example directories
add_custom_command(TARGET rps-pluginhost POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC=$<TARGET_FILE:rps-pluginhost>
        -DDST=${CMAKE_SOURCE_DIR}/examples/python/$<TARGET_FILE_NAME:rps-pluginhost>
        -P ${CMAKE_SOURCE_DIR}/cmake/try_copy.cmake
    COMMENT "Copying rps-pluginhost to example directories"
)
add_custom_target(copy-pluginhost ALL
    COMMAND ${CMAKE_COMMAND}
        -DSRC=$<TARGET_FILE:rps-pluginhost>
        -DDST=${CMAKE_SOURCE_DIR}/examples/python/$<TARGET_FILE_NAME:rps-pluginhost>
        -P ${CMAKE_SOURCE_DIR}/cmake/try_copy.cmake
    DEPENDS rps-pluginhost
    COMMENT "Ensuring rps-pluginhost is present in example directories"
)
