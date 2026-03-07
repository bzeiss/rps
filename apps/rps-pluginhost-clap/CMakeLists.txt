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

# Copy on rebuild and ensure copies exist on every build
add_custom_command(TARGET rps-pluginhost-clap POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC=$<TARGET_FILE:rps-pluginhost-clap>
        -DDST=${CMAKE_SOURCE_DIR}/examples/python/$<TARGET_FILE_NAME:rps-pluginhost-clap>
        -P ${CMAKE_SOURCE_DIR}/cmake/try_copy.cmake
    COMMENT "Copying rps-pluginhost-clap to example directories"
)
add_custom_target(copy-pluginhost-clap ALL
    COMMAND ${CMAKE_COMMAND}
        -DSRC=$<TARGET_FILE:rps-pluginhost-clap>
        -DDST=${CMAKE_SOURCE_DIR}/examples/python/$<TARGET_FILE_NAME:rps-pluginhost-clap>
        -P ${CMAKE_SOURCE_DIR}/cmake/try_copy.cmake
    DEPENDS rps-pluginhost-clap
    COMMENT "Ensuring rps-pluginhost-clap is present in example directories"
)
