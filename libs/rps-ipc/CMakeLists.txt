add_library(rps-ipc STATIC 
    src/Connection.cpp
    src/StdioPipeConnection.cpp
)
target_include_directories(rps-ipc PUBLIC include)
target_link_libraries(rps-ipc PUBLIC rps-proto Boost::interprocess Boost::date_time PRIVATE rps_warnings)
