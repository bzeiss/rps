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
    DEPENDS rps-pluginscanner
    COMMENT "Ensuring rps-pluginscanner is present for standalone"
)
add_dependencies(rps-standalone rps-standalone-deps)
