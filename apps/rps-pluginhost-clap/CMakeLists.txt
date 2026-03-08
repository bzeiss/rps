add_executable(rps-pluginhost-clap
    src/main.cpp
    src/ClapGuiHost.cpp
)

target_include_directories(rps-pluginhost-clap PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/libs/rps-core/include/rps/core/clap/include
)

target_link_libraries(rps-pluginhost-clap
    PRIVATE
        rps-gui
        rps-core
        rps-ipc
        rps_warnings
        Boost::program_options
        Boost::json
        Boost::filesystem
)

if(WIN32)
    target_link_libraries(rps-pluginhost-clap PRIVATE ole32)
elseif(APPLE)
    target_link_libraries(rps-pluginhost-clap PRIVATE dl)
else()
    target_link_libraries(rps-pluginhost-clap PRIVATE dl)
endif()

