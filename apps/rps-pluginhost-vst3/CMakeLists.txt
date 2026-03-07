set(VST3_SDK_ROOT "${CMAKE_SOURCE_DIR}/libs/rps-core/include/rps/core/vst3sdk")

# VST3 SDK hosting helpers — compiled as a separate object library
# to avoid our strict warning flags being applied to third-party code.
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

# The actual plugin host binary
add_executable(rps-pluginhost-vst3
    src/main.cpp
    src/Vst3GuiHost.cpp
)

target_include_directories(rps-pluginhost-vst3 PRIVATE
    src
    ${VST3_SDK_ROOT}
)

# Suppress warnings from VST3 SDK headers included in our code
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(rps-pluginhost-vst3 PRIVATE
        -Wno-unnecessary-virtual-specifier
        -Wno-pragma-pack
    )
endif()

target_link_libraries(rps-pluginhost-vst3
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
    target_link_libraries(rps-pluginhost-vst3 PRIVATE ole32)
    # Increase default stack size to 8MB — some JUCE-based plugins (Scaler 3)
    # overflow the default 1MB stack during createView().
    # The PE header stack size also applies to threads using default size.
    target_link_options(rps-pluginhost-vst3 PRIVATE /STACK:8388608)
elseif(APPLE)
    target_link_libraries(rps-pluginhost-vst3 PRIVATE dl)
else()
    target_link_libraries(rps-pluginhost-vst3 PRIVATE dl)
endif()

# Copy binary to example directories
add_custom_target(rps-pluginhost-vst3-deps ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginhost-vst3>
        "${CMAKE_SOURCE_DIR}/examples/python/$<TARGET_FILE_NAME:rps-pluginhost-vst3>"
    DEPENDS rps-pluginhost-vst3
    COMMENT "Copying rps-pluginhost-vst3 to example directories"
)
