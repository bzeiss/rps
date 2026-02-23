add_executable(rps-pluginscanorchestrator 
    src/main.cpp
)

target_link_libraries(rps-pluginscanorchestrator
    PRIVATE
        rps-engine
        rps_warnings
        Boost::program_options
        Boost::filesystem
)


