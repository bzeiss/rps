add_executable(vstscannermaster src/main.cpp)

# Ensure the scanner is built before we try to copy it
add_dependencies(vstscannermaster rps-pluginscanner)

target_link_libraries(vstscannermaster
    PRIVATE
        rps-engine
        rps_warnings
        Boost::program_options
        Boost::dll
)

add_custom_target(vstscannermaster-deps ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rps-pluginscanner>
        "$<TARGET_FILE_DIR:vstscannermaster>/$<TARGET_FILE_NAME:rps-pluginscanner>"
    DEPENDS rps-pluginscanner
    COMMENT "Ensuring rps-pluginscanner is present for vstscannermaster"
)
add_dependencies(vstscannermaster vstscannermaster-deps)
