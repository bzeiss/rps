set(VST3_SDK_ROOT "${CMAKE_SOURCE_DIR}/libs/rps-core/include/rps/core/vst3sdk")

# ---------------------------------------------------------------------------
# VST3 SDK hosting helpers — compiled as a separate object library
# to avoid our strict warning flags being applied to third-party code.
# ---------------------------------------------------------------------------
add_library(vst3-hosting-helpers OBJECT
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    # Base SDK sources for FUID, IID symbol definitions
    ${VST3_SDK_ROOT}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK_ROOT}/pluginterfaces/base/coreiids.cpp
    # VST-specific IID definitions (IComponentHandler, IPlugFrame, etc.)
    ${VST3_SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp
    # String conversion utilities (used by module_win32.cpp)
    ${VST3_SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/common/commonstringconvert.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/common/commoniids.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/common/memorystream.cpp
    # Parameter changes support (for delivering changes to processor)
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp
)

# Platform-specific module loader
if(WIN32)
    target_sources(vst3-hosting-helpers PRIVATE
        ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp
    )
elseif(APPLE)
    target_sources(vst3-hosting-helpers PRIVATE
        ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm
    )
else()
    target_sources(vst3-hosting-helpers PRIVATE
        ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_linux.cpp
    )
endif()

target_include_directories(vst3-hosting-helpers PRIVATE
    ${VST3_SDK_ROOT}
)

# Minimal warning flags for third-party SDK code (no -WX/rps_warnings)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(vst3-hosting-helpers PRIVATE
        -Wno-unnecessary-virtual-specifier
        -Wno-pragma-pack
        -Wno-error=unused-parameter
    )
endif()
# Do NOT link rps_warnings here — third-party code.

# ---------------------------------------------------------------------------
# Unified plugin host: all formats in one binary
# ---------------------------------------------------------------------------
add_executable(rps-pluginhost
    src/main.cpp
    # VST3 format host
    src/vst3/Vst3GuiHost.cpp
    # CLAP format host
    src/clap/ClapGuiHost.cpp
)

target_include_directories(rps-pluginhost PRIVATE
    src
    # VST3 host headers and SDK
    src/vst3
    ${VST3_SDK_ROOT}
    # CLAP host headers and SDK
    src/clap
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
        rps-coordinator
        rps_warnings
        vst3-hosting-helpers
        Boost::program_options
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
