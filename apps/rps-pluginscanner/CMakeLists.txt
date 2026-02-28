set(SCANNER_SOURCES
    src/main.cpp
    src/scanners/ClapScanner.cpp
    src/scanners/Vst3Scanner.cpp
    src/scanners/AaxScanner.cpp
)

if(RPS_ENABLE_VST2)
    list(APPEND SCANNER_SOURCES src/scanners/Vst2Scanner.cpp)
endif()

if(UNIX AND NOT APPLE)
    list(APPEND SCANNER_SOURCES src/scanners/Lv2Scanner.cpp)
    list(APPEND SCANNER_SOURCES src/scanners/LadspaScanner.cpp)
endif()

if(APPLE)
    enable_language(OBJCXX)
    list(APPEND SCANNER_SOURCES src/scanners/AuScanner.mm)
endif()

add_executable(rps-pluginscanner ${SCANNER_SOURCES})

target_include_directories(rps-pluginscanner PRIVATE 
    include
    ${CMAKE_SOURCE_DIR}/libs/rps-core/include/rps/core/vst3sdk
    ${CMAKE_SOURCE_DIR}/libs/rps-core/include/rps/core/lv2/include
)

if(RPS_ENABLE_VST2)
    target_include_directories(rps-pluginscanner PRIVATE ${RPS_VST2_SDK_PATH})
    target_compile_definitions(rps-pluginscanner PRIVATE RPS_VST2_ENABLED)
endif()

target_link_libraries(rps-pluginscanner
    PRIVATE
        rps-core
        rps-ipc
        rps_warnings
        Boost::program_options
        Boost::json
        Boost::filesystem
)

add_custom_target(rps-pluginscanner-deps ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginscanner>
        "${CMAKE_SOURCE_DIR}/examples/python/$<TARGET_FILE_NAME:rps-pluginscanner>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginscanner>
        "${CMAKE_SOURCE_DIR}/examples/java/$<TARGET_FILE_NAME:rps-pluginscanner>"
    DEPENDS rps-pluginscanner
    COMMENT "Ensuring rps-pluginscanner is present in example directories"
)

if(WIN32)
    target_link_libraries(rps-pluginscanner PRIVATE ws2_32 ole32)
elseif(APPLE)
    target_link_libraries(rps-pluginscanner PRIVATE dl "-framework CoreFoundation" "-framework Foundation" "-framework AudioToolbox" "-framework AVFoundation")
else()
    target_link_libraries(rps-pluginscanner PRIVATE dl)
endif()



