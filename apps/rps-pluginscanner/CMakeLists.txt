add_executable(rps-pluginscanner 
    src/main.cpp
    src/scanners/ClapScanner.cpp
    src/scanners/Vst3Scanner.cpp
)

target_include_directories(rps-pluginscanner PRIVATE 
    include
    ${CMAKE_SOURCE_DIR}/libs/rps-core/include/rps/core/vst3sdk
)

target_link_libraries(rps-pluginscanner
    PRIVATE
        rps-core
        rps-ipc
        rps_warnings
        Boost::program_options
        Boost::json
        Boost::filesystem
)

if(WIN32)
    target_link_libraries(rps-pluginscanner PRIVATE ws2_32 ole32)
else()
    target_link_libraries(rps-pluginscanner PRIVATE dl)
endif()



