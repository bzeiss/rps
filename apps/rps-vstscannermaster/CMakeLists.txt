add_executable(vstscannermaster src/main.cpp)
target_link_libraries(vstscannermaster
    PRIVATE
        rps-engine
        rps_warnings
        Boost::program_options
)
