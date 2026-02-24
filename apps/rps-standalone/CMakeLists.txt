add_executable(rps-standalone
    src/main.cpp
)

target_link_libraries(rps-standalone
    PRIVATE
        rps-engine
        rps_warnings
        Boost::program_options
)
