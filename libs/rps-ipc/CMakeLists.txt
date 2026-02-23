add_library(rps-ipc STATIC 
    src/Messages.cpp
    src/Connection.cpp
)
target_include_directories(rps-ipc PUBLIC include)
target_link_libraries(rps-ipc PUBLIC Boost::json PRIVATE rps_warnings)



