# rps-host: header-only interface library
add_library(rps-host INTERFACE)

target_include_directories(rps-host INTERFACE include)

target_link_libraries(rps-host
    INTERFACE
        rps-ipc
        Boost::filesystem
)
