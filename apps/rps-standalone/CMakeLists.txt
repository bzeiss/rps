add_executable(rps-standalone
    src/main.cpp
)

target_link_libraries(rps-standalone
    PRIVATE
        rps-engine
        rps_warnings
        Boost::program_options
)

# After build, copy rps-pluginscanner into standalone's directory
add_custom_command(
    TARGET rps-standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginscanner>
        "${CMAKE_CURRENT_BINARY_DIR}/$<TARGET_FILE_NAME:rps-pluginscanner>"
    COMMENT "Copying rps-pluginscanner to standalone directory"
)
