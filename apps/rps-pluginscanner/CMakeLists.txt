add_executable(rps-pluginscanner src/main.cpp)

target_include_directories(rps-pluginscanner PRIVATE include)

target_link_libraries(rps-pluginscanner
    PRIVATE
        rps-core
        rps-ipc
        Boost::program_options
        Boost::json
        Boost::filesystem
)

if(WIN32)
    target_link_libraries(rps-pluginscanner PRIVATE ws2_32)
endif()

