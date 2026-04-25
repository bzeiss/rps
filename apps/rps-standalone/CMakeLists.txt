add_executable(rps-standalone
    src/main.cpp
)

target_link_libraries(rps-standalone
    PRIVATE
        rps-engine
        rps_warnings
        Boost::program_options
)

add_custom_target(rps-standalone-deps ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginscanner>
        "$<TARGET_FILE_DIR:rps-standalone>/$<TARGET_FILE_NAME:rps-pluginscanner>"
    # Optional Rosetta fallback scanner
    COMMAND ${CMAKE_COMMAND} 
        -DSRC="$<TARGET_FILE_DIR:rps-pluginscanner>/rps-pluginscanner_x86_64" 
        -DDST="$<TARGET_FILE_DIR:rps-standalone>/rps-pluginscanner_x86_64" 
        -P "${CMAKE_SOURCE_DIR}/cmake/try_copy.cmake"
    DEPENDS rps-pluginscanner
    COMMENT "Ensuring rps-pluginscanner is present for standalone"
)
add_dependencies(rps-standalone rps-standalone-deps)
