add_executable(rps-pluginscanorchestrator 
    src/main.cpp
    src/ProcessPool.cpp
)

target_include_directories(rps-pluginscanorchestrator PRIVATE include)

target_link_libraries(rps-pluginscanorchestrator
    PRIVATE
        rps-core
        rps-ipc
        Boost::program_options
        Boost::json
        Boost::filesystem
        SQLite::SQLite3
)

if(WIN32)
    target_link_libraries(rps-pluginscanorchestrator PRIVATE ws2_32)
endif()


